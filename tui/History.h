/************************************************************************************
 * tui/History.h
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

#ifndef _TUI_HISTORY_H
#define _TUI_HISTORY_H

#define MAX_HISTORY_ITEMS   2000

/*
==============================================================================
Definition of our History class
==============================================================================
*/

class CHistory
{
  public:
    CHistory();
    ~CHistory();
     
  /* Public Class methods */

  public:
    void    ReadHistory(const char *pFilename);
    void    SaveHistory(const char *pFilename);

    void    Add(const char *pCmd);
    int     GetFirstHistoryItem(const char **ppCmd);
    int     GetHistoryItem(int index, const char **ppCmd);
    int     GetOlderHistoryItem(int index, const char **ppCmd);
    int     GetNewerHistoryItem(int index, const char **ppCmd);
    int     Count(void) { return m_Count; }

  private:
    int       m_Count;
    int       m_FirstItemIndex;
    char *    m_pHistory[MAX_HISTORY_ITEMS];
};

#endif /* _TUI_HISTORY_H */

