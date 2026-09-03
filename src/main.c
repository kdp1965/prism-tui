// PRISM test / control CLI for TinyQV.
//
// Provides an interactive UART command line to control the PRISM peripheral
// via the prism.c SDK library: chroma loading, config space testing, the
// debugger (halt / resume / single step / breakpoints), peripheral value
// registers (counters, compare, comm, host_in, FIFO) and raw register
// access.  Type 'help' at the prompt for the command list.
//
// The features live in their own files (see prism_tui.h); this file is
// just the PRISM interrupt handler and the main loop.

#include <stdio.h>
#include <stdbool.h>
#include <gpio.h>
#include <prism.h>
#include <tqv_fs.h>
#include "prism_tui.h"

// ==========================================================================
// PRISM user interrupt handler.  The chroma's interrupt request is level
// style (re-asserted while the FSM sits in its "done" state), so disable
// the interrupt here and let the foreground acknowledge it.
// ==========================================================================
volatile bool prism_irq_fired;

void tqv_user_interrupt08(void)
{
//    prism_disable_interrupt();
    prism_clear_interrupt();
    prism_irq_fired = true;
}

// ==========================================================================
// Main loop
// ==========================================================================

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n\033[93mPRISM control CLI (peripheral %d at %08lx)\033[0m\n",
           PRISM_PERIPHERAL_NUM, (unsigned long)PRISM_BASE_ADDRESS);

    enable_all_outputs();

    // WS2812 data comes out on uo_out[1] (PRISM out[0])
    prism_claim_pins(0x02);

    printf("PRISM ID word: %08lx\n", (unsigned long)prism_get_id());

    // Settle the host filesystem before the first prompt.  The window
    // covers tqv.py still finishing its start-up: it only serves
    // requests once its console is up, which can be after we boot.
    if (tqv_fs_probe_wait(2000))
        printf("host fs: %s\n", tqv_fs_host());
    else
        printf("host fs: none (plain terminal; 'fs probe' re-checks)\n");
    printf("type 'help' for commands\n");

    while (1) {
        printf("prism> ");
        char *line = cli_readline();

        char *argv[4];
        int argc = cli_split(line, argv, 4);
        if (argc)
            cli_execute(argc, argv);
    }
}

// vim: sw=4 ts=4 et
