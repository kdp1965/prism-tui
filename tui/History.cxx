/************************************************************************************
 * tui/History.cxx
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

#include <string.h>
#include <stdio.h>
#include "History.h"

#ifndef NULL
#define NULL  0
#endif

/* Byte-at-a-time string ops for buffers at ARBITRARY alignment.
 *
 * newlib's optimized strcpy/strcmp/strlen read a word at a time, and
 * this core's unaligned word loads do not return what they expect: a
 * history value parsed out of "HISTORY_10=..." starts at an ODD line
 * offset, and newlib's strcpy saw a phantom NUL in the misread word and
 * stopped after exactly three characters.  That is why every entry
 * numbered 10 and up came back as its first three letters ("ope",
 * "clo") while entries 0-9 survived - the one-digit prefix leaves the
 * value at an even offset.  Same erratum the VT100 key decoder hit
 * (tc_streq in tcurses_vt100.c).
 */
static int h_strlen(const char *p)
{
  int n = 0;

  while (p[n] != '\0')
    n++;
  return n;
}

static void h_strcpy(char *dst, const char *src)
{
  while ((*dst++ = *src++) != '\0')
    ;
}

static int h_streq(const char *a, const char *b)
{
  while (*a != '\0' && *a == *b)
  {
    a++;
    b++;
  }
  return *a == *b;
}

/*
==============================================================================
CHistory class constructor
==============================================================================
*/
CHistory::CHistory()
{
  int     x;

  m_Count          = 0;
  m_FirstItemIndex = -1;
  for (x = 0; x < MAX_HISTORY_ITEMS; x++)
    m_pHistory[x] = NULL;
}

/*
==============================================================================
CHistory class destructor
==============================================================================
*/
CHistory::~CHistory()
{
  int     x;

  for (x = 0; x < MAX_HISTORY_ITEMS; x++)
  {
    if (m_pHistory[x] != NULL)
      delete[] m_pHistory[x];
  }
}

/*
==============================================================================
Read history items from a preferences file
==============================================================================
*/
void CHistory::ReadHistory(const char *pFilename)
{
  FILE     *fd;
  char     line[256];
  char     *pref, *value, *savetok;

  // Try to open the p8sim resource file
  if ((fd = fopen(pFilename, "r")) == NULL)
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
    if (strncmp("HISTORY_", pref, 8) == 0)
    {
      // Add the history item
      Add(value);
    }
  }

  // Close the file
  fclose(fd);

}

/*
==============================================================================
Save history items to a preferences file
==============================================================================
*/
void CHistory::SaveHistory(const char *pFilename)
{
  FILE     *fd;
  int       x;
  int       index;

  if (m_Count == 0)
    return;

  // Try to open the p8sim resource file
  if ((fd = fopen(pFilename, "a+")) == NULL)
  {
    return;
  }

  // Save Source window position
  fseek(fd, 0, SEEK_END);

  index = m_FirstItemIndex + 1;
  if (index >= MAX_HISTORY_ITEMS)
    index = 0;

  while (m_pHistory[index] == NULL)
  {
    if (++index >= MAX_HISTORY_ITEMS)
      index = 0;
  }

  for (x = 0; x < m_Count; x++)
  {
    fprintf(fd, "HISTORY_%d=%s\n", x, m_pHistory[index++]);
    if (index >= MAX_HISTORY_ITEMS)
      index =0;
  }

  fclose(fd);
}

/*
==============================================================================
Save history items to a preferences file
==============================================================================
*/
void CHistory::Add(const char *pCmd)
{
  int     addIndex;

  /* Skip any leading spaces */
  while (*pCmd == ' ')
    pCmd++;

  /* Test if this command matches the last command */
  if (m_Count > 0 && m_FirstItemIndex != -1)
  {
    if (h_streq(pCmd, m_pHistory[m_FirstItemIndex]))
      return;
  }

  /* Get index where to add next item */
  addIndex = m_FirstItemIndex + 1;
  if (addIndex >= MAX_HISTORY_ITEMS)
    addIndex = 0;

  /* Test if we have an existing command pointer at m_FirstItemIndex */
  if (m_pHistory[addIndex] != NULL)
  {
    delete[] m_pHistory[addIndex];
    m_pHistory[addIndex] = NULL;
  }

  /* Add item at m_FirstItemIndex */
  m_pHistory[addIndex] = new char[h_strlen(pCmd) + 1];
  h_strcpy(m_pHistory[addIndex], pCmd);

  /* Increment the count if it is less than our max */
  if (m_Count < MAX_HISTORY_ITEMS)
    m_Count++;

  /* Update m_FirstItemIndex */
  m_FirstItemIndex = addIndex;
}

/*
==============================================================================
Save history items to a preferences file
==============================================================================
*/
int CHistory::GetFirstHistoryItem(const char **ppCmd)
{
  /* Validate we have items in the history */
  if (m_Count == 0)
    return -1;

  /* Get the first history item */
  *ppCmd = m_pHistory[m_FirstItemIndex];
  return m_FirstItemIndex;
}

/*
==============================================================================
Save history items to a preferences file
==============================================================================
*/
int CHistory::GetHistoryItem(int index, const char **ppCmd)
{
  /* Test if any history exists */
  if (m_Count == 0)
    return -1;

  /* Return pointer to next older history item (or current item if at end) */
  *ppCmd = m_pHistory[index];
  return index;
}

/*
==============================================================================
Save history items to a preferences file
==============================================================================
*/
int CHistory::GetOlderHistoryItem(int index, const char **ppCmd)
{
  int     nextIndex;

  /* Test if we are at the last item (i.e. no older items) */
  nextIndex = index - 1;
  if (nextIndex < 0)
    nextIndex += MAX_HISTORY_ITEMS;
  if (nextIndex == m_FirstItemIndex || m_pHistory[nextIndex] == NULL)
  {
    /* We are at the oldest item.  Simply return pointer to the same
     * history item that we are currently on so the history appears 
     * to get "stuck"
     */
    nextIndex = index; 
  }

  /* Return pointer to next older history item (or current item if at end) */
  *ppCmd = m_pHistory[nextIndex];
  return nextIndex;
}

/*
==============================================================================
Save history items to a preferences file
==============================================================================
*/
int CHistory::GetNewerHistoryItem(int index, const char **ppCmd)
{
  int     nextIndex;

  /* Test if we are at the first item (i.e. no newer items) */
  if (index == m_FirstItemIndex)
  {
    /* Return a NULL pointer */
    *ppCmd = NULL;
    return -1;
  }

  /* Get next newer item */
  if (index == MAX_HISTORY_ITEMS - 1)
    nextIndex = 0;
  else
    nextIndex = index + 1;

  /* Get a pointer to the history command */
  *ppCmd = m_pHistory[nextIndex];
  return nextIndex;
}


