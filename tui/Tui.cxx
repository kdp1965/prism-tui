/************************************************************************************
 * tui/CTui.cxx
 *
 *   Copyright (C) 2018 Ken Pettit. All rights reserved.
 *   Author: Ken Pettit <pettitkd@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ************************************************************************************/

#ifndef STANDALONE
#include <nuttx/config.h>
#include <graphics/curses.h>
#include <strings.h>
#else
#include <ncursesw/ncurses.h>
#include <unistd.h>
#include <signal.h>
#include <locale.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/ioctl.h>


#include "Tui.h"
#include "TuiSource.h"

/*
==============================================================================
Local defines
==============================================================================
*/
#ifndef STANDALONE
#define  SYNTAX_COLOR_NORMAL        (COLOR_WHITE | 8)
#define  SYNTAX_COLOR_COMMENT       COLOR_CYAN
#define  SYNTAX_COLOR_LINENO        COLOR_GREEN
#define  SYNTAX_COLOR_BACKGROUND    COLOR_BLACK
#define  SYNTAX_COLOR_KEYWORD       COLOR_BLUE
#define  SYNTAX_COLOR_REG           COLOR_YELLOW
#define  SYNTAX_COLOR_LABEL         (COLOR_RED | 8)
#define  SYNTAX_COLOR_PUNCT         COLOR_WHITE
#define  SYNTAX_COLOR_DECLARATOR    COLOR_GREEN
#else
#define  SYNTAX_COLOR_NORMAL        (COLOR_WHITE)
#define  SYNTAX_COLOR_COMMENT       COLOR_CYAN
#define  SYNTAX_COLOR_LINENO        COLOR_GREEN
#define  SYNTAX_COLOR_BACKGROUND    COLOR_BLACK
#define  SYNTAX_COLOR_KEYWORD       COLOR_BLUE
#define  SYNTAX_COLOR_REG           COLOR_YELLOW
#define  SYNTAX_COLOR_LABEL         (COLOR_MAGENTA)
#define  SYNTAX_COLOR_PUNCT         COLOR_WHITE
#define  SYNTAX_COLOR_DECLARATOR    COLOR_GREEN
#endif

#define KEY_TAB     9

#ifndef CTRL_B
#define CTRL_B      2
#endif

#ifndef CTRL_D
#define CTRL_D      4
#endif

#ifndef CTRL_F
#define CTRL_F      6
#endif

#ifndef CTRL_G
#define CTRL_G      7
#endif

#ifndef CTRL_K
#define CTRL_K      11
#endif

#ifndef CTRL_L
#define CTRL_L      12
#endif

#ifndef CTRL_P
#define CTRL_P      16
#endif

#ifndef CTRL_W
#define CTRL_W      23
#endif

#ifndef CTRL_R
#define CTRL_R      18
#endif

#ifndef CTRL_T
#define CTRL_T      20
#endif

#ifndef CTRL_U
#define CTRL_U      21
#endif

#ifndef CTRL_Y
#define CTRL_Y      25
#endif

#ifdef STANDALONE
#define ALT_0           (KEY_MAX+0x197)
#define ALT_1           (KEY_MAX+0x198)
#define ALT_2           (KEY_MAX+0x199)
#define ALT_3           (KEY_MAX+0x19a)
#define ALT_4           (KEY_MAX+0x19b)
#define ALT_5           (KEY_MAX+0x19c)
#define ALT_6           (KEY_MAX+0x19d)
#define ALT_7           (KEY_MAX+0x19e)
#define ALT_8           (KEY_MAX+0x19f)
#define ALT_9           (KEY_MAX+0x1a0)
#define ALT_A           (KEY_MAX+0x1a1)
#define ALT_B           (KEY_MAX+0x1a2)
#define ALT_C           (KEY_MAX+0x1a3)
#define ALT_D           (KEY_MAX+0x1a4)
#define ALT_E           (KEY_MAX+0x1a5)
#define ALT_F           (KEY_MAX+0x1a6)
#define ALT_G           (KEY_MAX+0x1a7)
#define ALT_H           (KEY_MAX+0x1a8)
#define ALT_I           (KEY_MAX+0x1a9)
#define ALT_J           (KEY_MAX+0x1aa)
#define ALT_K           (KEY_MAX+0x1ab)
#define ALT_L           (KEY_MAX+0x1ac)
#define ALT_M           (KEY_MAX+0x1ad)
#define ALT_N           (KEY_MAX+0x1ae)
#define ALT_O           (KEY_MAX+0x1af)
#define ALT_P           (KEY_MAX+0x1b0)
#define ALT_Q           (KEY_MAX+0x1b1)
#define ALT_R           (KEY_MAX+0x1b2)
#define ALT_S           (KEY_MAX+0x1b3)
#define ALT_T           (KEY_MAX+0x1b4)
#define ALT_U           (KEY_MAX+0x1b5)
#define ALT_V           (KEY_MAX+0x1b6)
#define ALT_W           (KEY_MAX+0x1b7)
#define ALT_X           (KEY_MAX+0x1b8)
#define ALT_Y           (KEY_MAX+0x1b9)
#define ALT_Z           (KEY_MAX+0x1ba)
#define ALT_PLUS        (KEY_MAX+0x225)  /* alt-+ key */
#define ALT_MINUS       (KEY_MAX+0x1e4)
#define CTL_UP         (KEY_MAX+1024 + 3)
#define KEY_SUP         (KEY_MAX+1024 + 4)
#define CTL_DOWN       (KEY_MAX+1024 + 5)
#define KEY_SDOWN       (KEY_MAX+1024 + 6)
#define CTL_RIGHT      (KEY_MAX+1024 + 7)
#define CTL_LEFT       (KEY_MAX+1024 + 8)
#endif
#define ALT_SHIFT_F     143
#define ALT_SHIFT_B     177

int gCtrlCTerm = 0;

#ifdef STANDALONE
int     gLastWasCtrlC = 0;
CTui*   gpCtrlCTui = NULL;

bool PDC_check_key(void)
{
  int   ch = wgetch(gpCtrlCTui->m_pFocuswin);

  if (ch != ERR || gCtrlCTerm)
  {
    if (ch != 3)
    {
      gLastWasCtrlC = 0;
      ungetch(ch);
    }
    return 1;
  }
  return 0;
}

#ifdef STANDALONE

/*
==============================================================================
Signal handler for CTRL-C key
==============================================================================
*/
void sig_handler(int sig_no)
{
  if (sig_no == SIGINT)
  {
    if (gLastWasCtrlC > 1)
    {
      gCtrlCTerm = 1;
      gLastWasCtrlC++;
    }
    else
    {
      // Call the TUI's control C handler
      gLastWasCtrlC++;
      gpCtrlCTui->HandleCtrlC();
    }

    if (gLastWasCtrlC > 3)
    {
      endwin();
      exit(1);
    }
  }
}
#endif

#else
extern "C" bool PDC_check_key(void);
#endif

/*
==============================================================================
Class constructor
==============================================================================
*/
CTui::CTui()
{
  int   x;

  m_pWin                        = NULL;
  m_pSrc                        = NULL;
  m_pCmdwin                     = NULL;
  m_pRegwin                     = NULL;
  m_CmdLineCount                = 0;
  m_CmdLineNo                   = 0;
  m_CmdTopLine                  = 0;
  m_ScrollBackCount             = 0;
  m_CmdCurrentLine              = 0;
  m_CmdCol                      = 0;
  m_ConnectCount                = 0;
  m_ThreadStarted               = 0;
  m_SrcWinPosition              = FOCUS_LEFT;
  m_CmdWinHeight                = 0;
  m_FirstTab                    = 1;
  m_BreakpointRestoreInProgress = 0;
  m_CursorLine                  = 0;
  m_Terminate                   = 0;
  m_Terminated                  = 0;
  m_Focus                       = FOCUS_CMD;
  m_Terminated                  = 0;
  m_KeyIn                       = 0;
  m_KeyOut                      = 0;
  m_CtrlCEraseLine              = 0;
  m_KeyDebug                    = 0;
  m_CursesError                 = false;
  m_CursesInitialized           = false;
  m_Request                     = -1;
  m_pTabs                       = NULL;
  m_HistoryIndex                = -1;
  m_pTabList                    = NULL;
  m_TabCmd[0]                   = 0;
  m_CtrlRActive                 = 0;
  m_CtrlRText[0]                = 0;
  m_PasteBuf[0]                 = 0;
  m_WatchPaused                 = 0;

  for (x = 0; x < TUI_CMD_LINES; x++)
    m_CmdLines[x] = (char *) malloc(TUI_CMD_LEN);

  m_CursesLock.Open();
  m_CursesLock.Release();
  m_RequestLock.Open();
  m_RequestLock.Release();
  m_RequestTrigger.Open();
  m_CompleteTrigger.Open();
  m_KeyRequest.Open();
}

/*
==============================================================================
Class destructor
==============================================================================
*/
CTui::~CTui()
{
  int   x;

  // Delete all the tabs
  if (m_pTabs)
    delete m_pTabs; 

  for (x = 0; x < TUI_CMD_LINES; x++)
    free(m_CmdLines[x]);
}

/*
==============================================================================
Attach a TuiSource object
==============================================================================
*/
void CTui::AttachTuiSource(CTuiSource *pSrc)
{
  // Save a pointer to the source
  m_pSrc = pSrc;
  pSrc->SetParent(this);
  if (m_pTabs != NULL)
  {
    m_pTabs->AttachTuiSource(pSrc, NULL);
    m_pTabs->Redraw();
  }
}

/*
===================================================================================
Returns the x-y size of the terminal
===================================================================================
 * k
*/
void CTui::GetTermSize(int &rows, int &cols) 
{
#ifdef CONFIG_PDCURSES_MULTITHREAD
  FAR struct pdc_context_s *ctx = PDC_ctx();
#endif

#ifndef STANDALONE
  if (SP != NULL)
    {
      rows = SP->lines;
      cols = SP->cols;
    }
  else
#else
  struct winsize size;
  if (ioctl(0, TIOCGWINSZ, (char *) &size) >= 0)
    {
      rows = size.ws_row;
      cols = size.ws_col;
    }
  else
#endif
    {
      rows = 16;
      cols = 80;
    }
}

/*
==============================================================================
Initialize the pdcurses screen
==============================================================================
*/
int CTui::UIInit(void)
{
  int     rows, cols, width, h;
  int     srcX, regX;

  /* Initialize Curses */

#ifdef STANDALONE
  setlocale(LC_ALL, "");
#endif

  if ( (m_pWin = initscr()) == NULL ) 
    {
      fprintf(stderr, "Error initialising ncurses.\n");
      return 1;
    }


  nl();
  noecho();
  timeout(0);
  keypad(m_pWin, true);
  nodelay(m_pWin, TRUE);

  GetTermSize(rows, cols);

  // Initialze colors
  start_color();
  init_color(0, 0, 0, 0);
  init_color(COLOR_CYAN, 12*41, 12*41, 12*41);
  init_color(COLOR_BLUE, 5*41, 5*41, 20*41);
  init_color(SYNTAX_COLOR_REG, 10*41, 20*41, 20*41);
  init_color(COLOR_MAGENTA | 8, 1000, 647, 0);   // bright orange for
                                                 // FSM route color 5
  init_pair(SYNTAX_PAIR_NORMAL,         SYNTAX_COLOR_NORMAL,       SYNTAX_COLOR_BACKGROUND);
  init_pair(SYNTAX_PAIR_COMMENT,        SYNTAX_COLOR_COMMENT,      SYNTAX_COLOR_BACKGROUND);
  init_pair(SYNTAX_PAIR_LINENO,         SYNTAX_COLOR_LINENO,       SYNTAX_COLOR_BACKGROUND);
  init_pair(SYNTAX_PAIR_KEYWORD,        SYNTAX_COLOR_KEYWORD,      SYNTAX_COLOR_BACKGROUND);
  init_pair(SYNTAX_PAIR_REG,            SYNTAX_COLOR_REG,          SYNTAX_COLOR_BACKGROUND);
  init_pair(SYNTAX_PAIR_LABEL,          SYNTAX_COLOR_LABEL,        SYNTAX_COLOR_BACKGROUND);
  init_pair(SYNTAX_PAIR_DECLARATOR,     SYNTAX_COLOR_DECLARATOR,   SYNTAX_COLOR_BACKGROUND);
  init_pair(SYNTAX_PAIR_NOTES_VOCAL,    COLOR_YELLOW | 8,          SYNTAX_COLOR_BACKGROUND);
  init_pair(ROUTE_PAIR_BASE + 0,        COLOR_YELLOW | 8,          SYNTAX_COLOR_BACKGROUND);
  init_pair(ROUTE_PAIR_BASE + 1,        COLOR_GREEN | 8,           SYNTAX_COLOR_BACKGROUND);
  init_pair(ROUTE_PAIR_BASE + 2,        COLOR_BLUE | 8,            SYNTAX_COLOR_BACKGROUND);
  init_pair(ROUTE_PAIR_BASE + 3,        COLOR_RED | 8,             SYNTAX_COLOR_BACKGROUND);
  init_pair(ROUTE_PAIR_BASE + 4,        COLOR_MAGENTA | 8,         SYNTAX_COLOR_BACKGROUND);
#ifndef STANDALONE
  init_pair(SYNTAX_PAIR_MENU_BAR,       COLOR_WHITE,               COLOR_BLUE|8);
  init_pair(SYNTAX_PAIR_MENU_FIRSTCHAR, SYNTAX_COLOR_REG,          COLOR_BLUE|8);
#else
  init_pair(SYNTAX_PAIR_MENU_BAR,       COLOR_WHITE,               COLOR_BLUE);
  init_pair(SYNTAX_PAIR_MENU_FIRSTCHAR, SYNTAX_COLOR_REG,          COLOR_BLUE);
#endif

  // Read the UI preferences
  ReadUIPreferences();

  // Get the terminal size
  GetTermSize(rows, cols);
  m_CmdHeight = rows > 34 ? 10 : 8;
  if (m_CmdWinHeight != 0)
    m_CmdHeight = m_CmdWinHeight;
  width = cols;
  m_Lines = rows;
  m_Cols = cols;
  h = rows-m_CmdHeight-1;

  m_SourceWindowLineCount = rows-m_CmdHeight-3;
  m_RegWindowWidth = cols/5+4;
  if (m_RegWindowWidth < 20)
     m_RegWindowWidth = 20;
  if (m_RegWindowWidth > 32)
     m_RegWindowWidth = 32;


  // Set Src and Reg window position based on preferences
  if (m_SrcWinPosition == FOCUS_LEFT)
  {
     srcX = 0;
     regX = width-m_RegWindowWidth;
  }
  else
  {
     regX = 0;
     srcX = m_RegWindowWidth;
  }
  int listWidth = width-m_RegWindowWidth;

  // ===========================================================
  // Create Menu Bar
  // ===========================================================
  m_pMainMenu = subwin(m_pWin, 1, width, 0, 0);
  wbkgd(m_pMainMenu, COLOR_PAIR(SYNTAX_PAIR_MENU_BAR));
  werase(m_pMainMenu);
  wattron(m_pMainMenu, COLOR_PAIR(SYNTAX_PAIR_MENU_FIRSTCHAR));
  mvwprintw(m_pMainMenu, 0, 2, "F");
  mvwprintw(m_pMainMenu, 0, 8, "D");
  mvwprintw(m_pMainMenu, 0, 15, "H");
  wattron(m_pMainMenu, COLOR_PAIR(SYNTAX_PAIR_MENU_BAR));
  mvwprintw(m_pMainMenu, 0, 3, "ile");
  mvwprintw(m_pMainMenu, 0, 9, "ebug");
  mvwprintw(m_pMainMenu, 0, 16, "elp");
  wattroff(m_pMainMenu, COLOR_PAIR(SYNTAX_PAIR_MENU_BAR));
  wnoutrefresh(m_pMainMenu);

  // ===========================================================
  // Create Source window
  // ===========================================================
  attron(COLOR_PAIR(SYNTAX_PAIR_COMMENT));
  m_pSrcframe = subwin(m_pWin,      h,   listWidth,   1, srcX);

  // ===========================================================
  // Create a Tab Container in the source window
  // ===========================================================
  m_pTabs = new CTabContainer(this, m_pSrcframe);
  m_pTabs->AttachTuiSource(m_pSrc, NULL);
  m_pTabs->Redraw();
  m_pSrc->DrawSplash(m_pTabs->GetTabWindow());

  // ===========================================================
  // Create register / watch window
  // ===========================================================
  m_pRegframe = subwin(m_pWin, h,   m_RegWindowWidth,   1, regX);
  m_pRegwin   = subwin(m_pWin, h-2, m_RegWindowWidth-2, 2, regX+1);
  box(m_pRegframe, 0, 0);
  mvwprintw(m_pRegframe, 0, 4, "Watch");
  mvwhline(m_pRegframe, h-3, 1, 0, m_RegWindowWidth-2);
  mvwprintw(m_pRegframe, h-3, 3, " Key Debug ");
  wnoutrefresh(m_pSrcframe);
  wrefresh(m_pRegframe);
  attroff(COLOR_PAIR(SYNTAX_PAIR_COMMENT));

  // ===========================================================
  // Create command frame and window
  // ===========================================================
  m_pCmdframe = subwin(m_pWin, m_CmdHeight,   width,   rows-m_CmdHeight,   0);
  m_pCmdwin   = subwin(m_pWin, m_CmdHeight-2, width-4, rows-m_CmdHeight+1, 2);
  m_CmdHeight = m_CmdHeight-2;
  m_CmdWidth  = width-4;
  m_CmdHeightPrev = m_CmdHeight;
  box(m_pCmdframe, 0, 0);
  mvwprintw(m_pCmdframe, 0, 4, "Command");
  keypad(m_pCmdwin, true);

  m_pFocuswin = m_pCmdwin;
  nodelay(m_pCmdwin, TRUE);
  nodelay(m_pRegwin, TRUE);
  nodelay(m_pSrcframe, TRUE);

  wnoutrefresh(m_pCmdframe);
  wattrset(m_pSrcframe, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  wattrset(m_pRegwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));

  wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
  wnoutrefresh(m_pCmdwin);
  doupdate();
  m_NeedUpdate = 0;
  return OK;
}

void CTui:: RedrawScreen(void)
{
  int     h;

  h = m_Lines-m_CmdHeight-1;

  // Redraw the menu
  wbkgd(m_pMainMenu, COLOR_PAIR(SYNTAX_PAIR_MENU_BAR));
  werase(m_pMainMenu);
  wattron(m_pMainMenu, COLOR_PAIR(SYNTAX_PAIR_MENU_FIRSTCHAR));
  mvwprintw(m_pMainMenu, 0, 2, "F");
  mvwprintw(m_pMainMenu, 0, 8, "D");
  mvwprintw(m_pMainMenu, 0, 15, "H");
  wattron(m_pMainMenu, COLOR_PAIR(SYNTAX_PAIR_MENU_BAR));
  mvwprintw(m_pMainMenu, 0, 3, "ile");
  mvwprintw(m_pMainMenu, 0, 9, "ebug");
  mvwprintw(m_pMainMenu, 0, 16, "elp");
  wnoutrefresh(m_pMainMenu);
  wattroff(m_pMainMenu, COLOR_PAIR(SYNTAX_PAIR_MENU_BAR));

  // Redraw the source window
  m_pTabs->Redraw();
  // If no tabs present, then draw the splash screen; with a tab up
  // (instrument panel, notes) repaint that instead - splashing over a
  // live tab left the tab invisible after every terminal resize
  if (m_pTabs->GetActiveTab() == NULL)
    m_pSrc->DrawSplash(m_pTabs->GetTabWindow());
  else
    DrawSourceWindow();
  wnoutrefresh(m_pSrcframe);

  // Redraw the watch window
  attron(COLOR_PAIR(SYNTAX_PAIR_COMMENT));
  box(m_pRegframe, 0, 0);
  mvwprintw(m_pRegframe, 0, 4, "Watch");
  mvwhline(m_pRegframe, h-3, 1, 0, m_RegWindowWidth-2);
  mvwprintw(m_pRegframe, h-3, 3, " Key Debug ");
  wrefresh(m_pRegframe);
  attroff(COLOR_PAIR(SYNTAX_PAIR_COMMENT));

  // Draw box around command window
  werase(m_pCmdwin);
  box(m_pCmdframe, 0, 0);
  mvwprintw(m_pCmdframe, 0, 4, "Command");
  wrefresh(m_pCmdframe);
  RedrawCommandWindow();
}

/*
===================================================================================
Resize the windows
===================================================================================
*/
void CTui::ResizeWindows(void)
{
  int     h;

  m_CmdHeight += 2;
  h = m_Lines-m_CmdHeight-1;
  int listWidth = m_Cols-m_RegWindowWidth;

  // ===========================================================
  // Resize the source window
  // ===========================================================
  attron(COLOR_PAIR(SYNTAX_PAIR_COMMENT));
  wresize(m_pSrcframe, h,   listWidth);
  wclear(m_pSrcframe);
  // The tab window is a subwin of the frame: rebuild it at the new size
  // before anything draws through it (sources size their content from
  // it - the Notes tab splits its height across the four channels)
  m_pTabs->Resize();
  m_pTabs->Redraw();
  DrawSourceWindow();

  // ===========================================================
  // Resize the Reg / watch window
  // ===========================================================
  wresize(m_pRegframe, h,   m_RegWindowWidth);
  wresize(m_pRegwin,   h-2, m_RegWindowWidth-2);
  wclear(m_pRegframe);
  box(m_pRegframe, 0, 0);
  mvwprintw(m_pRegframe, 0, 4, "Watch");
  mvwhline(m_pRegframe, h-3, 1, 0, m_RegWindowWidth-2);
  mvwprintw(m_pRegframe, h-3, 3, " Key Debug ");
  wnoutrefresh(m_pSrcframe);
  wnoutrefresh(m_pRegframe);
  attroff(COLOR_PAIR(SYNTAX_PAIR_COMMENT));

  // ===========================================================
  // Resize the Command window
  // ===========================================================
  if (m_CmdHeight > m_CmdHeightPrev)
  {
    mvwin(m_pCmdframe,   m_Lines-m_CmdHeight,   0);
    mvwin(m_pCmdwin,     m_Lines-m_CmdHeight+1, 2);
    wresize(m_pCmdframe, m_CmdHeight,   m_Cols);
    wresize(m_pCmdwin,   m_CmdHeight-2, m_Cols-4);
  }
  else
  {
    wresize(m_pCmdframe, m_CmdHeight,   m_Cols);
    wresize(m_pCmdwin,   m_CmdHeight-2, m_Cols-4);
    mvwin(m_pCmdframe,   m_Lines-m_CmdHeight,   0);
    mvwin(m_pCmdwin,     m_Lines-m_CmdHeight+1, 2);
  }
  m_CmdHeightPrev = m_CmdHeight;
  wclear(m_pCmdframe);
  box(m_pCmdframe, 0, 0);
  mvwprintw(m_pCmdframe, 0, 4, "Command");
  wnoutrefresh(m_pCmdframe);

  // Refresh command window text
  RedrawCommandWindow();

  doupdate();
  m_NeedUpdate = 0;
  m_CmdHeight -= 2;
}

/*
===================================================================================
Redraw the Source window frame in case the filename changed.
===================================================================================
*/
void CTui::RedrawSourceFrame(void)
{
  return;
  if (m_Focus != FOCUS_SRC)
    wattron(m_pSrcframe, COLOR_PAIR(SYNTAX_PAIR_COMMENT));

  box(m_pSrcframe, 0, 0);
  if (m_pSrc)
    strcpy(m_SourceFilename, m_pSrc->GetFilename());
  mvwprintw(m_pSrcframe, 0, 12, m_SourceFilename);
  wnoutrefresh(m_pSrcframe);
  wattroff(m_pSrcframe, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
  m_NeedUpdate = 1;
}

/*
===================================================================================
Read the UI preferences from ~/.p8simrc
===================================================================================
*/
void CTui::ReadUIPreferences(void)
{
  FILE     *fd;
  char     line[128];
  char     *pref, *value, *savetok;

  if (m_pSrc == NULL)
    return;

  // Try to open the p8sim resource file
  if ((fd = fopen((const char *) m_pSrc->GetPrefsFilename(), "r")) == NULL)
    return;

  while (fgets(line, sizeof(line), fd) != NULL)
  {
    // Get preference name
    pref = strtok_r(line, "= \n", &savetok);
    if (pref == NULL)
      continue;

    // Get preference value
    value = strtok_r(NULL, "\n", &savetok);
    if (value == NULL)
      continue;
    
    // Set the preference
    if (strcmp("SRC_POS", pref) == 0)
      m_SrcWinPosition = atoi(value);

    // Test for command window height
    else if (strcmp("CMD_HEIGHT", pref) == 0)
      m_CmdWinHeight = atoi(value);

    else if (strcmp("WATCH", pref) == 0)
      m_pSrc->RestoreWatchItem(value);

    else
      m_pSrc->RestoreOtherPref(pref, value);

    /*
    else if (strcmp("BREAK", pref) == 0)
    {
      m_SavedBreakpoints.Add(value);
    }
    */
  }

  // Close the file
  fclose(fd);

  // Read the history
  m_History.ReadHistory((const char *) m_pSrc->GetPrefsFilename());
}

/*
===================================================================================
Save the UI preferences to /mnt/.tui_prefs
===================================================================================
*/
void CTui::WriteUIPreferences(void)
{
  FILE     *fd;

  if (m_pSrc == NULL)
    return;

  // Try to open the p8sim resource file
  if ((fd = fopen((const char *) m_pSrc->GetPrefsFilename(), "w+")) == NULL)
  {
    return;
  }

  // Save Source window position
  fprintf(fd, "SRC_POS=%d\n", m_SrcWinPosition);
  fprintf(fd, "CMD_HEIGHT=%d\n", m_CmdHeight + 2);

  // Save all watch variables
  m_pSrc->SaveWatchItems(fd);

  /*
  count = m_WatchList.GetSize();
  for (x = 0; x < count; x++)
    fprintf(fd, "WATCH=%s\n", (const char *) m_WatchList[x]);

  // Save all breakpoints
  count = m_pSim->GetLineCount();
  for (x = 0; x < count; x++)
  {
    const char  *pLine;

    // Test for break at this lineNo
    if (m_pSim->IsBreakAtLine(x))
    { 
      pLine = m_pSim->GetLineByNumber(x);
      fprintf(fd, "BREAK=%d:%s\n", x, pLine);
    }
  }
  */

  // Close the file
  fclose(fd);

  // Save the history in the preferences file
  m_History.SaveHistory((const char *) m_pSrc->GetPrefsFilename());
}

/*
===================================================================================
Get next key from the TUI thread
===================================================================================
*/
#ifdef STANDALONE
int CTui::MapKey(int key)
{
  /* Test for DEL */
  if (key == 127)
    return 8;

  /* Test for ESC */
  if (key == 27)
  {
    /* Get the next byte in the ESC sequence */
    key = wgetch(m_pCmdwin);
    if (key == '[')
    {
      /* Have to perform mapping */
      key = wgetch(m_pCmdwin);
      if (key == 'A')
        key = KEY_UP;
      else if (key == 'B')
        key = KEY_DOWN;
      else if (key == 'C')
        key = KEY_RIGHT;
      else if (key == 'D')
        key = KEY_LEFT;

      /* Test for shift or ctrl Up/down */
      if (key == '1')
      {
        int  ctrl_or_shift;

        /* Get ';' */
        key = wgetch(m_pCmdwin);

        if (key == '~')
        {
          key = KEY_HOME;
        }
        else
        {
          ctrl_or_shift = wgetch(m_pCmdwin);
          key = wgetch(m_pCmdwin);
          if (key == 'A')
            key = ctrl_or_shift == '2' ? KEY_SUP : CTL_UP;
          else if (key == 'B')
            key = ctrl_or_shift == '2' ? KEY_SDOWN : CTL_DOWN;
          else if (key == 'C')
            key = ctrl_or_shift == '2' ? KEY_SRIGHT : CTL_RIGHT;
          else if (key == 'D')
            key = ctrl_or_shift == '2' ? KEY_SLEFT : CTL_LEFT;
        }
      }
      else if (key == '4')
      {
        // Read the '~' character from the buffer
        key = wgetch(m_pCmdwin);
        key = KEY_END;
      }
      else if (key == '5')
      {
        // Read the '~' character from the buffer
        key = wgetch(m_pCmdwin);
        key = KEY_NPAGE;
      }
      else if (key == '6')
      {
        // Read the '~' character from the buffer
        key = wgetch(m_pCmdwin);
        key = KEY_PPAGE;
      }
    }

    else if (key >= 'a' && key <= 'z')
    {
      key = ALT_A + key - 'a';
    }
    else if (key >= '0' && key <= '9')
    {
      key = ALT_0 + key - '0';
    }
    else if (key == '+')
      key = ALT_PLUS;
    else if (key == '-')
      key = ALT_MINUS;
    else if (key == -1)
      return 27;
  }

  // Test for CTRL-e
  else if (key == 5)
    key = KEY_END;

  // Test for CTRL-a
  else if (key == 1)
    key = KEY_HOME;

  // Test For OSX CTL_UP
  else if (key == 0x235)
    key = CTL_UP;

  // Test for OSX CTL_DOWN
  else if (key == 0x20C)
    key = CTL_DOWN;

  // Test for OSX SHIFT_UP
  else if (key == 0x151)
    key = KEY_SUP;

  // Test for OSX SHIFT_DOWN
  else if (key == 0x150)
    key = KEY_SDOWN;

  return key;
}
#endif

/*
===================================================================================
Get next key from the TUI thread
===================================================================================
*/
int CTui::ProcessInput(char* pBuffer, int &index, int maxSize)
{
  int    key;
  char   ch;
  int    ret;
  int    cmdLen;
  char   str[48];

  if (m_NeedUpdate)
  {
    doupdate();
    m_NeedUpdate = 0;
  }
  // Get key from the UI thread 
  key = wgetch(m_pFocuswin);
  if (key == -1)
    return -1;

  cmdLen = strlen(pBuffer);

#ifdef STANDALONE
  if (key == KEY_BACKSPACE)
    key = 8;
  key = MapKey(key);

  if (key != 3)
  {
    gLastWasCtrlC = 0;
  }
#endif

  // Test if we were in scrollback mode and this isn't CTRL_D or CTRL_U
  if (m_ScrollBackCount && (key > 26 || key == '\n'))
  {
    m_ScrollBackCount = 0;
    RedrawCommandWindow();
    doupdate();
    m_NeedUpdate = 0;
  }

  if (m_KeyDebug)
  {
    sprintf(str, "Key = %d (0x%X)", key, key);
    UIWindowPrintString(m_pRegwin, m_SourceWindowLineCount-1, 1, str);
    wnoutrefresh(m_pRegwin);
    wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
    doupdate();
  }

  // WantKeys: when the source window has focus and the active tab's
  // source asked for raw keys (the instrument panel), it gets first
  // pick - ahead of the special-mode scroll keys, so the arrows reach
  // it.  The global navigation keys stay with the framework: CTRL-W
  // (window focus), CTRL-T (next tab), CTRL-C.  A key the source
  // returns 0 for falls through to the normal handling below.
  if (m_Focus == FOCUS_SRC && m_pSrc && m_pSrc->WantProcessKey() &&
      key != CTRL_W && key != CTRL_T && key != 3)
  {
    if (m_pSrc->ProcessKey(key))
    {
      SetSourceFocus();
      return -1;
    }
  }

  // Test if key processed as special mode key
  if ((ret = ProcessAsSpecialModeKey(key, pBuffer, maxSize, index)))
  {
    if (m_Terminate)
      return -1;

    if (ret != 2)
      index = strlen(pBuffer);
    return -1;
  }

  // Test if key is ASCII
  if (key <= 0x7F)
  {
    ch = key;

    // Test if CTRL-R mode active
    if (m_CtrlRActive)
    {
      ret = ProcessCtrlRKey(key, pBuffer, index, maxSize);
      if (ret == OK)
        strcat (m_CmdLines[m_CmdCurrentLine-1], pBuffer);
      return ret;
    }

    // Process key normally
    if (ch == 8)
    {
      if (index > 0)
      {
        // Perform backspace ... delete previous character
        if (index == cmdLen)
        {
           index--;
           m_CmdCol--;
           pBuffer[index] = 0;
           wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
           mvwaddch(m_pCmdwin, m_CursorLine, m_CmdCol, ' ');
           wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
           wrefresh(m_pCmdwin);
        }
        else
        {
           index--;
           m_CmdCol--;
           memmove(&pBuffer[index], &pBuffer[index+1], cmdLen-index);
           cmdLen--;
           pBuffer[cmdLen] = 0;
           wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
           wclrtoeol(m_pCmdwin);
           PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, &pBuffer[index]);
           wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
           wrefresh(m_pCmdwin);
        }
      }
    }
    else if (ch == '\n')
    {
      // Terminate the string and return so command can be processd
      strcat (m_CmdLines[m_CmdCurrentLine-1], pBuffer);
      if (m_pTabList)
      {
        m_pSrc->FreeTabList(m_pTabList);
        m_pTabList = NULL;
      }
      return OK;
    }
    else
    {
      // Add character to our command buffer
      if (index == cmdLen)
      {
         pBuffer[index++] = ch;
         pBuffer[index] = 0;
         wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
         wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
         mvwaddch(m_pCmdwin, m_CursorLine, m_CmdCol, ch);
         wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
         wrefresh(m_pCmdwin);
      }
      else
      {
         memmove(&pBuffer[index+1], &pBuffer[index], cmdLen-index);
         pBuffer[index] = ch;
         wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
         wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
         PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, &pBuffer[index]);
         wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
         wmove(m_pCmdwin, m_CursorLine, m_CmdCol+1);
         wrefresh(m_pCmdwin);
         index++;
      }
      m_CmdCol++;
    }
  }
  else if (key == KEY_LEFT && m_Focus == FOCUS_CMD)
  {
    /* Test if we are in r-search mode */
    if (m_CtrlRActive && m_CtrlRHistIndex != -1)
    {
      const char *pCmd;
      m_HistoryIndex = m_History.GetHistoryItem(m_CtrlRHistIndex, &pCmd);;
      strcpy(m_CmdLines[m_CmdCurrentLine-1], m_pPrompt);
      pBuffer[strlen(pBuffer)+1] = 0;
      m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
      PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_pPrompt);
      wclrtoeol(m_pCmdwin);

      PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pCmd);
      memset(m_CmdBuf, 0, sizeof(m_CmdBuf));
      strcpy(pBuffer, pCmd);
      m_CmdCol += strlen(pCmd);
      m_CmdCol--;
      index = strlen(pCmd)-1;
      wmove(m_pCmdwin, m_CursorLine, m_CmdCol);

      m_CtrlRActive = 0;
    }
    else if (index > 0)
    {
      index--;
      m_CmdCol--;
      wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
      wrefresh(m_pCmdwin);
    }
  }
  else if (key == KEY_RIGHT && m_Focus == FOCUS_CMD)
  {
    /* Test if we are in r-search mode */
    if (m_CtrlRActive && m_CtrlRHistIndex != -1)
    {
      const char *pCmd;
      m_HistoryIndex = m_History.GetHistoryItem(m_CtrlRHistIndex, &pCmd);
      strcpy(m_CmdLines[m_CmdCurrentLine-1], m_pPrompt);
      m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
      PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_pPrompt);
      wclrtoeol(m_pCmdwin);

      PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pCmd);
      memset(m_CmdBuf, 0, sizeof(m_CmdBuf));
      strcpy(pBuffer, pCmd);
      m_CmdCol += 1;
      index = 1;
      wmove(m_pCmdwin, m_CursorLine, m_CmdCol);

      m_CtrlRActive = 0;
    }
    else if (pBuffer[index] != 0)
    {
      index++;
      m_CmdCol++;
      wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
      wrefresh(m_pCmdwin);
    }
  }
  else if (key == KEY_HOME)
  {
    if (m_Focus == FOCUS_CMD)
    {
      /* Test if we are in r-search mode */
      if (m_CtrlRActive && m_CtrlRHistIndex != -1)
      {
        const char *pCmd;
        m_HistoryIndex = m_History.GetHistoryItem(m_CtrlRHistIndex, &pCmd);
        strcpy(m_CmdLines[m_CmdCurrentLine-1], m_pPrompt);
        m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
        PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_pPrompt);
        wclrtoeol(m_pCmdwin);

        memset(m_CmdBuf, 0, sizeof(m_CmdBuf));
        strcpy(pBuffer, pCmd);
        PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pCmd);
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        wrefresh(m_pCmdwin);
        index = 0;
      }
      else
      {
        index = 0;
        m_CmdCol = CalculateCmdCol(m_pPrompt);
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        wrefresh(m_pCmdwin);
      }
    }
    else
    {
      UISetSourceTopLineNo(1);
    }
  }
  else if (key == KEY_END)
  {
    if (m_Focus == FOCUS_CMD)
    {
      /* Test if we are in r-search mode */
      if (m_CtrlRActive && m_CtrlRHistIndex != -1)
      {
        const char *pCmd;
        m_HistoryIndex = m_History.GetHistoryItem(m_CtrlRHistIndex, &pCmd);
        strcpy(m_CmdLines[m_CmdCurrentLine-1], m_pPrompt);
        m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
        PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_pPrompt);
        wclrtoeol(m_pCmdwin);

        memset(m_CmdBuf, 0, sizeof(m_CmdBuf));
        strcpy(pBuffer, pCmd);
        PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pCmd);
        index = strlen(pBuffer);
        m_CmdCol = CalculateCmdCol(m_pPrompt) + index;
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        wrefresh(m_pCmdwin);
      }
      else
      {
        index = strlen(m_CmdBuf);
        m_CmdCol = CalculateCmdCol(m_pPrompt) + index;
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        wrefresh(m_pCmdwin);
      }
      return 1;
    }
    else
    {
      UISetSourceTopLineNo(0xfffffff);
      return 1;
    }
  }
  else
  {
    sprintf(str, "Key = %d (0x%X)", key, key);
    UIWindowPrintString(m_pRegwin, m_SourceWindowLineCount-1, 1, str);
  }

  return -1;
}

/*
===================================================================================
Draw a screen full of lines starting with lineNo at the top
===================================================================================
*/
int CTui::SetSourceFocus(void) 
{
  CTab    *pTab;
  WINDOW  *win;
  int     topLine;
  int     lineCount;
  void    *pSrcCtx;
  CTuiSource *pTuiSrc;

  pTab = m_pTabs->GetActiveTab();
  if (pTab != NULL)
  {
    pTuiSrc   = pTab->GetTuiSource();
    if (pTuiSrc && pTuiSrc->WantFocus())
    {
      // Get the tab Panel
      win       = pTab->GetWindow();
      topLine   = pTab->SourceFirstLine();
      lineCount = pTab->SourceWindowLineCount();
      pSrcCtx   = pTab->SourceContext();

      // Turn the cursor off for screen draw 
      if (pTuiSrc)
      {
        pTuiSrc->SetFocus(pSrcCtx, win, topLine, lineCount);
        wrefresh(win);
        return 1;
      }
    }
  }

  return 0;
}

/*
===================================================================================
Draw a screen full of lines starting with lineNo at the top
===================================================================================
*/
// Terminal rectangle of the command window - the pack tab's playback
// waveform draws there (and Ctrl-W back to the command window restores
// the prompt over it).
void CTui::CmdWindowRect(int rect[4])
{
  int by, bx, wy, wx;

  rect[0] = rect[1] = rect[2] = rect[3] = -1;
  if (m_pCmdwin == NULL)
    return;
  getbegyx(m_pCmdwin, by, bx);
  getmaxyx(m_pCmdwin, wy, wx);
  rect[0] = by;
  rect[1] = by + wy - 1;
  rect[2] = bx;
  rect[3] = bx + wx - 1;
}

// Terminal rectangle (0-based, inclusive) of the active tab's window,
// for code that draws into the tab behind curses' back (play -w).
void CTui::ActiveTabRect(int rect[4])
{
  CTab *pTab = m_pTabs->GetActiveTab();

  rect[0] = rect[1] = rect[2] = rect[3] = -1;
  if (pTab == NULL)
    return;

  WINDOW *win = pTab->GetWindow();
  int by, bx, wy, wx;

  if (win == NULL)
    return;
  getbegyx(win, by, bx);
  getmaxyx(win, wy, wx);
  rect[0] = by;
  rect[1] = by + wy - 1;
  rect[2] = bx;
  rect[3] = bx + wx - 1;
}

// Pan the active listing tab horizontally: shift the physical
// screen cells sideways (PDC_hscroll_region mirrors the shift in
// pdc_lastscr), then draw normally - the refresh diff transmits
// only the newly exposed columns.
void CTui::HScrollActive(int dir) 
{
  CTab    *pTab;
  WINDOW  *win;
  int     topLine;
  int     lineCount;
  void    *pSrcCtx;

  // Get the active tab
  curs_set(0);
  pTab = m_pTabs->GetActiveTab();
  if (pTab != NULL)
  {
    // Get the tab Panel
    win       = pTab->GetWindow();
    topLine   = pTab->SourceFirstLine();
    lineCount = pTab->SourceWindowLineCount();
    pSrcCtx   = pTab->SourceContext();

    // Pan, shift the physical cells to match, then redraw normally -
    // the refresh diff transmits only the newly exposed columns
    if (pTab->GetTuiSource())
    {
      int tcol = pTab->GetTuiSource()->HScrollSource(pSrcCtx, dir);

      if (tcol >= 0)
      {
        int by, bx, wy, wx;

        getbegyx(win, by, bx);
        getmaxyx(win, wy, wx);
        int clip = pTab->GetTuiSource()->HScrollClip(pSrcCtx);

        PDC_hscroll_region(by, by + wy - 1, bx + tcol,
                           bx + wx - 1 - clip, dir * 8);
        pTab->GetTuiSource()->DrawSourceWindow(pSrcCtx, win, topLine,
                                               lineCount);
        wrefresh(win);
      }
    }
  }
  else
  {
    m_pSrc->DrawSplash(m_pTabs->GetTabWindow());
  }

  // Turn the cursor back on
  wmove(m_pCmdwin, m_CursorLine, m_CmdCol);

  UpdateWatchWindows();
  curs_set(1);
}

void CTui::DrawSourceWindow(void) 
{
  CTab    *pTab;
  WINDOW  *win;
  int     topLine;
  int     lineCount;
  void    *pSrcCtx;

  // Get the active tab
  curs_set(0);
  pTab = m_pTabs->GetActiveTab();
  if (pTab != NULL)
  {
    // Get the tab Panel
    win       = pTab->GetWindow();
    topLine   = pTab->SourceFirstLine();
    lineCount = pTab->SourceWindowLineCount();
    pSrcCtx   = pTab->SourceContext();

    // Turn the cursor off for screen draw 
    if (pTab->GetTuiSource())
      pTab->GetTuiSource()->DrawSourceWindow(pSrcCtx, win, topLine, lineCount);
    wrefresh(win);
  }
  else
  {
    m_pSrc->DrawSplash(m_pTabs->GetTabWindow());
  }

  // Turn the cursor back on
  wmove(m_pCmdwin, m_CursorLine, m_CmdCol);

  UpdateWatchWindows();
  curs_set(1);
}

/*
===================================================================================
Print string to the command window.  

This is a UI only thread!
===================================================================================
*/
void CTui::UICommandPrintFormat(const char* fmt, va_list args)
{
  int     x;
  char    *temp;

  // Test if we need to scroll the history buffer
  if (m_CmdLineCount == TUI_CMD_LINES)
  {
    // Just copy them
    temp = m_CmdLines[0];
    for (x = 0; x < m_CmdLineCount-1; x++)
      m_CmdLines[x] = m_CmdLines[x+1];
    m_CmdLines[x] = temp;

    vsnprintf(m_CmdLines[x], TUI_CMD_LEN, fmt, args);
    m_CmdCurrentLine = m_CmdLineCount;
  }
  else
  {
    vsnprintf(m_CmdLines[m_CmdCurrentLine], TUI_CMD_LEN, fmt, args);
    m_CmdCurrentLine++;
    m_CmdLineCount++;
  }

  m_CmdPrevLine = m_CmdCurrentLine - 1;
  CommandProcessLastLine();

  if (m_CmdLineCount >= m_CmdHeight)
    m_CmdTopLine = m_CmdCurrentLine - m_CmdHeight;

  RedrawCommandWindow();
  doupdate();
  m_NeedUpdate = 0;
}

/*
===================================================================================
Print string to the command window.  

This is a UI only thread!
===================================================================================
*/
void CTui::UICommandPrintFormat(const char* fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  UICommandPrintFormat(fmt, args);
  va_end(args);
}

/*
===================================================================================
Calculate the m_CmdCol value based on embedded color escape sequences
===================================================================================
*/
int CTui::CalculateCmdCol(const char *pBuf, bool expandColor, int maxLen)
{
  int     len = 0;
  int     x;

  if (maxLen < 0)
    maxLen = strlen(pBuf);

  len = 0;

  for (x = 0; x < maxLen; x++)
  {
    // Test for embedded color code
    if (expandColor && pBuf[x] == '\\' && pBuf[x+1] == 'c' && isdigit(pBuf[x+2]))
    {
      // Skip the color code
      x += 2;
      continue;
    }

    len++;
  }

  return len;
}

/*
===================================================================================
Process the last line, handling special characters
===================================================================================
*/
void CTui::CommandProcessLastLine(void)
{
  char buf[TUI_CMD_LEN];
  char *temp;
  int in_len;
  int max_i = 0;
  int out_i = 0;
  int i;

  // Handle '\r'
  // TODO: Handle lines with embedded \r\n followed by more text
  temp = m_CmdLines[m_CmdPrevLine];
  in_len = strlen(temp);
  if (strchr(temp, '\r') != NULL)
  {
    memcpy(buf, temp, in_len + 1);

    for (i = 0; i < in_len; ++i)
    {
      temp[out_i++] = buf[i];
      if (buf[i] == '\r')
      {
        if (out_i > max_i)
          max_i = out_i;
        out_i = 0;
      }
    }
    if (out_i > max_i)
      max_i = out_i;

    temp[max_i] = 0;
    m_CmdCol = CalculateCmdCol(temp, true, out_i);
  }
  else
  {
    m_CmdCol = CalculateCmdCol(temp);
  }
}

/*
===================================================================================
Print string to the command window.  

This is a UI only thread!
===================================================================================
*/
void CTui::UICommandPrintString(const char* buffer)
{
  int    x;
  char  *temp;

  // Test if we need to scroll the history buffer
  if (m_CmdLineCount == TUI_CMD_LINES)
  {
    temp = m_CmdLines[0];
    for (x = 0; x < m_CmdLineCount-1; x++)
      m_CmdLines[x] = m_CmdLines[x+1];
    m_CmdLines[x] = temp;

    // Just copy them
    strncpy(m_CmdLines[x], buffer, TUI_CMD_LEN);
    m_CmdCurrentLine = m_CmdLineCount;
  }
  else
  {
    strncpy(m_CmdLines[m_CmdCurrentLine++], buffer, TUI_CMD_LEN);
    m_CmdLineCount++;
  }

  m_CmdPrevLine = m_CmdCurrentLine - 1;
  CommandProcessLastLine();

  // Add new line at current line
  if (m_CmdLineCount >= m_CmdHeight)
    m_CmdTopLine = m_CmdCurrentLine - m_CmdHeight;

  RedrawCommandWindow();
  doupdate();
  m_NeedUpdate = 0;
}

/*
===================================================================================
Append string to the command window last line.  

This is a UI only thread!
===================================================================================
*/
void CTui::UICommandAppendString(const char* buffer)
{
  int prevlen = strlen(m_CmdLines[m_CmdPrevLine]);

  snprintf(m_CmdLines[m_CmdPrevLine] + prevlen, TUI_CMD_LEN - prevlen, "%s", buffer);
  CommandProcessLastLine();

  RedrawCommandWindow();
  doupdate();
  m_NeedUpdate = 0;
}

/*
===================================================================================
Redraw the Command window and frame
===================================================================================
*/
void CTui::RedrawCommandWindow(bool debugPrint)
{
  int     x;
  char   *ptr;
  CTab    *pTab;
  WINDOW  *win;
  int     topLine;
  int     lineCount;
  void    *pSrcCtx;
  CTuiSource *pTuiSrc;

  // Reprint the display in case of scroll
  for (x = 0; x < m_CmdHeight && m_CmdTopLine - m_ScrollBackCount + x <
      m_CmdLineCount; x++)
  {
    // Move to the line
    wmove(m_pCmdwin, x, 0);
    wclrtoeol(m_pCmdwin);
    ptr = m_CmdLines[m_CmdTopLine - m_ScrollBackCount + x];
    while (*ptr == '\n')
      ptr++;
    wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
    PrintFormatStr(m_pCmdwin, x, 0, ptr);
    if (debugPrint)
      m_pSrc->DebugPrintf("%s\n", ptr);
    wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  }
  m_CursorLine = x-1;

  wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
  wnoutrefresh(m_pCmdwin);
  doupdate();
  m_NeedUpdate = 0;

  // Test for source window focus
  if (m_Focus == FOCUS_SRC)
  {
    pTab = m_pTabs->GetActiveTab();
    if (pTab != NULL)
    {
      pTuiSrc   = pTab->GetTuiSource();
      if (pTuiSrc && pTuiSrc->WantFocus())
      {
        // Get the tab Panel
        win       = pTab->GetWindow();
        topLine   = pTab->SourceFirstLine();
        lineCount = pTab->SourceWindowLineCount();
        pSrcCtx   = pTab->SourceContext();

        // Turn the cursor off for screen draw 
        if (pTuiSrc)
          pTuiSrc->SetFocus(pSrcCtx, win, topLine, lineCount);
        wrefresh(win);
      }
    }
  }
}

/*
===================================================================================
Display the source file such that the specified line is visible
===================================================================================
*/
int CTui::DisplaySourceAtLine(int lineNo) 
{

  // Issue the request
  //m_pTuiThread->RequestDisplaySourceAtLine(lineNo);

  return 0;
}

/*
===================================================================================
Display the source file such that the specified line is visible
===================================================================================
*/
void CTui::UIDisplaySourceAtLine(int lineNo) 
{
  int      sourceLines;
  int      windowLines;
  int      topLine;
  CTab  *  pTab;

  // Get the active tab
  pTab = m_pTabs->GetActiveTab();
  if (pTab == NULL)
    return;

  // Test if the line is already visible
  if (lineNo < 1)
  {
    topLine = 1;
  }
  else
  {
    // Get the window rows and cols
    windowLines = pTab->SourceWindowLineCount();

    if (pTab->SourceFirstLine() <= lineNo && lineNo < 
        pTab->SourceFirstLine() + windowLines)
    {
      topLine = pTab->SourceFirstLine();
    }
    else
    {
      // Get the line count from the simulation so we
      sourceLines = pTab->GetSourceLineCount();
      if (sourceLines - lineNo < windowLines/2)
        topLine = sourceLines - windowLines;
      else
        topLine = lineNo - windowLines/3;

      if (topLine < 0)              // top line is 0-based (see
        topLine = 0;                // UISetSourceTopLineNo)
    }
  }

  pTab->SourceFirstLine(topLine);
  DrawSourceWindow();
  
//  UpdateWatchWindows();

  wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
  m_NeedUpdate = 1;
}

/*
===================================================================================
Set the top line of the displayed source file
===================================================================================
*/
int CTui::SetSourceTopLineNo(int lineNo) 
{
  // Issue the request from the UI thread
  RequestSetSourceTopLineNo(lineNo);

  return 0;
}

/*
===================================================================================
Set the top line of the displayed source file
===================================================================================
*/
int CTui::UpdateWatchWindows(void) 
{
  CTab *pTab;
  int   rows;
  int   cols;
  int   row;
  int   col;

  pTab = m_pTabs->GetActiveTab();

  /* TinyQV port: the watch window shows live peripheral state, so update
   * it from the attached source even when no source tab is open (the
   * original only refreshed watches for an active tab's source).
   */

  if (pTab == NULL && m_pSrc != NULL && !m_WatchPaused)
  {
    m_pSrc->DrawWatchWindow(m_pRegwin, 0);
    wnoutrefresh(m_pRegwin);
    doupdate();
    return OK;
  }

  if (pTab && pTab->GetTuiSource())
  {
    // Test if the watch window is paused
    if (m_WatchPaused)
    {
      // Draw a "-- PAUSED -- " message
      getmaxyx(m_pRegwin, rows, cols);
      col = cols/2 - 9;
      row = (rows-2)/2-5;
      if (row < 1)
        row = 3;
      mvwprintw(m_pRegwin, row,   col, "+----------------+");
      mvwprintw(m_pRegwin, row+1, col, "|                |");
      mvwprintw(m_pRegwin, row+2, col, "|  -- PAUSED --  |");
      mvwprintw(m_pRegwin, row+3, col, "|                |");
      mvwprintw(m_pRegwin, row+4, col, "+----------------+");
      wnoutrefresh(m_pRegwin);
    }
    else
      pTab->GetTuiSource()->DrawWatchWindow(m_pRegwin, 0);
  }
  m_NeedUpdate = 1;
  return 0;
}

/*
===================================================================================
Change the focused window to the next window
===================================================================================
*/
void CTui::NextWindowFocus(void) 
{
  int   sourceFocus = 0;
  static const uint8_t focus_dir[2][3] = 
  {
    { 1, 2, 0 },
    { 2, 0, 1 }
  };

  m_CursesLock.Acquire();
  curs_set(0);

  // Update the border color for the current window
  switch (m_Focus)
  {
    case FOCUS_CMD:
      wattrset(m_pCmdframe, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
      box(m_pCmdframe, 0, 0);
      mvwprintw(m_pCmdframe, 0, 4, "Command");
      wnoutrefresh(m_pCmdframe);
      m_NeedUpdate = 1;
      break;

    case FOCUS_SRC:
      m_pTabs->SetFocus(0);
      m_pTabs->Redraw();
      DrawSourceWindow();
      break;

    case FOCUS_REG:
      wattrset(m_pRegframe, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
      box(m_pRegframe, 0, 0);
      mvwprintw(m_pRegframe, 0, 4, "Watch");
      wnoutrefresh(m_pRegframe);
      m_NeedUpdate = 1;
      break;
  }

  // Advance to next window
  m_Focus = m_SrcWinPosition == FOCUS_LEFT ? focus_dir[0][m_Focus] : focus_dir[1][m_Focus];

  // Update the border color for the current window
  switch (m_Focus)
  {
    case FOCUS_CMD:
      wattrset(m_pCmdframe, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
      box(m_pCmdframe, 0, 0);
      mvwprintw(m_pCmdframe, 0, 4, "Command");
      wnoutrefresh(m_pCmdframe);

      // A pack-play waveform may be parked over the command window;
      // it was drawn behind curses' back, so both screen models still
      // believe the prompt is on screen and a plain redraw would emit
      // nothing.  Poison the region: the doupdate below repaints it.
      {
        int by, bx, wy, wx;

        getbegyx(m_pCmdwin, by, bx);
        getmaxyx(m_pCmdwin, wy, wx);
        PDC_touch_region(by, by + wy - 1, bx, bx + wx - 1);
      }
      m_NeedUpdate = 1;
      m_pFocuswin = m_pCmdwin;
      break;

    case FOCUS_SRC:
      m_pTabs->SetFocus(1);
      m_pTabs->Redraw();
      DrawSourceWindow();
      sourceFocus = SetSourceFocus();
      if (m_pTabs->GetActiveTab() == NULL)
        m_pFocuswin = m_pSrcframe;
      else
        m_pFocuswin = m_pTabs->GetActiveTab()->GetWindow();
      break;

    case FOCUS_REG:
      wattrset(m_pRegframe, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
      box(m_pRegframe, 0, 0);
      mvwprintw(m_pRegframe, 0, 4, "Watch");
      wnoutrefresh(m_pRegframe);
      m_NeedUpdate = 1;
      m_pFocuswin = m_pCmdwin;
      break;
  }

  if (!sourceFocus)
  {
    curs_set(1);
    wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
    wnoutrefresh(m_pCmdwin);
    m_NeedUpdate = 1;
  }
  else
    m_NeedUpdate = 0;

  doupdate();
  curs_set(1);
  m_CursesLock.Release();
}

/*
===================================================================================
Set the top line of the displayed source file

This is a UI thread only function.
===================================================================================
*/
void CTui::UISetSourceTopLineNo(int lineNo) 
{
  int      sourceLines;
  int      topLine;
  CTab   * pTab = m_pTabs->GetActiveTab();

  if (pTab == NULL)
    return;

  // The top line is a 0-BASED index: tabs start with m_TopLine = 0 and
  // every source draws starting at that index.  The old clamps here
  // floored at 1, so any page down / page up round trip came back with
  // the SECOND line at the top of the window.
  sourceLines = pTab->GetSourceLineCount();
  topLine = lineNo;
  if (topLine > sourceLines - pTab->SourceWindowLineCount())
    topLine = sourceLines - pTab->SourceWindowLineCount();
  if (topLine < 0)
    topLine = 0;

  // Set the top line and redraw
  pTab->SourceFirstLine(topLine);
  curs_set(0);
  DrawSourceWindow();
  wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
  curs_set(1);
}

/*
===================================================================================
Test for processing of special mode keys.  Perform the special mode processing
and return TRUE if processed, otherwise FALSE.
===================================================================================
*/
int CTui::ProcessAsSpecialModeKey(int key, char *pBuffer, int maxSize, int &index)
{
  int        topLine;
  int        len;
  int        pasteLen;
  CTab   *   pTab;
  const char *pCmd;

  if (key != KEY_TAB)
    m_FirstTab = TRUE;

  // Get the active source tab
  pTab = m_pTabs->GetActiveTab();

  // Process base on known keys
  switch (key)
  {
    case ALT_X:
      m_Terminate = 1;
      *pBuffer = 0;
      return 1;

    case ALT_PLUS:
      m_CmdHeight--;
      ResizeWindows();
      *pBuffer = 0;
      return 1;

    case ALT_MINUS:
      m_CmdHeight++;
      ResizeWindows();
      *pBuffer = 0;
      return 1;

    /* Horizontal scrolling for sources wider than the window (the MIDI
     * tab's score).  Like the page keys these work with the command
     * window focused, so the view can be driven while typing commands.
     * ALT-L / ALT-R: SHIFT-arrows already move the window focus and
     * CTRL-arrows already switch tabs, and on a Mac the letters are the
     * comfortable chord - they arrive either as ESC l / ESC r (Option
     * as Meta) or as the Option characters, both of which the decoder
     * maps to ALT_L / ALT_R.  The ALT-arrows stay as an alias for
     * keyboards that send them.  HOME snaps back to the beginning.
     */
    case ALT_L:
    case ALT_R:
    case ALT_LEFT:
    case ALT_RIGHT:
    case KEY_HOME:
      if (pTab != NULL && pTab->GetTuiSource() != NULL)
      {
        bool page = true;
        int  dir  = (key == ALT_R || key == ALT_RIGHT) ? 1 :
                    (key == KEY_HOME) ? 0 : -1;

        if (pTab->GetTuiSource()->ScrollSource(dir, page))
        {
          curs_set(0);
          DrawSourceWindow();
          wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
          curs_set(1);
        }
      }
      return 1;

    case CTRL_F:
    case KEY_NPAGE:
      // Source Window page down
      if (pTab != NULL)
      {
        topLine = pTab->SourceFirstLine() +
          pTab->SourceWindowLineCount() - 3;
        UISetSourceTopLineNo(topLine);
      } 
      return 1;

    case CTRL_B:
    case KEY_PPAGE:
      if (pTab != NULL)
      {
        topLine = pTab->SourceFirstLine() -
          (pTab->SourceWindowLineCount() - 3);
        if (topLine < 0)            // top line is 0-based (see
           topLine = 0;             // UISetSourceTopLineNo)
        UISetSourceTopLineNo(topLine);
      }
      return 1;

    case KEY_LEFT:
    case KEY_RIGHT:
      // Pan a listing tab when the source window has focus; the command
      // line keeps the arrows for editing otherwise
      if (m_Focus != FOCUS_SRC)
        return 0;
      HScrollActive(key == KEY_RIGHT ? 1 : -1);
      return 1;

    case KEY_SUP:
    case KEY_UP:
      /* Test for history operations in CMD window */
      if (key == KEY_UP && m_Focus == FOCUS_CMD)
      {
        /* Test if we are in r-search mode */
        if (m_CtrlRActive && m_CtrlRHistIndex != -1)
        {
          m_HistoryIndex = m_History.GetOlderHistoryItem(m_CtrlRHistIndex, &pCmd);;
          strcpy(m_CmdLines[m_CmdCurrentLine-1], m_pPrompt);
          m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
          PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_pPrompt);
          wclrtoeol(m_pCmdwin);

          m_CtrlRActive = 0;
        }
        else
        {
          /* Get first or next older history item */
          if (m_HistoryIndex == -1)
             m_HistoryIndex = m_History.GetFirstHistoryItem(&pCmd);
          else
             m_HistoryIndex = m_History.GetOlderHistoryItem(m_HistoryIndex, &pCmd);
        }

        if (m_HistoryIndex == -1)
          return 1;

        /* Replace the current command line with the history item */
        *pBuffer = 0;
        //m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
        m_CmdCol = CalculateCmdCol(m_pPrompt);
        strncpy(pBuffer, pCmd, maxSize);

        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        wclrtoeol(m_pCmdwin);
        wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
        PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pBuffer, false);
        m_CmdCol += CalculateCmdCol(pCmd, false);
        wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
        wrefresh(m_pCmdwin);

        return 1;
      }

      if (key == KEY_UP && m_Focus != FOCUS_SRC)
        return 0;
       
      if (pTab != NULL)
        UISetSourceTopLineNo(pTab->SourceFirstLine() - 1);
      if (key != KEY_SUP)
        *pBuffer = 0;
      return 1;

    case KEY_SDOWN:
    case KEY_DOWN:
      /* Test for history operations in CMD window */
      if (key == KEY_DOWN && m_Focus == FOCUS_CMD)
      {
        /* Test if we are in r-search mode */
        if (m_CtrlRActive && m_CtrlRHistIndex != -1)
        {
          m_HistoryIndex = m_History.GetNewerHistoryItem(m_CtrlRHistIndex, &pCmd);;
          strcpy(m_CmdLines[m_CmdCurrentLine-1], m_pPrompt);
          m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
          PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_pPrompt);
          wclrtoeol(m_pCmdwin);

          m_CtrlRActive = 0;
        }
        else
        {
          /* Get first or next older history item */
          if (m_HistoryIndex == -1)
            return 1;
          else
            m_HistoryIndex = m_History.GetNewerHistoryItem(m_HistoryIndex, &pCmd);
        }

        /* Replace the current command line with the history item */
        if (m_HistoryIndex == -1)
        {
          // NULL out the command
          *pBuffer = 0;
          m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
          wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
          wclrtoeol(m_pCmdwin);
          wrefresh(m_pCmdwin);
        }
        else
        {
          *pBuffer = 0;
          //m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
          m_CmdCol = CalculateCmdCol(m_pPrompt);
          strncpy(pBuffer, pCmd, maxSize);

          wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
          wclrtoeol(m_pCmdwin);
          wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
          PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pBuffer, false);
          m_CmdCol += CalculateCmdCol(pCmd, false);
          wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
          wrefresh(m_pCmdwin);
        }

        return 1;
      }

      if (key == KEY_DOWN && m_Focus != FOCUS_SRC)
        return 0;
      
      if (pTab != NULL)
        UISetSourceTopLineNo(pTab->SourceFirstLine() + 1);
      if (key != KEY_SDOWN)
        *pBuffer = 0;
      return 1;

    case CTRL_W:
      NextWindowFocus();
      return 1;

    case CTRL_R:
      HandleCtrlR(pBuffer, maxSize);
      *pBuffer = 0;
      return 1;

    case CTRL_G:
      // Test for reverse-search
      if (m_CtrlRActive)
        CancelCtrlR();
      return 1;

    case CTRL_T:
      NextTab();
      return 1;

    case ALT_1:
    case ALT_2:
    case ALT_3:
    case ALT_4:
    case ALT_5:
    case ALT_6:
    case ALT_7:
    case ALT_8:
    case ALT_9:
    case ALT_0:
      if (key == ALT_0)
        SelectTab(9);
      else
        SelectTab(key - ALT_1);
      return 1;

    case ALT_SHIFT_F:
    case ALT_W:
      len = strlen(pBuffer);

      // Advance past the curent "word"
      while (index < len)
      {
        // Test for end of word
        if (pBuffer[index] != ' ' && pBuffer[index] != '/' &&
            pBuffer[index-1] != '.')
        {
          index++;
          m_CmdCol++;
        }
        else
          break;
      }

      // Skip spaces and /
      while (index < len)
      {
        // Skip all spaces and / chars
        if (pBuffer[index] == ' ' || pBuffer[index] == '/' ||
            pBuffer[index-1] == '.')
        {
          index++;
          m_CmdCol++;
        }
        else
          break;
      }

      wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
      return 2;

    case ALT_K:
      m_KeyDebug ^= 1;
      return 1;

    case ALT_SHIFT_B:
    case ALT_B:
      if (index > 0)
      {
        // Test if we are at the start of a word
        if (pBuffer[index-1] == ' ' || pBuffer[index-1] == '/' ||
            pBuffer[index-1] == '.')
        {
          // Rewind to the space
          index--;
          m_CmdCol--;
          while (index && (pBuffer[index] == ' ' || pBuffer[index] == '/' ||
                           pBuffer[index-1] == '.'))
          {
            index--;
            m_CmdCol--;
          }

          // Rewind to beginning of the word
          while (index)
          {
            if (pBuffer[index-1] == ' ' || pBuffer[index-1] == '/' ||
                pBuffer[index-1] == '.')
              break;
            index--;
            m_CmdCol--;
          }
        }
        else
        {
          // Rewinde to beginning of the word
          while (index)
          {
            // Test if we are a the beginning of the word
            if (pBuffer[index-1] == ' ' || pBuffer[index-1] == '/' ||
                pBuffer[index-1] == '.')
              break;
            index--;
            m_CmdCol--;
          }
        }
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
      }
      return 2;

    case CTRL_Y:
      // Paste to command line at current position
      if (strlen(m_PasteBuf) > 0)
      {

        // Make space in the m_CmdBUf for the paste operation
        len = strlen(&pBuffer[index]);
        pasteLen = strlen(m_PasteBuf);
        memmove(&pBuffer[index + pasteLen], &pBuffer[index], len+1);
        memcpy(&pBuffer[index], m_PasteBuf, pasteLen);
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, &pBuffer[index], false);
        m_pSrc->DebugPrintf("CMD: '%s'\n", m_CmdBuf);
        index += pasteLen;
        m_CmdCol = CalculateCmdCol(m_pPrompt) + index;
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        m_pSrc->DebugPrintf("Len=%d index=%d\n", strlen(m_CmdBuf), index);
      }
      return 2;

    case CTRL_K:
      // Cut cursor to EOL to paste buffer
      snprintf(m_PasteBuf, sizeof(m_PasteBuf), "%s", &pBuffer[index]);
      memset(&pBuffer[index], 0, maxSize-index);
      wclrtoeol(m_pCmdwin);
      return 1;

#if 0
    case CTRL_U:
      // Cut line prior to cursor
      if (index > 0)
      {
        // Copy to paste buffer
        snprintf(m_PasteBuf, index+1, "%s", pBuffer);
        m_PasteBuf[index+1] = 0;

        // Remove the text from the buffer
        memmove(pBuffer, &pBuffer[index], strlen(&pBuffer[index]) + 1);

        m_CmdCol -= index;
        index = 0;
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        wclrtoeol(m_pCmdwin);
        PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pBuffer, false);
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
      }
      return 2;
#endif

    case CTRL_L:
      fflush(stdout);
      werase(m_pWin);
      refresh();
      RedrawScreen();

      DrawSourceWindow();
      doupdate();
      m_NeedUpdate = 0;
      curs_set(1);
      return 1;

    // Pause the Watch Window updates
    case CTRL_P:
      m_WatchPaused ^= 1;
      return 1;

    case CTL_UP:
    case CTRL_U:
      // Page Up in Command Window
      m_ScrollBackCount += m_CmdHeight-1;
      if (m_CmdTopLine - m_ScrollBackCount < 0)
        m_ScrollBackCount = m_CmdTopLine;
      RedrawCommandWindow(true);
      *pBuffer = 0;
      return 1;

    case CTL_DOWN:
    case CTRL_D:
      // Page Down in Command Window
      m_ScrollBackCount -= m_CmdHeight-1;
      if (m_ScrollBackCount < 0)
        m_ScrollBackCount = 0;
      RedrawCommandWindow(true);
      *pBuffer = 0;
      return 1;

    case KEY_SLEFT:
      MoveFocusWinLeft();
      wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
      wrefresh(m_pCmdwin);
      WriteUIPreferences();
      *pBuffer = 0;
      return 1;

    case KEY_SRIGHT:
      MoveFocusWinRight();
      wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
      wrefresh(m_pCmdwin);
      WriteUIPreferences();
      *pBuffer = 0;
      return 1;

    case CTL_LEFT:
      m_pTabs->MakeTabActive(m_pTabs->GetActiveTab()->GetPrevTab());
      m_pTabs->Redraw();
      DrawSourceWindow();
      *pBuffer = 0;
      return 1;

    case CTL_RIGHT:
      m_pTabs->MakeTabActive(m_pTabs->GetActiveTab()->GetNextTab());
      m_pTabs->Redraw();
      DrawSourceWindow();
      *pBuffer = 0;
      return 1;

    // Close tab with F4
    case KEY_F0 + 4:
      if (m_pTabs->GetActiveTab() != NULL)
        m_pSrc->CloseTab(m_pTabs->GetActiveTab());
      break;

    // Perform program single step with F8
    case KEY_F0 + 8:
      if (m_pTabs->GetActiveTab() != NULL)
        m_pSrc->SingleStep();
      return 1;

    // Perform program single step with F9
    case KEY_F0 + 9:
      if (m_pTabs->GetActiveTab() != NULL)
        m_pSrc->StepOver();
      return 1;

    // Perform program continue with F5
    case KEY_F0 + 5:
      if (m_pTabs->GetActiveTab() != NULL)
        m_pSrc->Cont();
      return 1;

    case KEY_TAB:
      if (m_Focus != FOCUS_CMD)
        break;

      ProcessTabKey(pBuffer, maxSize);
      return 1;
  }

  return 0;
}

/*
===================================================================================
Process the Tab key by performing tab expansion.
===================================================================================
*/
void CTui::ProcessTabKey(char *pBuffer, int maxSize)
{
  int           c;
  bool          cmdMode;
  char          cmd[64];
  TuiSortItem_t *pItem;

  // Ensure we have a TuiSource attached
  if (m_pSrc == NULL)
    return;

  // First test if we have a full command with a space.  If we do,
  // then we will perform command specific tab expansion.
  cmdMode = false;
  cmd[0] = 0;
  for (c = 0; pBuffer[c] != 0; c++)
  {
    // Build command
    cmd[c] = pBuffer[c];
    cmd[c+1] = 0;

    // Test for space in the buffer
    if (pBuffer[c] == ' ')
    {
      cmdMode = true;
      cmd[c] = 0;
      break;
    }
  }
  if (!cmdMode)
    cmd[0] = 0;

  // Test if command changed from previous
  if (m_pTabList && (cmdMode || strcmp(cmd, m_TabCmd) != 0))
  {
    // New command or command changed.  Free the old table
    m_pSrc->FreeTabList(m_pTabList);
    m_pTabList = NULL;
  }

  if (strcmp(cmd, m_TabCmd) != 0)
  {
    // Copy command to m_TabCmd
    m_TabCmd[0] = 0;
    if (cmdMode)
    {
      strcpy(m_TabCmd, cmd);
    }
    m_FirstTab = 1;
  }

  // Get the list pointer from the TuiSource
  if (m_pTabList == NULL)
  {
    m_pSrc->GetCommandTabList(m_TabCmd, pBuffer, m_pTabList);
    if (m_pTabList != NULL)
    {
      // Get First list item
      pItem = m_pTabList->pFirst;
      while (pItem != NULL)
      {
        //m_pSrc->DebugPrintf("CMD: %s\n", pItem->name);
        pItem = pItem->pNext;
      }
    }
  }

  // If we have no expansion table for this command, then return ... there
  // is nothing to do.
  if (m_pTabList == NULL)
    return;

  // If this is the first tab then do a shortest match search
  if (m_FirstTab)
  {
    DoFirstTabSearch(pBuffer, maxSize);
  }
  else
  {
    // Print any matching items
    DoSecondTabSearch(pBuffer, maxSize);
  }
}

/*
===================================================================================
Do processing for CTRL-R reverse search.
===================================================================================
*/
int CTui::HandleCtrlR(char *pBuffer, int maxSize) 
{
  int     cmdLine;

  cmdLine = m_CmdCurrentLine;
  if (cmdLine == TUI_CMD_LINES)
    cmdLine--;

  // Test if CtrlR currently active
  if (m_CtrlRActive == 0)
  {
    // Active CTRL-R mode
    m_CtrlRActive = 1;
    m_CtrlRText[0] = 0;
    m_CtrlRHistIndex = -1;
  
    // Replace text on current line
    strcpy(m_CmdLines[cmdLine], "(r-search)'': ");
    wmove(m_pCmdwin, m_CursorLine, 0);
    m_CmdCol = strlen(m_CmdLines[cmdLine]);
    wclrtoeol(m_pCmdwin);
    wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
    PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_CmdLines[cmdLine]);
    wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
    wrefresh(m_pCmdwin);
  }
  else
  {
    // Find next matching item
    DoReverseHistorySearch(true);
  }

  return 1;
}

/*
===================================================================================
Cancel the CTRL-R reverse search mode
===================================================================================
*/
void CTui::CancelCtrlR(void)
{
  int cmdLine;

  // Cancel reverse search
  m_CtrlRActive = 0;
  m_CtrlRText[0] = 0;

  // Get line index of current line
  cmdLine = m_CmdCurrentLine;
  if (cmdLine == TUI_CMD_LINES)
    cmdLine--;

  strcpy(m_CmdLines[cmdLine], m_pPrompt);
  wmove(m_pCmdwin, m_CursorLine, 0);
  m_CmdCol = CalculateCmdCol(m_CmdLines[cmdLine]);
  wclrtoeol(m_pCmdwin);
  wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_CmdLines[cmdLine]);
  wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  wrefresh(m_pCmdwin);
}

/*
===================================================================================
Do the reverse history search with command window update
===================================================================================
*/
int CTui::DoReverseHistorySearch(bool searchPrev)
{
  int         histIndex;
  int         olderIndex = -1;
  const char *pHist;
  const char *pLastHist;
  const char *pFoundHist = NULL;

  // Do a reverse search through history for the text
  histIndex = m_CtrlRHistIndex;
  if (histIndex == -1)
  {
    histIndex = m_History.GetFirstHistoryItem(&pHist);
    if (histIndex == -1)
      return -1;
  }
  else
    m_History.GetHistoryItem(histIndex, &pHist);
  pLastHist = pHist;

  // If we are searching for previous, then get next older
  if (searchPrev)
  {
    histIndex = m_History.GetOlderHistoryItem(histIndex, &pHist);
  }

  // Search through history for a match
  while (histIndex != olderIndex)
  {
    olderIndex = histIndex;

    // Find matching text
    if (strcasestr(pHist, m_CtrlRText) != NULL)
    {
      // Test if this history item matches exactly the last
      if (strcmp(pHist, pLastHist) == 0)
      {
        // Get next older history item to test for match
        histIndex = m_History.GetOlderHistoryItem(histIndex, &pHist);
        continue;
      }
      pFoundHist = pHist;
      break;
    }
    else
    {
      // Get next older history item to test for match
      histIndex = m_History.GetOlderHistoryItem(histIndex, &pHist);
    }
  }

  ShowRSearchPrompt(pFoundHist, histIndex);
  return OK;
}

/*
===================================================================================
Process keys during CTRL-R mode
===================================================================================
*/
void CTui::ShowRSearchPrompt(const char *pFoundHist, int histIndex) 
{
  int         cmdLine;
  const char *pHist;

  // Get line index of current line
  cmdLine = m_CmdCurrentLine;
  if (cmdLine == TUI_CMD_LINES)
    cmdLine--;

  // Test if history item found
  if (pFoundHist != NULL)
  {
    m_CtrlRHistIndex = histIndex;
    snprintf(m_CmdLines[cmdLine], TUI_CMD_LEN, "(r-search)'%s': %s",
        m_CtrlRText, pFoundHist);
  }
  else
  {
    if (m_CtrlRHistIndex == -1)
      return;

    m_History.GetHistoryItem(m_CtrlRHistIndex, &pHist);
    snprintf(m_CmdLines[cmdLine], TUI_CMD_LEN, "(failed-r-search)'%s': %s",
        m_CtrlRText, pHist);
  }

  wmove(m_pCmdwin, m_CursorLine, 0);
  m_CmdCol = strlen(m_CmdLines[cmdLine]);
  wclrtoeol(m_pCmdwin);
  wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_CmdLines[cmdLine], false);
  wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  wrefresh(m_pCmdwin);
}

/*
===================================================================================
Process keys during CTRL-R mode
===================================================================================
*/
int CTui::ProcessCtrlRKey(int key, char *pBuffer, int &index, int maxSize) 
{
  int         len;
  int         histIndex;
  int         cmdLine;
  const char  *pHist;

  len = strlen(m_CtrlRText);

  // Test key for backspace
  if (key == 8)
  {
    // Ensure search length is not zero
    if (len > 0)
      m_CtrlRText[len-1] = 0;
    else
      return -1;
  }

  // Test for ESC key
  else if (key == 27)
  {
    CancelCtrlR();
    return -1;
  }

  // Test key for enter
  else if (key == '\n')
  {
    // Get line index of current line
    cmdLine = m_CmdCurrentLine;
    if (cmdLine == TUI_CMD_LINES)
      cmdLine--;

    // Do a reverse search through history for the text
    histIndex = m_CtrlRHistIndex;
    if (histIndex == -1)
      return -1;

    m_History.GetHistoryItem(histIndex, &pHist);
    sprintf(m_CmdLines[cmdLine], "%s", m_pPrompt);
    wmove(m_pCmdwin, m_CursorLine, 0);
    wclrtoeol(m_pCmdwin);
    PrintFormatStr(m_pCmdwin, m_CursorLine, 0, m_pPrompt);
    m_CmdCol = CalculateCmdCol(m_pPrompt);
    PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pHist);
    wrefresh(m_pCmdwin);
    strncpy(pBuffer, pHist, maxSize);
    m_CtrlRHistIndex = 0;
    m_CtrlRText[0] = 0;
    return OK;
  }

  // Add this key to the search text
  else
  {
    // Only add if we haven't exceeded max size
    if (len < (int) sizeof(m_CtrlRText)-1)
    {
      m_CtrlRText[len++] = key;
      m_CtrlRText[len] = 0;

      // Test if current history item still matches with new text added
      if (m_CtrlRHistIndex != -1)
      {
        m_History.GetHistoryItem(m_CtrlRHistIndex, &pHist);
        if (strcasestr(pHist, m_CtrlRText) != 0)
        {
          ShowRSearchPrompt(pHist, m_CtrlRHistIndex);
          return -1;
        }
      }
      else
      {
        // Test if this is the first character and the first history item matches
        if (len == 1)
        {
          // Get the first item
          histIndex = m_History.GetFirstHistoryItem(&pHist);
          if (strchr(pHist, key) != NULL)
          {
            ShowRSearchPrompt(pHist, m_CtrlRHistIndex);
            m_CtrlRHistIndex = histIndex;
            return -1;
          }
        }
      }
    }
  }

  // Do the history search
  DoReverseHistorySearch(false);

  return -1;
}

/*
===================================================================================
Perform tab key expansion for 1st tab key pressed (perform expansion to matching
entries from the table).
===================================================================================
*/
/* Byte-at-a-time copies for the tab-completion buffers.  The argument
 * being completed usually sits at an ODD offset inside the command
 * buffer ("bp 0 " = 5 chars), and newlib's word-optimized strcpy/strncpy
 * misread unaligned words on this core (same erratum as tc_streq in the
 * key decoder and the history truncation): "state_b" copied through them
 * came back as "s atetb", which then poisoned the common-prefix scan.
 */
static void tui_bstrcpy(char *d, const char *s)
{
  while ((*d++ = *s++) != '\0')
    ;
}

static void tui_bstrncpy(char *d, const char *s, int n)
{
  while (n > 1 && *s != '\0')
  {
    *d++ = *s++;
    n--;
  }
  *d = '\0';
}

static void tui_bstrcat(char *d, const char *s)
{
  while (*d != '\0')
    d++;
  tui_bstrcpy(d, s);
}

void CTui::DoFirstTabSearch(char *pBuffer, int maxSize)
{
  int           c;
  int           len;
  char          *pBuf;
  int           diff_found = 0;
  int           match_count = 0;
  int           adder;
  char          cmpstr[512];
  TuiSortItem_t *pFirstMatch = NULL;
  TuiSortItem_t *pItem;

  // Test if we are in command mode or argument mode and get a pointer
  pBuf = pBuffer;
  //for (c = 0; pBuffer[c] != 0; c++)
  for (c = strlen(pBuf)-1; c > 0; c--)
  {
    // Test for space delimiting command and argument
    if (pBuffer[c] == ' ')
    {
      // Set pBuf to point to the argument
      c++;
      //while (pBuffer[c] == ' ')
      //  c++;
      pBuf = &pBuffer[c];
      break;
    }
  }

  // Test for directory type search
  c = strlen(pBuf);
  //while (c >= 0)  KDP
  while (c > 0)
  {
    if (pBuf[c-1] == '/' || pBuf[c-1] == '\\' || pBuf[c-1] == '.')
    {
      pBuf = &pBuf[c];
      break;
    }
    c--;
  }

  // Count the number of items that have a partial match
  len = strlen(pBuf);
  if (len == 0)
  {
    m_FirstTab = 0;
    DoSecondTabSearch(pBuffer, maxSize);
    return;
  }

  pItem = m_pTabList->pFirst;
  while (pItem != NULL)
  {
    // Count matching items
    if (strncasecmp(pItem->name, pBuf, len) == 0)
    {
      // Save the index of the first match
      if (pFirstMatch == NULL)
        pFirstMatch = pItem;
      match_count++;
    }

    // Next item in the list
    pItem = pItem->pNext;
  }

  // Test for no matching items
  if (match_count == 0)
  {
    return;
  }

  // Test for exactly 1 matching item
  if (match_count == 1)
  {
    // Test if we match the whole name
    if (strcasecmp(pFirstMatch->name, pBuf) != 0)
    {
      // Perform expansion (byte-wise: pBuf is usually unaligned)
      *pBuf = 0;
      tui_bstrcpy(pBuf, pFirstMatch->name);

      // For directory expansion, don't add a 2nd tab
      if (pBuf[strlen(pBuf)-1] != '/' && 
          pBuf[strlen(pBuf)-1] != '\\' &&
          pBuf[strlen(pBuf)-1] != '.')
      {
        tui_bstrcat(pBuf, " ");
      }

      m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);

      wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
      wclrtoeol(m_pCmdwin);
      wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
      PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pBuffer);
      wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
      wrefresh(m_pCmdwin);

      m_CmdCol += strlen(pBuffer);
      return;
    }
  }

  // Create a compare string to find longest common match (byte-wise
  // copy - see tui_bstrcpy above)
  tui_bstrncpy(cmpstr, pBuf, sizeof(cmpstr));
  len = strlen(cmpstr);
  m_FirstTab = 0;
  if (pFirstMatch->name[len] == 0)
    return;

  match_count = 0;
  cmpstr[len] = pFirstMatch->name[len];
  cmpstr[len+1] = '\0';
  adder = 1;
  while (diff_found == 0)
  {
    /* Start at first block */
    pItem = m_pTabList->pFirst;
    while (pItem)
    {
      if ((strncasecmp(pItem->name, cmpstr, len+adder-1) == 0) &&
          (strncasecmp(pItem->name, cmpstr, len+adder) != 0))
      {
        diff_found = 1;
        break;
      }

      /* Get pointer to next item */
      pItem = pItem->pNext;
    }

    /* If no differenece found, then add another character to the search */
    if (!diff_found)
    {
      if (pFirstMatch->name[len+adder] == 0)
        break;

      cmpstr[len+adder] = pFirstMatch->name[len+adder];
      cmpstr[len+adder+1] = '\0';
      adder++;
    }
    else
    {
      adder--;
    }
  }

  /* Test if we need to expand the text automatically */
  if (adder > 0)
  {
    /* Expand the text - taking the CANONICAL case from the match list,
     * so "state_b" grows into "STATE_B_" exactly as it is defined */
    tui_bstrncpy(cmpstr, pFirstMatch->name, len + adder + 1);
    tui_bstrcpy(pBuf, cmpstr);

    m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);

    wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
    wclrtoeol(m_pCmdwin);
    wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
    PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pBuffer);
    wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
    wrefresh(m_pCmdwin);

    m_CmdCol += strlen(pBuffer);
  }
  else
    DoSecondTabSearch(pBuffer, maxSize);
}

/*
===================================================================================
Perform tab key expansion for 2nd, 3rd, etc. tab key pressed (list matching
entries from the table).
===================================================================================
*/
void CTui::DoSecondTabSearch(char *pBuffer, int maxSize)
{

  int           c;
  int           len;
  char          *pBuf;
  int           maxLen = 0;
  int           itemLen;
  int           itemsPerLine;
  int           match_count = 0;
  int           items;
  int           lines;
  char          prompt[32];
  char          fmt[16];
  char          token[240];
  char          line[512];
  const char    *pName;
  int           key;
  TuiSortItem_t *pFirstMatch = NULL;
  TuiSortItem_t *pItem;

  // Test if we are in command mode or argument mode and get a pointer
  pBuf = pBuffer;
  for (c = strlen(pBuf)-1; c > 0; c--)
  //for (c = 0; pBuffer[c] != 0; c++)
  {
    // Test for space delimiting command and argument
    if (pBuffer[c] == ' ')
    {
      // Set pBuf to point to the argument
      c++;
      //while (pBuffer[c] == ' ')
        //c++;
      pBuf = &pBuffer[c];
      break;
    }
  }

  // Test for dirctory / file matching mode
  len = strlen(pBuf);
  c = len;
  while (c > 0 && pBuf[c-1] != '/' && pBuf[c-1] != '\\' &&
         pBuf[c-1] != '.')
    c--;
  if (pBuf[c-1] == '/' || pBuf[c-1] == '\\' || pBuf[c-1] == '.')
  {
    pBuf = &pBuf[c];
  }

  // Count the number of items that have a partial match
  len = strlen(pBuf);
  pItem = m_pTabList->pFirst;
  while (pItem != NULL)
  {
    // Count matching items
    if (strncasecmp(pItem->name, pBuf, len) == 0)
    {
      // Save the index of the first match
      if (pFirstMatch == NULL)
        pFirstMatch = pItem;
      match_count++;
      pName = pItem->name;
      itemLen = strlen(pItem->name);
      if (itemLen > maxLen)
        maxLen = itemLen;
      //m_pSrc->DebugPrintf("%s\n", pItem->name);
    }

    // Next item in the list
    pItem = pItem->pNext;
  }

  // Test for no matching items
  if (match_count == 0)
  {
    return;
  }

  // Calculate the number of items per line
  maxLen += 2;
  itemsPerLine = m_CmdWidth / maxLen;
  if (itemsPerLine == 0)
    itemsPerLine = 1;

  strcpy(prompt, m_CmdLines[m_CmdCurrentLine-1]);
  strcat (m_CmdLines[m_CmdCurrentLine-1], pBuffer);
  sprintf(fmt, "%%-%ds", maxLen);

  pItem = m_pTabList->pFirst;
  items = 0;
  lines = 0;
  line[0] = 0;
  key   = -1;
  while (pItem != NULL && key != 'q')
  {
    if (items == 0)
      line[0] = 0;
    // Count matching items
    if (strncasecmp(pItem->name, pBuf, len) == 0)
    {
      pName = pItem->name;
      m_pSrc->DebugPrintf("%s\n", pItem->name);
      snprintf(token, sizeof(token), fmt, pName);
      strcat(line, token);
      items++;
      if (items >= itemsPerLine)
      {
        if (++lines >= m_CmdHeight)
        {
          key = PauseCmdListing();

          if (key != 'q')
          {
            // Replace current line with the next line of text
            strcpy(m_CmdLines[m_CmdCurrentLine-1], line);
            RedrawCommandWindow();
            lines = 0;
          }
        }
        else
        {
          UICommandPrintString(line);
          m_pSrc->DebugPrintf("%s\n", line);
          m_pSrc->DebugPrintf("line=%d\n", m_CmdCurrentLine);
          strcpy(m_CmdLines[m_CmdCurrentLine-1], line);
          RedrawCommandWindow();
        }
        items = 0;
      }
    }

    // Next item in the list
    pItem = pItem->pNext;
  }
  if (items > 0)
  {
    if (++lines >= m_CmdHeight)
    {
      key = PauseCmdListing();
      if (key != 'q')
      {
        // Replace current line with the next line of text
        strcpy(m_CmdLines[m_CmdCurrentLine-1], line);
        RedrawCommandWindow();
        lines = 0;
      }
    }
    else
      UICommandPrintString(line);
  }

  if (key != 'q')
    UICommandPrintString(prompt);
  else
    strcpy(m_CmdLines[m_CmdCurrentLine-1], prompt);

  RedrawCommandWindow();
  doupdate();
  m_NeedUpdate = 0;

  m_CmdCol = CalculateCmdCol(m_CmdLines[m_CmdCurrentLine-1]);
  wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
  wclrtoeol(m_pCmdwin);
  wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  PrintFormatStr(m_pCmdwin, m_CursorLine, m_CmdCol, pBuffer);
  wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  wrefresh(m_pCmdwin);
  m_CmdCol += strlen(pBuffer);
}

/*
===================================================================================
Pause command window listing and display a prompt to wait for a key to continue.
Process key input to get the key.
===================================================================================
*/
int CTui::PauseCmdListing(void)
{
  int     key;

  UICommandPrintString("-- more --  (any key continues, 'q' quits)");

  key = -1;
  while (key == -1)
  {
    // Sleep if we had no key last time
    while (!PDC_check_key() && !gCtrlCTerm)
    {
      // Sleep for 2ms
      usleep(500);
      continue;
    }

    if (gCtrlCTerm)
      return -1;

    // Get key from the UI thread 
    key = wgetch(m_pCmdwin);
    if (key == -1)
      continue;

    // Test for 'q' key
    if (key == 'q')
    {
      break;
    }
  }

  return key;
}

/*
===================================================================================
Get input string from the keyboard.  Return when ENTER pressed.
===================================================================================
*/
#ifdef STANDALONE
int CTui::GetInput(char *pBuf, int maxLen)
{
  int     key;
  int     len = 0;
  int     count = 0;

  key = -1;
  while (key != '\n')
  {
    // Sleep if we had no key last time
    while (!PDC_check_key() && !gLastWasCtrlC)
    {
      // Sleep for 2ms
      usleep(500);

      // Test for timed callback
      if (++count > 200)
      {
        UpdateWatchWindows();
        count = 0;
      }
    }

    if (gLastWasCtrlC)
      return 0;

    // Get the key
    key = wgetch(gpCtrlCTui->m_pFocuswin);
    if (key != '\r' && key != '\n')
      pBuf[len++] = key;
    if (len == maxLen)
      return len;
  }

  return len;
}
#endif

/*
===================================================================================
Check for input from the keyboard
===================================================================================
*/
#ifdef STANDALONE
int CTui::CheckInput(void)
{
  if (PDC_check_key() || gLastWasCtrlC)
    return 0;

  return -1;
}
#endif

/*
===================================================================================
Move to the next tab
===================================================================================
*/
void CTui::NextTab(void)
{
  CTab    *pTab;

  // Get the active tab
  pTab = m_pTabs->GetActiveTab();
  if (pTab == NULL)
    return;

  // Get the next tab to the right
  pTab = pTab->GetNextTab();
  if (pTab == NULL)
    pTab = m_pTabs->GetFirstTab();

  // Set the tab active and redraw
  if (pTab != NULL)
  {
    pTab->SetFocus();
    m_pTabs->Redraw();
    DrawSourceWindow();
  }
}

/*
===================================================================================
Make selected ALT-1 through ALT-0 (10) tab active
===================================================================================
*/
void CTui::SelectTab(int which)
{
  CTab    *pTab;

  // Get the first tab
  pTab = m_pTabs->GetFirstTab();
  if (pTab == NULL)
    return;

  // Keep getting the next tab to the right until which or at the end
  while (which && pTab)
  {
    pTab = pTab->GetNextTab();
    which--;
  }

  // Test if selected tab exists
  if (pTab == NULL)
    return;

  // Set the tab active and redraw
  if (pTab != NULL)
  {
    pTab->SetFocus();
    m_pTabs->Redraw();
    DrawSourceWindow();
  }
}

/*
===================================================================================
Make the specified tab the active tab
===================================================================================
*/
void CTui::MakeTabActive(CTab *pTab)
{
  // Set the tab active and redraw
  if (pTab != NULL)
  {
    pTab->SetFocus();
    m_pTabs->Redraw();
    DrawSourceWindow();
  }
}

/*
===================================================================================
Move the focus window left
===================================================================================
*/
void CTui::MoveFocusWinLeft(void) 
{
   int   rows, cols;

   // Command window not moveable for now
   if (m_Focus == FOCUS_CMD)
      return;

   curs_set(0);
   if (m_Focus == FOCUS_SRC)
   {
      m_SrcWinPosition = FOCUS_LEFT;
      getmaxyx(m_pSrcframe, rows, cols);
      mvwin(m_pSrcframe, 1, 0);
      mvwin(m_pRegframe, 1, cols);
      mvwin(m_pRegwin,   2, cols+1);
   }
   else
   {
      m_SrcWinPosition = FOCUS_RIGHT;
      getmaxyx(m_pRegframe, rows, cols);
      mvwin(m_pSrcframe, 1, cols);
      mvwin(m_pRegframe, 1, 0);
      mvwin(m_pRegwin,   2, 1);
   }
   (void) rows;
   wnoutrefresh(m_pRegframe);
   wnoutrefresh(m_pRegwin);
   wnoutrefresh(m_pSrcframe);
   DrawSourceWindow();
   doupdate();
   m_NeedUpdate = 0;

   curs_set(1);
}

/*
===================================================================================
Move the focus window left
===================================================================================
*/
void CTui::MoveFocusWinRight(void) 
{
   int   rows, cols;

   // Command window not moveable for now
   if (m_Focus == FOCUS_CMD)
      return;

   curs_set(0);
   if (m_Focus == FOCUS_REG)
   {
      m_SrcWinPosition = FOCUS_LEFT;
      getmaxyx(m_pSrcframe, rows, cols);
      mvwin(m_pSrcframe, 1, 0);
      mvwin(m_pRegframe, 1, cols);
      mvwin(m_pRegwin,   2, cols+1);
   }
   else
   {
      m_SrcWinPosition = FOCUS_RIGHT;
      getmaxyx(m_pRegframe, rows, cols);
      mvwin(m_pSrcframe, 1, cols);
      mvwin(m_pRegframe, 1, 0);
      mvwin(m_pRegwin,   2, 1);
   }
   (void) rows;
   wnoutrefresh(m_pRegframe);
   wnoutrefresh(m_pRegwin);
   wnoutrefresh(m_pSrcframe);
   DrawSourceWindow();
   doupdate();
   m_NeedUpdate = 0;
   curs_set(1);
}

/*
===================================================================================
Print a string at the specified location in the specified window.

This is a UI THREAD ONLY function.
===================================================================================
*/
void CTui::UIWindowPrintString(WINDOW* pWin, int row, int col, const char *pStr)
{
  curs_set(0);
  wattrset(pWin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  wmove(pWin, row, col);
  PrintFormatStr(pWin, row, col, pStr);
  m_NeedUpdate = 1;
  curs_set(1);
}

/*
===================================================================================
This is the main thread of the CTui class.
===================================================================================
*/
void CTui::RunThread(void) 
{
  int      ret;
  int      count = 0;

#ifdef STANDALONE
  gpCtrlCTui = this;
  signal(SIGINT, &sig_handler);
#endif
  // Initialize the UI
  ret = UIInit();
  if (ret != OK)
   {
    m_CursesError = true;
    return;
   }

  // Indicated curses initialized and ready
  m_CursesInitialized = true;

  // Print command prompt
  UICommandPrintString(m_pPrompt);

  // Screen is up: let the source create its startup tabs etc.
  if (m_pSrc)
    m_pSrc->UIReady();

  // Loop until we are terminated
  m_CmdIndex = 0;
  m_CmdBuf[m_CmdIndex] = 0;
  ret = -1;
  while (!m_Terminated && !gCtrlCTerm)
  {
    // Sleep if we had no key last time
    if (!PDC_check_key())
    {
      // Source idle work (the synth's envelope service lives here so
      // ADSR stages and pitch/timbre slide stop-writes keep running
      // while the TUI waits for keys)
      if (m_pSrc)
        m_pSrc->IdlePoll();


      // Sleep for 100 uS
      usleep(100);

      // Test for timed callback
#if 1
      if (++count > 2000)
      {
        //curs_set(0);
        UpdateWatchWindows();
        count = 0;
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        //curs_set(1);
      }
#endif

#ifdef STANDALONE
      if (gLastWasCtrlC == 0)
        continue;
#endif
    }

    // Test for CTRL-C clear line
    if (m_CtrlCEraseLine)
    {
      // Zero out the command buffer
      m_CmdBuf[m_CmdIndex] = 0;
      strcat(m_CmdBuf, "^C");
      strcat (m_CmdLines[m_CmdCurrentLine-1], m_CmdBuf);
      mvwprintw(m_pCmdwin, m_CursorLine, m_CmdCol, "^C");
      m_CmdIndex = 0;
      m_CmdBuf[m_CmdIndex] = 0;
      UICommandPrintString(m_pPrompt);
      m_CtrlCEraseLine = 0;
#ifdef STANDALONE
      gLastWasCtrlC = 0;
#endif
      continue;
    }

    // Test for and process keyboard input
    ret = ProcessInput(m_CmdBuf, m_CmdIndex, sizeof(m_CmdBuf));

    if (ret == OK)
    {
      // Process the command
      m_CmdIndex = 0;
      m_CtrlRActive = 0;
      ProcessCommand(m_CmdBuf);
      if (m_Terminate)
        break;

      // Display a new command prompt 
      memset(m_CmdBuf, 0 , sizeof(m_CmdBuf));
      UICommandPrintString(m_pPrompt);
    }

    // Test for shutdown
    if (m_Terminate)
      break;

    // Test for UI requests from other threads 
    if (m_Request == -1)
      continue;

    // Process the request
    switch (m_Request)
    {
      case REQ_COMMAND_PRINT_STRING:
        UICommandPrintString(m_ReqStrArg1);
        break;

      case REQ_COMMAND_APPEND_STRING_AND_PROMPT:
        UICommandPrintString(m_ReqStrArg1);
        break;

      case REQ_DISPLAY_SOURCE_AT_LINE:
        UIDisplaySourceAtLine(m_ReqIntArg1);
        break;

      case REQ_SET_SOURCE_TOP_LINENO:
        UISetSourceTopLineNo(m_ReqIntArg1);
        break;

      case REQ_WAIT_INIT_COMPLETE:
        break;

      case REQ_CMD_PUT_CH:
        wmove(m_pCmdwin, m_ReqIntArg1, m_ReqIntArg2);
        wattron(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
        mvwaddch(m_pCmdwin, m_ReqIntArg1, m_ReqIntArg2, m_ReqIntArg3);
        wattroff(m_pCmdwin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
        wrefresh(m_pCmdwin);
        wmove(m_pCmdwin, m_ReqIntArg1, m_ReqIntArg2+1);
        break;

      case REQ_CMD_MOVE:
        wmove(m_pCmdwin, m_ReqIntArg1, m_ReqIntArg2);
        wrefresh(m_pCmdwin);
        break;

      case REQ_NEXT_WINDOW_FOCUS:
        NextWindowFocus();
        break;

      case REQ_WINDOW_PRINT_STRING:
        UIWindowPrintString(m_ReqWindow, m_ReqIntArg1, m_ReqIntArg2, m_ReqStrArg1);
        break;

      case REQ_WATCH_WINDOW_UPDATE:
        UpdateWatchWindows();
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        wrefresh(m_pCmdwin);
        break;

      case REQ_MOVE_FOCUS_WIN_LEFT:
        MoveFocusWinLeft();
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        wrefresh(m_pCmdwin);
        break;

      case REQ_MOVE_FOCUS_WIN_RIGHT:
        MoveFocusWinRight();
        wmove(m_pCmdwin, m_CursorLine, m_CmdCol);
        wrefresh(m_pCmdwin);
        break;

      case REQ_TERMINATE:
        m_Terminated = 1;
        break;

      case REQ_HANDLE_CTRL_C:
        if (m_pSrc && m_pSrc->HandleCtrlC())
        {
#ifdef STANDALONE
          gLastWasCtrlC = 0;
#endif
        }
        break;
    }
    
    // Signal completion of the request
    m_Request = -1;
    m_CompleteTrigger.Trigger();
  }

  WriteUIPreferences();

  move(m_Lines-1, 0);
  refresh();

  delwin(m_pSrcframe);
  delwin(m_pRegframe);
  delwin(m_pRegwin);
  delwin(m_pCmdframe);
  delwin(m_pCmdwin);
  delwin(m_pMainMenu);

  /* TinyQV port: m_pWin IS stdscr - endwin()'s delscreen() frees it,
   * so deleting it here (as the NuttX build did) was a double free on
   * the newlib heap.  The "Terminated" printf is gone too: with the
   * stdout hook still installed it would draw into the just-deleted
   * command window.
   */

  endwin();

  m_Terminated = true;
}

/*
===================================================================================
Process command line
===================================================================================
*/
int CTui::ProcessCommand(char *pBuf)
{
  char            *cmd;
  char            *argv[128];
  int             argc, x;
  const TuiCmd_t  *pCmds;
  int             ret;

  /* Save buffer for secondary command processing */
  strncpy(m_HistBuf, pBuf, sizeof(m_HistBuf));
  m_HistoryIndex = -1;

  /* Test if an empty line was given */
  while (*pBuf == ' ')
    pBuf++;
  if (!*pBuf)
    return -1;

  /* Test for exit command */
  if (strcmp(pBuf, "exit") == 0 || strcmp(pBuf,"q") == 0 ||
      strcmp(pBuf, "quit") == 0)
  {
    m_Terminate = true;
    return OK;
  }

  /* Ensure a TuiSource was provided */
  if (m_pSrc == NULL)
  {
    UICommandPrintString("Unknown command");
    return -1;
  }

  /* Remove trailing spaces */
  x = strlen(pBuf);
  while (pBuf[x-1] == ' ')
  {
    x--;
    pBuf[x] = '\0';
  }

  /*
  ====================================
  Split the line into arguments
  ====================================
  */
  cmd = pBuf;
  argv[0] = pBuf;
  argc = 1;
  while (*cmd)
  {
     // Test for space between arguments
     if (*cmd == ' ')
     {
        // NULL terminate perv arg
        *cmd++ = '\0';
        while (*cmd == ' ')
           cmd++;

        // Test for next arg
        if (*cmd)
        {
           argv[argc++] = cmd;
        }
     }

     cmd++;
  }

  /*
  =================================================
  Search for the command in the global command list
  Split the line into arguments
  =================================================
  */
  pCmds = m_pSrc->GetCommandTable();
  for (x = 0; pCmds[x].name != NULL; x++)
  {
    int len = strlen(argv[0]);

    // Search for command in the table
    if (strncmp(argv[0], pCmds[x].name, len) == 0)
    {
      /* We found the command.  Test the number of arguments */

      if (pCmds[x].min_args > 0)
      {
        /* Validate argc is in range */
        if (argc-1 < pCmds[x].min_args || 
           argc-1 > pCmds[x].max_args)
        {
          UICommandPrintString("Invalid number of args");
          return -1;
        }
      }

      /* Add this command line to the command history */
      m_History.Add(m_HistBuf);

      /* Okay, the number of arguments checks out.
       * call the command's handler.
       */
      return (m_pSrc->*pCmds[x].pFunc)(argc, argv);
    }
  }
  
  /* Test for special command processing */
  if ((ret = m_pSrc->ProcessLine(m_HistBuf)) >= 0)
  {
    /* Add this command line to the command history */
    m_History.Add(m_HistBuf);

    return ret;
  }

  /* Command not found */
  UICommandPrintString("Unknown command");

  return -1;
}

/*
===================================================================================
Issue a request from the TuiThread and wait for completion.
===================================================================================
*/
void CTui::IssueRequest(int req, WINDOW* pWin, const char *pStr1,
               int intArg1, int intArg2, int intArg3)
{
  // Acquire the request interface
  m_RequestLock.Acquire();

  // Set the request ID and parameters
  m_Request = req;
  m_ReqWindow = pWin;
  m_ReqStrArg1 = pStr1;
  m_ReqIntArg1 = intArg1;
  m_ReqIntArg2 = intArg2;
  m_ReqIntArg3 = intArg3;

  // Trigger the request
  m_RequestTrigger.Trigger();

  // Wait for completion
  m_CompleteTrigger.Wait();

  // Release the request interface
  m_RequestLock.Release();
}

/*
===================================================================================
Request a thread terminate
===================================================================================
*/
void CTui::RequestTerminate(void)
{
  IssueRequest(REQ_TERMINATE, NULL, NULL, 0, 0, 0);
}

/*
===================================================================================
Remove a variable from the watch window.
===================================================================================
*/
void CTui::RequestCommandPrintString(const char *pStr) 
{
  IssueRequest(REQ_COMMAND_PRINT_STRING, NULL, pStr, 0, 0, 0);
}

/*
===================================================================================
Draw / redraw the source screen so the given lineNo is visible.
===================================================================================
*/
void CTui::RequestDisplaySourceAtLine(int lineNo)
{
  IssueRequest(REQ_DISPLAY_SOURCE_AT_LINE, NULL, NULL, lineNo, 0, 0);
}

/*
===================================================================================
Draw / redraw the source screen so the given lineNo is visible.
===================================================================================
*/
void CTui::RequestSetSourceTopLineNo(int lineNo)
{
  IssueRequest(REQ_SET_SOURCE_TOP_LINENO, NULL, NULL, lineNo, 0, 0);
}

/*
===================================================================================
Simply wait for initialization to complete
===================================================================================
*/
void CTui::RequestWaitInitComplete(void)
{
  IssueRequest(REQ_WAIT_INIT_COMPLETE, NULL, NULL, 0, 0, 0);
}

/*
===================================================================================
Simply wait for initialization to complete
===================================================================================
*/
void CTui::RequestCmdPutCh(int row, int col, int ch)
{
  IssueRequest(REQ_CMD_PUT_CH, NULL, NULL, row, col, ch);
}

/*
===================================================================================
Simply wait for initialization to complete
===================================================================================
*/
void CTui::RequestCmdMove(int row, int col)
{
  IssueRequest(REQ_CMD_MOVE, NULL, NULL, row, col, 0);
}

/*
===================================================================================
Request changing focus to next window in the cycle
===================================================================================
*/
void CTui::RequestNextWindowFocus(void)
{
  IssueRequest(REQ_NEXT_WINDOW_FOCUS, NULL, NULL, 0, 0, 0);
}

/*
===================================================================================
Request printing a string in a specified window at a specified location
===================================================================================
*/
void CTui::RequestWindowPrintString(WINDOW* pWin, int row, int col, const char *pStr)
{
  IssueRequest(REQ_WINDOW_PRINT_STRING, pWin, pStr, row, col, 0);
}

/*
===================================================================================
Wait for a key to be available.
===================================================================================
*/
int CTui::RequestGetKey(void)
{
  int   key;

  // Acquire the request interface
  m_KeyRequest.Acquire();

  key = m_Key[m_KeyOut];
  m_KeyOut = (m_KeyOut+1) % 16;
  return key;
}

/*
===================================================================================
Request update of all watch windows
===================================================================================
*/
void CTui::RequestWatchWindowUpdate(void)
{
  IssueRequest(REQ_WATCH_WINDOW_UPDATE, NULL, NULL, 0, 0, 0);
}

/*
===================================================================================
Request to move the focused window left
===================================================================================
*/
void CTui::RequestMoveFocusWinLeft(void)
{
  IssueRequest(REQ_MOVE_FOCUS_WIN_LEFT, NULL, NULL, 0, 0, 0);
}

/*
===================================================================================
Request to move the focused window right
===================================================================================
*/
void CTui::RequestMoveFocusWinRight(void)
{
  IssueRequest(REQ_MOVE_FOCUS_WIN_RIGHT, NULL, NULL, 0, 0, 0);
}

/*
===================================================================================
Get the command window rows anc cols
===================================================================================
*/
void CTui::GetCmdWinGeometry(int &rows, int &cols)
{
  getmaxyx(m_pCmdwin, rows, cols);
}

/*
===================================================================================
Get the source window rows and cols
===================================================================================
*/
void CTui::GetSourceWinGeometry(int &rows, int &cols)
{
  getmaxyx(m_pSrcframe, rows, cols);
}

/*
===================================================================================
Routine to handle CTRL-C key
===================================================================================
*/
void CTui::HandleCtrlC(void)
{
  // Test for canceling keyboard input
  if (m_CmdIndex != 0)
  {
    m_CtrlCEraseLine = 1;
  }
  // Test for reverse-search
  else if (m_CtrlRActive)
  {
    CancelCtrlR();
  }
  else if (m_pSrc)
  {
    m_pSrc->HandleCtrlC();
  }
}

/*
===================================================================================
Print a string with possible format characters to the given window
===================================================================================
*/
void CTui::PrintFormatStr(WINDOW *pWin, int line, int col, const char *pBuf,
                          bool expandColor)
{
  char      temp[1024];
  int       c;
  int       color;
  static const int colors[9] = {
                        SYNTAX_PAIR_NORMAL, SYNTAX_PAIR_COMMENT,
                        SYNTAX_PAIR_LINENO, SYNTAX_PAIR_KEYWORD,
                        SYNTAX_PAIR_REG, SYNTAX_PAIR_LABEL,
                        SYNTAX_PAIR_DECLARATOR, SYNTAX_PAIR_MENU_BAR,
                        SYNTAX_PAIR_MENU_FIRSTCHAR
                      };

  c = 0;
  while (*pBuf != 0 && c < (int) sizeof(temp)-1)
  {
    // Test for embedded '%'
    if (*pBuf == '%')
      temp[c++] = '%';
    else if (expandColor && *pBuf == '\\' && *(pBuf+1) == 'c' && isdigit(*(pBuf+2)))
    {
      // Print the temp string
      temp[c] = 0;
      mvwprintw(pWin, line, col, temp);
      col += strlen(temp);
      c = 0;

      // Get the new highlight color
      color = *(pBuf+2) - '0';
      wattron(pWin, COLOR_PAIR(colors[color]));
      pBuf += 3;
      continue;
    }
    temp[c++] = *pBuf++;
  }
  temp[c] = 0; 
  mvwprintw(pWin, line, col, temp);
  wattron(pWin, COLOR_PAIR(colors[0]));
}

// vim: sw=2 ts=2 et cindent

