/************************************************************************************
 * tui/Tui.h
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

#ifndef _TUI_TUI_H
#define _TUI_TUI_H

#ifdef STANDALONE
#include <curses.h>
#include <stdint.h>
#else
#include <graphics/curses.h>
#endif
#include <string.h>
#include "simevent.h"
#include "thread.h"
#include "Tabs.h"
#include "History.h"

#ifndef STANDALONE
/* TinyQV port: was 20 lines on NuttX; ram_a has megabytes free, so keep
 * a real scrollback (PgUp/PgDn browse it). */
#define   TUI_CMD_LINES     200
#define   TUI_CMD_LEN       200
#else
#define   TUI_CMD_LINES     400
#define   TUI_CMD_LEN       300
#endif
#define   FOCUS_CMD         0
#define   FOCUS_SRC         1
#define   FOCUS_REG         2

#define   FOCUS_LEFT        0
#define   FOCUS_RIGHT       1

#define  SYNTAX_PAIR_NORMAL          1
#define  SYNTAX_PAIR_COMMENT         2
#define  SYNTAX_PAIR_LINENO          3
#define  SYNTAX_PAIR_KEYWORD         4
#define  SYNTAX_PAIR_REG             5
#define  SYNTAX_PAIR_LABEL           6
#define  SYNTAX_PAIR_DECLARATOR      7
#define  SYNTAX_PAIR_MENU_BAR        8
#define  SYNTAX_PAIR_MENU_BODY       9
#define  SYNTAX_PAIR_MENU_SELECT     10
#define  SYNTAX_PAIR_MENU_FIRSTCHAR  11
#define  SYNTAX_PAIR_DYNAMIC         12
#define  SYNTAX_PAIR_NOTES_VOCAL     13
#define  ROUTE_PAIR_BASE             14    /* 5 FSM connection colors:
                                              yellow green blue red
                                              orange (pairs 14..18) */    /* TinyQV port: bright yellow
                                            * melody notes in the Notes tab */

class CTuiSource;

/*
==============================================================================
Definition of our Tui class
==============================================================================
*/

class CTui : public CThread
{
  public:
    CTui();
    ~CTui();
     
  /* Public Class methods */

  public:
    void    WriteUIPreferences(void);
    void    ReadUIPreferences(void);
    void    GetTermSize(int &rows, int &cols);

	enum	{
		REQ_COMMAND_PRINT_STRING = 1,
    REQ_COMMAND_APPEND_STRING_AND_PROMPT,
		REQ_WINDOW_PRINT_STRING,
		REQ_DISPLAY_SOURCE_AT_LINE,
		REQ_SET_SOURCE_TOP_LINENO,
		REQ_WAIT_INIT_COMPLETE,
		REQ_CMD_PUT_CH,
		REQ_CMD_MOVE,
		REQ_WATCH_WINDOW_UPDATE,
		REQ_MOVE_FOCUS_WIN_LEFT,
		REQ_MOVE_FOCUS_WIN_RIGHT,
		REQ_NEXT_WINDOW_FOCUS,
    REQ_TERMINATE,
    REQ_HANDLE_CTRL_C
	};

    void          RunThread();
    void          AttachTuiSource(CTuiSource *pSrc);
                  
    void          RequestCommandPrintString(const char *pStr);
    void          RequestWindowPrintString(WINDOW* pWin, int row, int col, const char *pStr);
    void          RequestDisplaySourceAtLine(int lineNo);
    void          RequestSetSourceTopLineNo(int lineNo);
    void          RequestWaitInitComplete(void);
    void          RequestNextWindowFocus(void);
    void          RequestCmdPutCh(int row, int col, int ch);
    void          RequestCmdMove(int row, int col);
    void          RequestWatchWindowUpdate(void);
    int           RequestGetKey(void);
    void          RequestMoveFocusWinLeft(void);
    void          RequestMoveFocusWinRight(void);
    void          RequestTerminate(void);
    void          NextTab(void);
    void          SelectTab(int which);
    void          MakeTabActive(CTab *pTab);

  /* Methods called only from the CursesThread */

  public:
    int           UIInit(void);
    virtual int   ProcessInput(char* pBuffer, int &index, int maxSize);
    void          UICommandPrintString(const char* pStr);
    void          UICommandPrintFormat(const char* fmt, ...);
    void          UICommandPrintFormat(const char* fmt, va_list args);
    void          UICommandAppendString(const char* pStr);
    void          GetCmdWinGeometry(int &rows, int &cols);
    void          GetSourceWinGeometry(int &rows, int &cols);
    void          UIDisplaySourceAtLine(int lineNo);
    void          UISetSourceTopLineNo(int lineNo);
    void          UIWindowPrintString(WINDOW *pWin, int row, int col, const char *pStr);
    void          DrawSourceWindow(void);
    void HScrollActive(int dir);   // arrow-key tab pan
    void ActiveTabRect(int rect[4]); // terminal rect of the active tab
    void CmdWindowRect(int rect[4]); // terminal rect of the command window
    virtual int   DisplaySourceAtLine(int lineNo);
    void          RedrawCommandWindow(bool debugPrint = false);
    CTab*         CreateNewTab(const char *name) { return m_pTabs->CreateNewTab(name); }
    void          DeleteTab(CTab *pTab) { m_pTabs->DeleteTab(pTab); DrawSourceWindow(); }
    CTab*         GetActiveSrcTab(void) { return m_pTabs->GetActiveTab(); }
    CTab*         GetFirstTab(void) { return m_pTabs->GetFirstTab(); }
    void          HandleCtrlC(void);
    // Public so a CTuiSource can page its own long listings (the help
    // command does); returns the key pressed, 'q' meaning stop.
    int           PauseCmdListing(void);
    void          ReplaceHistBuf(const char *pBuf) { strncpy(m_HistBuf, pBuf,
                                                      sizeof(m_HistBuf));
                                                   }
    virtual int   UpdateWatchWindows(void);
#ifdef STANDALONE
    int           GetInput(char *pBuf, int maxLen);
    int           CheckInput(void);
#endif

  private:
    void          IssueRequest(int req, WINDOW* pWin, const char *pStr1,
                               int intArg1, int intArg2, int intArg3);
    int           ProcessCommand(char *pBuf);
    CSimEvent     m_RequestLock;
    CSimEvent     m_RequestTrigger;
    CSimEvent     m_CompleteTrigger;
    CSimEvent     m_KeyRequest;
    CTabContainer *m_pTabs;
                  
    int           m_Request;
    WINDOW *      m_ReqWindow;
    const char*   m_ReqStrArg1;
    int           m_ReqIntArg1;
    int           m_ReqIntArg2;
    int           m_ReqIntArg3;
    int           m_Key[16];
    int           m_KeyIn;
    int           m_KeyOut;
    int           m_NeedUpdate;
    int           m_CtrlCEraseLine;
    int           m_LastWasAltT;
    char          m_CmdBuf[512];
    char          m_HistBuf[512];
    char          m_PasteBuf[512];
    int           m_CmdIndex;

  protected:
    void          MoveFocusWinLeft(void);
    void          MoveFocusWinRight(void);
    virtual int   GetSourceTopLineNo(void) { return m_SourceFirstLine; }
    virtual int   SetSourceTopLineNo(int lineNo);
    void          NextWindowFocus(void);
    int           SetSourceFocus(void);
    int           ProcessAsSpecialModeKey(int key, char *pBuffer, int maxSize,
                      int &index);
    void          RedrawSourceFrame(void);
    void          ResizeWindows(void);
    void          ProcessTabKey(char *pBuffer, int maxSize);
    void          DoFirstTabSearch(char *pBuffer, int maxSize);
    void          DoSecondTabSearch(char *pBuffer, int maxSize);
    void          PrintFormatStr(WINDOW *pWin, int line, int col,
                      const char *pBuf, bool expandColor = true);
    int           CalculateCmdCol(const char *pBuf, bool expandColor = true, int maxLen = -1);
    void          CommandProcessLastLine(void);
    int           HandleCtrlR(char *pBuffer, int maxSize);
    int           ProcessCtrlRKey(int key, char *pBuffer, int &index, int maxSize);
    void          CancelCtrlR(void);
    void          RedrawScreen(void);
    int           DoReverseHistorySearch(bool searchPrev);
    void          ShowRSearchPrompt(const char *pFoundHist, int histIndex);

#ifdef STANDALONE
    int           MapKey(int key);
#endif

  /* Public Class variables */
  public:
    SCREEN *        m_pScreen;
    WINDOW *        m_pWin;
    WINDOW *        m_pMainMenu;
    WINDOW *        m_pCmdframe;
    WINDOW *        m_pCmdwin;
    WINDOW *        m_pSrcframe;
    WINDOW *        m_pSrcwin;
    WINDOW *        m_pFocuswin;
    WINDOW *        m_pRegframe;
    WINDOW *        m_pRegwin;

    uint8_t         m_Terminate;
    uint8_t         m_Terminated;
    uint16_t        m_CmdLineCount;
    uint16_t        m_CmdLineNo;
    uint16_t        m_CmdTopLine;
    int16_t         m_ScrollBackCount;
    uint16_t        m_CmdHeight;
    uint16_t        m_CmdWidth;
    uint16_t        m_CmdHeightPrev;
    uint16_t        m_CmdCol;
    uint16_t        m_CursorLine;
    uint16_t        m_CmdCurrentLine;
    uint16_t        m_CmdPrevLine;
    char          * m_CmdLines[TUI_CMD_LINES];

    uint16_t        m_ConnectCount;
    uint16_t        m_SrcWinPosition;
    uint16_t        m_CmdWinHeight;
    uint16_t        m_SourceFirstLine;
    uint16_t        m_SourceWindowLineCount;
    uint16_t        m_RegWindowWidth;
    uint16_t        m_Focus;
    CSimEvent       m_CursesLock;
    uint16_t        m_FirstTab;
    uint16_t        m_BreakpointRestoreInProgress;
    uint16_t        m_Lines;
    uint16_t        m_Cols;
    uint16_t        m_CtrlRActive;
    char            m_CtrlRText[64];
    int             m_CtrlRHistIndex;
    int             m_KeyDebug;
    int             m_WatchPaused;

    uint16_t        m_ThreadStarted;

    const char *    m_pPrompt;
    char            m_SourceFilename[256];

    bool            m_CursesInitialized;
    bool            m_CursesError;

    CHistory        m_History;
    int             m_HistoryIndex;

    CTuiSource    * m_pSrc;
    TuiSortList_t * m_pTabList;
    char            m_TabCmd[64];
};

#endif /* _TUI_TUI_H */

// vim: et sw=2 ts=2 cindent

