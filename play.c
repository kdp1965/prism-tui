// DSM audio playback ('play' / 'speed' commands).
//
// The DSM chroma shifts a pre-encoded 1-bit delta-sigma stream out of the
// 24-bit shift register to the audio PMOD on uo_out[7]; the CPU keeps the
// preload register fed with the next 24 sample bits, paced by the PRISM
// interrupt.

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <gpio.h>
#include <csr.h>
#include <uart.h>
#include <prism.h>
#include "prism_tui.h"

const uint32_t chroma_dsm[PRISM_CHROMA_WORDS*2] = {
    0x00000bc0, 0x00001000,
    0x00000bc0, 0x00001000,
    0x00000bc0, 0x00001000,
    0x00000bc0, 0x00001000,
    0x000009b8, 0x24003191,
    0x00000960, 0x0400301d,
    0x00000a92, 0x0200501e,
    0x000009b8, 0x24003190,
};
// CHROMA_DSM_CTRL is in prism_tui.h

// comm register time base for the sample clock ('speed' command)
uint32_t play_speed = 60;

// ==========================================================================
// L4Z streamed playback ('playz' command).
//
// The song sits in flash as independently compressed L4Z blocks (built by
// tools/l4z).  The foreground stream-decompresses blocks into one of two
// ping-pong sample buffers in PSRAM and unpacks the 3-byte samples to one
// aligned word each; the raw interrupt handler in prism_isr.s plays them,
// one word load + PRELOAD write + HOST_TOGGLE write per PRISM interrupt.
// The ISR swaps buffers itself and raises l4z_ch.consumed so the
// foreground knows to refill the drained half.  Blob layout:
//   u32 total uncompressed bytes, u32 block size,
//   then per block { u16 clen, data }, clen 0xFFFF = stored raw.
// ==========================================================================

#define L4Z_BLK      3072u              // uncompressed bytes per block
#define L4Z_SAMPLES  (L4Z_BLK / 3)      // 24-bit samples per buffer
#define L4Z_SILENCE  0x00555555u        // DSM idle pattern (analog mid)

// The channel struct lives in prism_tui.h; the instance is here
struct l4z_channel l4z_ch;

static uint8_t  l4z_tmp[L4Z_BLK] __attribute__((aligned(4)));
static uint32_t l4z_pcm[2][L4Z_SAMPLES];    // unpacked ping-pong buffers

// Stream cursor over the compressed blob
static const uint8_t *l4z_src;
static uint32_t l4z_left;                   // uncompressed bytes remaining

// LZ4 block decode.  Behaviour-identical to the reference decoder in
// tools/l4z.c (which verifies every block at compression time) but tuned
// for TinyQV, where every data access to flash/PSRAM breaks the QSPI
// instruction fetch burst: literal runs go through the word-optimized
// newlib memcpy, offset-1 matches (RLE runs) through memset, and other
// matches through an unrolled byte copy.
static uint32_t l4z_decode(const uint8_t *src, uint32_t slen, uint8_t *dst)
{
    const uint8_t *s_end = src + slen;
    uint8_t *d = dst;

    for (;;) {
        uint8_t  tok = *src++;
        uint32_t len = tok >> 4;

        if (len == 15) {
            uint8_t b;
            do { b = *src++; len += b; } while (b == 255);
        }
        if (len) {
            memcpy(d, src, len);
            d += len;
            src += len;
        }
        if (src >= s_end)
            break;

        uint32_t off = src[0] | (src[1] << 8);
        src += 2;
        len = (tok & 15) + 4;
        if ((tok & 15) == 15) {
            uint8_t b;
            do { b = *src++; len += b; } while (b == 255);
        }
        const uint8_t *m = d - off;
        if (off == 1) {                     // RLE run (silence etc)
            memset(d, *m, len);
            d += len;
        } else {
            while (len >= 4) {
                d[0] = m[0]; d[1] = m[1]; d[2] = m[2]; d[3] = m[3];
                d += 4; m += 4; len -= 4;
            }
            while (len--)
                *d++ = *m++;
        }
    }
    return (uint32_t)(d - dst);
}

// Decompress and unpack the next block into dst; returns samples (0 = end)
static uint32_t l4z_fill(uint32_t *dst)
{
    if (!l4z_left)
        return 0;

    uint32_t n = l4z_left < L4Z_BLK ? l4z_left : L4Z_BLK;
    uint32_t clen = l4z_src[0] | (l4z_src[1] << 8);

    l4z_src += 2;
    const uint32_t *w;
    if (clen == 0xFFFF) {                   // stored: the payload is padded
        // to word alignment in the blob - unpack straight from flash
        l4z_src = (const uint8_t *)(((uintptr_t)l4z_src + 3) & ~(uintptr_t)3);
        w = (const uint32_t *)l4z_src;
        l4z_src += n;
    } else {
        l4z_decode(l4z_src, clen, l4z_tmp);
        l4z_src += clen;
        w = (const uint32_t *)l4z_tmp;
    }
    l4z_left -= n;

    // Unpack 3-byte samples to one aligned word each for the ISR.
    // Word in, word out: three source words make four samples.
    for (uint32_t nw = n / 12; nw--; ) {
        uint32_t s0 = *w++, s1 = *w++, s2 = *w++;

        dst[0] = s0 & 0xFFFFFFu;
        dst[1] = (s0 >> 24) | ((s1 & 0xFFFFu) << 8);
        dst[2] = (s1 >> 16) | ((s2 & 0xFFu) << 16);
        dst[3] = s2 >> 8;
        dst += 4;
    }
    const uint8_t *b = (const uint8_t *)w;  // 0, 3, 6 or 9 tail bytes
    for (uint32_t t = n % 12; t >= 3; t -= 3, b += 3)
        *dst++ = b[0] | (b[1] << 8) | ((uint32_t)b[2] << 16);
    return n / 3;
}

// Play an L4Z compressed DSM blob (from RAM or flash)
static void l4z_play_blob(const uint8_t *blob)
{
    uint32_t total = blob[0] | (blob[1] << 8) |
                     ((uint32_t)blob[2] << 16) |
                     ((uint32_t)blob[3] << 24);
    uint32_t blk = blob[4] | (blob[5] << 8) |
                   ((uint32_t)blob[6] << 16) |
                   ((uint32_t)blob[7] << 24);

    if (blk != L4Z_BLK) {
        printf("blob block size %lu != built-in %lu\n",
               (unsigned long)blk, (unsigned long)L4Z_BLK);
        return;
    }
    printf("\nL4Z DSM stream: %lu bytes, %lu samples\n",
           (unsigned long)total, (unsigned long)(total / 3));

    printf("Loading DSM Chroma\n");
    int res = prism_load_chroma(chroma_dsm, CHROMA_DSM_CTRL);
    chroma_set_loaded(chroma_dsm);
    if (res) {
        printf("load FAILED, error mask 0x%05x\n", res);
        return;
    }
    set_gpio_func(7, 8);                    // audio PMOD to the PRISM
    prism_comm_write(play_speed);

    // Prime both buffers and hand sample 0 to the FSM by hand; the ISR
    // takes over from sample 1
    l4z_src = blob + 8;
    l4z_left = total;
    uint32_t n0 = l4z_fill(l4z_pcm[0]);
    uint32_t n1 = l4z_fill(l4z_pcm[1]);

    l4z_ch.rd = &l4z_pcm[0][1];
    l4z_ch.end = &l4z_pcm[0][n0];
    l4z_ch.next = l4z_pcm[1];
    l4z_ch.next_end = &l4z_pcm[1][n1];
    l4z_ch.consumed = 0;
    l4z_ch.active = 1;

    prism_clear_interrupt();
    prism_enable_interrupt();
    prism_set_count1_preload(l4z_pcm[0][0]);
    prism_host_toggle();                    // and we're rolling

    uint32_t refill = 0;                    // which pcm[] to refill next
    uint32_t blocks = 2, underruns = 0, max_us = 0;
    int ending = 0;

    for (;;) {
        if (l4z_ch.consumed) {
            if (ending)                     // silent tail started: all real
                break;                      // samples have been played
            l4z_ch.consumed = 0;
            uint32_t t0 = read_time();
            uint32_t n = l4z_fill(l4z_pcm[refill]);

            if (n == 0) {                   // out of song: one silent tail
                for (uint32_t i = 0; i < L4Z_SAMPLES; ++i)
                    l4z_pcm[refill][i] = L4Z_SILENCE;
                n = L4Z_SAMPLES;
                ending = 1;
            } else
                ++blocks;
            l4z_ch.next = l4z_pcm[refill];
            l4z_ch.next_end = &l4z_pcm[refill][n];
            refill ^= 1;

            uint32_t dt = read_time() - t0;
            if (dt > max_us)
                max_us = dt;
            if (l4z_ch.consumed)            // refill took a whole buffer
                ++underruns;
        }
        if (uart_getc() != -1)
            break;
    }

    l4z_ch.active = 0;
    prism_disable_interrupt();
    prism_clear_interrupt();

    printf("%lu blocks, %lu underruns, max refill %luus"
           " (budget %luus per buffer)\n",
           (unsigned long)blocks, (unsigned long)underruns,
           (unsigned long)max_us,
           (unsigned long)(L4Z_SAMPLES * 24u * (play_speed + 1) / 64u));
}

// ==========================================================================
// ADPCM playback through the PCM chroma ('play' command).
//
// The PCM chroma is a fixed 250KHz 8-bit PWM DAC: comm holds the carrier
// period (254 -> 256 clocks), count2_compare holds the duty, and count1
// raises the interrupt once per sample period (preload rounded up to the
// carrier grid; 1984 -> 2048 clocks = 31250Hz at 64MHz).  Because
// HOST_TOGGLE byte writes land in count2_compare and clear the interrupt,
// the ISR (prism_isr.s mode 2) delivers each sample with a single store.
//
// The song is IMA ADPCM from mp3_to_dsm.py --format adpcm: 4 bits per
// sample in independently decodable 1024-sample blocks, ~8x smaller than
// the equivalent DSM stream.  The foreground decodes blocks straight into
// duty-byte ping-pong buffers.
// ==========================================================================

// Chroma bitstream generated by the PRISM Yosys backend (chroma_pcm.v)
const uint32_t chroma_pcm[PRISM_CHROMA_WORDS*2] = {
    0x000003d6, 0x00008000,
    0x000002ba, 0x00c00015,
    0x000002a4, 0x0280c01e,
    0x00000294, 0x02c0a016,
    0x000003d6, 0x00000000,
    0x000002ba, 0x00c08015,
    0x000002a4, 0x0280401e,
    0x00000294, 0x02c02016,
};
// CHROMA_PCM_CTRL is in prism_tui.h

#define ADPCM_BLOCK     1024u           // samples per block / buffer
#define ADPCM_PERIOD    254u            // carrier period register (comm)
#define ADPCM_SILENCE   1u              // pin idle: tool-generated songs
                                        //   ramp from/to -1, so staging and
                                        //   tail at duty 1 is seamless

static uint8_t adpcm_pcmbuf[2][ADPCM_BLOCK] __attribute__((aligned(4)));

static const uint16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
// Fused decode table, built at startup: entry (idx, nib&7) packs the
// magnitude delta (low 16) and the already-clamped next step index
// (high 16).  On this fetch-bound core instruction count is everything:
// the whole IMA update becomes one table load plus a few ALU ops.
static uint32_t adpcm_dtab[89 * 8];

static void adpcm_build_dtab(void)
{
    static const int8_t adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

    for (int idx = 0; idx < 89; ++idx) {
        int32_t step = ima_step[idx];
        for (int m = 0; m < 8; ++m) {
            int32_t delta = step >> 3;
            if (m & 4) delta += step;
            if (m & 2) delta += step >> 1;
            if (m & 1) delta += step >> 2;
            int32_t nx = idx + adj[m];
            if (nx < 0) nx = 0;
            else if (nx > 88) nx = 88;
            adpcm_dtab[(idx << 3) | m] = (uint32_t)delta | ((uint32_t)nx << 16);
        }
    }
}

// Decode one block (header + nibbles) into PWM duty bytes.  Behaviour
// matches adpcm_decode_check() in mp3_to_dsm.py, which verifies the
// encoder against it at generation time.  Nibbles are pulled a word at
// a time (one flash transaction per 8 samples); everything else is the
// minimum instruction count per sample.
static void adpcm_decode_block(const uint8_t *src, uint32_t cnt, uint8_t *dst)
{
    int32_t pred = (int16_t)(src[0] | (src[1] << 8));
    uint32_t idx = src[2];
    const uint32_t *wp = (const uint32_t *)(src + 4);   // blocks are aligned
    uint32_t w = 0;

    for (uint32_t i = 0; i < cnt; ++i) {
        if (!(i & 7))
            w = *wp++;                  // 8 nibbles per word load
        uint32_t nib = w & 0xF;
        w >>= 4;

        uint32_t e = adpcm_dtab[(idx << 3) | (nib & 7)];
        int32_t delta = (int32_t)(e & 0xFFFF);
        idx = e >> 16;
        if (nib & 8) {
            pred -= delta;
            if (pred < -32768) pred = -32768;
        } else {
            pred += delta;
            if (pred > 32767) pred = 32767;
        }

        // 16-bit PCM -> 8-bit PWM duty around mid scale
        int32_t d = 128 + (pred >> 8);
        if (d < 1) d = 1;
        else if (d > 253) d = 253;
        *dst++ = (uint8_t)d;
    }
}

// Stream cursor over the ADPCM blob
static const uint8_t *adpcm_src;
static uint32_t adpcm_left;             // samples remaining

// 'resume' bookmark: a keypress-stopped 'play' remembers its song and
// position; pack-tab sounds (adpcm_hold) never touch it
static const uint8_t *resume_blob;
static uint32_t resume_pre, resume_at;
static uint32_t adpcm_start, adpcm_from;    // resume request / applied
static char resume_name[28], pending_name[28];

static uint32_t adpcm_fill(uint8_t *dst)
{
    if (!adpcm_left)
        return 0;

    uint32_t cnt = adpcm_left < ADPCM_BLOCK ? adpcm_left : ADPCM_BLOCK;

    adpcm_decode_block(adpcm_src, cnt, dst);
    adpcm_src += 4 + (cnt + 1) / 2;
    adpcm_left -= cnt;
    return cnt;
}

// Play an IMA ADPCM blob (from RAM or flash) through the PCM chroma
// Set while the TUI plays a sound-pack entry: the progress prints would
// go through curses into the command window, and a screen refresh takes
// tens of milliseconds of UART - long enough to starve the 32ms refill
// budget and underrun the sound being played.
uint8_t adpcm_quiet;

// ==========================================================================
// 'play -w': braille waveform, drawn live in the plain console.  A strip
// of WAVE_ROWS text rows is reserved under the command line; every decoded
// block contributes one character column (min/max envelope of its two
// halves as 2x4 braille dots), pushed in from the right edge with
// DCH-based left shifts (works on any ANSI terminal).  ~150 bytes per
// column at ~30 columns/s: ~5 KB/s of the 1M baud link.
// ==========================================================================
#define WAVE_ROWS   6               // 64MHz strip height (85MHz: 2x)
uint8_t adpcm_wave;                 // -v requested for this play
static int wave_top, wave_row_after, wave_cols, wave_on;
static int wave_tui, wave_left;     // TUI mode: draw inside the tab rect
static int wave_rows, wave_dots;    // strip height (rows, braille dots)
static int wave_cpb;                // scroll: columns per block (0 = half)
static int wave_mode;               // 0 = scrolling envelope, 1 = scope
static int wave_rpb;                // scope: rows emitted per block
static int wave_srow, wave_schunk;  // scope: next row / half-row chunk
static int wave_ncols;              // scope: dot columns
static uint8_t wave_y[242];         // scope: captured raw samples
static int wave_scope_chunk(char *out);          // fwd: end-of-play
static int wave_col_bytes(char *out, int tb[2][2]);  // frame flush

// TUI-tab mode leans on the curses backend: the same host-side region
// scroll the arrow-key pan uses, plus a model invalidation so the tab
// repaints itself after playback.
void PDC_hscroll_region(int top, int bot, int left, int right, int n);
void PDC_touch_region(int top, int bot, int left, int right);

static void wave_raw(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

// The column bytes are BUILT here but transmitted from the refill
// loop's idle spins (wave_drain): a blocking fputs of the whole burst
// measurably blew the block budget on both 64 and 90MHz.
static char wave_out[1024];
static int  wave_wr, wave_rd;
static uint32_t wave_build_max, wave_build_sum, wave_build_cnt;
static uint32_t wave_cap_max, wave_row_max, wave_left_max;
static uint8_t  wave_denied;        // scope requested below 85MHz
static int wave_force_scope;        // pack plays: scope frames only
static int  wave_phase, wave_tb[2][2];   // per-block dot columns

// Tiny decimal formatter: newlib's sprintf costs 10-40ms PER CALL here
// (its huge vfprintf thrashes the icache against the decode loop over
// QSPI XIP), which single-handedly blew the block budget.
static int wave_put_num(char *p, int v)
{
    int n = 0;

    if (v >= 100) { p[n++] = (char)('0' + v / 100); v %= 100; }
    if (v >= 10 || n) { p[n++] = (char)('0' + v / 10); v %= 10; }
    p[n++] = (char)('0' + v);
    return n;
}

static void wave_drain(int budget)
{
    while (budget-- > 0 && wave_rd < wave_wr)
        uart_putc(wave_out[wave_rd++]);
}


// Parse one ESC[<row>;<col>R cursor-position report (junk tolerated)
static int wave_cpr(int *row, int *col)
{
    uint32_t t0 = read_time();
    int st = 0, r = 0, c = 0, ch;

    while (read_time() - t0 < us_ticks(400000u)) {
        if ((ch = uart_getc()) == -1)
            continue;
        if (st == 0)
            st = ch == 0x1b ? 1 : 0;
        else if (st == 1)
            st = ch == '[' ? 2 : 0;
        else if (st == 2) {
            if (ch >= '0' && ch <= '9') r = r * 10 + ch - '0';
            else if (ch == ';') st = 3;
            else { st = 0; r = 0; }
        } else {
            if (ch >= '0' && ch <= '9') c = c * 10 + ch - '0';
            else if (ch == 'R') { *row = r; *col = c; return 1; }
            else { st = 0; r = 0; c = 0; }
        }
    }
    return 0;
}

static void wave_setup(void)
{
    int r, c, i;

    wave_on = 0;
    wave_tui = 0;
    wave_left = 1;
    wave_wr = wave_rd = 0;
    wave_phase = 0;

    // 85MHz headroom: double-height strip; in a TUI tab (persistent
    // margins = cheap columns) also two columns per block
    {
        int fast = clock_hz >= 85000000u;

        wave_cpb  = fast ? 1 : 0;       // 31 col/s everywhere (62 hurts my eyes)
        wave_rpb  = fast ? 2 : 1;       // scope half-row chunks per block
        wave_rows = fast ? 2 * WAVE_ROWS : WAVE_ROWS;
        wave_dots = wave_rows * 4;
        wave_srow = wave_rows;          // scope: capture a fresh frame
        wave_force_scope = adpcm_wave == 2;
    }
    wave_build_max = wave_build_sum = wave_build_cnt = 0;

    if (tui_running) {
        // Pack-tab sounds (adpcm_wave == 2) show only the stable scope
        // frame - and that needs the 85MHz block budget; below it they
        // draw nothing at all
        if (wave_force_scope && !wave_cpb)
            return;

        // Draw INSIDE the active tab; the TUI stashed its terminal
        // rectangle (0-based) right before dispatching this command.
        // The strip is centered vertically in the tab.
        int rt = tui_tab_rect[0], rb = tui_tab_rect[1];
        int rl = tui_tab_rect[2], rr = tui_tab_rect[3];

        if (rt >= 0 && rb - rt + 1 < wave_rows && wave_rows > WAVE_ROWS) {
            wave_rows = WAVE_ROWS;      // small rect (command window):
            wave_dots = WAVE_ROWS * 4;  // fall back to the short strip
        }
        if (rt < 0 || rb - rt + 1 < wave_rows || rr - rl + 1 < 24) {
            printf("-w needs an open tab at least %d rows x 24 cols\n",
                   wave_rows);
            return;
        }
        wave_top  = rt + 1 + ((rb - rt + 1) - wave_rows) / 2;
        wave_left = rl + 1;
        wave_cols = rr + 1;
        // Margins are set ONCE and stay up for the whole play (nothing
        // else writes the terminal until wave_end releases them): every
        // queued column then only needs a cursor move and an SL.
        {
            char mg[48];
            int  ml = 0;

            mg[ml++] = '\x1b'; mg[ml++] = '[';
            ml += wave_put_num(mg + ml, wave_top);
            mg[ml++] = ';';
            ml += wave_put_num(mg + ml, wave_top + wave_rows - 1);
            mg[ml++] = 'r';
            mg[ml++] = '\x1b'; mg[ml++] = '['; mg[ml++] = '?';
            mg[ml++] = '6'; mg[ml++] = '9'; mg[ml++] = 'h';
            mg[ml++] = '\x1b'; mg[ml++] = '[';
            ml += wave_put_num(mg + ml, wave_left);
            mg[ml++] = ';';
            ml += wave_put_num(mg + ml, wave_cols);
            mg[ml++] = 's';
            mg[ml] = 0;
            wave_raw("\x1b[?25l\x1b[96m");
            wave_raw(mg);
        }
        wave_ncols = (wave_cols - wave_left + 1) * 2;
        if (wave_ncols > 236)
            wave_ncols = 236;
        wave_on = 1;
        wave_tui = 1;
        return;
    }

    while (uart_getc() != -1)
        ;
    for (i = 0; i < wave_rows; i++)
        putchar('\n');
    printf("\x1b[6n");
    fflush(stdout);
    if (!wave_cpr(&r, &c) || r <= wave_rows) {
        printf("(-w: terminal did not answer a cursor query)\n");
        return;
    }
    wave_row_after = r;
    wave_top = r - wave_rows;
    printf("\x1b" "7\x1b[999C\x1b[6n\x1b" "8");
    fflush(stdout);
    if (!wave_cpr(&r, &c) || c < 20)
        c = 80;
    wave_cols = c < 110 ? c : 110;
    wave_ncols = (wave_cols - wave_left + 1) * 2;
    if (wave_ncols > 236)
        wave_ncols = 236;
    printf("\x1b[?25l\x1b[96m");
    wave_on = 1;
}

static void wave_end(void)
{
    if (!wave_on)
        return;
    wave_on = 0;
    while (wave_rd < wave_wr)               // finish the last column
        uart_putc(wave_out[wave_rd++]);

    // A short sound can end mid scope frame, leaving the top rows from
    // the new capture over the bottom rows of the previous one: finish
    // the frame now - playback is over, so there is no block budget to
    // respect.  Scroll mode flushes its pending half column the same
    // way (the second dot column duplicates the first).
    if (wave_mode == 1 || wave_force_scope) {
        while (wave_srow < wave_rows) {
            int fl = wave_scope_chunk(wave_out);

            for (int i = 0; i < fl; i++)
                uart_putc(wave_out[i]);
        }
    } else if (wave_cpb == 0 && wave_phase != 0) {
        wave_tb[1][0] = wave_tb[0][0];
        wave_tb[1][1] = wave_tb[0][1];
        int fl = wave_col_bytes(wave_out, wave_tb);

        for (int i = 0; i < fl; i++)
            uart_putc(wave_out[i]);
        wave_phase = 0;
    }
    wave_rd = wave_wr = 0;

    if (wave_tui) {
        // Release the margins, then leave the waveform on screen: the
        // Wave tab is a blank canvas in curses' model, so nothing
        // repaints over it until the tab itself redraws
        wave_raw("\x1b[?69l\x1b[r\x1b[0m\x1b[?25h");
    } else {
        printf("\x1b[0m\x1b[?25h\x1b[%d;1H", wave_row_after);
        fflush(stdout);
    }
    if (!adpcm_quiet)
        printf("\nwave: build avg %luus max %luus (cap %luus rows %luus"
               " left %luB)\n",
               (unsigned long)(wave_build_cnt ?
                               wave_build_sum / wave_build_cnt : 0),
               (unsigned long)wave_build_max,
               (unsigned long)wave_cap_max, (unsigned long)wave_row_max,
               (unsigned long)wave_left_max);
    wave_build_max = wave_build_sum = wave_build_cnt = 0;
    wave_cap_max = wave_row_max = wave_left_max = 0;
    if (wave_denied) {
        wave_denied = 0;
        if (!adpcm_quiet)
            printf("(scope view needs the 85MHz clock: run --freq 85)\n");
    }
}

// duty byte (128 = midscale) -> dot row 0..WAVE_DOTS-1, top = positive
static inline int wave_dot(int v)
{
    // v 0..255 -> dot row 0..wave_dots-1, top = positive.  Shift-adds
    // only: this core's multiply is a ~20us __mulsi3 call, and the
    // scope path converts hundreds of samples per block.
    int t = 255 - v;                    // 0..255, top-first
    int y = wave_dots == 48 ? ((t << 5) + (t << 4)) >> 8
                            : ((t << 4) + (t << 3)) >> 8;

    if (y >= wave_dots)
        y = wave_dots - 1;
    return y;
}

// Min/max envelope of buf[a..b) as one dot column (top,bottom).
// Stride 8: the buffer is in PSRAM (~10us+ a read with the sample ISR
// on the bus); an envelope needs no better.
static void wave_scan(const uint8_t *buf, uint32_t a, uint32_t b, int *tb)
{
    int mn = 255, mx = 0;

    for (uint32_t i = a; i < b; i += 8) {
        int v = buf[i];

        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    tb[0] = wave_dot(mx);               // top (screen-wards)
    tb[1] = wave_dot(mn);
}

// Append one braille character column (dot columns in tb[0]/tb[1]) to
// out: the strip shift (margin-confined SL in a TUI tab, per-row DCH in
// the plain console) plus the glyphs for the new rightmost cells.
static int wave_col_bytes(char *out, int tb[2][2])
{
    static const uint8_t brl[2][4] = {
        { 0x01, 0x02, 0x04, 0x40 }, { 0x08, 0x10, 0x20, 0x80 }
    };
    int len = 0, r, dc, dr;

    if (wave_tui) {
        // Margins were set at wave_setup and persist: a column is just
        // "cursor into the region, scroll left one" (PSRAM buffer bytes
        // are ~6us each to write - byte economy IS the block budget)
        out[len++] = '\x1b'; out[len++] = '[';
        len += wave_put_num(out + len, wave_top);
        out[len++] = ';';
        len += wave_put_num(out + len, wave_left);
        out[len++] = 'H';
        out[len++] = '\x1b'; out[len++] = '['; out[len++] = '1';
        out[len++] = ' '; out[len++] = '@';
    }
    for (r = 0; r < wave_rows; r++) {
        unsigned bits = 0, cp;

        for (dc = 0; dc < 2; dc++)
            for (dr = 0; dr < 4; dr++) {
                int dy = r * 4 + dr;
                if (dy >= tb[dc][0] && dy <= tb[dc][1])
                    bits |= brl[dc][dr];
            }
        cp = 0x2800 + bits;
        if (!wave_tui) {
            out[len++] = '\x1b'; out[len++] = '[';
            len += wave_put_num(out + len, wave_top + r);
            out[len++] = ';'; out[len++] = '1'; out[len++] = 'H';
            out[len++] = '\x1b'; out[len++] = '['; out[len++] = 'P';
        }
        out[len++] = '\x1b'; out[len++] = '[';
        len += wave_put_num(out + len, wave_top + r);
        out[len++] = ';';
        len += wave_put_num(out + len, wave_cols);
        out[len++] = 'H';
        out[len++] = (char)(0xe0 | (cp >> 12));
        out[len++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[len++] = (char)(0x80 | (cp & 0x3f));
    }
    return len;
}

// --- scope view: a triggered, time-zoomed snapshot -----------------
// One sample per braille dot column (~7ms of audio across the strip,
// a few cycles of a mid-range note), phase-locked by a rising
// zero-crossing trigger.  A frame is amortized: one block captures,
// the following blocks emit wave_rpb rows each (~4.5 frames/s) -
// building a whole frame at once would blow the block budget on
// PSRAM writes alone.
static void wave_scope_capture(const uint8_t *buf, uint32_t n)
{
    uint32_t i, trig = 0;
    uint32_t search = n / 2 < 512 ? n / 2 : 512;
    int prev = buf[0];

    // Rising zero-crossing trigger; stride 4 (jitter of +/-4 samples is
    // invisible) and a bounded search keep the PSRAM reads cheap
    for (i = 4; i < search; i += 4) {
        int v = buf[i];

        if (prev < 128 && v >= 128) {
            trig = i;
            break;
        }
        prev = v;
    }
    if (trig + (uint32_t)wave_ncols + 1 >= n)
        trig = 0;
    // RAW samples only - the dot-row conversion happens on the fly in
    // the chunk emitter, spreading those multiplies across the frame
    // (the one-block capture was the tallest pole in the 64MHz budget)
    for (i = 0; i <= (uint32_t)wave_ncols; i++)
        wave_y[i] = buf[trig + i];
    wave_srow = 0;
}

// Emit ONE half-row chunk of the scope frame (~57 chars, ~181 bytes,
// ~115 PSRAM reads).  A whole row per block starved the refill budget
// at 64MHz - between the PSRAM traffic and the icache thrash it added
// to the decode loop itself - so the frame is sliced this finely and
// wave_rpb chunks go out per block (~2.4 frames/s; a triggered display
// doesn't need more).
static int wave_scope_chunk(char *out)
{
    static const uint8_t brl[2][4] = {
        { 0x01, 0x02, 0x04, 0x40 }, { 0x08, 0x10, 0x20, 0x80 }
    };
    int len = 0, cc, dc, dr;
    int width = wave_ncols / 2;
    int w1 = (width + 1) / 2;
    int c0 = wave_schunk ? w1 : 0;
    int c1 = wave_schunk ? width : w1;
    int band = wave_srow * 4;
    int prev = wave_dot(wave_y[c0 * 2]);

    out[len++] = '\x1b'; out[len++] = '[';
    len += wave_put_num(out + len, wave_top + wave_srow);
    out[len++] = ';';
    len += wave_put_num(out + len, wave_left + c0);
    out[len++] = 'H';
    for (cc = c0; cc < c1; cc++) {
        unsigned bits = 0, cp;

        for (dc = 0; dc < 2; dc++) {
            int cur = wave_dot(wave_y[cc * 2 + dc + 1]);
            int lo = prev < cur ? prev : cur;
            int hi = prev < cur ? cur : prev;

            if (lo <= band + 3 && hi >= band)
                for (dr = 0; dr < 4; dr++) {
                    int dy = band + dr;

                    if (dy >= lo && dy <= hi)
                        bits |= brl[dc][dr];
                }
            prev = cur;
        }
        cp = 0x2800 + bits;
        out[len++] = (char)(0xe0 | (cp >> 12));
        out[len++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[len++] = (char)(0x80 | (cp & 0x3f));
    }
    if (++wave_schunk >= 2) {
        wave_schunk = 0;
        wave_srow++;
    }
    return len;
}

// SPACE during playback flips between the scrolling envelope and the
// scope; the mode is remembered for the next play.
static void wave_toggle(void)
{
    // The scope frame cannot fit beside the decoder in the 64MHz block
    // budget (decode alone is 21-25ms of the 32.8ms block); at 85MHz
    // there is room.  Below that, stay in scroll and say why later.
    if (!wave_cpb) {
        wave_denied = 1;
        return;
    }
    wave_mode ^= 1;
    wave_srow = wave_rows;              // scope: start a fresh frame
    wave_schunk = 0;
}

static void wave_emit(const uint8_t *buf, uint32_t n)
{
    int len = 0;

    if (!wave_on || n < 2)
        return;

    // Any leftover of the previous column goes out first (only a very
    // slow link ever leaves one)
    if ((uint32_t)(wave_wr - wave_rd) > wave_left_max)
        wave_left_max = (uint32_t)(wave_wr - wave_rd);
    while (wave_rd < wave_wr)
        uart_putc(wave_out[wave_rd++]);
    wave_rd = wave_wr = 0;

    uint32_t t0 = read_time();

    if (wave_mode == 1 || wave_force_scope) {
        if (wave_srow >= wave_rows) {
            wave_scope_capture(buf, n);     // this block: trigger+capture
            uint32_t dc_ = read_time() - t0;
            if (dc_ > wave_cap_max)
                wave_cap_max = dc_;
        } else {
            for (int q = 0; q < wave_rpb && wave_srow < wave_rows; q++)
                len += wave_scope_chunk(wave_out + len);
            uint32_t dr_ = read_time() - t0;
            if (dr_ > wave_row_max)
                wave_row_max = dr_;
        }
    } else if (wave_cpb) {
        uint32_t seg = n / wave_cpb;

        for (int c = 0; c < wave_cpb && seg >= 2; c++) {
            uint32_t base = c * seg;

            wave_scan(buf, base, base + seg / 2, wave_tb[0]);
            wave_scan(buf, base + seg / 2, base + seg, wave_tb[1]);
            len += wave_col_bytes(wave_out + len, wave_tb);
        }
    } else {
        wave_scan(buf, 0, n, wave_tb[wave_phase]);
        wave_phase ^= 1;
        if (wave_phase != 0)
            return;
        len = wave_col_bytes(wave_out, wave_tb);
    }
    wave_wr = len;

    uint32_t dt = read_time() - t0;
    if (dt > wave_build_max)
        wave_build_max = dt;
    wave_build_sum += dt;
    wave_build_cnt++;
}

uint8_t adpcm_hold;                 // leave the carrier at mid scale after

// Carrier parked at mid scale between sound-pack plays (adpcm_hold).
// Moving the output's DC level at all is what pops the speaker, so
// consecutive sounds - which start and end at mid scale - play with no
// DC movement whatsoever, and the two real transitions (first play,
// pack tab closing) are made slowly enough not to thump.
static uint8_t  adpcm_carrier_live;
static uint32_t adpcm_live_preload;

#define ADPCM_MID       128u        // zero level of pack audio
#define ADPCM_RAMP_US   250u        // per duty count: half scale in ~32ms

// Glide the PWM duty between two levels.  Sound-pack entries start and
// end at MID scale (their fades go to the zero LEVEL, not to the pin's
// idle), so stepping between the silence duty and the first/last sample
// was a half scale DC step - a pop.  The rate matters as much as the
// step: half scale in a few ms is still a thump through the PMOD's
// coupling cap, so this takes ~32ms, like the ramp the songs carry in
// their own data.  Songs still pass through here with from == to.
static void adpcm_duty_ramp(uint8_t from, uint8_t to)
{
    int step = from < to ? 1 : -1;

    while (from != to) {
        from = (uint8_t)(from + step);
        prism_set_count2_compare(from);
        uint32_t t0 = read_time();
        while (read_time() - t0 < us_ticks(ADPCM_RAMP_US))
            ;
    }
}

// Ramp a held carrier down and turn the PWM off (pack tab closing, or a
// non-pack player about to restage the chroma)
void adpcm_carrier_release(void)
{
    if (!adpcm_carrier_live)
        return;
    adpcm_duty_ramp(ADPCM_MID, ADPCM_SILENCE);
    prism_disable();
    adpcm_carrier_live = 0;
}

static void play_adpcm_blob(const uint8_t *blob, uint32_t preload)
{
    uint32_t total = blob[0] | (blob[1] << 8) |
                     ((uint32_t)blob[2] << 16) |
                     ((uint32_t)blob[3] << 24);
    uint32_t rate = blob[4] | (blob[5] << 8) |
                    ((uint32_t)blob[6] << 16) |
                    ((uint32_t)blob[7] << 24);
    uint32_t blk = blob[8] | (blob[9] << 8);

    if (blk != ADPCM_BLOCK) {
        printf("blob block size %lu != built-in %lu\n",
               (unsigned long)blk, (unsigned long)ADPCM_BLOCK);
        return;
    }

    // sample period = preload rounded up to the 256-clock carrier grid
    uint32_t period = ((preload >> 8) + 1) << 8;
    if (!adpcm_quiet) {
        printf("\nADPCM: %lu samples at %luHz (%lus)\n",
               (unsigned long)total, (unsigned long)rate,
               (unsigned long)(total / rate));
        printf("Loading PCM Chroma (%luHz sample clock)\n",
               (unsigned long)(clock_hz / period));
    }

    if (adpcm_wave)
        wave_setup();

    adpcm_build_dtab();

    // A carrier held from a previous pack play can be reused as long as
    // nothing changed underneath it: same sample rate (the preload write
    // also carries the compare byte, so rewriting it would glitch the
    // duty) and the PCM chroma still in the FSM.  Otherwise release it
    // and stage from scratch.
    uint8_t start_duty = ADPCM_SILENCE;

    if (adpcm_carrier_live &&
        (preload != adpcm_live_preload ||
         prism_chroma_loaded() != prism_chroma_index("pcm")))
        adpcm_carrier_release();

    if (adpcm_carrier_live)
        start_duty = ADPCM_MID;
    else {
        // Stage registers first so the PWM starts up at silence
        prism_comm_write(ADPCM_PERIOD);
        prism_set_count2_compare(ADPCM_SILENCE);
        prism_set_count1_preload(preload);

        int res = prism_load_chroma(chroma_pcm, CHROMA_PCM_CTRL);

        if (res) {
            printf("load FAILED, error mask 0x%05x\n", res);
            return;
        }
        chroma_set_loaded(chroma_pcm);
        set_gpio_func(7, 8);            // audio PMOD to the PRISM
    }

    adpcm_src = blob + 12;
    adpcm_left = total;

    // 'resume': skip whole blocks (they decode independently) to the
    // bookmarked position of a keypress-stopped song
    adpcm_from = 0;

    if (adpcm_start != 0) {

        uint32_t skip = adpcm_start / ADPCM_BLOCK;

        if (skip * ADPCM_BLOCK < total) {
            adpcm_src  += skip * (4 + ADPCM_BLOCK / 2);
            adpcm_left -= skip * ADPCM_BLOCK;
            adpcm_from  = skip * ADPCM_BLOCK;
        }
        adpcm_start = 0;
    }

    uint32_t n0 = adpcm_fill(adpcm_pcmbuf[0]);
    uint32_t n1 = adpcm_fill(adpcm_pcmbuf[1]);

    l4z_ch.rd = (uint32_t *)(void *)adpcm_pcmbuf[0];
    l4z_ch.end = (uint32_t *)(void *)(adpcm_pcmbuf[0] + n0);
    l4z_ch.next = (uint32_t *)(void *)adpcm_pcmbuf[1];
    l4z_ch.next_end = (uint32_t *)(void *)(adpcm_pcmbuf[1] + n1);
    l4z_ch.consumed = 0;
    l4z_ch.active = 2;                  // ISR byte mode

    // Ease the carrier from wherever it idles up to the first sample
    // before the stream starts (no pop at note-on).  From a held
    // carrier both levels are mid scale and this is instant.
    uint8_t last_duty = n0 ? adpcm_pcmbuf[0][0] : start_duty;
    adpcm_duty_ramp(start_duty, last_duty);

    prism_clear_interrupt();
    prism_enable_interrupt();
    // The chroma raises the first sample interrupt on its own (count1
    // starts at zero), so the ISR takes over from here.

    uint32_t refill = 0;
    uint32_t blocks = 2, underruns = 0, max_us = 0;
    int ending = 0;
    int stop_key = -1;

    for (;;) {
        if (l4z_ch.consumed) {
            if (ending)
                break;
            l4z_ch.consumed = 0;
            uint32_t t0 = read_time();
            uint32_t n = adpcm_fill(adpcm_pcmbuf[refill]);

            if (n == 0) {
                // Out of song: the tail block glides from the last real
                // sample to the idle level (one count per 8 samples =
                // half scale in ~32ms), then holds it - the ISR plays
                // the ramp out like any other audio.  Holding for the
                // pack tab, the idle level IS the audio's own end level
                // and this block is flat.
                int idle = adpcm_hold ? (int)ADPCM_MID : (int)ADPCM_SILENCE;
                int v = last_duty;

                for (uint32_t i = 0; i < ADPCM_BLOCK; ++i) {
                    if ((i & 7) == 7 && v != idle)
                        v += v < idle ? 1 : -1;
                    adpcm_pcmbuf[refill][i] = (uint8_t)v;
                }
                n = ADPCM_BLOCK;
                ending = 1;
            } else {
                last_duty = adpcm_pcmbuf[refill][n - 1];
                ++blocks;
                if (adpcm_wave)
                    wave_emit(adpcm_pcmbuf[refill], n);
            }
            l4z_ch.next = (uint32_t *)(void *)adpcm_pcmbuf[refill];
            l4z_ch.next_end = (uint32_t *)(void *)(adpcm_pcmbuf[refill] + n);
            refill ^= 1;

            uint32_t dt = read_time() - t0;
            if (dt > max_us)
                max_us = dt;
            if (l4z_ch.consumed)
                ++underruns;
        }
        if (adpcm_wave)
            wave_drain(8);
        if ((stop_key = uart_getc()) != -1) {
            if (wave_on && adpcm_wave == 1 && stop_key == ' ') {
                wave_toggle();          // SPACE: scope <-> scroll view
                stop_key = -1;
            } else
                break;
        }
    }

    l4z_ch.active = 0;
    prism_disable_interrupt();
    prism_clear_interrupt();

    // Bookmark for 'resume': a keypress stop remembers the spot (a
    // couple of blocks early - the two in-flight buffers); a natural
    // end clears its own song's bookmark
    if (!adpcm_hold) {
        if (stop_key != -1) {
            resume_blob = blob;
            resume_pre  = preload;
            resume_at   = adpcm_from +
                          (blocks > 2 ? (blocks - 2) * ADPCM_BLOCK : 0);
            for (int i = 0; i < (int)sizeof(resume_name); i++)
                if ((resume_name[i] = pending_name[i]) == '\0')
                    break;
        } else if (resume_blob == blob)
            resume_blob = NULL;
    }

    // A keypress stops mid sound: glide from the byte the ISR delivered
    // last to the idle level so the cut is soft too.  On a natural end
    // the tail block has already parked the duty there and this ramp is
    // a no-op.
    {
        const uint8_t *rd = (const uint8_t *)l4z_ch.rd;
        uint8_t idle = adpcm_hold ? ADPCM_MID : ADPCM_SILENCE;
        uint8_t cur = idle;

        if (rd > &adpcm_pcmbuf[0][0] && rd <= &adpcm_pcmbuf[1][ADPCM_BLOCK])
            cur = rd[-1];
        adpcm_duty_ramp(cur, idle);
    }
    // An arrow key cancelling playback is a 3 byte escape sequence: the
    // ESC alone stopped the loop above, and the "[A" tail would be typed
    // into whatever prompt runs next.  Give the tail a moment to arrive,
    // then swallow everything pending (any typing during a blocking play
    // is a stop request, never command input).
    wave_end();

    if (stop_key == 0x1B) {
        uint32_t t0 = read_time();

        while (read_time() - t0 < us_ticks(20000u))
            ;
        while (uart_getc() != -1)
            ;
    }

    if (adpcm_hold) {
        // Pack tab: leave the carrier running at mid scale so the next
        // sound starts (and this one ended) with no DC movement at all
        adpcm_carrier_live = 1;
        adpcm_live_preload = preload;
    } else
        prism_disable();                // PWM off, pin low

    if (!adpcm_quiet)
        printf("%lu blocks, %lu underruns, max refill %luus"
               " (budget %luus per buffer)\n",
               (unsigned long)blocks, (unsigned long)underruns,
               (unsigned long)max_us,
               (unsigned long)(ADPCM_BLOCK * (period / 64u)));
    adpcm_underruns = underruns;
}

uint32_t adpcm_underruns;           // from the last play, for the TUI

// ==========================================================================
// Play an ADPCM blob the caller already has in memory (the TUI's sound
// pack entries).  The PCM chroma's sample clock comes from the blob's own
// rate field, so a pack built at any rate plays correctly: the carrier is
// always 256 clocks and the sample period is the nearest whole number of
// carriers.
// ==========================================================================
uint32_t adpcm_preload_for_rate(uint32_t rate)
{
    uint32_t period;

    if (rate == 0)
        rate = 31250;
    period = (clock_hz + rate / 2) / rate;          // clocks per sample
    period = ((period + 128) / 256) * 256;          // nearest carrier grid
    if (period < 512)
        period = 512;
    return period - 256;    // play_adpcm_blob rounds back up to `period`
}

void play_adpcm_data(const uint8_t *blob, uint32_t preload)
{
    play_adpcm_blob(blob, preload);
}


// ==========================================================================
// Song download into RAM B ('download' / 'play' commands).
//
// The SDK leaves RAM B (0x1800000..0x1FFFFFF, 8MB) completely unused: the
// heap and all data/bss/stacks live in RAM A.  Downloaded songs are stored
// in RAM B behind a small descriptor, so they survive across plays until
// power off.
//
// Transfer protocol (host side: tqv.py 'send', or the auto prompt in
// tqv.py console): the design prints a request marker, then the host
// streams {u32 'TQVD', u32 kind, u32 length, u32 crc32} + payload, all
// DLE stuffed: 0x10 -> 0x10 0x00, 0x11 -> 0x10 0x01, 0x03 -> 0x10 0x02,
// so the RP2350 UART bridge's Ctrl-Q escape and the SDK's Ctrl-C
// interrupt character never see their trigger bytes.  CRC32 is checked
// on the fly; the descriptor magic is only written on success.
// ==========================================================================

#define SONG_BASE       ((uint8_t *)0x1800000)     // RAM B
#define SONG_MAX        (8u * 1024 * 1024 - 16)
// SONG_MAGIC / kinds / struct song_desc now live in prism_tui.h (the
// 'fat' command decodes descriptors too)
// Payload of a descriptor-wrapped image in RAM B, if it is of `kind`
// ('tqv.py load <file>' puts one there).  NULL when the slot holds
// something else or nothing.
const uint8_t *song_ram_payload(uint32_t kind, uint32_t *length)
{
    const struct song_desc *desc = (const struct song_desc *)SONG_BASE;

    if (desc->magic != SONG_MAGIC || desc->kind != kind)
        return NULL;
    if (length != NULL)
        *length = desc->length;
    return SONG_BASE + 16;
}

// ==========================================================================
// The allocation tables (see prism_tui.h for the layout)
// ==========================================================================

const struct tqfat_table *tqfat_get(const void *table)
{
    const struct tqfat_table *t = (const struct tqfat_table *)table;

    if (t->magic != TQFAT_MAGIC || t->count > TQFAT_MAX)
        return NULL;
    return t;
}

const struct tqfat_entry *tqfat_find(const void *table, const char *name)
{
    const struct tqfat_table *t = tqfat_get(table);

    if (t == NULL)
        return NULL;
    for (uint32_t i = 0; i < t->count; ++i) {
        const char *a = t->e[i].name, *b = name;

        while (*a != '\0' && *a == *b) {   // byte-wise: the core's word
            a++;                            // loads misread odd addresses
            b++;
        }
        if (*a == *b)
            return &t->e[i];
    }
    return NULL;
}

// Named (or first, name == NULL) entry of `kind` in one table
static const uint8_t *song_table_named(const void *table, uint32_t kind,
                                       const char *name, uint32_t *length)
{
    const struct tqfat_table *t = tqfat_get(table);

    if (t == NULL)
        return NULL;
    for (uint32_t i = 0; i < t->count; ++i) {
        const struct song_desc *d =
            (const struct song_desc *)(uintptr_t)t->e[i].addr;

        if (d->magic != SONG_MAGIC || d->kind != kind)
            continue;
        if (name != NULL && tqfat_find(table, name) != &t->e[i])
            continue;
        if (length != NULL)
            *length = d->length;
        return (const uint8_t *)(uintptr_t)t->e[i].addr + 16;
    }
    return NULL;
}

// RAM lookup, with the legacy fixed-slot fallback keeping old flows
// working when no table exists
const uint8_t *song_ram_named(uint32_t kind, const char *name, uint32_t *length)
{
    const uint8_t *p = song_table_named(TQFAT_RAM, kind, name, length);

    if (p != NULL || name != NULL || tqfat_get(TQFAT_RAM) != NULL)
        return p;
    return song_ram_payload(kind, length);
}

// Flash lookup: the payload is XIP-mapped, so a pack plays in place
const uint8_t *song_flash_named(uint32_t kind, const char *name, uint32_t *length)
{
    return song_table_named(TQFAT_FLASH, kind, name, length);
}

static uint32_t crc32_tab[16];

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 4; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc32_tab[i] = c;
    }
}

static uint32_t crc32_byte(uint32_t crc, uint8_t b)
{
    crc = crc32_tab[(crc ^ b) & 15] ^ (crc >> 4);
    return crc32_tab[(crc ^ (b >> 4)) & 15] ^ (crc >> 4);
}

// Receive one DLE-destuffed byte; -1 on timeout
static int dl_getb(uint32_t timeout_us)
{
    uint32_t deadline = read_time() + timeout_us;

    for (;;) {
        int c = uart_getc();

        if (c == -1) {
            if ((int32_t)(deadline - read_time()) <= 0)
                return -1;
            continue;
        }
        if (c != 0x10)
            return c;
        // escape: next byte selects 0x10 / 0x11 / 0x03
        deadline = read_time() + timeout_us;
        for (;;) {
            c = uart_getc();
            if (c != -1)
                break;
            if ((int32_t)(deadline - read_time()) <= 0)
                return -1;
        }
        return c == 0 ? 0x10 : c == 1 ? 0x11 : 0x03;
    }
}

void song_download(void)
{
    struct song_desc *desc = (struct song_desc *)SONG_BASE;
    uint8_t hdr[16];

    crc32_init();
    desc->magic = 0;                    // invalidate any previous song

    int16_t old_intr = uart_rx_interrupt_char;
    uart_rx_interrupt_char = -1;        // Ctrl-C is data during a download

    printf("\x05TQVRX\x05\n");         // host tools watch for this marker
    printf("ready to receive (30s timeout, any host key after that"
           " returns to the prompt)\n");

    int ok = 1;
    for (int i = 0; i < 16; ++i) {
        int c = dl_getb(i == 0 ? 30000000u : 3000000u);
        if (c < 0) {
            printf("timed out waiting for the header\n");
            ok = 0;
            break;
        }
        hdr[i] = (uint8_t)c;
    }

    uint32_t kind = 0, length = 0, want_crc = 0;
    if (ok) {
        uint32_t magic = hdr[0] | (hdr[1] << 8) | ((uint32_t)hdr[2] << 16) |
                         ((uint32_t)hdr[3] << 24);
        kind = hdr[4] | (hdr[5] << 8);
        length = hdr[8] | (hdr[9] << 8) | ((uint32_t)hdr[10] << 16) |
                 ((uint32_t)hdr[11] << 24);
        want_crc = hdr[12] | (hdr[13] << 8) | ((uint32_t)hdr[14] << 16) |
                   ((uint32_t)hdr[15] << 24);
        if (magic != SONG_MAGIC || length == 0 || length > SONG_MAX) {
            printf("bad header (magic %08lx, length %lu)\n",
                   (unsigned long)magic, (unsigned long)length);
            ok = 0;
        }
    }

    uint32_t crc = 0xFFFFFFFFu;
    if (ok) {
        uint8_t *dst = SONG_BASE + 16;

        printf("receiving %lu bytes ", (unsigned long)length);
        for (uint32_t n = 0; n < length; ++n) {
            int c = dl_getb(3000000u);
            if (c < 0) {
                printf("\ntimed out at byte %lu\n", (unsigned long)n);
                ok = 0;
                break;
            }
            dst[n] = (uint8_t)c;
            crc = crc32_byte(crc, (uint8_t)c);
            if ((n & 0x7FFF) == 0x7FFF)
                putchar('.');
        }
    }

    uart_rx_interrupt_char = old_intr;
    if (!ok)
        return;

    crc ^= 0xFFFFFFFFu;
    if (crc != want_crc) {
        printf("\nCRC FAIL: got %08lx, expected %08lx\n",
               (unsigned long)crc, (unsigned long)want_crc);
        return;
    }

    desc->kind = kind;
    desc->length = length;
    desc->crc = crc;
    desc->magic = SONG_MAGIC;           // valid only now
    printf("\nreceived %s song: %lu bytes, crc %08lx OK - 'play' plays it\n",
           kind == SONG_KIND_ADPCM ? "ADPCM" :
           kind == SONG_KIND_L4Z ? "L4Z DSM" : "unknown",
           (unsigned long)length, (unsigned long)crc);
}

// Play a descriptor-wrapped song image ({magic,kind,length,crc} + blob)
// from anywhere in the address space; argval is the optional playback
// argument (ADPCM: preload, L4Z DSM: speed)
static void song_play_desc(const uint8_t *base, const char *argval)
{
    const struct song_desc *desc = (const struct song_desc *)base;

    if (desc->magic != SONG_MAGIC) {
        printf("no song at %08lx\n", (unsigned long)(uintptr_t)base);
        return;
    }

    const uint8_t *blob = base + 16;
    if (desc->kind == SONG_KIND_ADPCM) {
        // Preload from the blob's own rate header so the sample clock is
        // right at any project clock (1984 was only 31250Hz at 64MHz)
        uint32_t pre = adpcm_preload_for_rate(*(const uint32_t *)(blob + 4));
        if (argval && !parse_u32(argval, &pre))
            return;
        play_adpcm_blob(blob, pre);
    } else if (desc->kind == SONG_KIND_L4Z) {
        if (argval)
            parse_u32(argval, &play_speed);
        l4z_play_blob(blob);
    } else
        printf("unknown song kind %lu\n", (unsigned long)desc->kind);
}

// Unified 'play <name> [arg]': one command, both allocation tables.
// RAM is searched first so a downloaded copy can temporarily override
// the flash version of the same name.  Bare 'play' lists what there is.
static void song_list_bank(const void *table, const char *bank)
{
    const struct tqfat_table *t = tqfat_get(table);

    if (t == NULL)
        return;
    for (uint32_t i = 0; i < t->count; ++i) {
        const struct song_desc *d =
            (const struct song_desc *)(uintptr_t)t->e[i].addr;

        if (d->magic != SONG_MAGIC || d->kind == SONG_KIND_SPK)
            continue;
        printf("  %-24s %s\n", t->e[i].name, bank);
    }
}

void song_play_any(int argc, char *argv[])
{
    // 'play -w ...': draw the waveform while it plays (plain console)
    adpcm_wave = 0;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'w' &&
        argv[1][2] == '\0') {
        adpcm_wave = 1;
        argv++;
        argc--;
    }

    if (argc < 2) {
        printf("songs ('play <name>'):\n");
        song_list_bank(TQFAT_RAM, "ram");
        song_list_bank(TQFAT_FLASH, "flash");
        return;
    }

    const struct tqfat_entry *e = tqfat_find(TQFAT_RAM, argv[1]);

    if (e == NULL)
        e = tqfat_find(TQFAT_FLASH, argv[1]);
    if (e == NULL) {
        printf("no song '%s' in the RAM or flash FAT ('fat' lists"
               " them)\n", argv[1]);
        return;
    }
    for (int i = 0; i < (int)sizeof(pending_name); i++)
        if ((pending_name[i] = e->name[i]) == '\0')
            break;
    pending_name[sizeof(pending_name) - 1] = '\0';
    song_play_desc((const uint8_t *)(uintptr_t)e->addr,
                   argc > 2 ? argv[2] : NULL);
    adpcm_wave = 0;             // strictly per-play: a later pack-tab
}                               // play must not inherit the waveform

// 'resume [-w]': continue the last keypress-stopped song from its
// bookmark.  RAM songs resume only while their copy is still loaded.
void song_resume(int argc, char *argv[])
{
    adpcm_wave = 0;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'w' &&
        argv[1][2] == '\0')
        adpcm_wave = 1;

    if (resume_blob == NULL) {
        printf("nothing to resume ('play' + a key to stop bookmarks)\n");
        adpcm_wave = 0;
        return;
    }

    uint32_t rate = *(const uint32_t *)(resume_blob + 4);
    uint32_t secs = rate ? resume_at / rate : 0;

    printf("resuming %s at %lu:%02lu\n", resume_name,
           (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    for (int i = 0; i < (int)sizeof(pending_name); i++)
        if ((pending_name[i] = resume_name[i]) == '\0')
            break;
    adpcm_start = resume_at;
    play_adpcm_blob(resume_blob, resume_pre);
    adpcm_wave = 0;
}

// Fast download: hand control to the host, which stops the design,
// writes the song into RAM B over SPI and restarts everything.  The
// tqv.py console recognizes the marker and orchestrates it.
void song_download_fast(void)
{
    printf("\x05TQVLD\x05\n");
    printf("fast load requested: the host now stops the design, writes the\n"
           "song over SPI and restarts it (~15s).  If nothing happens, this\n"
           "console is not './tqv.py console' - use './tqv.py load <file>'\n"
           "from the host instead.\n");
}
