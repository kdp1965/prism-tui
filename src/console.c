// CLI plumbing and the PRISM register / debug / chroma commands,
// including the scripted 'selftest' sequence.

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <gpio.h>
#include <csr.h>
#include <uart.h>
#include <prism.h>
#include <tqv_fs.h>
#include "prism_tui.h"

// Real project clock; every derived rate recomputes from this
uint32_t clock_hz = CLOCK_HZ;

// Set while the ncurses TUI owns the terminal ('play -w' declines then)
uint8_t tui_running;

// Terminal rectangle of the TUI's active tab (top,bot,left,right;
// 0-based, inclusive; -1 = none) - set by the TUI before 'play'
int tui_tab_rect[4] = { -1, -1, -1, -1 };

// ==========================================================================
// Chroma table.  Add new chromas here as they are compiled.
// ==========================================================================
typedef struct {
    const char *name;
    const uint32_t *words;
    uint32_t ctrl;
} chroma_entry_t;

static const chroma_entry_t chroma_table[] = {
    { "ws2812", chroma_ws2812, CHROMA_WS2812_CTRL },
    { "dsm",    chroma_dsm,    CHROMA_DSM_CTRL },
    { "synth",  chroma_synth,  CHROMA_SYNTH_CTRL },
    { "pcm",    chroma_pcm,    CHROMA_PCM_CTRL },
    { "dutymeter", chroma_dutymeter, CHROMA_DUTYMETER_CTRL },
};
#define NUM_CHROMAS ((int)(sizeof(chroma_table) / sizeof(chroma_table[0])))

// Index of the most recently loaded chroma, -1 if none
static int last_chroma = -1;

// Timing values for a 64MHz clock (from the verification tests):
// count2_compare = 0.8us bit time (64MHz / 1.25MHz), comm_data = 0.4us
#define WS2812_T_BIT        51
#define WS2812_T_HIGH       26

#define GRB_TEST_VALUE      0x00FF5367u

// Chroma table lookups for the TUI: 'open <name>' reads its listing from
// the host filesystem and needs to know whether that listing belongs to
// the chroma currently in the FSM before highlighting a state row.
int prism_chroma_index(const char *name)
{
    for (int i = 0; i < NUM_CHROMAS; ++i)
        if (!strcmp(chroma_table[i].name, name))
            return i;
    return -1;
}

int prism_chroma_loaded(void)
{
    return last_chroma;
}

void chroma_set_loaded(const uint32_t *words)
{
    for (int i = 0; i < NUM_CHROMAS; ++i)
        if (chroma_table[i].words == words)
            last_chroma = i;
}

// ==========================================================================
// Scripted self test (the original test sequence)
// ==========================================================================
static int tests_run;
static int tests_failed;

void check(bool ok, const char *what)
{
    ++tests_run;
    if (!ok)
        ++tests_failed;
    printf("  %s: %s\n", what, ok ? "PASS" : "FAIL");
}

static bool wait_for(volatile bool *flag, uint32_t timeout_us)
{
    uint32_t deadline = read_time() + us_ticks(timeout_us);
    while (!*flag) {
        if ((int32_t)(deadline - read_time()) <= 0)
            return *flag;
    }
    return true;
}

static bool wait_interrupt_pending(uint32_t timeout_us)
{
    uint32_t deadline = read_time() + us_ticks(timeout_us);
    while (!prism_interrupt_pending()) {
        if ((int32_t)(deadline - read_time()) <= 0)
            return prism_interrupt_pending();
    }
    return true;
}

static void run_selftest(void)
{
    tests_run = 0;
    tests_failed = 0;

    // ------------------------------------------------------------------
    // 1. Shift-register/latch configuration space
    // ------------------------------------------------------------------
    printf("\nConfig space shift/latch test\n");
    int res = prism_test_config();
    check(res == 0, "pattern load + reload-validate + recirculate");
    if (res)
        printf("    error mask %05x\n", res);

    // ------------------------------------------------------------------
    // 2. Chroma loading
    // ------------------------------------------------------------------
    printf("\nChroma load\n");
    res = prism_load_chroma(chroma_ws2812, CHROMA_WS2812_CTRL);
    chroma_set_loaded(chroma_ws2812);
    check(res == 0, "load ws2812 chroma");

    // Load it again, this time validating the first load word by word as
    // it is shifted out of the STEW array
    res = prism_load_chroma_verify(chroma_ws2812, chroma_ws2812,
                                   CHROMA_WS2812_CTRL);
    check(res == 0, "reload while validating previous load");
    if (res)
        printf("    error mask %05x\n", res);
    last_chroma = 0;

    // ------------------------------------------------------------------
    // 3. Peripheral value registers
    // ------------------------------------------------------------------
    printf("\nPeripheral registers\n");
    prism_set_count2_compare(WS2812_T_BIT);
    check(prism_get_count2_compare() == WS2812_T_BIT, "count2 compare");

    prism_comm_write(WS2812_T_HIGH);
    check(prism_comm_read() == WS2812_T_HIGH, "comm data");

    prism_shift24_write(GRB_TEST_VALUE);
    check(prism_get_count1_preload() == GRB_TEST_VALUE, "24-bit SR preload");

    prism_host_write(0);
    check(prism_host_read() == 0, "host_in");

    // ------------------------------------------------------------------
    // 4. Debugger: halt / single step / trace
    // ------------------------------------------------------------------
    printf("\nDebugger\n");
    prism_dbg_set_breakpoint(0, 1);
    check(prism_dbg_curr_state() == 0, "idle in state 0");

    // Start a transfer while halted, then single step and trace the FSM
    prism_host_write(1);
    check(prism_host_read(), "host_in");

    uint8_t trace[24];
    bool steps_ok = true;
    for (unsigned i = 0; i < sizeof(trace); ++i) {
        steps_ok &= prism_dbg_step();
        trace[i] = prism_dbg_curr_state();
    }
    check(steps_ok, "single stepping");

    printf("    state trace:");
    for (unsigned i = 0; i < sizeof(trace); ++i)
        printf(" %x", trace[i]);
    printf("\n");

    // Pick a state from the observed trace for the breakpoint test below
    uint8_t bp_state = 0;
    for (unsigned i = 0; i < sizeof(trace); ++i)
        if (trace[i] != 0) {
            bp_state = trace[i];
            break;
        }
    check(bp_state != 0, "FSM left idle state");

    // Resume and let the transfer finish; completion is signalled by the
    // PRISM user interrupt
    prism_dbg_resume();
    check(!prism_dbg_is_halted(), "resume");

    prism_irq_fired = false;
    prism_clear_interrupt();        // discard the halt-raised interrupt
    prism_enable_interrupt();

    check(wait_for(&prism_irq_fired, 500000), "completion interrupt");
    check(prism_shift24_read() == 0, "24-bit SR fully shifted out");

    // Acknowledge via the hardware auto-toggle: flips host_in[0] back to 0
    // and clears the interrupt while preserving the compare value
    prism_host_toggle();
    check(prism_host_read() == 0, "host_in[0] auto-toggle");
    check(prism_get_count2_compare() == WS2812_T_BIT,
          "compare preserved by toggle");

    // ------------------------------------------------------------------
    // 5. Breakpoints (polled, no interrupt)
    // ------------------------------------------------------------------
    printf("\nBreakpoint at state %x\n", bp_state);
    prism_dbg_set_breakpoint(0, bp_state);

    prism_shift24_write(GRB_TEST_VALUE);
    prism_host_write(1);            // start another transfer

    check(prism_dbg_wait_halt(100000), "breakpoint hit");
    check(prism_dbg_curr_state() == bp_state, "halted at breakpoint state");
    check(prism_dbg_break_active(), "break active flag");

    prism_dbg_clear_breakpoint(0);
    prism_dbg_resume();
    prism_clear_interrupt();        // discard the breakpoint-raised interrupt

    check(wait_interrupt_pending(500000), "transfer completed after resume");
    prism_host_toggle();

    // ------------------------------------------------------------------
    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    printf(tests_failed ? "*** FAIL ***\n" : "*** PASS ***\n");
}

// ==========================================================================
// CLI plumbing
// ==========================================================================

char *cli_readline(void)
{
    static char buf[96];
    unsigned len = 0;

    for (;;) {
        int c = uart_getc();

        if (c == -1)
           continue;
        if (c == 3)
           printf("\x1b[H");
        if (c == '\r' || c == '\n') {
            putchar('\n');
            buf[len] = '\0';
            return buf;
        }
        if (c == '\b' || c == 0x7F) {
            if (len) {
                --len;
                printf("\b \b");
            }
            continue;
        }
        if (c >= ' ' && c < 0x7F && len < sizeof(buf) - 1) {
            buf[len++] = (char)c;
            putchar(c);
        }
    }
}

int cli_split(char *line, char *argv[], int max)
{
    int argc = 0;
    while (argc < max) {
        while (*line == ' ')
            ++line;
        if (!*line)
            break;
        argv[argc++] = line;
        while (*line && *line != ' ')
            ++line;
        if (*line)
            *line++ = '\0';
    }
    return argc;
}

// C style number parse: 0x prefix for hex, else decimal
bool parse_u32(const char *s, uint32_t *out)
{
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end) {
        printf("bad number '%s'\n", s);
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

// Raw access address: below 0x40 is a PRISM register offset, else absolute
static uintptr_t raw_addr(uint32_t a)
{
    return a < 0x40 ? PRISM_BASE_ADDRESS + a : (uintptr_t)a;
}

// Generic read/[set] command bodies for 8 and 32-bit valued registers
static void rw_reg8(int argc, char *argv[], void (*setf)(uint8_t),
                    uint8_t (*getf)(void), const char *name)
{
    uint32_t v;
    if (argc > 1) {
        if (!parse_u32(argv[1], &v))
            return;
        setf((uint8_t)v);
    }
    printf("%s = 0x%02x\n", name, getf());
}

static void rw_reg32(int argc, char *argv[], void (*setf)(uint32_t),
                     uint32_t (*getf)(void), const char *name)
{
    uint32_t v;
    if (argc > 1) {
        if (!parse_u32(argv[1], &v))
            return;
        setf(v);
    }
    printf("%s = 0x%08lx\n", name, (unsigned long)getf());
}

// ==========================================================================
// Commands
// ==========================================================================

static void cmd_help(void)
{
    printf(
"PRISM CLI - numbers are C style (0x.. for hex, else decimal)\n"
"  flashy               Drives an AdaFruit 7-Neopixel Jewel using\n"
"                       a WS2812B driver Chroma\n"
"  play [-w] [name] [a] Plays a song from the RAM or flash FAT (RAM wins;\n"
"                       -w draws its waveform in braille as it plays;\n"
"                       SPACE toggles scrolling / triggered scope view)\n"
"  resume [-w]          Continues the last keypress-stopped song\n"
"  playz [speed]        Plays the flash L4Z DSM demo clip (ISR streamed)\n"
"  download             Receives a song over UART into RAM B (use\n"
"                       './tqv.py send <file>', or type it inside\n"
"                       './tqv.py console' to be prompted for a file)\n"
"  fat                  List the RAM B / flash allocation tables\n"
"  fatdel <name>        Remove a RAM table entry (flash: tqv.py fat)\n"
"  fatfmt               Format (empty) the RAM allocation table\n"
"  downloadf            Asks './tqv.py console' to fast-load a song over\n"
"                       SPI (stops and restarts the design, ~15s)\n"
"                       Default speed is 62\n"
"  midi [-i] <file>     Play a MIDI file from the served songs/ dir\n"
"                       (auto GM mapping, ch10 = kick/snare/hat kit)\n"
"  synthp [-i] [demo [n]] Polyphonic PWL synth (saw/tri + morph;\n"
"                       -i: 2x interpolated output);\n"
"                       demos: 1 = lanterns, 2 = gmlast (from MIDI)\n"
"  synth [demo [n]]     80's synthesizer on the audio PMOD (uo_out[7]);\n"
"                       'synth demo 0' lists the demo songs\n"
"  id                   read PRISM ID register\n"
"  regs                 dump all PRISM registers\n"
"  ctrl [v]             read / write CTRL register\n"
"  en | dis             enable / disable PRISM\n"
"  irq [clr|en|dis]     interrupt status / clear / enable / disable\n"
"  chromas              list chroma table\n"
"  load <n> [-v]        load chroma n; -v validates the previous load\n"
"                       as it shifts out of the STEW array\n"
"  vcfg [n]             verify config vs chroma n (recirculating)\n"
"  cfgr                 read config stage %d STEW (msw lsw)\n"
"  cfgw <msw> <lsw>     shift one STEW into the config array\n"
"  cfgtest              config space pattern self test\n"
"  tui                  full-screen debugger UI (ncurses over this UART;\n"
"                       exit/quit returns here)\n"
"  clk [MHz]            the real project clock, for playback/baud math\n"
"  baud [rate]          console baud rate (tqv.py follows the switch; a\n"
"                       reset returns to 115200)\n"
"  fs [probe]           host filesystem status (tqv.py serves a directory\n"
"                       to the design; the TUI keeps prism.cfg there)\n"
"  ls [dir]             list the host filesystem directory\n"
"  cat <file>           print a host file\n"
"  selftest             run the scripted test sequence\n"
"  cpptest              run the C++ support smoke test\n"
"  bp <0|1> <state>     set breakpoint\n"
"  bc <0|1>             clear breakpoint\n"
"  halt | go            halt / resume the FSM\n"
"  step [n]             single step n times (default 1)\n"
"  si [n]               show state index / force state while halted\n"
"  dbg                  show DBG_CTRL and DBG_STAT\n"
"  dbgw <v>             raw write DBG_CTRL\n"
"  host [v]             read / set host_in[1:0]\n"
"  tog                  auto-toggle host_in[0] (also clears irq)\n"
"  pre [v]              read / set 24-bit count1 preload / SR load\n"
"  cnt                  read live count1 / count2\n"
"  cmp [v]              read / set count2 compare\n"
"  comm [v]             read / set 8-bit comm register\n"
"  fifo [rd]            FIFO status / pop one byte\n"
"  io                   live input / output vectors and pins\n"
"  rd <a> | rdb <a>     raw 32 / 8-bit read\n"
"  wr <a> <v> | wrb ..  raw 32 / 8-bit write\n"
"                       (addr < 0x40 = PRISM offset, else absolute)\n"
"  pins <mask>          route uo_out pins in mask to the PRISM\n",
        PRISM_NUM_STATES - 1);
}

static void print_dbg(void)
{
    uint32_t c = prism_dbg_get_ctrl();
    uint32_t s = prism_dbg_status();

    printf("DBG_CTRL = 0x%03lx  halt_req=%d step=%d", (unsigned long)c,
           !!(c & PRISM_DBG_HALT_REQ), !!(c & PRISM_DBG_STEP));
    if (c & PRISM_DBG_BP0_EN)
        printf(" bp0=%lu", (unsigned long)((c >> 4) & 0x7));
    if (c & PRISM_DBG_BP1_EN)
        printf(" bp1=%lu", (unsigned long)((c >> 7) & 0x7));
    printf("\nDBG_STAT = 0x%03lx  curr=%lu next=%lu halted=%d break=%d\n",
           (unsigned long)s,
           (unsigned long)PRISM_DBG_STAT_CURR_SI(s),
           (unsigned long)PRISM_DBG_STAT_NEXT_SI(s),
           !!(s & PRISM_DBG_STAT_HALTED), !!(s & PRISM_DBG_STAT_BREAK));
}

static void cmd_regs(void)
{
    uint32_t msw, lsw;
    uint32_t ctrl = prism_get_ctrl();
    uint8_t fs = prism_fifo_status();

    printf("CTRL     = 0x%08lx  en=%d irq=%d pins=0x%02lx ui7=%d\n",
           (unsigned long)ctrl, !!(ctrl & PRISM_CTRL_ENABLE),
           !!(ctrl & PRISM_CTRL_INTERRUPT),
           (unsigned long)PRISM_CTRL_GET_OUT_PINS(ctrl),
           !!(ctrl & PRISM_CTRL_UI_IN7));
    print_dbg();
    prism_cfg_read(&msw, &lsw);
    printf("CFG[%d]   = 0x%03lx %08lx\n", PRISM_NUM_STATES - 1,
           (unsigned long)msw, (unsigned long)lsw);
    printf("COMM     = 0x%02x\n", prism_comm_read());
    printf("FIFO     = 0x%02x  empty=%d full=%d count=%lu\n", fs,
           !!(fs & PRISM_FIFO_STAT_EMPTY), !!(fs & PRISM_FIFO_STAT_FULL),
           (unsigned long)PRISM_FIFO_STAT_COUNT(fs));
    printf("HOST_IN  = 0x%x\n", prism_host_read());
    printf("PRELOAD  = 0x%06lx\n", (unsigned long)prism_get_count1_preload());
    printf("COUNT1   = 0x%06lx  COUNT2 = 0x%02x\n",
           (unsigned long)prism_get_count1(), prism_get_count2());
    printf("COMPARE  = 0x%02x\n", prism_get_count2_compare());
    printf("DECISION = 0x%08lx\n", (unsigned long)prism_get_decision_tree());
    printf("OUT_DATA = 0x%03lx  IN_DATA = 0x%04lx\n",
           (unsigned long)prism_get_outputs(),
           (unsigned long)prism_get_inputs());
    printf("ID       = 0x%08lx\n", (unsigned long)prism_get_id());
}

static void cmd_chromas(void)
{
    for (int i = 0; i < NUM_CHROMAS; ++i)
        printf("  %d: %-12s ctrl=0x%04lx%s\n", i, chroma_table[i].name,
               (unsigned long)chroma_table[i].ctrl,
               i == last_chroma ? "  (loaded)" : "");
}

static int find_chroma(const char *s)
{
    if (s[0] >= '0' && s[0] <= '9') {
        uint32_t n;
        if (!parse_u32(s, &n))
            return -1;
        if ((int)n < NUM_CHROMAS)
            return (int)n;
    } else {
        for (int i = 0; i < NUM_CHROMAS; ++i)
            if (!strcmp(s, chroma_table[i].name))
                return i;
    }
    printf("no such chroma '%s' - try 'chromas'\n", s);
    return -1;
}

static void cmd_load(int argc, char *argv[])
{
    if (argc < 2) {
        cmd_chromas();
        return;
    }

    int idx = find_chroma(argv[1]);
    if (idx < 0)
        return;

    bool verify = argc > 2 && !strcmp(argv[2], "-v");
    const chroma_entry_t *e = &chroma_table[idx];
    int res;

    if (verify && last_chroma >= 0) {
        printf("loading '%s', validating '%s' as it shifts out\n",
               e->name, chroma_table[last_chroma].name);
        res = prism_load_chroma_verify(e->words,
                                       chroma_table[last_chroma].words,
                                       e->ctrl);
    } else {
        if (verify)
            printf("no previous load to validate\n");
        res = prism_load_chroma(e->words, e->ctrl);
    }

    if (res == 0) {
        printf("loaded '%s', PRISM enabled\n", e->name);
        last_chroma = idx;
    } else {
        printf("load FAILED, error mask 0x%05x, PRISM disabled\n", res);
    }
}

static void cmd_vcfg(int argc, char *argv[])
{
    int idx = last_chroma;
    if (argc > 1)
        idx = find_chroma(argv[1]);
    if (idx < 0) {
        if (argc <= 1)
            printf("nothing loaded yet - 'vcfg <n>' to pick a chroma\n");
        return;
    }

    bool was_enabled = prism_is_enabled();
    if (was_enabled && !prism_dbg_is_halted())
        prism_dbg_halt();

    int res = prism_verify_config(chroma_table[idx].words);
    printf("verify vs '%s': %s", chroma_table[idx].name,
           res ? "FAIL, error mask 0x" : "PASS\n");
    if (res)
        printf("%05x\n", res);

    if (was_enabled)
        prism_dbg_resume();
}

static void cmd_step(int argc, char *argv[])
{
    uint32_t n = 1;
    if (argc > 1 && !parse_u32(argv[1], &n))
        return;
    if (!prism_dbg_is_halted()) {
        printf("not halted\n");
        return;
    }
    if (n > 256)
        n = 256;

    printf("trace:");
    for (uint32_t i = 0; i < n; ++i) {
        if (!prism_dbg_step()) {
            printf(" (step failed)");
            break;
        }
        printf(" %x", prism_dbg_curr_state());
    }
    printf("\n");
}

static void cmd_si(int argc, char *argv[])
{
    if (argc > 1) {
        uint32_t v;
        if (!parse_u32(argv[1], &v))
            return;
        if (!prism_dbg_is_halted())
            printf("warning: not halted, force may not stick\n");
        prism_dbg_set_state((uint8_t)v);
    }

    uint32_t s = prism_dbg_status();
    printf("curr=%lu next=%lu%s%s\n",
           (unsigned long)PRISM_DBG_STAT_CURR_SI(s),
           (unsigned long)PRISM_DBG_STAT_NEXT_SI(s),
           (s & PRISM_DBG_STAT_HALTED) ? " (halted)" : "",
           (s & PRISM_DBG_STAT_BREAK) ? " (break)" : "");
}

static void cmd_bp(int argc, char *argv[], bool set)
{
    uint32_t bp, state = 0;
    if (argc < (set ? 3 : 2)) {
        printf("usage: %s\n", set ? "bp <0|1> <state>" : "bc <0|1>");
        return;
    }
    if (!parse_u32(argv[1], &bp) || bp > 1)
        return;
    if (set) {
        if (!parse_u32(argv[2], &state))
            return;
        prism_dbg_set_breakpoint((int)bp, (uint8_t)state);
        printf("bp%lu set at state %lu\n", (unsigned long)bp,
               (unsigned long)state);
    } else {
        prism_dbg_clear_breakpoint((int)bp);
        printf("bp%lu cleared\n", (unsigned long)bp);
    }
}

static void cmd_irq(int argc, char *argv[])
{
    if (argc > 1) {
        if (!strcmp(argv[1], "clr"))
            prism_clear_interrupt();
        else if (!strcmp(argv[1], "en")) {
            prism_irq_fired = false;
            prism_enable_interrupt();
        } else if (!strcmp(argv[1], "dis"))
            prism_disable_interrupt();
        else {
            printf("usage: irq [clr|en|dis]\n");
            return;
        }
    }
    printf("irq pending=%d\n", prism_interrupt_pending());
}

static void cmd_fifo(int argc, char *argv[])
{
    if (argc > 1 && !strcmp(argv[1], "rd")) {
        int v = prism_fifo_read();
        if (v < 0)
            printf("fifo empty\n");
        else
            printf("popped 0x%02x\n", v);
        return;
    }

    uint8_t fs = prism_fifo_status();
    printf("FIFO stat=0x%02x  empty=%d full=%d count=%lu rd_ptr=%lu wr_ptr=%lu\n",
           fs, !!(fs & PRISM_FIFO_STAT_EMPTY), !!(fs & PRISM_FIFO_STAT_FULL),
           (unsigned long)PRISM_FIFO_STAT_COUNT(fs),
           (unsigned long)PRISM_FIFO_STAT_RD_PTR(fs),
           (unsigned long)PRISM_FIFO_STAT_WR_PTR(fs));
}

static void cmd_io(void)
{
    printf("IN_DATA  = 0x%04lx\n", (unsigned long)prism_get_inputs());
    printf("OUT_DATA = 0x%03lx\n", (unsigned long)prism_get_outputs());
    printf("uo_out[7:1] latched = 0x%02x\n", prism_get_out_pins());
    printf("DECISION = 0x%08lx\n", (unsigned long)prism_get_decision_tree());
}

static void cmd_raw(int argc, char *argv[], bool write, bool byte)
{
    uint32_t a, v = 0;
    if (argc < (write ? 3 : 2)) {
        printf("usage: %s%s <addr>%s\n", write ? "wr" : "rd",
               byte ? "b" : "", write ? " <val>" : "");
        return;
    }
    if (!parse_u32(argv[1], &a))
        return;
    if (write && !parse_u32(argv[2], &v))
        return;

    uintptr_t addr = raw_addr(a);
    if (write) {
        if (byte)
            *(volatile uint8_t*)addr = (uint8_t)v;
        else
            *(volatile uint32_t*)addr = v;
        printf("[%08lx] <= 0x%0*lx\n", (unsigned long)addr, byte ? 2 : 8,
               (unsigned long)v);
    } else {
        v = byte ? *(volatile uint8_t*)addr : *(volatile uint32_t*)addr;
        printf("[%08lx] = 0x%0*lx\n", (unsigned long)addr, byte ? 2 : 8,
               (unsigned long)v);
    }
}

// ==========================================================================
// Command dispatch
// ==========================================================================

void cli_execute(int argc, char *argv[])
{
    const char *cmd = argv[0];
    uint32_t v;

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        cmd_help();
    else if (!strcmp(cmd, "id"))
        printf("ID = 0x%08lx\n", (unsigned long)prism_get_id());
    else if (!strcmp(cmd, "regs"))
        cmd_regs();
    else if (!strcmp(cmd, "play"))
        song_play_any(argc, argv);
    else if (!strcmp(cmd, "resume"))
        song_resume(argc, argv);
    else if (!strcmp(cmd, "download"))
        song_download();
    else if (!strcmp(cmd, "downloadf"))
        song_download_fast();
    else if (!strcmp(cmd, "speed"))
        parse_u32(argv[1], &play_speed);
    else if (!strcmp(cmd, "synth"))
        cmd_synth(argc, argv);
    else if (!strcmp(cmd, "synthp"))
        cmd_synthp(argc, argv);
    else if (!strcmp(cmd, "midi"))
        cmd_midi(argc, argv);
    else if (!strcmp(cmd, "flashy"))
        cmd_flashy(argc, argv);
    else if (!strcmp(cmd, "ctrl"))
        rw_reg32(argc, argv, prism_set_ctrl, prism_get_ctrl, "CTRL");
    else if (!strcmp(cmd, "en")) {
        prism_enable();
        printf("enabled\n");
    } else if (!strcmp(cmd, "dis")) {
        prism_disable();
        printf("disabled\n");
    } else if (!strcmp(cmd, "irq"))
        cmd_irq(argc, argv);
    else if (!strcmp(cmd, "chromas"))
        cmd_chromas();
    else if (!strcmp(cmd, "load"))
        cmd_load(argc, argv);
    else if (!strcmp(cmd, "vcfg"))
        cmd_vcfg(argc, argv);
    else if (!strcmp(cmd, "cfgr")) {
        uint32_t msw, lsw;
        prism_cfg_read(&msw, &lsw);
        printf("CFG[%d] = 0x%03lx %08lx\n", PRISM_NUM_STATES - 1,
               (unsigned long)msw, (unsigned long)lsw);
    } else if (!strcmp(cmd, "cfgw")) {
        uint32_t msw;
        if (argc < 3) {
            printf("usage: cfgw <msw> <lsw>\n");
            return;
        }
        if (parse_u32(argv[1], &msw) && parse_u32(argv[2], &v))
            prism_cfg_write(msw, v);
    } else if (!strcmp(cmd, "cfgtest")) {
        int res = prism_test_config();
        printf("config test: %s (mask 0x%05x)\n", res ? "FAIL" : "PASS", res);
    } else if (!strcmp(cmd, "cpptest"))
        cpp_test_run();
    else if (!strcmp(cmd, "fat")) {
        // List both allocation tables (see prism_tui.h): tqv.py's
        // 'load' maintains them, the players look names up in them.
        static const struct { const char *label; const void *table; }
        banks[] = { { "RAM B", TQFAT_RAM }, { "flash", TQFAT_FLASH } };

        for (int b = 0; b < 2; ++b) {
            const struct tqfat_table *t = tqfat_get(banks[b].table);

            if (t == NULL) {
                printf("%s: no allocation table ('./tqv.py fat --format',"
                       " or just './tqv.py load')\n", banks[b].label);
                continue;
            }
            printf("%s: %lu of %u entries\n", banks[b].label,
                   (unsigned long)t->count, TQFAT_MAX);
            for (uint32_t i = 0; i < t->count; ++i) {
                const struct song_desc *d =
                    (const struct song_desc *)(uintptr_t)t->e[i].addr;
                const char *kind =
                    d->magic != SONG_MAGIC   ? "?" :
                    d->kind == SONG_KIND_ADPCM ? "adpcm" :
                    d->kind == SONG_KIND_L4Z   ? "l4z" :
                    d->kind == SONG_KIND_SPK   ? "pack" : "?";

                printf("  %-23s %08lx %7luKiB %s\n", t->e[i].name,
                       (unsigned long)t->e[i].addr,
                       (unsigned long)((t->e[i].len + 1023) / 1024), kind);
            }
        }
    }
    else if (!strcmp(cmd, "fatdel")) {
        // Remove one RAM entry (the data stays; the space is reusable).
        // Flash entries are managed by tqv.py, which owns the eraser.
        struct tqfat_table *t = (struct tqfat_table *)(void *)TQFAT_RAM;

        if (argc < 2) {
            printf("usage: fatdel <name>   (RAM entries; flash via"
                   " './tqv.py fat --delete')\n");
            return;
        }
        const struct tqfat_entry *e = tqfat_find(TQFAT_RAM, argv[1]);

        if (e == NULL) {
            printf("no RAM entry '%s'\n", argv[1]);
            return;
        }
        uint32_t i = (uint32_t)(e - t->e);

        for (; i + 1 < t->count; ++i)
            t->e[i] = t->e[i + 1];
        t->count--;
        printf("deleted '%s' (%lu left)\n", argv[1],
               (unsigned long)t->count);
    }
    else if (!strcmp(cmd, "fatfmt")) {
        // Fresh empty RAM table (does not touch file data or flash)
        struct tqfat_table *t = (struct tqfat_table *)(void *)TQFAT_RAM;

        t->magic = TQFAT_MAGIC;
        t->count = 0;
        printf("RAM allocation table formatted\n");
    }
    else if (!strcmp(cmd, "clk")) {
        uint32_t mhz = 0;

        if (argc > 1) {
            if (!parse_u32(argv[1], &mhz) || mhz < 1 || mhz > 200) {
                printf("clk: expected MHz (1-200)\n");
                return;
            }
            clock_hz = mhz * 1000000u;
        }
        printf("clock %lu.%02lu MHz (playback and baud math; tqv.py"
               " --freq announces it)\n",
               (unsigned long)(clock_hz / 1000000u),
               (unsigned long)((clock_hz % 1000000u) / 10000u));
    }
    else if (!strcmp(cmd, "baud")) {
        // The UART peripheral's baud divider is writable (13 bits at
        // 0x8000088, reset 555 = 115200 at 64MHz), so the console link
        // can be sped up at run time.  Both ends have to move together:
        // we print a marker, let the transmitter drain, then switch -
        // tqv.py sees the marker and rebuilds its side at the new rate.
        // A reset always brings the divider back to 115200, so a wrong
        // guess costs a './tqv.py run', not a reflash.
        uint32_t rate = 0, div;

        // The UART counts 0..divider inclusive, so the bit period is
        // divider+1 clocks (the 115200 reset value 555 = 556 clocks)
        if (argc < 2) {
            div = UART_BAUD_DIV;
            printf("baud %lu (divider %lu); 'baud <rate>' changes it\n",
                   (unsigned long)(clock_hz / (div + 1)),
                   (unsigned long)div);
            return;
        }
        if (!parse_u32(argv[1], &rate) || rate == 0)
            return;
        div = (clock_hz + rate / 2) / rate;
        if (div > 0) div -= 1;
        if (div < UART_DIV_MIN) div = UART_DIV_MIN;
        if (div > UART_DIV_MAX) div = UART_DIV_MAX;
        rate = clock_hz / (div + 1);

        printf("switching to %lu baud (divider %lu)\n",
               (unsigned long)rate, (unsigned long)div);
        printf("\x05TQVBAUD:%lu\x05\n", (unsigned long)rate);

        // Drain first: the SDK's transmitter is interrupt driven behind a
        // 64 byte ring, so "not busy" has to hold for a while before the
        // ring is really empty.  Changing the divider mid-byte would
        // garble it and leave the host resyncing.
        {
            uint32_t idle_since = read_time();

            while (read_time() - idle_since < us_ticks(5000u)) {
                if (UART_STATUS & 1u)           // transmitter busy
                    idle_since = read_time();
            }
        }
        UART_BAUD_DIV = div;
    }
    else if (!strcmp(cmd, "fs")) {
        // Status of the filesystem tqv.py's console serves.  Nothing
        // file related works from a plain terminal (the web console):
        // the probe settles that once and the rest fail fast.
        if (argc > 1 && !strcmp(argv[1], "probe"))
            tqv_fs_reprobe();
        if (tqv_fs_available())
            printf("host fs: %s\n", tqv_fs_host());
        else
            printf("host fs: none (run the console from tqv.py, then"
                   " 'fs probe')\n");
    } else if (!strcmp(cmd, "ls")) {
        static char list[512];
        int n = tqv_fs_list(argc > 1 ? argv[1] : ".", list, sizeof(list) - 1);

        if (n < 0) {
            printf("ls: no host filesystem\n");
            return;
        }
        list[n] = '\0';
        fputs(list, stdout);
    } else if (!strcmp(cmd, "cat")) {
        // Through stdio on purpose: proves fopen/fgets reach the host
        FILE *f;
        char line[128];

        if (argc < 2) {
            printf("usage: cat <file>\n");
            return;
        }
        if ((f = fopen(argv[1], "r")) == NULL) {
            printf("cat: cannot open %s\n", argv[1]);
            return;
        }
        while (fgets(line, sizeof(line), f) != NULL)
            fputs(line, stdout);
        fclose(f);
    }
    else if (!strcmp(cmd, "tuitest"))
        tui_smoke_test();
    else if (!strcmp(cmd, "tui")) {
        tui_running = 1;
        tui_run();
        tui_running = 0;
    }
    else if (!strcmp(cmd, "selftest"))
        run_selftest();
    else if (!strcmp(cmd, "bp"))
        cmd_bp(argc, argv, true);
    else if (!strcmp(cmd, "bc"))
        cmd_bp(argc, argv, false);
    else if (!strcmp(cmd, "halt"))
        printf(prism_dbg_halt() ? "halted at state %u\n" : "halt FAILED (state %u)\n",
               prism_dbg_curr_state());
    else if (!strcmp(cmd, "go")) {
        prism_dbg_resume();
        printf("resumed, halted=%d\n", prism_dbg_is_halted());
    } else if (!strcmp(cmd, "step"))
        cmd_step(argc, argv);
    else if (!strcmp(cmd, "si"))
        cmd_si(argc, argv);
    else if (!strcmp(cmd, "dbg"))
        print_dbg();
    else if (!strcmp(cmd, "dbgw")) {
        if (argc > 1 && parse_u32(argv[1], &v)) {
            prism_write32(PRISM_REG_DBG_CTRL, v);
            print_dbg();
        } else if (argc <= 1)
            printf("usage: dbgw <v>\n");
    } else if (!strcmp(cmd, "host"))
        rw_reg8(argc, argv, prism_host_write, prism_host_read, "host_in");
    else if (!strcmp(cmd, "tog")) {
        prism_host_toggle();
        printf("host_in = 0x%x, irq pending=%d\n", prism_host_read(),
               prism_interrupt_pending());
    } else if (!strcmp(cmd, "pre"))
        rw_reg32(argc, argv, prism_set_count1_preload,
                 prism_get_count1_preload, "PRELOAD");
    else if (!strcmp(cmd, "cnt"))
        printf("COUNT1 = 0x%06lx  COUNT2 = 0x%02x\n",
               (unsigned long)prism_get_count1(), prism_get_count2());
    else if (!strcmp(cmd, "cmp"))
        rw_reg8(argc, argv, prism_set_count2_compare,
                prism_get_count2_compare, "COMPARE");
    else if (!strcmp(cmd, "comm"))
        rw_reg8(argc, argv, prism_comm_write, prism_comm_read, "COMM");
    else if (!strcmp(cmd, "fifo"))
        cmd_fifo(argc, argv);
    else if (!strcmp(cmd, "io"))
        cmd_io();
    else if (!strcmp(cmd, "rd"))
        cmd_raw(argc, argv, false, false);
    else if (!strcmp(cmd, "rdb"))
        cmd_raw(argc, argv, false, true);
    else if (!strcmp(cmd, "wr"))
        cmd_raw(argc, argv, true, false);
    else if (!strcmp(cmd, "wrb"))
        cmd_raw(argc, argv, true, true);
    else if (!strcmp(cmd, "pins")) {
        if (argc > 1 && parse_u32(argv[1], &v)) {
            prism_claim_pins((uint8_t)v);
            printf("pins 0x%02lx routed to PRISM\n", (unsigned long)(v & 0xFE));
        } else if (argc <= 1)
            printf("usage: pins <mask>\n");
    } else
        printf("unknown command '%s' - try 'help'\n", cmd);
}
