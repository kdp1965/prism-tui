#pragma once

// Shared declarations for the PRISM test / control CLI.
//
//   main.c        minimal main loop and the PRISM interrupt handler
//   console.c     CLI plumbing, register/debug commands, chroma table,
//                 selftest
//   play.c        DSM audio playback ('play' command)
//   flashy.c      WS2812 LED demo ('flashy' command)
//   synth.c       80's synthesizer engine and 'synth' command
//   synth_demos.c demo song tables and the demo sequencer

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==========================================================================
// Chroma bitstreams.  Each feature file owns the chroma it drives; the
// chroma table in console.c indexes them all.
// ==========================================================================
extern const uint32_t chroma_ws2812[];      // flashy.c
extern const uint32_t chroma_dsm[];         // play.c
extern const uint32_t chroma_synth[];       // synth.c
extern const uint32_t chroma_pcm[];         // play.c (8-bit PWM DAC)
extern const uint32_t chroma_dutymeter[];   // PWM/PDM HIGH-time integrator

#define CHROMA_WS2812_CTRL  0x00002500u
#define CHROMA_DSM_CTRL     0x0000270Cu
#define CHROMA_SYNTH_CTRL   0x00000000u
#define CHROMA_PCM_CTRL     0x00000000u
#define CHROMA_DUTYMETER_CTRL 0x00000000u

// ==========================================================================
// main.c: PRISM user interrupt flag (set by tqv_user_interrupt08)
// ==========================================================================
extern volatile bool prism_irq_fired;

// ==========================================================================
// UART peripheral (index 2): TX/RX data at 0x00, status at 0x04 (bit 0 =
// tx busy, bit 1 = rx buffered), 13-bit baud divider at 0x08.  The divider
// resets to CLOCK/115200, and baud = CLOCK / divider.
#define CLOCK_HZ        64000000u   // build-time default; see clock_hz

// The REAL project clock ('clk <MHz>' command / tqv.py --freq).  rdtime
// ticks are always clk/64 (true us only at 64MHz), so wall-time waits
// scale their us constants through us_ticks().
extern uint32_t clock_hz;
extern uint8_t  tui_running;    // console.c: TUI owns the terminal
extern int      tui_tab_rect[4]; // active tab rect for 'play -v' in TUI
extern uint8_t  adpcm_wave;      // play.c: draw the waveform this play
static inline uint32_t us_ticks(uint32_t us)
{
    return (uint32_t)(((uint64_t)us * clock_hz) / 64000000u);
}
#define UART_STATUS     (*(volatile uint32_t *)0x8000084)
#define UART_BAUD_DIV   (*(volatile uint32_t *)0x8000088)
#define UART_DIV_MIN    8u          // 8 MBaud at 64MHz - well past useful
#define UART_DIV_MAX    8191u       // 13 bit register (7813 baud)

// console.c
// ==========================================================================
void check(bool ok, const char *what);      // PASS/FAIL line + test counters
bool parse_u32(const char *s, uint32_t *out);
void chroma_set_loaded(const uint32_t *words);  // mark 'load' table entry
int  prism_chroma_index(const char *name);      // chroma table index, -1 = none
int  prism_chroma_loaded(void);                 // index in the FSM now, -1 = none
char *cli_readline(void);
int cli_split(char *line, char *argv[], int max);
void cli_execute(int argc, char *argv[]);

// ==========================================================================
// Streaming channel shared with the raw interrupt handler in prism_isr.s
// (the field offsets are hardcoded there).  Used by playz/playr/playa and
// the poly synth: the ISR consumes samples, the foreground refills.
// ==========================================================================
struct l4z_channel {
    uint32_t *rd;                // 0: next sample the ISR will play
    uint32_t *end;               // 4: end of the current buffer
    uint32_t *next;              // 8: buffer to switch to at end
    uint32_t *next_end;          // 12
    volatile uint32_t consumed;  // 16: ISR set, foreground clears
    volatile uint32_t active;    // 20: 0 off, 1 = DSM words, 2 = duty bytes
};
extern struct l4z_channel l4z_ch;   // play.c

// ==========================================================================
// Feature entry points
// ==========================================================================
void play_dsm_l4z(void);                    // play.c ('playz': flash L4Z clip)
void play_adpcm_data(const uint8_t *blob, uint32_t preload);  // in-memory blob
uint32_t adpcm_preload_for_rate(uint32_t rate);   // PCM chroma count1 preload
const uint8_t *song_ram_payload(uint32_t kind, uint32_t *length);
extern uint8_t  adpcm_quiet;                // suppress progress prints
extern uint8_t  adpcm_hold;                 // park the carrier at mid scale after
void adpcm_carrier_release(void);           // ramp a held carrier down + PWM off
extern uint32_t adpcm_underruns;            // underruns of the last play
// Descriptor in front of every stored song/pack image (written by
// tqv.py load / the download command; play.c checks the CRC on receive)
#define SONG_MAGIC      0x44565154u         // 'TQVD'
#define SONG_KIND_ADPCM 1u
#define SONG_KIND_L4Z   2u
#define SONG_KIND_SPK   3u                  // sound pack

struct song_desc {
    uint32_t magic;
    uint32_t kind;
    uint32_t length;
    uint32_t crc;
};

// ==========================================================================
// Allocation tables ("FAT") for named songs/packs: the top 1KB of RAM B
// and the top flash sector hold {magic, count} + 32-byte entries naming
// descriptor-wrapped images anywhere in that bank.  tqv.py's 'load'
// writes files and maintains the tables; the design looks names up
// (playr/playf/open ram) and can delete/format the RAM one.  All
// addresses are ABSOLUTE.  Flash entries are 4KB aligned (erase size);
// the flash table lives in its own sector so rewriting it never touches
// file data.
// ==========================================================================
#define TQFAT_MAGIC     0x31465154u         // 'TQF1'
#define TQFAT_RAM       ((uint8_t *)0x1FFFC00)
#define TQFAT_FLASH     ((const uint8_t *)0xFFF000)
#define TQFAT_SIZE      1024u
#define TQFAT_NAME_LEN  24
#define TQFAT_MAX       31                  // (1024 - 8) / 32

struct tqfat_entry {
    char     name[TQFAT_NAME_LEN];          // NUL padded
    uint32_t addr;                          // absolute, of the TQVD descriptor
    uint32_t len;                           // bytes including the descriptor
};

struct tqfat_table {
    uint32_t magic;
    uint32_t count;
    struct tqfat_entry e[TQFAT_MAX];
};

// play.c: NULL if the table is absent/invalid or the name is not there
const struct tqfat_entry *tqfat_find(const void *table, const char *name);
const struct tqfat_table *tqfat_get(const void *table);   // NULL if invalid
// Payload of a named (or first, name==NULL) RAM entry of `kind`; falls
// back to the legacy fixed slot at 0x1800000 when the table is absent
const uint8_t *song_ram_named(uint32_t kind, const char *name, uint32_t *length);
// Same for the flash table (XIP: the payload plays in place, no copy)
const uint8_t *song_flash_named(uint32_t kind, const char *name, uint32_t *length);
void song_download(void);                   // play.c: UART -> RAM B
void song_play_any(int argc, char *argv[]); // play.c ('play': RAM, then flash)
void song_resume(int argc, char *argv[]);   // play.c ('resume')
void song_download_fast(void);              // play.c ('downloadf' marker)
extern uint32_t play_speed;                 // play.c ('speed' command)
void cmd_flashy(int argc, char *argv[]);    // flashy.c
void cmd_synth(int argc, char *argv[]);     // synth.c
void cmd_midi(int argc, char *argv[]);      // midi.c
void cmd_synthp(int argc, char *argv[]);    // synth.c (polyphonic PWL)
void cpp_test_run(void);                    // cpptest.cpp
void tui_smoke_test(void);                  // tui/tuitest.c
void tui_run(void);                         // tui/TuiMain.cxx ('tui' command)

#ifdef __cplusplus
}
#endif
