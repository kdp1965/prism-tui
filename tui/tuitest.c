// Hardware smoke test for the ported termcurses + pdcurses stack
// ('tuitest' command).  Draws a framed color test screen, reports the
// negotiated terminal geometry, then waits for one keypress (echoing
// its code) before restoring the plain console.  Proves: initscr over
// the UART, the ESC[6n window-size query, color pairs, box/ACS line
// drawing, damage-tracked refresh, keyboard decode, and endwin.

#include <graphics/curses.h>

#include <stdio.h>

void tui_smoke_test(void)
{
    WINDOW *win;
    int lines, cols, key, pair;
    static const char *names[] = {
        "red", "green", "yellow", "blue", "magenta", "cyan", "white"
    };

    win = initscr();
    if (win == NULL) {
        printf("tuitest: initscr FAILED\n");
        return;
    }

    lines = LINES;
    cols  = COLS;

    start_color();
    for (pair = 1; pair <= 7; pair++)
        init_pair(pair, pair, COLOR_BLACK);
    init_pair(8, COLOR_BLACK, COLOR_CYAN);

    noecho();
    cbreak();               /* keys return immediately, no line buffering */
    keypad(stdscr, true);

    box(stdscr, 0, 0);
    attron(A_BOLD);
    mvwprintw(stdscr, 1, 2, "PRISM TUI stack smoke test");
    attroff(A_BOLD);
    mvwprintw(stdscr, 2, 2, "terminal: %d rows x %d cols", lines, cols);

    for (pair = 1; pair <= 7; pair++) {
        attron(COLOR_PAIR(pair));
        mvwprintw(stdscr, 3 + pair, 4, "color pair %d: %s", pair,
                  names[pair - 1]);
        attroff(COLOR_PAIR(pair));
    }

    attron(COLOR_PAIR(8));
    mvwprintw(stdscr, 12, 4, " reverse block ");
    attroff(COLOR_PAIR(8));

    mvwprintw(stdscr, 14, 2, "press any key to exit...");
    wrefresh(stdscr);

    // Poll for the key ourselves (nodelay) so a broken blocking path
    // can't wedge the console; count the spins for diagnostics.
    {
        int spins = 0;
        nodelay(stdscr, true);
        key = ERR;
        while (key == ERR && spins < 600) {     // ~30s at 50ms/spin
            key = wgetch(stdscr);
            if (key == ERR) {
                napms(50);
                spins++;
            }
        }
        endwin();
        printf("\ntuitest: %dx%d, key=0x%x, spins=%d - %s\n",
               lines, cols, key, spins, key == ERR ? "NO KEY" : "OK");
    }
}
