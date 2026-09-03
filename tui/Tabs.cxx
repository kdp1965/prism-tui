/************************************************************************************
 * tui/CTabs.cxx
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

#ifndef STANDALONE
#include <nuttx/config.h>
#include <graphics/curses.h>
#else
#include <curses.h>
#include <unistd.h>
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>


#include "Tui.h"
#include "Tabs.h"

/*
==============================================================================
CTab Class constructor
==============================================================================
*/
CTab::CTab(const char *name, CTabContainer *pContainer)
{
  strncpy(m_Name, name, sizeof(m_Name));
  m_pParent = pContainer;

  /* Create a new PANEL object */
  //m_pPanel    = new_panel(m_pParent->GetTabWindow());
  m_pNextTab         = NULL;
  m_pNextZTab        = NULL;

  /* TinyQV port: these were never initialized - on a reused heap block
   * m_TopLine came up as garbage, so a fresh tab drew no lines until
   * the first scroll key clamped it. */
  m_pTuiSource       = NULL;
  m_pTuiCtx          = NULL;
  m_TopLine          = 0;
}

/*
==============================================================================
Class destructor
==============================================================================
*/
CTab::~CTab()
{
  /* Destroy the PANEL */
//  if (m_pPanel != NULL)
//  {
//    del_panel(m_pPanel);
//  }
}

/*
==============================================================================
Set the tab as the focus
==============================================================================
*/
void CTab::SetFocus(void)
{
  /* Request the container to make our tab active */
  m_pParent->MakeTabActive(this);
}

/*
==============================================================================
Set the tab as the focus
==============================================================================
*/
CTab * CTab::GetPrevTab(void)
{
  /* Request the container to make our tab active */
  return m_pParent->GetPrevTab(this);
}

/*
==============================================================================
Get the continain window
==============================================================================
*/
WINDOW * CTab::GetWindow(void)
{
  /* Request the container to make our tab active */
  return m_pParent->GetTabWindow();
}

/*
==============================================================================
Get the number of source lines that fit on the window
==============================================================================
*/
int CTab::SourceWindowLineCount(void)
{
  int     row, col;

  getmaxyx(GetWindow(), row, col);

  /* Request the container to make our tab active */
  (void) col;
  return row;
}

/*
==============================================================================
Get the number of lines in the source document
==============================================================================
*/
int CTab::GetSourceLineCount(void)
{
  int   ret = 0;

  if (m_pTuiSource != NULL)
    ret = m_pTuiSource->GetSourceLineCount(m_pTuiCtx);

  return ret;
}

/*
==============================================================================
CTabContainer constructor
==============================================================================
*/
CTabContainer::CTabContainer(CTui *pParent, WINDOW *win)
{
  int   rows, cols;
  int   begx, begy;

  /* Save pointers  */
  m_pParent           = pParent;
  m_pWin              = win;
  m_pFirstTab         = NULL;
  m_pTopTab           = NULL;
  m_Selected          = 0;
  m_pFirstVisibleTab  = NULL;

  /* Create a tab window inset from the main window.  A SUBWIN, not a
   * newwin: subwindows share the parent's character buffer, so a
   * refresh of the frame re-emits the tab CONTENT instead of the
   * frame's blank interior.  With a separate buffer every frame
   * refresh blanked the tab area and the next tab redraw re-sent the
   * whole window - kilobytes of UART per event, long enough for the
   * design to drop incoming keys while transmitting.
   */
  getbegyx(m_pWin, begy, begx);
  getmaxyx(m_pWin, rows, cols);

  m_pTabWin = subwin(m_pWin, rows-3, cols-2, begy+2, begx+1);
  nodelay(m_pTabWin, TRUE);
}

/*
==============================================================================
CTabContainer destructor
==============================================================================
*/
CTabContainer::~CTabContainer()
{
  CTab* pTab;
  CTab* pNextTab;

  /* Delete all tabs in the container */
  pTab = m_pFirstTab;
  while (pTab)
  {
    /* Get pointer to next tab */
    pNextTab = pTab->m_pNextTab;
    delete pTab;
    pTab = pNextTab;
  }

  /* Delete the m_pTabWin */
  delwin(m_pTabWin);
}

/*
===================================================================================
Create a new tab within the container
===================================================================================
*/
CTab * CTabContainer::CreateNewTab(const char *name)
{
  CTab* pTab;
  CTab* pListTab;

  /* Create the CTab object */
  pTab = new CTab(name, this);
  if (pTab == NULL)
    return pTab;

  /* Add the new tab to the end of the list */
  if (m_pFirstTab == NULL)
  {
    m_pFirstTab = pTab;
  }  
  else
  {
    /* Find the end of the list */
    pListTab = m_pFirstTab;
    while (pListTab->m_pNextTab != NULL)
      pListTab = pListTab->m_pNextTab;

    /* Insert at end of the list */
    pListTab->m_pNextTab = pTab;
  }

  /* Make the tab first in Z order */
  pTab->m_pNextZTab = m_pTopTab;
  m_pTopTab = pTab;

  /* Find the first visible tab */
  FindFirstVisibleTab();

  Redraw();

  return pTab;
}

/*
===================================================================================
Delete the specified tab
===================================================================================
*/
void CTabContainer::DeleteTab(CTab *pTab)
{
  CTab* pListTab;

  /* Remove the tab from the tab list */
  if (m_pFirstTab == pTab)
  {
    m_pFirstTab = pTab->m_pNextTab;
  }  
  else
  {
    /* Find the end of the list */
    pListTab = m_pFirstTab;
    while (pListTab->m_pNextTab != pTab && pListTab->m_pNextTab != NULL)
      pListTab = pListTab->m_pNextTab;

    /* Remove pTab from the list */
    pListTab->m_pNextTab = pTab->m_pNextTab;
  }

  /* Remove the tab from the Z order list */
  if (m_pTopTab == pTab)
    m_pTopTab = pTab->m_pNextZTab;
  else
  {
    pListTab = m_pTopTab;
    while (pListTab->m_pNextTab != pTab && pListTab->m_pNextTab != NULL)
      pListTab = pListTab->m_pNextZTab;

    pListTab->m_pNextZTab = pTab->m_pNextZTab;
  }

  /* Delete the tab */
  delete pTab;

  /* Find the first visible tab */
  FindFirstVisibleTab();

  /* Refresh the tabs */
  Redraw();
}

/*
===================================================================================
Redraw the tabs
===================================================================================
*/
/*
===================================================================================
Follow the parent frame after it is resized.

CTui::ResizeWindows() wresize()s the source FRAME; the tab window is a
subwin of it and does not follow on its own - and worse, a subwin whose
parent was reallocated is left pointing at the old lines.  Rebuild it at
the frame's new geometry (TinyQV port: tabs postdate that resize path,
so before this a single ALT-+ left every tab drawing into hyperspace).
===================================================================================
*/
void CTabContainer::Resize(void)
{
  int rows, cols, begx, begy;

  if (m_pWin == NULL)
    return;

  getbegyx(m_pWin, begy, begx);
  getmaxyx(m_pWin, rows, cols);

  if (m_pTabWin != NULL)
    delwin(m_pTabWin);
  m_pTabWin = subwin(m_pWin, rows-3, cols-2, begy+2, begx+1);
  if (m_pTabWin == NULL)
    return;
  nodelay(m_pTabWin, TRUE);
  keypad(m_pTabWin, TRUE);              // panels read arrows from this window
}

void CTabContainer::Redraw(void)
{
  CTab  *pTab;
  int   w;
  int   h;
  int   x;
  int   tabx;
  int   tabend;
  int   tabEndX;
  char  text[512];

  getmaxyx(m_pWin, h, w);

  /* Put spaces in the first line */
  for (x = 0; x < w; x++)
    mvwaddch(m_pWin, 0,   x, ' ');

  if (m_Selected)
    wattron(m_pWin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  else
    wattron(m_pWin, COLOR_PAIR(SYNTAX_PAIR_COMMENT));

  /* Draw a box, inset on top by 1 line */
  mvwhline(m_pWin, 1,   0, 0, w);
  mvwhline(m_pWin, h-1, 0, 0, w);
  mvwvline(m_pWin, 1,   0,           0, h-2);
  mvwvline(m_pWin, 1,   w-1, 0, h-2);
  mvwaddch(m_pWin, 1,   0,   ACS_ULCORNER);
  mvwaddch(m_pWin, 1,   w-1, ACS_URCORNER);
  mvwaddch(m_pWin, h-1, 0,   ACS_LLCORNER);
  mvwaddch(m_pWin, h-1, w-1, ACS_LRCORNER);

  /* Start at first tab */
  pTab = m_pFirstVisibleTab;
  tabx = 2;

  if (pTab == NULL && m_pTuiSource)
  {
    /* Draw into the tab window, NOT the frame: splash pixels left in
     * the frame buffer get re-emitted by every later frame refresh,
     * painting over whatever tab content sits in the same screen area.
     */
    m_pTuiSource->DrawSplash(m_pTabWin);
  }

  /* If the first visible tab != first tab, show a ... partial tab */
  if (m_pFirstVisibleTab != m_pFirstTab)
  {
    wattron(m_pWin, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
    mvwprintw(m_pWin, 0, tabx,   "  ...  ");
    tabend = tabx + 7;

    mvwaddch(m_pWin, 0, tabx,   ACS_VLINE);
    mvwaddch(m_pWin, 0, tabend, ACS_VLINE);
    mvwaddch(m_pWin, 1, tabx,   ACS_BTEE);
    mvwaddch(m_pWin, 1, tabend, ACS_BTEE);
    mvwaddch(m_pWin, 0, tabx,   ACS_ULCORNER);
    mvwaddch(m_pWin, 0, tabend, ACS_URCORNER);
    tabx = tabend + 1;
  }

  while (pTab && tabx < w-1)
  {
    /* Draw the text */
    if (pTab == m_pTopTab)
      wattron(m_pWin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
    else
      wattron(m_pWin, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
    mvwprintw(m_pWin, 0, tabx,   "  ");

    snprintf(text, sizeof(text), "%s", pTab->m_Name);
    tabEndX = tabx+2+(int) strlen(text);
    if (tabEndX >= w)
      text[strlen(text) - (tabEndX - w) - 1] = 0;

    mvwprintw(m_pWin, 0, tabx+2, text);
    tabend = tabx+2+strlen(text);
    mvwprintw(m_pWin, 0, tabend,   "  ");
    tabend++;

    /* Test if this is the active (top) tab */
    if (pTab == m_pTopTab)
    {
      wattron(m_pWin, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
      for (x = tabx; x < tabend; x++)
        mvwaddch(m_pWin, 1,   x, ' ');

      mvwaddch(m_pWin, 0, tabx,   ACS_VLINE);
      mvwaddch(m_pWin, 0, tabend, ACS_VLINE);
      mvwaddch(m_pWin, 1, tabx,   ACS_LRCORNER);
      mvwaddch(m_pWin, 1, tabend, ACS_LLCORNER);
      mvwaddch(m_pWin, 0, tabx,   ACS_ULCORNER);
      mvwaddch(m_pWin, 0, tabend, ACS_URCORNER);
    }
    else
    {
      mvwaddch(m_pWin, 0, tabx,   ACS_VLINE);
      mvwaddch(m_pWin, 0, tabend, ACS_VLINE);
      mvwaddch(m_pWin, 1, tabx,   ACS_BTEE);
      mvwaddch(m_pWin, 1, tabend, ACS_BTEE);
      mvwaddch(m_pWin, 0, tabx,   ACS_ULCORNER);
      mvwaddch(m_pWin, 0, tabend, ACS_URCORNER);
    }

    wattron(m_pWin, COLOR_PAIR(SYNTAX_PAIR_COMMENT));

    /* Advance to next tab */
    tabx = tabend + 1;

    /* Get pointer to next tab */
    pTab = pTab->m_pNextTab;
  }

  wrefresh(m_pWin);
}

/*
===================================================================================
Change the top (active) tab to the tab given
===================================================================================
*/
void CTabContainer::MakeTabActive(CTab *pTab)
{
  CTab  *pTemp;

  /* Test for NULL tab.  This can happen when we GetNext()
   * or GetPrev() tab at the end of the list.
   */
  if (pTab == NULL)
    return;

  /* Get the curret top tab */
  if (m_pTopTab != pTab)
  {
    /* Remove this tab from the Z order list */
    pTemp = m_pTopTab;
    while (pTemp && pTemp->m_pNextZTab != pTab)
      pTemp = pTemp->m_pNextZTab;
    if (pTemp == NULL)
    {
      printf("ERROR internal tab linked list corruption\n");
      return;
    }

    pTemp->m_pNextZTab = pTab->m_pNextZTab;
    pTab->m_pNextZTab = m_pTopTab;
    m_pTopTab = pTab;

    /* Find the first visible tab */
    FindFirstVisibleTab();

    // Redraw the tabs
    Redraw();
  }
}

/*
===================================================================================
Get previous tab in the left/right ordered list
===================================================================================
*/
CTab * CTabContainer::GetPrevTab(CTab *pTab)
{
  CTab  *pTemp;

  /* Test for NULL pointers */
  if (pTab == NULL)
    return NULL;

  pTemp = m_pFirstTab;
  while (pTemp && pTemp->m_pNextTab != pTab)
    pTemp = pTemp->m_pNextTab;
  
  return pTemp;
}

/*
===================================================================================
Delete the specified tab
===================================================================================
*/
void CTabContainer::FindFirstVisibleTab(void)
{
  int   w;
  int   h;
  int   width;
  CTab  *pTab;

  m_pFirstVisibleTab = m_pFirstTab;
  getmaxyx(m_pWin, h, w);
  (void) h;

  // Start with the first tab
  pTab = m_pFirstTab;
  width = 0;

  // Loop until no more tabs
  while (pTab != NULL)
  {
    // Add this tab's size to the width
    width += 5 + strlen(pTab->m_Name);

    // Break the loop  if this tab is the top tab
    if (pTab == m_pTopTab)
      break;

    // Get next tab
    pTab = pTab->m_pNextTab;
  }

  // Now advance m_pFirstVisibleTab until width fits on the screen
  while (width-2 > w)
  {
    // If m_pFirstVisible tab is the first tab, add the size of
    // the "..." tab to width
    if (m_pFirstVisibleTab == m_pFirstTab)
      width += 6;

    m_pParent->m_pSrc->DebugPrintf("%-40swidth:%3d  w:%d\n", pTab->m_Name,
        width,  w);

    // Remove m_pFirstVisibleTab's width from the caculation
    width -= 5 + strlen(m_pFirstVisibleTab->m_Name);

    // Advance to next tab
    m_pFirstVisibleTab = m_pFirstVisibleTab->m_pNextTab;
  }
}

// vim: sw=2 ts=2 et cindent

