/************************************************************************************
 * tui/TuiSource.h
 *
 *   Copyright (C) 2019 Ken Pettit. All rights reserved.
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

#ifndef _TUI_TUISOURCE_H
#define _TUI_TUISOURCE_H

#ifdef STANDALONE
#include <curses.h>
#else
#include <graphics/curses.h>
#endif

/*
==============================================================================
Definition of our Tui class
==============================================================================
*/

class CTui;
class CTuiSource;
class CTab;

typedef int (CTuiSource::*CTuiSourceFunc_t)(int argc, char* argv[]);

typedef struct TuiCmd
{
   const char *      name;
   int               min_args;
   int               max_args;
   CTuiSourceFunc_t  pFunc;
   const char *      usage;
   const char *      help;
} TuiCmd_t;

typedef struct TuiSortItem
{
  const char*         name;
  struct TuiSortItem *pNext;
} TuiSortItem_t;

typedef struct TuiSortList
{
  struct TuiSortItem *pFirst;
} TuiSortList_t;

class CTuiSource
{
  public:
    CTuiSource() { m_pTab = NULL; }
    virtual ~CTuiSource() {}
     
  /* Public Class methods */

  public:
    virtual char *            GetFilename(void) { return m_Filename; }
    virtual const char *      GetPrefsFilename(void) { return m_PrefsFile; }
    virtual const char *      GetWorkingDir(void) { return m_WorkingDir; }
    virtual int               GetSourceLineCount(void *pCtx) = 0;
    virtual void              DrawSourceWindow(void *pCtx, WINDOW* pWnd, int topLine, int lineCount) = 0;
    virtual void              DrawWatchWindow(WINDOW* pWnd, int topLine) = 0;
    virtual const TuiCmd_t *  GetCommandTable(void) = 0;
    virtual int               GetCommandTabList(char *pCmd, const char *pBuffer,
                                TuiSortList_t *&pList) = 0;
    virtual void              FreeTabList(TuiSortList_t *pList) = 0;
    virtual bool              WantProcessKey(void) { return false; }
    virtual int               ProcessKey(int key) { return 0; }

    // Horizontal pan of the active tab's text area.  delta is +/-1 pan
    // requests; returns the 0-based window column where pannable text
    // begins, or -1 when the tab cannot pan (or is already at a limit).
    virtual int               HScrollSource(void *pCtx, int delta)
                                { (void)pCtx; (void)delta; return -1; }

    // Columns at the right edge excluded from horizontal panning (a
    // split-pane display such as the FSM diagram lives there)
    virtual int               HScrollClip(void *pCtx)
                                { (void)pCtx; return 0; }
    virtual int               ProcessLine(char *line) { return -1; };
    virtual void              CloseTab(CTab *pTab) { }
    virtual void              DrawSplash(WINDOW *pWnd) { }
    virtual void              DebugPrintf(const char *fmt, ...) = 0;
    virtual void              SingleStep(void) { };
    virtual void              StepOver(void) { };
    virtual void              Cont(void) { };
    virtual int               WantFocus(void) { return 0; }
    virtual void              SetFocus(void *pCtx, WINDOW* pWnd, int topLine, int lineCount) {};
    virtual void              SetParent(CTui *pParent) { m_pParent = pParent; }
    virtual void              UIReady(void) { }     // curses is up, before the key loop
    virtual void              IdlePoll(void) { }    // called from the idle key loop
    //virtual void              ProcessKey(void *pCtx, WINDOW* pWnd, int topLine, int lineCount, int key) {};
    virtual int               HandleCtrlC(void) { return 0; }
    // Horizontal scroll request (SHIFT/CTRL + arrows) for sources whose
    // content is wider than the window - cells is signed, and 'page'
    // asks for a windowful rather than a step.  Return true if the view
    // moved, so the caller knows to redraw.
    virtual bool              ScrollSource(int dir, bool page) { return false; }
    virtual void              SaveWatchItems(FILE *fd) { }
    virtual void              RestoreWatchItem(char *pStr) { }
    virtual void              RestoreOtherPref(char *pPref, char *pStr) { }
                  
  public:
    CTui             *  m_pParent;
    CTab             *  m_pTab;

  protected:
    char                m_Filename[256];
    const char *        m_WorkingDir;
    const char *        m_PrefsFile;
};

#endif /* _TUI_TUISOURCE_H */

