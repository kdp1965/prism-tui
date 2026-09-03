/************************************************************************************
 * tui/Prism.cxx
 *
 *   Copyright (C) 2026 Ken Pettit. All rights reserved.
 *   Author: Ken Pettit <pettitkd@gmail.com>
 *
 * CPrism: CTuiSource implementation for the TT Sky 25a PRISM on TinyQV.
 * See Prism.h for the design notes.
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

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "Tui.h"
#include "MidiFile.h"
#include "Prism.h"

extern "C" {
// Angle brackets: "prism.h" would hit our own Prism.h on macOS's
// case-insensitive filesystem; this resolves in the SDK include path.
#include <prism.h>
#include <csr.h>
#include <tqv_fs.h>
#include "../prism_tui.h"

// runtime.c: stdout redirection hook used by _write()
extern int (*__tinyqv_stdout_hook)(const char *buffer, int length);

// obj/chroma_lsts.c (generated): embedded chroma STEW listings and
// chroma Verilog sources
// Chroma listings and Verilog sources are READ FROM THE HOST at run time
// (tqv.py serves its --fs-root, default tinyQV-sdk/tqvfs; the Makefile
// stages the chroma tree into CHROMA_FS_SUBDIR there).  They used to be
// string literals linked into the image - 52KB of flash for files the
// host already has, and stale the moment a chroma was re-synthesized.
#define CHROMA_FS_SUBDIR   "chromas"
#define SOUND_FS_SUBDIR    "sounds"

// synth.h: note-progression sink ("C#4 " stream from demos/keypresses)
extern int (*synth_note_out)(const char *fmt, ...);
}

// The hook is a plain function pointer, so route through a singleton.
static CPrism *g_pPrism;

/*
==============================================================================
Notes tab: ring buffer of note-progression lines.  poly_note_on /
synth_note_on emit "C#4 " style chunks through synth_note_out; they are
folded into lines here and drawn by DrawSourceWindow when the Notes tab
is up (auto-following the newest line).
==============================================================================
*/

#define NOTE_LINES     48
#define NOTE_LINE_RAW  384  /* raw bytes: visible chars + ANSI escapes */
#define NOTE_VIS_DEF   44   /* visible-char wrap when no tab window yet */
#define NOTE_VIS_MAX   110  /* cap so worst-case escapes still fit RAW */

static char s_NoteRing[NOTE_LINES][NOTE_LINE_RAW + 1];
static int  s_NoteHead;         // line being filled
static int  s_NoteCount;        // total lines used (caps at NOTE_LINES)
static int  s_NoteLen;          // raw fill position in the head line
static int  s_NoteVis;          // VISIBLE chars in the head line (line
                                //   wrapping counts these, never the
                                //   color escape bytes)
static int  s_NoteEsc;          // mid-escape while appending
static int  s_NoteAbs;          // absolute index of the head line
// MIDI tab layout (the tab draw and GetSourceLineCount share these)
#define MIDI_HEAD_LINES 2               // title + blank
#define MIDI_TRK_LINES  4               // text line + 3 braille lines
#define MIDI_FOOT_LINES 4               // conversion settings footer
#define MIDI_TAB_GUTTER 3               // "12 " track number column
#define MIDI_PAIR(n)    (ROUTE_PAIR_BASE + ((n) % 5))

static int  s_NotesCtxMarker;   // &s_NotesCtxMarker = notes tab context
static int  s_WaveCtxMarker;    // &s_WaveCtxMarker = wave tab context

// Visible length of a chunk: everything except ANSI escape sequences
static int notes_vis_len(const char *s)
{
  int vis = 0;

  while (*s != 0)
  {
    if (*s == '\x1b')
    {
      s++;
      if (*s == '[')
        s++;
      while (*s != 0 && !isalpha((unsigned char)*s))
        s++;
      if (*s != 0)
        s++;                    // final letter of the sequence
      continue;
    }
    vis++;
    s++;
  }
  return vis;
}

/*
==============================================================================
Command table.  Every console command appears here so tab completion and
'help' cover the whole CLI; almost all forward to console.c's dispatcher
(Legacy).  min_args of 0 disables CTui's argc validation - the legacy
handlers do their own checking and usage prints.

NOTE: CTui::ProcessCommand prefix-matches the typed word against this
table in order, so keep it alphabetical and list exact names.
==============================================================================
*/

typedef struct PrismCmd
{
   const char *      name;
   int               min_args;
   int               max_args;
   CPrismFunc_t      pFunc;
   const char *      usage;
   const char *      help;
} PrismCmd_t;

static const PrismCmd_t s_TuiCmds[] =
{
  { "bc",        0, 1, &CPrism::Legacy, "bc <0|1>",          "Clear breakpoint" },
  { "automap",   0, 0, &CPrism::Automap,"automap",           "Guess melody/bass/pad/drums from the open MIDI" },
  { "bp",        0, 2, &CPrism::Legacy, "bp <0|1> <state>",  "Set breakpoint on state" },
  { "cat",       1, 1, &CPrism::Legacy, "cat <file>",        "Print a host file (tqv.py serves the directory)" },
  { "cfgr",      0, 0, &CPrism::Legacy, "cfgr",              "Read config stage STEW (msw lsw)" },
  { "cfgtest",   0, 0, &CPrism::Legacy, "cfgtest",           "Config space pattern self test" },
  { "cfgw",      0, 2, &CPrism::Legacy, "cfgw <msw> <lsw>",  "Shift one STEW into the config array" },
  { "chromas",   0, 0, &CPrism::Legacy, "chromas",           "List chroma table" },
  { "clear",     0, 0, &CPrism::Clear,  "clear",             "Clear the command window" },
  { "clk",       0, 1, &CPrism::Legacy, "clk [MHz]",         "Set/show the real project clock (playback math)" },
  { "close",     0, 0, &CPrism::Close,  "close",             "Close the active source tab" },
  { "cmp",       0, 1, &CPrism::Legacy, "cmp [v]",           "Read / set count2 compare" },
  { "cnt",       0, 0, &CPrism::Legacy, "cnt",               "Read live count1 / count2" },
  { "comm",      0, 1, &CPrism::Legacy, "comm [v]",          "Read / set 8-bit comm register" },
  { "cpptest",   0, 0, &CPrism::Legacy, "cpptest",           "Run the C++ support smoke test" },
  { "ctrl",      0, 1, &CPrism::Legacy, "ctrl [v]",          "Read / write CTRL register" },
  { "dbg",       0, 0, &CPrism::Legacy, "dbg",               "Show DBG_CTRL and DBG_STAT" },
  { "dbgw",      0, 1, &CPrism::Legacy, "dbgw <v>",          "Raw write DBG_CTRL" },
  { "dis",       0, 0, &CPrism::Legacy, "dis",               "Disable PRISM" },
  { "download",  0, 0, &CPrism::Legacy, "download",          "Receive a song over UART into RAM B" },
  { "downloadf", 0, 0, &CPrism::Legacy, "downloadf",         "Fast song load via tqv.py console (restarts design)" },
  { "en",        0, 0, &CPrism::Legacy, "en",                "Enable PRISM" },
  { "fifo",      0, 1, &CPrism::Legacy, "fifo [rd]",         "FIFO status / pop one byte" },
  { "flashy",    0, 2, &CPrism::Legacy, "flashy",            "WS2812 Neopixel Jewel demo" },
  { "fat",       0, 0, &CPrism::Legacy, "fat",               "List the RAM B / flash allocation tables" },
  { "fatdel",    1, 1, &CPrism::Legacy, "fatdel <name>",     "Remove a RAM table entry" },
  { "fatfmt",    0, 0, &CPrism::Legacy, "fatfmt",            "Format (empty) the RAM allocation table" },
  { "fs",        0, 1, &CPrism::Legacy, "fs [probe]",        "Host filesystem status / re-probe" },
  { "go",        0, 0, &CPrism::Legacy, "go",                "Resume the FSM from halt" },
  { "halt",      0, 0, &CPrism::Legacy, "halt",              "Halt the FSM" },
  { "help",      0, 1, &CPrism::Help,   "help [cmd]",        "Paged list; 'help <cmd>' one entry, 'help legacy' console text" },
  { "id",        0, 0, &CPrism::Legacy, "id",                "Read PRISM ID register" },
  { "io",        0, 0, &CPrism::Legacy, "io",                "Live input / output vectors and pins" },
  { "irq",       0, 1, &CPrism::Legacy, "irq [clr|en|dis]",  "Interrupt status / clear / enable / disable" },
  { "inst",      0, 2, &CPrism::Inst,   "inst <role|chN> <name>", "Role or MIDI-channel instrument ('inst' lists)" },
  { "load",      0, 2, &CPrism::Legacy, "load <n> [-v]",     "Load chroma n (-v validates previous)" },
  { "ls",        0, 1, &CPrism::Legacy, "ls [dir]",          "List the host filesystem directory" },
  { "map",       0, 2, &CPrism::Map,    "map <role> <ch|off|+ch|-ch>", "MIDI channels -> roles (melody/bass/pad/drums/satb)" },
  { "midi",      0, 3, &CPrism::Legacy, "midi [-i] <file>",  "Play a MIDI file (served songs/; GM mapped, ch10 = drum kit)" },
  { "notes",     0, 0, &CPrism::Notes,  "notes",             "Open the synth note-progression tab" },
  { "open",      0, 3, &CPrism::Open,   "open <x>[.v|.spk]", "Open a chroma listing/.v tab or a sound pack (.spk / ram|flash [name]; -w plays with scope)" },
  { "pins",      0, 1, &CPrism::Legacy, "pins <mask>",       "Route uo_out pins in mask to the PRISM" },
  { "play",      0, 3, &CPrism::Legacy, "play [-w] [name] [arg]", "Play a FAT song; bare 'play' on a MIDI tab converts & plays it" },
  { "pre",       0, 1, &CPrism::Legacy, "pre [v]",           "Read / set 24-bit count1 preload" },
  { "rd",        0, 1, &CPrism::Legacy, "rd <a>",            "Raw 32-bit read" },
  { "rdb",       0, 1, &CPrism::Legacy, "rdb <a>",           "Raw 8-bit read" },
  { "print",     0, 1, &CPrism::Print,  "print [signal]",    "Show a parsed in/out pin's current value" },
  { "regs",      0, 0, &CPrism::Legacy, "regs",              "Dump all PRISM registers" },
  { "resume",    0, 1, &CPrism::Legacy, "resume [-w]",       "Continue the last keypress-stopped song" },
  { "selftest",  0, 0, &CPrism::Legacy, "selftest",          "Run the scripted test sequence" },
  { "hide",      1, 1, &CPrism::Hide,   "hide fsm",          "Hide the FSM diagram (split pane or FSM tab)" },
  { "show",      1, 2, &CPrism::Show,   "show <state>|fsm [tab]", "Center a state's code / toggle the FSM diagram ('tab' forces its own tab)" },
  { "si",        0, 1, &CPrism::Legacy, "si [n]",            "Show state index / force state while halted" },
  { "speed",     0, 1, &CPrism::Legacy, "speed <v>",         "Set DSM playback speed" },
  { "step",      0, 1, &CPrism::Legacy, "step [n]",          "Single step n times (default 1)" },
  { "synth",     0, 3, &CPrism::Legacy, "synth [demo [n]]",  "80's synthesizer ('synth demo 0' lists songs)" },
  { "synthp",    0, 4, &CPrism::Legacy, "synthp [-i] [demo [n]]", "3-voice polyphonic PWL synth (-i: 2x interpolated out)" },
  { "tog",       0, 0, &CPrism::Legacy, "tog",               "Auto-toggle host_in[0] (clears irq)" },
  { "trim",      0, 2, &CPrism::Trim,   "trim <n> [bars]",   "Cut the first n beats (or bars) of the open MIDI" },
  { "vcfg",      0, 1, &CPrism::Legacy, "vcfg [n]",          "Verify config vs chroma n" },
  { "wr",        0, 2, &CPrism::Legacy, "wr <a> <v>",        "Raw 32-bit write" },
  { "wrb",       0, 2, &CPrism::Legacy, "wrb <a> <v>",       "Raw 8-bit write" },
  { NULL,        0, 0, NULL,            NULL,                NULL }
};

/*
==============================================================================
Construction
==============================================================================
*/

CPrism::CPrism()
{
  m_pCmdTabList     = NULL;
  m_WatchValCol     = 9;        // watch pane: values line up here
  m_BpState[0]      = -1;
  m_BpState[1]      = -1;
  m_LastHalted      = -1;
  m_HaltPollAt      = 0;
  m_OutLen          = 0;
  m_OutOpen         = false;
  m_cstyle_comment  = 0;
  m_Filename[0]     = 0;
  m_WorkingDir      = "/";

  // CTui reads/writes this file itself (window layout + command history)
  // through stdio, which reaches the directory tqv.py serves.  Empty when
  // no host is serving one - fopen would fail anyway, but settling it
  // here keeps the framework from retrying on every save.  Re-probe
  // rather than trust the cache: the console can be detached and
  // reattached (or reattached with --no-fs) between TUI sessions.
  // Named prism.cfg so a served directory shared with the PWL synth
  // TUI (tui.cfg) does not mix the two watch lists.
  tqv_fs_reprobe();
  m_PrefsFile       = tqv_fs_available() ? "prism.cfg" : "";
  g_pPrism          = this;
}

CPrism::~CPrism()
{
  adpcm_carrier_release();      // 'exit' with a pack tab still open
  RemoveStdoutHook();
  if (m_pCmdTabList != NULL)
  {
    // Free the persistent list for real
    TuiSortItem_t *pItem = m_pCmdTabList->pFirst;
    while (pItem != NULL)
    {
      TuiSortItem_t *pNext = pItem->pNext;
      free(pItem);
      pItem = pNext;
    }
    free(m_pCmdTabList);
    m_pCmdTabList = NULL;
  }
  if (g_pPrism == this)
    g_pPrism = NULL;
}

/*
==============================================================================
Stdout redirection: printf() output from legacy commands lands here (via
runtime.c's __tinyqv_stdout_hook) and is folded into the command window
line by line.  UICommandPrintString starts a new line per call, so track
whether the current line is still "open" for appends.
==============================================================================
*/

static int prism_stdout_hook(const char *buffer, int length)
{
  if (g_pPrism != NULL)
    g_pPrism->StdoutChunk(buffer, length);
  return length;
}

static int prism_note_sink(const char *fmt, ...)
{
  char    buf[64];
  va_list ap;
  int     len;

  va_start(ap, fmt);
  len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (g_pPrism != NULL)
    g_pPrism->NoteChunk(buf);
  return len;
}

void CPrism::InstallStdoutHook(void)
{
  m_OutLen  = 0;
  m_OutOpen = false;
  __tinyqv_stdout_hook = prism_stdout_hook;
  synth_note_out       = prism_note_sink;
}

void CPrism::RemoveStdoutHook(void)
{
  if (__tinyqv_stdout_hook == prism_stdout_hook)
    __tinyqv_stdout_hook = NULL;
  if (synth_note_out == prism_note_sink)
    synth_note_out = NULL;
}

// Draw note-ring text starting at a given visible column, translating
// the ANSI color escapes the synth emits (\x1b[93m = bright yellow
// melody note, \x1b[0m = reset) into curses attributes.  Anything else
// after ESC[ up to the final letter is swallowed.  Tokens are
// self-contained (escape on + text + reset), so a span can start at any
// token boundary with attributes off.
static void notes_draw_span(WINDOW *pWnd, int row, const char *line,
                            int col, int maxcols)
{
  while (*line != 0 && col < maxcols)
  {
    if (line[0] == '\x1b' && line[1] == '[')
    {
      const char *p = line + 2;
      while (*p != 0 && (*p == ';' || (*p >= '0' && *p <= '9')))
        p++;
      if (*p == 'm')
      {
        if (line[2] == '9' && line[3] == '3')
          wattron(pWnd, A_BOLD | COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL));
        else
          wattroff(pWnd, A_BOLD | COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL));
      }
      line = (*p != 0) ? p + 1 : p;
      continue;
    }
    mvwaddch(pWnd, row, col++, (chtype)(unsigned char)*line++);
  }
  // never leak the attribute into other drawing
  wattroff(pWnd, A_BOLD | COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL));
}

static void notes_advance_line(void)
{
  s_NoteRing[s_NoteHead][s_NoteLen] = 0;
  s_NoteHead = (s_NoteHead + 1) % NOTE_LINES;
  s_NoteLen = 0;
  s_NoteVis = 0;
  s_NoteEsc = 0;
  s_NoteRing[s_NoteHead][0] = 0;
  if (s_NoteCount < NOTE_LINES)
    s_NoteCount++;
  s_NoteAbs++;
}

void CPrism::NoteChunk(const char *text)
{
  bool dirty   = false;
  bool scrolled = false;
  int  tlen    = (int)strlen(text);
  int  tvis    = notes_vis_len(text);
  int  budget  = NOTE_VIS_DEF;

  // Wrap against the VISIBLE width of the notes window (color escape
  // bytes never count toward line length), so lines use the whole tab.
  if (m_pParent != NULL)
  {
    CTab *pTab;
    for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
         pTab = pTab->GetNextTab())
    {
      if (pTab->SourceContext() == &s_NotesCtxMarker)
      {
        WINDOW *pWnd = pTab->GetWindow();
        if (pWnd != NULL)
        {
          int rows, cols;
          getmaxyx(pWnd, rows, cols);
          (void)rows;
          budget = cols - 1;
        }
        break;
      }
    }
  }
  if (budget > NOTE_VIS_MAX)
    budget = NOTE_VIS_MAX;

  // Keep whole tokens (one note + its color escapes) on one line so an
  // escape sequence never splits across the ring wrap.
  if ((tvis <= budget && s_NoteVis + tvis > budget) ||
      s_NoteLen + tlen > NOTE_LINE_RAW)
  {
    notes_advance_line();
    dirty    = true;
    scrolled = true;
  }

  while (*text != 0)
  {
    char ch = *text++;

    if (ch == '\n' || s_NoteLen >= NOTE_LINE_RAW)
    {
      // Line complete (or raw storage full): advance the ring
      notes_advance_line();
      dirty    = true;
      scrolled = true;
    }
    if (ch != '\n')
    {
      s_NoteRing[s_NoteHead][s_NoteLen++] = ch;
      s_NoteRing[s_NoteHead][s_NoteLen] = 0;
      // visible-length accounting: skip escape sequence bytes
      if (s_NoteEsc)
      {
        if (isalpha((unsigned char)ch))
          s_NoteEsc = 0;        // final letter ends the sequence
      }
      else if (ch == '\x1b')
        s_NoteEsc = 1;
      else
        s_NoteVis++;
      dirty = true;
    }
  }

  // Live update when the Notes tab is on screen.  The poly synth's ISR
  // refill budget is tight, so playback updates only ever touch ONE
  // window line: lines live at fixed circular rows (abs % visible), so
  // even a completed line just moves the write position - no scroll,
  // no full repaint (a full-window diff costs enough CPU to cause
  // audio underruns).
  if (dirty && m_pParent != NULL)
  {
    CTab *pTab = m_pParent->GetActiveSrcTab();
    if (pTab != NULL && pTab->SourceContext() == &s_NotesCtxMarker)
    {
      (void)scrolled;
      NoteLineUpdate();
    }
  }
}

// Last state the head line was drawn with, for delta updates
static int s_DrawnAbs = -1;
static int s_DrawnLen;
static int s_DrawnVis;

void CPrism::NoteLineUpdate(void)
{
  CTab   *pTab = m_pParent->GetActiveSrcTab();
  WINDOW *pWnd;
  int     rows, cols, vis, row;

  if (pTab == NULL)
    return;
  pWnd = pTab->GetWindow();
  if (pWnd == NULL)
    return;

  getmaxyx(pWnd, rows, cols);
  vis = pTab->SourceWindowLineCount();
  if (vis > rows)
    vis = rows;
  if (vis <= 0)
    return;

  // The head line's row is fixed by its absolute index
  row = s_NoteAbs % vis;

  if (s_NoteAbs == s_DrawnAbs && s_NoteLen >= s_DrawnLen)
  {
    // Same line, text only appended: draw just the new span (per-note
    // UART cost stays a few bytes regardless of how full the line is)
    notes_draw_span(pWnd, row, &s_NoteRing[s_NoteHead][s_DrawnLen],
                    s_DrawnVis, cols - 1);
  }
  else
  {
    // New (recycled) row: clear it and draw from the start
    wmove(pWnd, row, 0);
    wclrtoeol(pWnd);
    notes_draw_span(pWnd, row, s_NoteRing[s_NoteHead], 0, cols - 1);
  }
  s_DrawnAbs = s_NoteAbs;
  s_DrawnLen = s_NoteLen;
  s_DrawnVis = s_NoteVis;
  wrefresh(pWnd);
}

void CPrism::StdoutChunk(const char *buffer, int length)
{
  int x;

  // Drop output once the UI is tearing down - the command window may
  // already be deleted (late prints from the exit path).
  if (m_pParent == NULL || m_pParent->m_Terminate)
    return;

  for (x = 0; x < length; x++)
  {
    char ch = buffer[x];

    if (ch == '\n')
    {
      // Complete the current line
      m_OutLine[m_OutLen] = 0;
      if (m_OutOpen)
        m_pParent->UICommandAppendString(m_OutLine);
      else
        m_pParent->UICommandPrintString(m_OutLine);
      m_OutLen  = 0;
      m_OutOpen = false;
    }
    else
    {
      if (m_OutLen < (int)sizeof(m_OutLine) - 1)
        m_OutLine[m_OutLen++] = ch;

      // Long running commands print progress with '\r'; flush so the
      // user sees it live (CommandProcessLastLine gives '\r' overwrite
      // semantics in the window).
      if (ch == '\r')
      {
        m_OutLine[m_OutLen] = 0;
        if (m_OutOpen)
          m_pParent->UICommandAppendString(m_OutLine);
        else
          m_pParent->UICommandPrintString(m_OutLine);
        m_OutLen  = 0;
        m_OutOpen = true;
      }
    }
  }
}

void CPrism::FlushStdoutLine(void)
{
  if (m_OutLen > 0 && m_pParent != NULL)
  {
    m_OutLine[m_OutLen] = 0;
    if (m_OutOpen)
      m_pParent->UICommandAppendString(m_OutLine);
    else
      m_pParent->UICommandPrintString(m_OutLine);
  }
  m_OutLen  = 0;
  m_OutOpen = false;
}

int CPrism::CmdPrintf(const char *fmt, ...)
{
  char    buf[256];
  va_list ap;
  int     len;

  va_start(ap, fmt);
  len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  StdoutChunk(buf, strlen(buf));
  return len;
}

void CPrism::DebugPrintf(const char *fmt, ...)
{
  // Key/debug chatter is not routed anywhere yet
  (void)fmt;
}

/*
==============================================================================
Command handling
==============================================================================
*/

const TuiCmd_t *CPrism::GetCommandTable(void)
{
  return (const TuiCmd_t *)&s_TuiCmds[0];
}

int CPrism::Legacy(int argc, char *argv[])
{
  char bpArg[8];

  // Breakpoints by STATE NAME, resolved through the loaded chroma's
  // parsed .v tab ('open pcm.v' first).  bp translates its state
  // argument; bc can name the state instead of the slot, matched
  // against the shadow of what each slot was set to.
  if (strcmp(argv[0], "bp") == 0 && argc >= 3 &&
      (argv[2][0] < '0' || argv[2][0] > '9'))
  {
    int si = FsmStateByName(argv[2]);

    if (si < 0)
    {
      CmdPrintf("no state '%s' in the loaded chroma's .v"
                " (open <chroma>.v first)\n", argv[2]);
      return OK;
    }
    snprintf(bpArg, sizeof(bpArg), "%d", si);
    argv[2] = bpArg;
  }
  if (strcmp(argv[0], "bp") == 0 && argc >= 3 &&
      (argv[1][0] == '0' || argv[1][0] == '1') && argv[1][1] == 0)
    m_BpState[argv[1][0] - '0'] = atoi(argv[2]);

  if (strcmp(argv[0], "bc") == 0 && argc >= 2 &&
      (argv[1][0] < '0' || argv[1][0] > '9'))
  {
    int si = FsmStateByName(argv[1]);
    int slot = -1;

    if (si >= 0)
      slot = m_BpState[0] == si ? 0 : m_BpState[1] == si ? 1 : -1;
    if (slot < 0)
    {
      CmdPrintf("no breakpoint set at '%s' ('bc 0' / 'bc 1' clears by"
                " slot)\n", argv[1]);
      return OK;
    }
    snprintf(bpArg, sizeof(bpArg), "%d", slot);
    argv[1] = bpArg;
  }
  if (strcmp(argv[0], "bc") == 0 && argc >= 2 &&
      (argv[1][0] == '0' || argv[1][0] == '1') && argv[1][1] == 0)
    m_BpState[argv[1][0] - '0'] = -1;

  // The synth engines stream their note progression to the Notes tab -
  // bring it up front before they start playing.
  if (strcmp(argv[0], "synth") == 0 || strcmp(argv[0], "synthp") == 0 ||
      strcmp(argv[0], "midi") == 0)
    EnsureNotesTab();

  // 'play -w' draws its waveform into its own Wave tab (created or
  // refocused here); the player gets that tab's terminal rectangle
  if (strcmp(argv[0], "play") == 0)
  {
    // Bare 'play' with a MIDI tab up: convert the mapping and play it
    // on the poly engine (pwl-tui's flow) instead of the FAT list
    CMidiFile *pMidi = argc < 2 ? ActiveMidi(false) : NULL;

    if (pMidi != NULL)
    {
      if (pMidi->m_Seq == NULL)
      {
        int n = MidiConvert(pMidi);

        if (n < 0)
        {
          CmdPrintf("convert failed\n");
          return -1;
        }
        CmdPrintf("(converted: %d events)\n", n);
      }
      if (pMidi->m_SeqCount == 0)
      {
        CmdPrintf("nothing mapped ('automap' or 'map' first)\n");
        return -1;
      }
      EnsureNotesTab();
      midi_play_events(pMidi->m_Seq, pMidi->m_SeqCount,
                       pMidi->m_Cvt.drums >= 0);
      FlushStdoutLine();
      ActivateMidiTab(pMidi);
      return OK;
    }
    if (argc >= 2 && strcmp(argv[1], "-w") == 0)
      EnsureWaveTab();
    m_pParent->ActiveTabRect(tui_tab_rect);
  }

  // Whether the core was halted BEFORE this command: a redundant
  // 'halt' must not yank a view the user scrolled elsewhere
  int wasHalted = prism_dbg_is_halted() ? 1 : 0;

  // Forward to the plain console dispatcher; its printf output comes
  // back through the stdout hook into the command window.
  cli_execute(argc, argv);
  FlushStdoutLine();

  // Debug commands move the FSM: bring the halted state into view in a
  // parsed .v tab, then refresh the listing highlight / arrow
  if (strcmp(argv[0], "halt") == 0 || strcmp(argv[0], "go") == 0 ||
      strcmp(argv[0], "step") == 0 || strcmp(argv[0], "si") == 0 ||
      strcmp(argv[0], "load") == 0 || strcmp(argv[0], "en") == 0 ||
      strcmp(argv[0], "dis") == 0)
  {
    m_LastHalted = prism_dbg_is_halted() ? 1 : 0;
    if (!(strcmp(argv[0], "halt") == 0 && wasHalted))
      FsmJumpToState();
    m_pParent->DrawSourceWindow();
  }
  return OK;
}

// One help row, paged against the command window.  Returns 0 once the
// user has asked to stop.
int CPrism::HelpEmit(const char *usage, const char *help,
                     int &onPage, int rows)
{
  char line[110];

  snprintf(line, sizeof(line), "  %-19s %s", usage, help);
  m_pParent->UICommandPrintString(line);

  // rows - 1: the pause prompt itself takes the last line of the window,
  // so a full page plus the prompt is exactly what fits
  if (++onPage >= rows - 1)
  {
    onPage = 0;
    if (m_pParent->PauseCmdListing() == 'q')
      return 0;
  }
  return 1;
}

int CPrism::Help(int argc, char *argv[])
{
  const PrismCmd_t *pCmd;
  char              line[110];
  int               rows, cols, onPage = 0, len;

  if (argc > 1 && strcmp(argv[1], "legacy") == 0)
  {
    char *help[2];
    char  cmd[8];

    // console.c's cmd_help text.  Build the argv rather than forwarding
    // &argv[1]: that hands the dispatcher argv[0] = "legacy", which it
    // rightly calls an unknown command.
    strcpy(cmd, "help");
    help[0] = cmd;
    help[1] = NULL;
    return Legacy(1, help);
  }

  // The command window is what the listing has to fit; it changes with
  // ALT-+/ALT-- and with the terminal, so ask for it every time
  m_pParent->GetCmdWinGeometry(rows, cols);
  (void)cols;
  if (rows < 3)
    rows = 3;                              // always make forward progress

  // 'help <name>': that command alone.  An exact name wins outright;
  // otherwise every command the text prefixes is shown, so 'help c'
  // still surveys the c's.
  if (argc > 1)
  {
    int matched = 0;

    len = (int)strlen(argv[1]);
    for (pCmd = s_TuiCmds; pCmd->name != NULL; pCmd++)
      if (strcmp(pCmd->name, argv[1]) == 0)
      {
        HelpEmit(pCmd->usage, pCmd->help, onPage, rows);
        return OK;
      }

    for (pCmd = s_TuiCmds; pCmd->name != NULL; pCmd++)
    {
      if (strncmp(pCmd->name, argv[1], len) != 0)
        continue;
      matched++;
      if (!HelpEmit(pCmd->usage, pCmd->help, onPage, rows))
        return OK;
    }
    if (matched == 0)
    {
      snprintf(line, sizeof(line), "No help for '%s' ('help' lists"
                                   " everything)", argv[1]);
      m_pParent->UICommandPrintString(line);
      return -1;
    }
    return OK;
  }

  for (pCmd = s_TuiCmds; pCmd->name != NULL; pCmd++)
    if (!HelpEmit(pCmd->usage, pCmd->help, onPage, rows))
      return OK;

  HelpEmit("exit | quit", "Leave the TUI (plain console resumes)",
           onPage, rows);
  return OK;
}

int CPrism::Clear(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  m_pParent->m_CmdLineCount   = 0;
  m_pParent->m_CmdCurrentLine = 0;
  m_pParent->m_CmdTopLine     = 0;
  m_pParent->m_CmdPrevLine    = 0;
  werase(m_pParent->m_pCmdwin);
  m_pParent->RedrawCommandWindow();
  return OK;
}

int CPrism::ProcessLine(char *line)
{
  // Everything known lives in the command table; unmatched lines are
  // genuinely unknown.
  (void)line;
  return -1;
}

int CPrism::HandleCtrlC(void)
{
  // The SDK's UART interrupt watcher (uart_rx_interrupt_seen) already
  // aborts long running legacy commands; nothing extra to do here.
  return 0;
}

/*
==============================================================================
Tab completion
==============================================================================
*/

void CPrism::AddTuiSortItem(TuiSortList_t *pList, const char *pStr)
{
  TuiSortItem_t *pItem;
  TuiSortItem_t *pCurr;
  TuiSortItem_t *pPrev;

  // Don't add duplicates
  for (pItem = pList->pFirst; pItem != NULL; pItem = pItem->pNext)
    if (strcmp(pItem->name, pStr) == 0)
      return;

  pItem = (TuiSortItem_t *)malloc(sizeof(TuiSortItem_t) + strlen(pStr) + 1);
  if (pItem == NULL)
    return;
  pItem->name = ((char *)pItem) + sizeof(TuiSortItem_t);
  strcpy((char *)pItem->name, pStr);

  // Sorted insert
  pPrev = NULL;
  pCurr = pList->pFirst;
  while (pCurr != NULL && strcmp(pCurr->name, pStr) < 0)
  {
    pPrev = pCurr;
    pCurr = pCurr->pNext;
  }
  pItem->pNext = pCurr;
  if (pPrev == NULL)
    pList->pFirst = pItem;
  else
    pPrev->pNext = pItem;
}

TuiSortList_t *CPrism::BuildListFromNames(const char *const *names)
{
  TuiSortList_t *pList;
  int            x;

  pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
  if (pList == NULL)
    return NULL;
  pList->pFirst = NULL;

  for (x = 0; names[x] != NULL; x++)
    AddTuiSortItem(pList, names[x]);

  return pList;
}

int CPrism::GetCommandTabList(char *pCmd, const char *pBuffer,
                              TuiSortList_t *&pList)
{
  (void)pBuffer;

  // Completing the command word itself: hand out the persistent list
  if (pCmd == NULL || pCmd[0] == 0)
  {
    if (m_pCmdTabList == NULL)
    {
      const PrismCmd_t *p;

      m_pCmdTabList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
      if (m_pCmdTabList == NULL)
        return 0;
      m_pCmdTabList->pFirst = NULL;
      for (p = s_TuiCmds; p->name != NULL; p++)
        AddTuiSortItem(m_pCmdTabList, p->name);
      AddTuiSortItem(m_pCmdTabList, "exit");
      AddTuiSortItem(m_pCmdTabList, "quit");
    }
    pList = m_pCmdTabList;
    return 1;
  }

  // Argument completion for commands with fixed keyword arguments
  if (strcmp(pCmd, "synth") == 0 || strcmp(pCmd, "synthp") == 0)
  {
    static const char *const names[] = { "demo", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "irq") == 0)
  {
    static const char *const names[] = { "clr", "dis", "en", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "fifo") == 0)
  {
    static const char *const names[] = { "rd", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "fs") == 0)
  {
    static const char *const names[] = { "probe", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "help") == 0)
  {
    static const char *const names[] = { "legacy", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "print") == 0)
  {
    // Parsed in/out signal names of the loaded chroma's .v
    pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
    if (pList == NULL)
      return 0;
    pList->pFirst = NULL;
    AddVarNames(pList);
    return pList->pFirst != NULL;
  }
  if (strcmp(pCmd, "bc") == 0 || strcmp(pCmd, "show") == 0 ||
      strcmp(pCmd, "bp") == 0)
  {
    // bp completes its STATE argument only once the slot is typed
    if (strcmp(pCmd, "bp") == 0)
    {
      const char *pArg = strchr(pBuffer, ' ');

      if (pArg == NULL)
        return 0;
      while (*pArg == ' ')
        pArg++;
      pArg = strchr(pArg, ' ');
      if (pArg == NULL)
        return 0;                       // still typing the slot number
    }
    pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
    if (pList == NULL)
      return 0;
    pList->pFirst = NULL;
    AddStateNames(pList);
    if (strcmp(pCmd, "show") == 0)
      AddTuiSortItem(pList, "fsm");
    return pList->pFirst != NULL;
  }
  if (strcmp(pCmd, "midi") == 0)
  {
    // .mid files in the served songs/ directory
    static char list[1024];
    int         got = tqv_fs_list("songs", list, sizeof(list) - 1);
    const char *ptr;

    pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
    if (pList == NULL)
      return 0;
    pList->pFirst = NULL;
    if (got < 0)
      got = 0;
    list[got] = 0;
    for (ptr = list; *ptr != 0; )
    {
      const char *nl = strchr(ptr, '\n');
      const char *fn = ptr;
      int         flen = nl ? (int)(nl - ptr) : (int)strlen(ptr);
      char        name[64];

      for (int i = 0; i < 2; i++)     // skip the "f <size> " columns
      {
        const char *sp = (const char *)memchr(fn, ' ',
                                              (size_t)flen - (fn - ptr));
        if (sp == NULL)
          break;
        fn = sp + 1;
      }
      flen -= (int)(fn - ptr);
      if (flen > 4 && strncmp(&fn[flen - 4], ".mid", 4) == 0 &&
          flen < (int)sizeof(name))
      {
        snprintf(name, sizeof(name), "%.*s", flen, fn);
        AddTuiSortItem(pList, name);
      }
      if (nl == NULL)
        break;
      ptr = nl + 1;
    }
    return pList->pFirst != NULL;
  }
  if (strcmp(pCmd, "play") == 0)
  {
    // Songs from BOTH allocation tables (RAM first; AddFatNames skips
    // a name the list already has, so an override shows only once)
    pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
    if (pList == NULL)
      return 0;
    pList->pFirst = NULL;
    AddFatNames(pList, TQFAT_RAM, false);
    AddFatNames(pList, TQFAT_FLASH, false);
    return pList->pFirst != NULL;
  }
  if (strcmp(pCmd, "open") == 0)
  {
    // 'open ram <TAB>' / 'open flash <TAB>': packs in that bank's table
    // (pBuffer holds the whole line; pCmd is only the first word)
    const char *pArg = pBuffer;

    while (*pArg == ' ')
      pArg++;
    pArg = strchr(pArg, ' ');
    if (pArg != NULL)
    {
      while (*pArg == ' ')
        pArg++;
      bool isRam   = strncmp(pArg, "ram ", 4) == 0;
      bool isFlash = strncmp(pArg, "flash ", 6) == 0;

      if (isRam || isFlash)
      {
        pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
        if (pList == NULL)
          return 0;
        pList->pFirst = NULL;
        AddFatNames(pList, isRam ? (const void *)TQFAT_RAM
                                 : (const void *)TQFAT_FLASH, true);
        return pList->pFirst != NULL;
      }
    }
    // Completion comes from the served directory, so it lists exactly
    // what 'open' can actually read right now
    static char list[512];
    int         got = tqv_fs_list(CHROMA_FS_SUBDIR, list, sizeof(list) - 1);
    const char *ptr;
    char        vname[48];

    pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
    if (pList == NULL)
      return 0;
    pList->pFirst = NULL;
    if (got < 0)
      got = 0;
    list[got] = 0;
    for (ptr = list; *ptr != 0; )
    {
      const char *nl = strchr(ptr, '\n');
      const char *fn = ptr;
      int         flen = nl ? (int)(nl - ptr) : (int)strlen(ptr);
      int         i;

      for (i = 0; i < 2; i++)          // skip the "f <size> " columns
      {
        const char *sp = (const char *)memchr(fn, ' ', (size_t)flen - (fn - ptr));
        if (sp == NULL)
          break;
        fn = sp + 1;
      }
      flen -= (int)(fn - ptr);
      if (flen > 11 && strncmp(fn, "chroma_", 7) == 0)
      {
        bool isv = fn[flen - 2] == '.' && fn[flen - 1] == 'v';
        snprintf(vname, sizeof(vname), "%.*s%s",
                 flen - 7 - (isv ? 2 : 4), fn + 7, isv ? ".v" : "");
        AddTuiSortItem(pList, vname);
      }
      if (nl == NULL)
        break;
      ptr = nl + 1;
    }
    // Sound packs complete by their file name; OpenSoundPack() finds
    // them under sounds/ on its own
    got = tqv_fs_list(SOUND_FS_SUBDIR, list, sizeof(list) - 1);
    if (got < 0)
      got = 0;
    list[got] = 0;
    for (ptr = list; *ptr != 0; )
    {
      const char *nl = strchr(ptr, '\n');
      const char *fn = ptr;
      int         flen = nl ? (int)(nl - ptr) : (int)strlen(ptr);
      int         i;

      for (i = 0; i < 2; i++)
      {
        const char *sp = (const char *)memchr(fn, ' ', (size_t)flen - (fn - ptr));
        if (sp == NULL)
          break;
        fn = sp + 1;
      }
      flen -= (int)(fn - ptr);
      if (flen > 4 && strncmp(&fn[flen - 4], ".spk", 4) == 0 &&
          flen < (int)sizeof(vname))
      {
        snprintf(vname, sizeof(vname), "%.*s", flen, fn);
        AddTuiSortItem(pList, vname);
      }
      if (nl == NULL)
        break;
      ptr = nl + 1;
    }
    // .mid files in the served songs/ directory open as MIDI tabs
    got = tqv_fs_list("songs", list, sizeof(list) - 1);
    if (got < 0)
      got = 0;
    list[got] = 0;
    for (ptr = list; *ptr != 0; )
    {
      const char *nl = strchr(ptr, '\n');
      const char *fn = ptr;
      int         flen = nl ? (int)(nl - ptr) : (int)strlen(ptr);
      int         i;

      for (i = 0; i < 2; i++)
      {
        const char *sp = (const char *)memchr(fn, ' ',
                                              (size_t)flen - (fn - ptr));
        if (sp == NULL)
          break;
        fn = sp + 1;
      }
      flen -= (int)(fn - ptr);
      if (flen > 4 && strncmp(&fn[flen - 4], ".mid", 4) == 0 &&
          flen < (int)sizeof(vname))
      {
        snprintf(vname, sizeof(vname), "%.*s", flen, fn);
        AddTuiSortItem(pList, vname);
      }
      if (nl == NULL)
        break;
      ptr = nl + 1;
    }
    return 1;
  }

  pList = NULL;
  return 0;
}

// Allocation-table names into a tab list: packs for 'open ram/flash',
// playable songs (everything else) for playr/playf
void CPrism::AddFatNames(TuiSortList_t *pList, const void *table, bool packs)
{
  const struct tqfat_table *t = tqfat_get(table);

  if (t == NULL)
    return;
  for (uint32_t i = 0; i < t->count; ++i)
  {
    const struct song_desc *d =
        (const struct song_desc *)(uintptr_t)t->e[i].addr;

    if (d->magic != SONG_MAGIC)
      continue;
    if ((d->kind == SONG_KIND_SPK) != packs)
      continue;

    // Skip a name the list already holds (RAM overrides flash)
    TuiSortItem_t *pDup = pList->pFirst;

    while (pDup != NULL && strcasecmp(pDup->name, t->e[i].name) != 0)
      pDup = pDup->pNext;
    if (pDup != NULL)
      continue;
    AddTuiSortItem(pList, t->e[i].name);
  }
}

// State names of the loaded chroma's parsed .v, for tab completion
void CPrism::AddStateNames(TuiSortList_t *pList)
{
  FsmInfo *f = LoadedFsm(NULL);

  if (f == NULL)
    return;
  for (int i = 0; i < f->nStates; i++)
    AddTuiSortItem(pList, f->st[i].name);
}

// Parsed in/out signal names, for 'print' tab completion
void CPrism::AddVarNames(TuiSortList_t *pList)
{
  FsmInfo *f = LoadedFsm(NULL);

  if (f == NULL)
    return;
  for (int i = 0; i < f->nVars; i++)
    AddTuiSortItem(pList, f->var[i].name);
}

void CPrism::FreeTabList(TuiSortList_t *pList)
{
  TuiSortItem_t *pItem;
  TuiSortItem_t *pNext;

  // The persistent command list survives for the session
  if (pList == NULL || pList == m_pCmdTabList)
    return;

  pItem = pList->pFirst;
  while (pItem != NULL)
  {
    pNext = pItem->pNext;
    free(pItem);
    pItem = pNext;
  }
  free(pList);
}

/*
==============================================================================
Source window: chroma STEW listing tabs.  The .lst decode tables are
embedded in flash (obj/chroma_lsts.c, generated by tools/lst2c.sh); one
tab per chroma, and while the FSM is halted the current SI's row is
highlighted (state N lives on listing line N + 3).
==============================================================================
*/

#define LST_STATE_LINE(si)  ((si) + 3)   // header is 3 lines

/*
==============================================================================
Verilog syntax highlighting, ported from the original pico16 CPrism
tokenizer: keywords blue, comments cyan, ALL_CAPS names yellow-ish
(REG pair), strings/numbers green, labels/compiler directives bold red.
Adjusted for Verilog: no '#' comments ('#10' is a delay), and 'h/'b/'d/'o
base literals tokenize as numbers instead of char strings.
==============================================================================
*/

static const char *gDeclarator[] =
{
   "`ifdef",
   "`ifndef",
   "`else",
   "`endif",
   "`define",
   "`include",
   "`timescale",
   "`default_nettype",
   NULL
};

static const char *gKeywords[] =
{
   "if",
   "else",
   "case",
   "casez",
   "casex",
   "wire",
   "reg",
   "signal",
   "begin",
   "end",
   "assign",
   "localparam",
   "parameter",
   "integer",
   "genvar",
   "initial",
   "input",
   "output",
   "inout",
   "module",
   "always",
   "posedge",
   "negedge",
   "or",
   "endcase",
   "endmodule",
   "function",
   "endfunction",
   "generate",
   "endgenerate",
   "default",
   NULL
};

void CPrism::AppendWS(char *pLine)
{
   int idx = (int)strlen(m_token);

   pLine += idx;
   while (*pLine == ' ' || *pLine == '\t')
   {
      m_token[idx++] = *pLine++;
      if (idx >= (int)sizeof(m_token) - 1)
        break;
   }
   m_token[idx] = 0;
}

char *CPrism::GetLineToken(char *pLine, int col, int &syntax)
{
   int   x;

   (void)col;

   // Test for end of string
   if (*pLine == '\0')
      return NULL;

   // Test for C style comments
   if (*pLine == '/' && *(pLine + 1) == '*')
   {
     m_cstyle_comment++;
   }

   // Process text within c-style comments
   if (m_cstyle_comment)
   {
     for (x = 0; pLine[x] && x < (int)sizeof(m_token) - 3; x++)
     {
       if (pLine[x] == '*' && pLine[x + 1] == '/')
       {
         m_token[x] = pLine[x];
         m_token[x + 1] = pLine[x + 1];
         x += 2;
         m_cstyle_comment--;
         break;
       }
       else
         m_token[x] = pLine[x];
     }
     m_token[x] = 0;
     syntax = SYNTAX_PAIR_COMMENT;
     return m_token;
   }

   // Test for keywords
   for (x = 0; gKeywords[x] != NULL; x++)
   {
      if (strncmp(gKeywords[x], pLine, strlen(gKeywords[x])) == 0)
      {
         int len = (int)strlen(gKeywords[x]);

         // Validate next byte isn't alnum
         if (!isalnum((unsigned char)pLine[len]) && pLine[len] != '_')
         {
            strcpy(m_token, gKeywords[x]);
            AppendWS(pLine);
            syntax = SYNTAX_PAIR_KEYWORD;
            return m_token;
         }
      }
   }

   // Test for compiler directives (`define and friends)
   for (x = 0; gDeclarator[x] != NULL; x++)
   {
      if (strncmp(gDeclarator[x], pLine, strlen(gDeclarator[x])) == 0)
      {
         int len = (int)strlen(gDeclarator[x]);

         if (!isalnum((unsigned char)pLine[len]) && pLine[len] != '_')
         {
            strcpy(m_token, gDeclarator[x]);
            AppendWS(pLine);
            syntax = SYNTAX_PAIR_LABEL;
            return m_token;
         }
      }
   }

   // Test for whitespace.  Simply print it using normal syntax
   if (*pLine == ' ' || *pLine == '\t')
   {
      for (x = 0; pLine[x] == ' ' || pLine[x] == '\t'; x++)
         m_token[x] = pLine[x];
      m_token[x] = 0;
      syntax = SYNTAX_PAIR_NORMAL;
      return m_token;
   }

   // Test for comment
   if (pLine[0] == '/' && pLine[1] == '/')
   {
      syntax = SYNTAX_PAIR_COMMENT;
      return pLine;
   }

   // Test for all CAPS (localparam state names etc.)
   if (isupper((unsigned char)*pLine))
   {
      for (x = 0; isupper((unsigned char)pLine[x]) ||
                  isdigit((unsigned char)pLine[x]) || pLine[x] == '_'; x++)
         m_token[x] = pLine[x];
      m_token[x] = 0;
      syntax = SYNTAX_PAIR_REG;
      return m_token;
   }

   // Verilog base literal: 'h3C, 'b0101, 'd15, 'o17 (checked before the
   // string rule so it doesn't eat the rest of the line)
   if (*pLine == '\'' && strchr("bBdDhHoO", pLine[1]) != NULL)
   {
      m_token[0] = pLine[0];
      m_token[1] = pLine[1];
      for (x = 2; isxdigit((unsigned char)pLine[x]) || pLine[x] == '_' ||
                  pLine[x] == 'x' || pLine[x] == 'z'; x++)
         m_token[x] = pLine[x];
      m_token[x] = 0;
      AppendWS(pLine);
      syntax = SYNTAX_PAIR_DECLARATOR;
      return m_token;
   }

   // Test for string
   if (*pLine == '"')
   {
      m_token[0] = *pLine;
      for (x = 1; pLine[x] != *pLine && pLine[x] != 0 &&
                  x < (int)sizeof(m_token) - 3; x++)
      {
         if (pLine[x] == '\\')
         {
            m_token[x] = pLine[x];
            x++;
         }
         m_token[x] = pLine[x];
      }
      if (pLine[x] != 0)
      {
         m_token[x] = pLine[x];
         x++;
      }
      m_token[x] = 0;
      AppendWS(pLine);
      syntax = SYNTAX_PAIR_DECLARATOR;
      return m_token;
   }

   // Test for punctuation
   if (ispunct((unsigned char)*pLine))
   {
      for (x = 0; ispunct((unsigned char)pLine[x]) && pLine[x] != '"'; x++)
         m_token[x] = pLine[x];
      m_token[x] = 0;
      AppendWS(pLine);
      syntax = SYNTAX_PAIR_NORMAL;
      return m_token;
   }

   // Test for numeric value
   if (isdigit((unsigned char)*pLine))
   {
      for (x = 0; isxdigit((unsigned char)pLine[x]) || pLine[x] == 'x'; x++)
         m_token[x] = pLine[x];
      m_token[x] = 0;
      AppendWS(pLine);
      syntax = SYNTAX_PAIR_DECLARATOR;
      return m_token;
   }

   // Test for normal text
   if (isalnum((unsigned char)*pLine))
   {
      for (x = 0; isalnum((unsigned char)pLine[x]) || pLine[x] == '_'; x++)
         m_token[x] = pLine[x];
      m_token[x] = 0;
      AppendWS(pLine);
      syntax = SYNTAX_PAIR_NORMAL;
      return m_token;
   }

   syntax = SYNTAX_PAIR_NORMAL;
   return pLine;
}

// Render one Verilog source line: green line number, then tokens in
// their syntax colors.
void CPrism::DrawVerilogLine(WINDOW *pWnd, int y, int lineNo,
                             const char *text, int len, int cols,
                             int gutter, bool hot, int lcol)
{
  char  buf[240];
  char *pToken;
  int   col, syntax;

  if (len > (int)sizeof(buf) - 1)
    len = (int)sizeof(buf) - 1;
  memcpy(buf, text, len);
  buf[len] = 0;

  wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_LINENO));
  mvwprintw(pWnd, y, 0, "%4d ", lineNo + 1);
  wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_LINENO));

  // FSM tabs keep a gutter between the number and the text where the
  // halted-state arrow lands
  if (gutter > 0)
  {
    if (hot)
    {
      wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL) | A_BOLD);
      mvwprintw(pWnd, y, 5, "-->");
      wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL) | A_BOLD);
    }
    else
      mvwprintw(pWnd, y, 5, "%*s", gutter, "");
  }

  col = 0;
  while ((pToken = GetLineToken(&buf[col], col, syntax)) != NULL)
  {
    // The parser walks the FULL line (comment state etc.); only the
    // drawing is clipped against the pan offset lcol.
    int tl0 = (int)strlen(pToken);
    int scol = col - lcol;              // token start in view space
    char *pd = pToken;
    int  tl = tl0;

    if (scol < 0)
    {
      pd -= scol;                       // clip the head off-view chars
      tl += scol;
      scol = 0;
    }
    if (tl > 0)
    {
      wattron(pWnd, COLOR_PAIR(syntax));
      if (syntax == SYNTAX_PAIR_LABEL)
        wattron(pWnd, A_BOLD);
      if (5 + gutter + scol + tl >= cols &&
          cols - (5 + gutter + scol) - 1 >= 0)
        pd[cols - (5 + gutter + scol) - 1] = 0;
      mvwprintw(pWnd, y, 5 + gutter + scol, "%s", pd);
      wattroff(pWnd, COLOR_PAIR(syntax));
      wattroff(pWnd, A_BOLD);
    }

    col += tl0;
    if (5 + gutter + col - lcol >= cols - 1)
      break;
  }
  wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
}

// Read a whole host file into a NUL-terminated buffer the caller owns.
// Returns NULL (and complains) when the file is not there - which on a
// plain terminal is every file, since nothing is serving one.
char *CPrism::LoadHostFile(const char *path, int &len)
{
  FILE *f;
  char *buf;
  long  size;
  int   got;

  len = 0;
  if ((f = fopen(path, "r")) == NULL)
    return NULL;

  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > 512 * 1024)
  {
    fclose(f);
    CmdPrintf("open: %s has an unusable size (%ld)\n", path, size);
    return NULL;
  }

  if ((buf = (char *)malloc((size_t)size + 1)) == NULL)
  {
    fclose(f);
    CmdPrintf("open: out of memory for %s (%ld bytes)\n", path, size);
    return NULL;
  }

  got = (int)fread(buf, 1, (size_t)size, f);
  fclose(f);
  if (got <= 0)
  {
    free(buf);
    CmdPrintf("open: %s read failed\n", path);
    return NULL;
  }
  buf[got] = 0;
  len = got;
  return buf;
}

// Where a chroma's listing / source lives on the host.  Anything that
// already looks like a path (a '/' in it, or its own extension) is taken
// verbatim, so a file outside the staged tree can be opened directly.
bool CPrism::ChromaPath(const char *name, bool verilog, char *path, int size)
{
  int len = (int)strlen(name);

  if (strchr(name, '/') != NULL ||
      (len > 4 && strcmp(&name[len - 4], ".lst") == 0))
  {
    // Verbatim path.  Open() strips a ".v" suffix before calling here
    // (it doubles as the source-vs-listing selector), so put it back.
    snprintf(path, size, verilog ? "%s.v" : "%s", name);
    return true;
  }
  snprintf(path, size, "%s/chroma_%s.%s", CHROMA_FS_SUBDIR, name,
           verilog ? "v" : "lst");
  return true;
}

// Byte-safe suffix test: newlib's word-optimized strcmp misreads
// odd-address strings (the TinyQV unaligned-word erratum, sighting #5 -
// 'open <long name>.mid' put the suffix at a misread alignment while
// printf showed it perfectly)
static int suffix_is(const char *s, int len, const char *suf, int n)
{
  if (len < n)
    return 0;
  s += len - n;
  while (n--)
    if (*s++ != *suf++)
      return 0;
  return 1;
}

int CPrism::Open(int argc, char *argv[])
{
  ListingCtx *pCtx;
  CTab       *pTab;
  char       *text;
  const char *ptr;
  char        want[64];
  char        path[96];
  char        name[48];
  int         idx, n, len;
  bool        verilog = false;

  if (argc < 2)
  {
    // The tqvfs root as the host serves it right now - files added on
    // the host show up here without any rebuild.  The naming shortcuts
    // below the listing cover the two structured subdirectories.
    static char list[768];
    int         got = tqv_fs_list(".", list, sizeof(list) - 1);

    if (got < 0)
    {
      CmdPrintf("open: no host filesystem (run the console from tqv.py,"
                " then 'fs probe')\n");
      return -1;
    }
    list[got] = 0;
    CmdPrintf("served files (tqvfs root):\n");
    for (ptr = list; *ptr != 0; )
    {
      const char *nl = strchr(ptr, '\n');
      int         flen = nl ? (int)(nl - ptr) : (int)strlen(ptr);
      char        kind = ptr[0];
      const char *fn = ptr;
      long        size = 0;

      // rows are "f <size> <name>" / "d 0 <name>"
      if (flen > 2 && (kind == 'f' || kind == 'd'))
      {
        fn = ptr + 2;
        size = atol(fn);
        while (fn < ptr + flen && *fn != ' ')
          fn++;
        if (fn < ptr + flen)
          fn++;
        flen -= (int)(fn - ptr);
        if (kind == 'd')
          CmdPrintf("  %.*s/\n", flen, fn);
        else if (size >= 10240)
          CmdPrintf("  %-24.*s %5ldKiB\n", flen, fn, size / 1024);
        else
          CmdPrintf("  %-24.*s %5ldB\n", flen, fn, size);
      }
      if (nl == NULL)
        break;
      ptr = nl + 1;
    }
    CmdPrintf("'open <chroma>[.v]' reads chromas/, 'open <pack>.spk' reads"
              " sounds/,\n'open <dir>/<file>' shows any text file"
              " ('ls <dir>' explores)\n");
    return OK;
  }

  // A sound pack is its own kind of tab.  'open ram|flash [name]' picks
  // a pack out of that bank's allocation table by name (bare: the first
  // one there); a flash pack plays straight from XIP, a RAM one from
  // PSRAM - neither ever fetches over the host link.
  int wave = 0;                 // 'open ... -w': pack plays draw the
                                // scope in the command window
  for (int a = 1; a < argc; a++)
    if (strcmp(argv[a], "-w") == 0)
    {
      wave = 1;
      for (int b = a; b < argc - 1; b++)
        argv[b] = argv[b + 1];
      argc--;
      a--;
    }
  len = (int)strlen(argv[1]);
  if (strcmp(argv[1], "ram") == 0)
    return OpenSoundPack(argc > 2 ? argv[2] : NULL, 1, wave);
  if (strcmp(argv[1], "flash") == 0)
    return OpenSoundPack(argc > 2 ? argv[2] : NULL, 2, wave);
  if (suffix_is(argv[1], len, ".spk", 4))
    return OpenSoundPack(argv[1], 0, wave);
  if (suffix_is(argv[1], len, ".mid", 4))
    return LoadMidi(argv[1]);

  // A trailing ".v" selects the Verilog source instead of the listing
  snprintf(want, sizeof(want), "%s", argv[1]);
  len = (int)strlen(want);
  if (suffix_is(want, len, ".v", 2))
  {
    verilog = true;
    want[len - 2] = 0;
  }

  ChromaPath(want, verilog, path, sizeof(path));

  // The host-link fetch takes seconds at 1M baud; acknowledge the
  // command instantly so the wait doesn't look like a hang
  CmdPrintf("fetching %s over the host link...\n", path);
  doupdate();
  fflush(stdout);
  if ((text = LoadHostFile(path, len)) == NULL)
  {
    CmdPrintf("open: cannot read %s ('open' lists what is served)\n", path);
    return -1;
  }

  // The SI highlight needs the console chroma table index; a listing
  // outside that table (or a .v source) simply has no highlight.
  idx = prism_chroma_index(want);

  // Count lines, then build the index
  n = 1;
  for (ptr = text; *ptr != 0; ptr++)
    if (*ptr == '\n')
      n++;

  pCtx = new ListingCtx;
  if (pCtx == NULL)
  {
    free(text);
    return -1;
  }
  pCtx->kind      = CTX_LISTING;
  pCtx->chromaIdx = idx;
  pCtx->verilog   = verilog;
  pCtx->fsm       = NULL;
  pCtx->leftCol   = 0;
  pCtx->diag      = NULL;
  pCtx->diagShow  = 0;
  pCtx->count     = 0;
  pCtx->text      = text;
  pCtx->starts    = new const char *[n];
  pCtx->lens      = new int[n];
  if (pCtx->starts == NULL || pCtx->lens == NULL)
  {
    delete[] pCtx->starts;
    delete[] pCtx->lens;
    delete pCtx;
    free(text);
    return -1;
  }
  ptr = text;
  while (*ptr != 0 && pCtx->count < n)
  {
    const char *nl = strchr(ptr, '\n');
    pCtx->starts[pCtx->count] = ptr;
    pCtx->lens[pCtx->count]   = nl ? (int)(nl - ptr) : (int)strlen(ptr);
    pCtx->count++;
    if (nl == NULL)
      break;
    ptr = nl + 1;
  }

  // Create and activate the tab
  snprintf(name, sizeof(name), "%s.%s", want, verilog ? "v" : "lst");
  pTab = m_pParent->CreateNewTab(name);
  if (pTab == NULL)
  {
    delete[] pCtx->starts;
    delete[] pCtx->lens;
    delete pCtx;
    free(text);
    return -1;
  }
  pTab->AttachTuiSource(this, pCtx);
  m_pParent->MakeTabActive(pTab);
  m_pParent->DrawSourceWindow();

  if (verilog)
  {
    // Let the freshly drawn tab reach the terminal before the (slow)
    // FSM scan runs, so the wait isn't spent on a stale screen
    doupdate();
    fflush(stdout);
    ParseFsm(pCtx);
    if (pCtx->fsm != NULL)
    {
      FsmJumpToState();                 // arrow/topline if halted in a state
      m_pParent->DrawSourceWindow();    // repaint with the fsm gutter
      CmdPrintf("fsm: %d states, %d in/out vars parsed ('bp 0 <state"
                " name>' works now)\n", pCtx->fsm->nStates,
                pCtx->fsm->nVars);
    }
  }
  return OK;
}

/*
==============================================================================
Chroma .v FSM parsing (states, their begin-block lines, in/out vars)
==============================================================================
*/

// Identifier at p (A-Za-z0-9_), copied into out (size cap), returns length
static int fsm_ident(const char *p, int max, char *out, int outsz)
{
  int n = 0;

  while (n < max && (isalnum((unsigned char)p[n]) || p[n] == '_'))
  {
    if (n < outsz - 1)
      out[n] = p[n];
    n++;
  }
  out[n < outsz - 1 ? n : outsz - 1] = 0;
  return n;
}

// Small positive-number parser (the compat headers lack strtol)
static int fsm_num(const char *p, int base)
{
  int v = 0;

  for (;; p++)
  {
    int d;

    if (*p >= '0' && *p <= '9')
      d = *p - '0';
    else if (*p >= 'a' && *p <= 'f')
      d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F')
      d = *p - 'A' + 10;
    else
      break;
    if (d >= base)
      break;
    v = v * base + d;
  }
  return v;
}

static const char *fsm_findtok(const char *p, int len, const char *tok)
{
  int tl = (int)strlen(tok);

  for (int i = 0; i + tl <= len; i++)
    if (strncmp(&p[i], tok, tl) == 0)
      return &p[i];
  return NULL;
}

void CPrism::ParseFsm(ListingCtx *pCtx)
{
  FsmInfo *f = (FsmInfo *)malloc(sizeof(FsmInfo));

  if (f == NULL)
    return;
  memset(f, 0, sizeof(*f));

  // Pass 1: localparam STATE_X = 3'h2, and the in/out assigns
  for (int ln = 0; ln < pCtx->count; ln++)
  {
    const char *t = pCtx->starts[ln];
    int         len = pCtx->lens[ln];
    const char *q;

    if (fsm_findtok(t, len, "localparam") != NULL &&
        (q = fsm_findtok(t, len, "STATE_")) != NULL &&
        f->nStates < (int)(sizeof(f->st) / sizeof(f->st[0])))
    {
      FsmState *st = &f->st[f->nStates];
      int n = fsm_ident(q, (int)(len - (q - t)), st->name, sizeof(st->name));
      const char *eq = fsm_findtok(q + n, (int)(len - (q + n - t)), "=");

      if (eq != NULL)
      {
        const char *tick = fsm_findtok(eq, (int)(len - (eq - t)), "'");

        if (tick != NULL && tick + 2 < t + len)
          st->si = fsm_num(tick + 2,
                           (tick[1] == 'h' || tick[1] == 'H') ? 16 :
                           (tick[1] == 'b' || tick[1] == 'B') ? 2 : 10);
        else
          st->si = atoi(eq + 1);
        st->caseLine = st->assignLine = -1;
        f->nStates++;
      }
    }

    if ((q = fsm_findtok(t, len, "assign")) != NULL &&
        f->nVars < (int)(sizeof(f->var) / sizeof(f->var[0])))
    {
      const char *od = fsm_findtok(t, len, "out_data[");
      const char *id = fsm_findtok(t, len, "in_data[");
      const char *eq = fsm_findtok(t, len, "=");
      FsmVar     *v = &f->var[f->nVars];

      if (od != NULL && eq != NULL && od < eq && eq + 1 < t + len)
      {
        v->dir = 'o';
        v->num = atoi(od + 9);
        const char *nm = eq + 1;

        while (nm < t + len && (*nm == ' ' || *nm == '\t'))
          nm++;
        if (fsm_ident(nm, (int)(len - (nm - t)), v->name, sizeof(v->name)) > 0)
          f->nVars++;
      }
      else if (id != NULL && eq != NULL && eq < id)
      {
        v->dir = 'i';
        v->num = atoi(id + 8);
        const char *nm = q + 6;

        while (nm < t + len && (*nm == ' ' || *nm == '\t'))
          nm++;
        if (fsm_ident(nm, (int)(len - (nm - t)), v->name, sizeof(v->name)) > 0)
          f->nVars++;
      }
    }
  }

  // Pass 2: each state's case label and the first assignment in its
  // begin block (skipping comments, begin/end and if/case headers)
  for (int i = 0; i < f->nStates; i++)
  {
    for (int ln = 0; ln < pCtx->count; ln++)
    {
      const char *t = pCtx->starts[ln];
      int         len = pCtx->lens[ln];
      const char *q = fsm_findtok(t, len, f->st[i].name);

      if (q == NULL || fsm_findtok(t, len, "localparam") != NULL)
        continue;
      const char *after = q + strlen(f->st[i].name);

      while (after < t + len && *after == ' ')
        after++;
      if (after >= t + len || *after != ':')
        continue;

      f->st[i].caseLine = ln;
      for (int j = ln + 1; j < pCtx->count && j < ln + 40; j++)
      {
        const char *u = pCtx->starts[j];
        int         ul = pCtx->lens[j];
        const char *body = u;

        while (body < u + ul && (*body == ' ' || *body == '\t'))
          body++;
        int bl = (int)(ul - (body - u));

        if (bl <= 0 || (bl >= 2 && body[0] == '/' && body[1] == '/'))
          continue;
        if (fsm_findtok(body, bl, "STATE_") != NULL &&
            fsm_findtok(body, bl, ":") != NULL)
          break;
        if (fsm_findtok(body, bl, "endcase") != NULL)
          break;
        if (strncmp(body, "if", 2) == 0 || strncmp(body, "case", 4) == 0 ||
            strncmp(body, "begin", 5) == 0 || strncmp(body, "end", 3) == 0)
          continue;
        const char *eq = fsm_findtok(body, bl, "=");

        if (eq != NULL && !(eq + 1 < body + bl && eq[1] == '=') &&
            !(eq > body && (eq[-1] == '=' || eq[-1] == '!' ||
                            eq[-1] == '<' || eq[-1] == '>')))
        {
          f->st[i].assignLine = j;
          break;
        }
      }
      if (f->st[i].assignLine < 0)
        f->st[i].assignLine = f->st[i].caseLine;
      break;
    }
  }

  pCtx->fsm = f;
}

// The parsed .v tab matching the chroma currently in the FSM
CPrism::FsmInfo *CPrism::LoadedFsm(ListingCtx **ppCtx)
{
  for (CTab *pTab = m_pParent->GetFirstTab(); pTab != NULL;
       pTab = pTab->GetNextTab())
  {
    void *pCtx = pTab->SourceContext();

    if (pCtx == NULL || pCtx == &s_NotesCtxMarker ||
        *(const int *)pCtx != CTX_LISTING)
      continue;
    ListingCtx *pList = (ListingCtx *)pCtx;

    if (pList->verilog && pList->fsm != NULL &&
        pList->chromaIdx == prism_chroma_loaded())
    {
      if (ppCtx != NULL)
        *ppCtx = pList;
      return pList->fsm;
    }
  }
  return NULL;
}

// State name -> SI, from the loaded chroma's parsed .v ("STATE_" prefix
// optional, case-insensitive)
int CPrism::FsmStateByName(const char *name)
{
  FsmInfo *f = LoadedFsm(NULL);

  if (f == NULL)
    return -1;
  for (int i = 0; i < f->nStates; i++)
  {
    if (strcasecmp(name, f->st[i].name) == 0)
      return f->st[i].si;
    if (strncmp(f->st[i].name, "STATE_", 6) == 0 &&
        strcasecmp(name, f->st[i].name + 6) == 0)
      return f->st[i].si;
  }
  return -1;
}

// Line the halted arrow points at in this tab, -1 when not applicable
int CPrism::FsmArrowLine(ListingCtx *pCtx)
{
  if (pCtx == NULL || pCtx->fsm == NULL ||
      pCtx->chromaIdx != prism_chroma_loaded() || !prism_dbg_is_halted())
    return -1;
  int si = (int)PRISM_DBG_STAT_CURR_SI(prism_dbg_status());

  for (int i = 0; i < pCtx->fsm->nStates; i++)
    if (pCtx->fsm->st[i].si == si)
      return pCtx->fsm->st[i].assignLine;
  return -1;
}

// Bring the halted state's line into view in the active .v tab
void CPrism::FsmJumpToState(void)
{
  // The loaded chroma's parsed .v tab, WHEREVER it is - not just the
  // active tab: halting from any other tab still brings the state's
  // code (and the --> arrow) into view.
  ListingCtx *pCtx = NULL;
  FsmInfo *f = LoadedFsm(&pCtx);

  if (f == NULL)
    return;

  int hot = FsmArrowLine(pCtx);

  if (hot < 0)
    return;

  CTab *pTab;

  for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
       pTab = pTab->GetNextTab())
    if (pTab->SourceContext() == pCtx)
      break;
  if (pTab == NULL)
    return;
  m_pParent->MakeTabActive(pTab);

  int first = pTab->SourceFirstLine();
  int visible = pTab->SourceWindowLineCount();

  if (hot < first || hot >= first + visible)
    pTab->SourceFirstLine(hot - 3 > 0 ? hot - 3 : 0);
}

// Async breakpoint hits: poll the halted flag a few times a second and
// jump the source view to the break state on the transition
void CPrism::IdlePoll(void)
{
  uint32_t now = read_time();

  if ((int32_t)(now - m_HaltPollAt) < 0)
    return;
  m_HaltPollAt = now + 200000u;

  int halted = prism_dbg_is_halted() ? 1 : 0;

  if (halted != m_LastHalted)
  {
    int was = m_LastHalted;

    m_LastHalted = halted;
    if (halted && was == 0 && LoadedFsm(NULL) != NULL)
    {
      FsmJumpToState();
      m_pParent->DrawSourceWindow();
    }
  }
}

/*
==============================================================================
Sound packs (.spk)

A pack is an index of short ADPCM sounds built by mp3todcm/mk_soundpack.py:

    "SPK1", uint32 count, uint32 rate, uint32 block
    count x { char name[40]; uint32 offset, length, samples; }
    the ADPCM blobs, each exactly what the song player already understands

Only the index is read when the tab opens.  Audio is fetched per entry -
the host filesystem link runs at roughly 10KB/s, so a two second sound
takes a few seconds to arrive and the last one played is kept for instant
repeats.  A pack written into RAM B with 'tqv.py load <pack>.spk' skips
the fetch entirely.
==============================================================================
*/

#define SPK_MAGIC       "SPK1"
#define SPK_HDR_LEN     16
#define SPK_ENTRY_LEN   52      // char name[40] + 3 x uint32

static uint32_t spk_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A status line under the list: what is loading / playing, without
// disturbing the entries themselves.
void CPrism::SoundStatus(SoundCtx *pCtx, const char *fmt, ...)
{
  WINDOW *pWnd = pCtx ? pCtx->pWnd : NULL;
  va_list args;
  char    line[160];
  int     rows, cols;

  if (pWnd == NULL)
    return;

  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  getmaxyx(pWnd, rows, cols);
  wmove(pWnd, rows - 1, 0);
  wclrtoeol(pWnd);
  wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
  mvwprintw(pWnd, rows - 1, 0, "%.*s", cols - 1, line);
  wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
  wrefresh(pWnd);
}

int CPrism::OpenSoundPack(const char *arg, int bank, int wave)
{
  SoundCtx *pCtx;
  CTab     *pTab;
  FILE     *f = NULL;
  uint8_t   hdr[SPK_HDR_LEN];
  uint8_t  *index = NULL;
  char      path[72];
  char      name[48];
  const uint8_t *pBase = NULL;
  uint32_t  packLen = 0;
  int       count, i, n;

  if (bank != 0)
  {
    // A pack already on the board (named table entry, or the first pack
    // in that bank): no fetching at all
    const char *label = bank == 1 ? "RAM B" : "flash";

    pBase = bank == 1 ? song_ram_named(SONG_KIND_SPK, arg, &packLen)
                      : song_flash_named(SONG_KIND_SPK, arg, &packLen);
    if (pBase == NULL)
    {
      CmdPrintf("open: no%s%s sound pack in %s"
                " ('./tqv.py load <pack>.spk%s' loads one; 'fat' lists)\n",
                arg ? " " : "", arg ? arg : "", label,
                bank == 2 ? " --flash" : "");
      return -1;
    }
    if (packLen < SPK_HDR_LEN || memcmp(pBase, SPK_MAGIC, 4) != 0)
    {
      CmdPrintf("open: %s entry does not hold a %s pack\n", label, SPK_MAGIC);
      return -1;
    }
    memcpy(hdr, pBase, sizeof(hdr));
    snprintf(path, sizeof(path), "%s", "");
    snprintf(name, sizeof(name), "%s.spk", arg ? arg : label);
  }
  else
  {
    // Try the name as given, then under sounds/
    snprintf(path, sizeof(path), "%s", arg);
    if ((f = fopen(path, "r")) == NULL && strchr(arg, '/') == NULL)
    {
      snprintf(path, sizeof(path), "%s/%s", SOUND_FS_SUBDIR, arg);
      f = fopen(path, "r");
    }
    if (f == NULL)
    {
      CmdPrintf("open: cannot open %s ('open' lists the packs served)\n", arg);
      return -1;
    }
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, SPK_MAGIC, 4) != 0)
    {
      CmdPrintf("open: %s is not a %s sound pack\n", path, SPK_MAGIC);
      fclose(f);
      return -1;
    }
    const char *slash = strrchr(path, '/');
    snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
  }

  count = (int)spk_u32(&hdr[4]);
  if (count <= 0 || count > 512)
  {
    CmdPrintf("open: %s claims %d entries\n", name, count);
    if (f)
      fclose(f);
    return -1;
  }

  n = count * SPK_ENTRY_LEN;
  if (pBase != NULL)
    index = (uint8_t *)((uintptr_t)pBase + SPK_HDR_LEN);
  else
  {
    if ((index = (uint8_t *)malloc((size_t)n)) == NULL ||
        (int)fread(index, 1, (size_t)n, f) != n)
    {
      CmdPrintf("open: %s index read failed\n", name);
      free(index);
      fclose(f);
      return -1;
    }
  }

  pCtx = new SoundCtx;
  if (pCtx == NULL || (pCtx->pEntries = new SoundEntry[count]) == NULL)
  {
    delete pCtx;
    if (pBase == NULL)
      free(index);
    if (f)
      fclose(f);
    return -1;
  }
  pCtx->kind     = CTX_SOUND;
  pCtx->pBase    = pBase;
  pCtx->bank     = bank;
  pCtx->count    = count;
  pCtx->rate     = spk_u32(&hdr[8]);
  pCtx->sel      = 0;
  pCtx->wave     = wave;
  pCtx->pLoaded  = NULL;
  snprintf(pCtx->path, sizeof(pCtx->path), "%s", path);

  for (i = 0; i < count; i++)
  {
    const uint8_t *e = &index[i * SPK_ENTRY_LEN];

    memcpy(pCtx->pEntries[i].name, e, sizeof(pCtx->pEntries[i].name));
    pCtx->pEntries[i].name[sizeof(pCtx->pEntries[i].name) - 1] = 0;
    pCtx->pEntries[i].offset  = spk_u32(&e[40]);
    pCtx->pEntries[i].length  = spk_u32(&e[44]);
    pCtx->pEntries[i].samples = spk_u32(&e[48]);
  }
  if (pBase == NULL)
  {
    free(index);
    fclose(f);
  }

  pTab = m_pParent->CreateNewTab(name);
  if (pTab == NULL)
  {
    delete[] pCtx->pEntries;
    delete pCtx;
    return -1;
  }
  pTab->AttachTuiSource(this, pCtx);
  m_pParent->MakeTabActive(pTab);
  m_pParent->DrawSourceWindow();
  CmdPrintf("%s: %d sounds at %luHz - CTRL-W to focus, UP/DOWN to select,"
            " SPACE or 'p' to play\n", name, count,
            (unsigned long)pCtx->rate);
  return OK;
}

// The sound tab of the active source tab, or NULL when something else
// (or nothing) is up front.
CPrism::SoundCtx *CPrism::ActiveSoundTab(void)
{
  CTab *pTab = m_pParent ? m_pParent->GetActiveSrcTab() : NULL;
  void *pCtx = pTab ? pTab->SourceContext() : NULL;

  if (pCtx == NULL || pCtx == &s_NotesCtxMarker)
    return NULL;
  if (*(const int *)pCtx != CTX_SOUND)
    return NULL;
  return (SoundCtx *)pCtx;
}

// One list row's text, so the full draw and the single-row repaint can
// never drift apart
void CPrism::SoundRowText(SoundCtx *pCtx, int idx, char *line, int size)
{
  SoundEntry *pEnt = &pCtx->pEntries[idx];
  double      secs = pCtx->rate ? (double)pEnt->samples / (double)pCtx->rate
                                : 0.0;

  snprintf(line, size, " %3d  %-40s %5d.%02ds  %4luKiB%s",
           idx + 1, pEnt->name, (int)secs,
           (int)((secs - (int)secs) * 100.0 + 0.5),
           (unsigned long)((pEnt->length + 1023) / 1024),
           SoundFind(pCtx, idx) != NULL ? "  [loaded]" : "");
}

// Repaint a single row.  Moving the selection redraws the row it left
// and the row it landed on - a full DrawSourceWindow would push the
// whole list out of the UART on every arrow key, which is slow enough
// that held keys start getting dropped.
// Width of a list row.  Deliberately the width of the CONTENT rather
// than the window: every character of the highlight bar is a byte the
// UART has to carry on a scroll, and at 115200 with no RX interrupt the
// design cannot hear a keystroke while it is transmitting.
#define SOUND_ROW_W   76        // fits '999  <40 char name>  9999.99s  9999KiB  [loaded]'

void CPrism::SoundRowRepaint(SoundCtx *pCtx, int idx, int topLine)
{
  WINDOW *pWnd = pCtx->pWnd;
  char    line[160];
  int     rows, cols, y, w;

  if (pWnd == NULL || idx < 0 || idx >= pCtx->count)
    return;
  y = idx - topLine;
  getmaxyx(pWnd, rows, cols);
  if (y < 0 || y >= rows - 1)
    return;
  w = cols - 1 < SOUND_ROW_W ? cols - 1 : SOUND_ROW_W;

  SoundRowText(pCtx, idx, line, sizeof(line));
  if (idx == pCtx->sel)
    wattron(pWnd, A_REVERSE | A_BOLD);
  mvwprintw(pWnd, y, 0, "%-*.*s", w, w, line);
  if (idx == pCtx->sel)
    wattroff(pWnd, A_REVERSE | A_BOLD);
  wrefresh(pWnd);
}

void CPrism::DrawSoundPack(SoundCtx *pCtx, WINDOW *pWnd, int topLine,
                           int lineCount)
{
  int rows, cols, y, i, w;

  pCtx->pWnd = pWnd;            // SoundStatus repaints through this
  getmaxyx(pWnd, rows, cols);
  w = cols - 1 < SOUND_ROW_W ? cols - 1 : SOUND_ROW_W;
  if (lineCount > rows)
    lineCount = rows;
  if (topLine < 0)
    topLine = 0;

  werase(pWnd);

  // Entries fill the window except its last row, which carries the
  // key help / status so it stays put while the list scrolls
  for (y = 0; y < lineCount - 1; y++)
  {
    char line[160];

    i = topLine + y;
    if (i >= pCtx->count)
      break;
    SoundRowText(pCtx, i, line, sizeof(line));

    if (i == pCtx->sel)
      wattron(pWnd, A_REVERSE | A_BOLD);
    mvwprintw(pWnd, y, 0, "%-*.*s", w, w, line);
    if (i == pCtx->sel)
      wattroff(pWnd, A_REVERSE | A_BOLD);
  }

  wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
  mvwprintw(pWnd, rows - 1, 0,
            "%.*s", cols - 1,
            pCtx->bank == 1
              ? "UP/DOWN select   SPACE or 'p' play   ^W focus   (pack in RAM B)"
            : pCtx->bank == 2
              ? "UP/DOWN select   SPACE or 'p' play   ^W focus   (pack in flash)"
              : "UP/DOWN select   SPACE or 'p' play   ^W focus   (a played sound"
                " stays loaded until the tab closes)");
  wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
}

// The loaded list, MRU first
CPrism::SoundBlob *CPrism::SoundFind(SoundCtx *pCtx, int idx)
{
  SoundBlob *pBlob;

  for (pBlob = pCtx->pLoaded; pBlob != NULL; pBlob = pBlob->pNext)
    if (pBlob->idx == idx)
      return pBlob;
  return NULL;
}

/*
==============================================================================
Fetch one soundpack entry from the host file onto the loaded list (head = most
recently used).  When an allocation fails, the least recently used
blobs are released one at a time until it succeeds - so the list only
shrinks when the RAM is genuinely needed for a new sound.
==============================================================================
*/
CPrism::SoundBlob *CPrism::SoundFetch(SoundCtx *pCtx, int idx)
{
  SoundEntry *pEnt = &pCtx->pEntries[idx];
  SoundBlob  *pBlob;
  FILE       *f;
  uint32_t    t0, ms;
  int         got;

  SoundStatus(pCtx, "loading %s (%luKiB)...", pEnt->name,
              (unsigned long)((pEnt->length + 1023) / 1024));

  for (;;)
  {
    pBlob = (SoundBlob *)malloc(sizeof(SoundBlob) + pEnt->length);
    if (pBlob != NULL)
      break;

    // Out of heap: drop the least recently used sound and retry
    SoundBlob **ppTail = &pCtx->pLoaded;

    if (*ppTail == NULL)
    {
      SoundStatus(pCtx, "out of memory for %s (%lu bytes)", pEnt->name,
                  (unsigned long)pEnt->length);
      return NULL;
    }
    while ((*ppTail)->pNext != NULL)
      ppTail = &(*ppTail)->pNext;
    free(*ppTail);
    *ppTail = NULL;
  }

  if ((f = fopen(pCtx->path, "r")) == NULL)
  {
    SoundStatus(pCtx, "cannot reopen %s", pCtx->path);
    free(pBlob);
    return NULL;
  }
  t0 = read_time();
  fseek(f, (long)pEnt->offset, SEEK_SET);
  got = (int)fread(pBlob->data, 1, pEnt->length, f);
  fclose(f);
  ms = (read_time() - t0) / 1000u;
  if (got != (int)pEnt->length)
  {
    SoundStatus(pCtx, "short read on %s (%d of %lu bytes)", pEnt->name,
                got, (unsigned long)pEnt->length);
    free(pBlob);
    return NULL;
  }

  pBlob->idx    = idx;
  pBlob->length = pEnt->length;
  pBlob->pNext  = pCtx->pLoaded;
  pCtx->pLoaded = pBlob;
  SoundStatus(pCtx, "loaded %s in %lu.%lus - playing...", pEnt->name,
              (unsigned long)(ms / 1000), (unsigned long)((ms / 100) % 10));
  return pBlob;
}

/*
==============================================================================
Play one entry from a sound pack, fetching it onto the loaded list first if
this is the first time.  Blocking, like every other player: any key stops it.
==============================================================================
*/
void CPrism::SoundPlay(SoundCtx *pCtx, int idx)
{
  SoundEntry    *pEnt;
  const uint8_t *blob = NULL;

  if (idx < 0 || idx >= pCtx->count)
    return;
  pEnt = &pCtx->pEntries[idx];

  if (pCtx->pBase != NULL)
  {
    blob = pCtx->pBase + pEnt->offset;
    SoundStatus(pCtx, "playing %s...", pEnt->name);
  }
  else
  {
    SoundBlob *pBlob = SoundFind(pCtx, idx);

    if (pBlob != NULL)
    {
      // Replay: move to the head so eviction hits stale sounds first
      if (pCtx->pLoaded != pBlob)
      {
        SoundBlob **pp = &pCtx->pLoaded;

        while (*pp != pBlob)
          pp = &(*pp)->pNext;
        *pp = pBlob->pNext;
        pBlob->pNext  = pCtx->pLoaded;
        pCtx->pLoaded = pBlob;
      }
      SoundStatus(pCtx, "playing %s...", pEnt->name);
    }
    else if ((pBlob = SoundFetch(pCtx, idx)) == NULL)
      return;
    blob = pBlob->data;
  }

  // Quiet: the player's progress prints would repaint the command
  // window mid-playback, and a screen refresh costs more UART time
  // than the 32ms refill budget allows.  Hold: the carrier parks at mid
  // scale between pack sounds instead of dropping to the pin idle, so
  // back-to-back plays move no DC at all (moving it is what pops the
  // speaker); CloseTab releases it.
  adpcm_wave = 0;
  if (pCtx->wave)                           // tab opened with -w: the
  {                                         // waveform draws in the
    m_pParent->CmdWindowRect(tui_tab_rect); // command window, scope-only
    adpcm_wave = 2;                         // (Ctrl-W restores the prompt)
  }
  adpcm_quiet = 1;
  adpcm_hold = 1;
  play_adpcm_data(blob, adpcm_preload_for_rate(pCtx->rate));
  adpcm_hold = 0;
  adpcm_quiet = 0;
  adpcm_wave = 0;

  {
    CTab *pTab = m_pParent->GetActiveSrcTab();
    int   first = pTab ? pTab->SourceFirstLine() : 0;    // 0-based

    SoundRowRepaint(pCtx, idx, first < 0 ? 0 : first);   // its [loaded] tag
  }
  SoundStatus(pCtx, "%s: %d/%d  %s", pEnt->name, idx + 1, pCtx->count,
              adpcm_underruns ? "(underruns - the link was busy)" : "done");
}

/*
==============================================================================
Key routing for the sound tab (CTRL-W focuses the source window)
==============================================================================
*/
bool CPrism::WantProcessKey(void)
{
  return ActiveSoundTab() != NULL;
}

int CPrism::WantFocus(void)
{
  return ActiveSoundTab() != NULL ? 1 : 0;
}

void CPrism::SetFocus(void *pCtx, WINDOW *pWnd, int topLine, int lineCount)
{
  if (pCtx == NULL || pCtx == &s_NotesCtxMarker ||
      *(const int *)pCtx != CTX_SOUND)
    return;
  DrawSoundPack((SoundCtx *)pCtx, pWnd, topLine, lineCount);
  wrefresh(pWnd);
}

/*
==============================================================================
This routine handles keystrokes while the active tab has the focus (i.e. if
the user used CTRL-W to change focus to the tabs / Source window.
==============================================================================
*/
int CPrism::ProcessKey(int key)
{
  SoundCtx *pCtx = ActiveSoundTab();
  CTab     *pTab;
  int       first, visible, sel, prev;

  if (pCtx == NULL)
    return 0;

  pTab    = m_pParent->GetActiveSrcTab();
  first   = pTab->SourceFirstLine();            // 0-based top entry
  visible = pTab->SourceWindowLineCount() - 1;  // last row is the status
  if (first < 0)
    first = 0;
  if (visible < 1)
    visible = 1;
  sel = pCtx->sel;

  switch (key)
  {
    case KEY_UP:      sel--;                    break;
    case KEY_DOWN:    sel++;                    break;
    case 2:                                     // CTRL-B, like page up
    case KEY_PPAGE:   sel -= visible;           break;
    case 6:                                     // CTRL-F, like page down
    case KEY_NPAGE:   sel += visible;           break;
    case KEY_HOME:    sel = 0;                  break;
    case KEY_END:     sel = pCtx->count - 1;    break;

    case ' ':
    case 'p':
    case 'P':
      SoundPlay(pCtx, pCtx->sel);
      return 1;

    default:
      return 0;                                 // not ours: normal handling
  }

  if (sel < 0)
    sel = 0;
  if (sel >= pCtx->count)
    sel = pCtx->count - 1;
  if (sel == pCtx->sel)
    return 1;                                   // already at the end
  prev      = pCtx->sel;
  pCtx->sel = sel;

  // Follow the selection with the view when it leaves the window;
  // otherwise just repaint the two rows that changed.
  //
  // The view moves a PAGE at a time, not a row: every scroll shifts the
  // whole list, which is a full window repaint - about 4KB, a third of a
  // second of UART - and this design reads one byte at a time with no RX
  // interrupt, so keys pressed while that is going out are simply lost.
  // Scrolling by pages makes that cost land once per screenful instead
  // of on every arrow key at the bottom of the list.
  if (sel < first)
    m_pParent->UISetSourceTopLineNo(sel - visible + 1);   // sel at the bottom
  else if (sel >= first + visible)
    m_pParent->UISetSourceTopLineNo(sel);                 // sel at the top
  else
  {
    SoundRowRepaint(pCtx, prev, first);
    SoundRowRepaint(pCtx, sel, first);
  }
  return 1;
}

// ===========================================================================
// 'show fsm': parse and draw a state diagram from the house-style .v
// (single-line conditions, one begin/end per line, defaults before the
// case statement).
// ===========================================================================

// "sig = val;" on one line -> name/val (verilog sized literals shrink
// to their value: 1'b1 -> 1, 6'h0 -> 0)
// Length of the CODE part of a line: tokens inside a // comment must
// never reach the FSM scans ("...end of transaction" closed a scope
// early and turned ws2812's if/else into an unconditional jump)
static int fsm_code_len(const char *t, int len)
{
  for (int i = 0; i + 1 < len; i++)
    if (t[i] == '/' && t[i + 1] == '/')
      return i;
  return len;
}

static int diag_assign(const char *t, int len, char *name, int ncap,
                       char *val, int vcap)
{
  int e = -1;

  for (int i = 1; i < len - 1; i++)
    if (t[i] == '=' && t[i-1] != '=' && t[i-1] != '<' && t[i-1] != '>' &&
        t[i-1] != '!' && t[i+1] != '=')
    {
      e = i;
      break;
    }
  if (e < 0)
    return 0;

  int b = 0;

  while (b < e && (t[b] == ' ' || t[b] == '\t'))
    b++;
  int ne = e;

  while (ne > b && (t[ne-1] == ' ' || t[ne-1] == '\t'))
    ne--;
  if (ne <= b || ne - b >= ncap)
    return 0;
  for (int i = b; i < ne; i++)
  {
    char c = t[i];

    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '[' || c == ']' ||
          c == ':'))
      return 0;
  }
  memcpy(name, t + b, ne - b);
  name[ne - b] = 0;

  int vs = e + 1;

  while (vs < len && t[vs] == ' ')
    vs++;
  int vend = vs;

  while (vend < len && t[vend] != ';')
    vend++;
  // strip a sized-literal prefix: <digits>'<b|h|d>
  int q = vs;

  while (q < vend && t[q] >= '0' && t[q] <= '9')
    q++;
  if (q < vend - 1 && t[q] == '\'')
    vs = q + 2;
  int n = vend - vs;

  if (n >= vcap)
    n = vcap - 1;
  memcpy(val, t + vs, n);
  val[n] = 0;
  return name[0] != 0;
}

/*
==============================================================================
Append data while building the FSM diagram
==============================================================================
*/
// Diagram-only shorthand so long control names fit small terminals:
// "count"->"cnt", "load"->"ld", "clear"->"clr".  Byte loops throughout -
// the newlib word-path str* functions misread odd addresses (the
// unaligned-word erratum).
static void diag_abbrev(char *dst, int cap, const char *nm)
{
  static const char *const from[] = { "count", "load", "clear" };
  static const char *const to[]   = { "cnt",   "ld",   "clr"   };
  static const int  flen[]        = { 5, 4, 5 };
  int o = 0;

  while (*nm != 0 && o < cap - 1)
  {
    int k, m;

    for (k = 0; k < 3; k++)
    {
      const char *f = from[k];

      for (m = 0; f[m] != 0 && nm[m] == f[m]; m++)
        ;
      if (f[m] == 0)
        break;
    }
    if (k < 3)
    {
      const char *t = to[k];

      while (*t != 0 && o < cap - 1)
        dst[o++] = *t++;
      nm += flen[k];
    }
    else
      dst[o++] = *nm++;
  }
  dst[o] = 0;
}

static void diag_append(char *buf, int cap, const char *name,
                        const char *val)
{
  char nm[28];
  int l = (int)strlen(buf);
  int need;

  diag_abbrev(nm, (int)sizeof(nm), name);
  need = (int)strlen(nm) + (int)strlen(val) + 2;
  if (l + need >= cap)
    return;
  if (l)
    buf[l++] = ' ';
  strcpy(buf + l, nm);
  strcat(buf, "=");
  strcat(buf, val);
}

/*
==============================================================================
This routine parses the open Verilog source for the active Chroma loaded to
determine the state names and values, state transitions, and input/output
pin variable names.
==============================================================================
*/
void CPrism::ParseFsmDiag(ListingCtx *pCtx)
{
  FsmInfo *f = pCtx->fsm;

  if (f == NULL || f->nStates == 0 || pCtx->diag != NULL)
    return;

  FsmDiag *d = (FsmDiag *)malloc(sizeof(FsmDiag));

  if (d == NULL)
    return;
  memset(d, 0, sizeof(*d));

  // The case statement, and the defaults just above it
  int caseLine = -1;

  for (int ln = 0; ln < pCtx->count; ln++)
    if (fsm_findtok(pCtx->starts[ln], pCtx->lens[ln], "case") != NULL &&
        fsm_findtok(pCtx->starts[ln], pCtx->lens[ln], "curr_state") != NULL)
    {
      caseLine = ln;
      break;
    }

  struct { char n[24]; char v[16]; } def[16];
  int nDef = 0;
  int from = caseLine > 24 ? caseLine - 24 : 0;

  for (int ln = from; ln >= 0 && ln < caseLine; ln++)
  {
    char nm[24], vl[16];

    if (diag_assign(pCtx->starts[ln], pCtx->lens[ln], nm, sizeof(nm),
                    vl, sizeof(vl)) && nDef < 16)
    {
      strcpy(def[nDef].n, nm);
      strcpy(def[nDef].v, vl);
      nDef++;
    }
  }

  int endcase = pCtx->count - 1;

  for (int ln = caseLine > 0 ? caseLine : 0; ln < pCtx->count; ln++)
    if (fsm_findtok(pCtx->starts[ln], pCtx->lens[ln], "endcase") != NULL)
    {
      endcase = ln;
      break;
    }

  for (int i = 0; i < f->nStates && i < 8; i++)
  {
    FsmState *st = &f->st[i];

    if (st->caseLine < 0 || st->si < 0 || st->si > 7)
      continue;

    FsmCell *c = &d->cell[st->si];
    int last = endcase;

    for (int j = 0; j < f->nStates; j++)
      if (f->st[j].caseLine > st->caseLine && f->st[j].caseLine < last)
        last = f->st[j].caseLine;
    // the default: arm is not in the state table - it bounds the last
    // state's block just like a case label does
    for (int ln = st->caseLine + 1; ln < last; ln++)
      if (fsm_findtok(pCtx->starts[ln],
                      fsm_code_len(pCtx->starts[ln], pCtx->lens[ln]),
                      "default") != NULL)
      {
        last = ln;
        break;
      }

    int depth = 0, condDepth = -1, scopeTgt = -1;
    char pendCond[30] = "", curCond[30] = "", scopeOuts[56] = "";

    for (int ln = st->caseLine + 1; ln < last; ln++)
    {
      const char *t = pCtx->starts[ln];
      int len = fsm_code_len(t, pCtx->lens[ln]);
      int hasBegin = fsm_findtok(t, len, "begin") != NULL;
      int hasEnd = fsm_findtok(t, len, "end") != NULL;

      // condition of an if / else if / else
      if (fsm_findtok(t, len, "if") != NULL)
      {
        const char *op = (const char *)memchr(t, '(', len);
        const char *cp = NULL;

        for (int k = len - 1; k >= 0; k--)
          if (t[k] == ')')
          {
            cp = t + k;
            break;
          }
        if (op != NULL && cp != NULL && cp > op)
        {
          int n = (int)(cp - op) - 1;

          if (n >= (int)sizeof(pendCond))
            n = sizeof(pendCond) - 1;
          memcpy(pendCond, op + 1, n);
          pendCond[n] = 0;
        }
      }
      else if (fsm_findtok(t, len, "else") != NULL)
        strcpy(pendCond, "else");

      if (hasBegin)
      {
        depth++;
        if (pendCond[0])
        {
          strcpy(curCond, pendCond);
          pendCond[0] = 0;
          condDepth = depth;
          scopeOuts[0] = 0;
          scopeTgt = -1;
        }
      }

      // next_state = STATE_X;
      const char *ns = fsm_findtok(t, len, "next_state");

      if (ns != NULL)
      {
        int tgt = -1;

        for (int j = 0; j < f->nStates; j++)
          if (fsm_findtok(t, len, f->st[j].name) != NULL)
          {
            tgt = f->st[j].si;
            break;
          }
        if (tgt >= 0 && tgt != st->si)
        {
          if (condDepth > 0 && depth >= condDepth)
            scopeTgt = tgt;
          else if (pendCond[0])
          {
            // single-statement conditional (no begin)
            if (c->nTr < 3)
            {
              FsmTrans *tr = &c->tr[c->nTr++];

              tr->tgt = (uint8_t)tgt;
              strcpy(tr->cond, pendCond);
              tr->outs[0] = 0;
            }
            pendCond[0] = 0;
          }
          else if (c->nTr < 3)
          {
            FsmTrans *tr = &c->tr[c->nTr++];

            tr->tgt = (uint8_t)tgt;
            tr->cond[0] = 0;
            tr->outs[0] = 0;
          }
        }
      }
      else
      {
        char nm[24], vl[16];

        if (diag_assign(t, len, nm, sizeof(nm), vl, sizeof(vl)))
        {
          if (condDepth > 0 && depth >= condDepth)
            diag_append(scopeOuts, sizeof(scopeOuts), nm, vl);
          else if (depth == 1)
          {
            int isDef = 0;

            for (int k = 0; k < nDef; k++)
              if (strcmp(def[k].n, nm) == 0 && strcmp(def[k].v, vl) == 0)
                isDef = 1;
            if (!isDef)
              diag_append(c->steady, sizeof(c->steady), nm, vl);
          }
        }
      }

      if (hasEnd && !hasBegin)
      {
        if (depth == condDepth)
        {
          if (scopeTgt >= 0 && c->nTr < 3)
          {
            FsmTrans *tr = &c->tr[c->nTr++];

            tr->tgt = (uint8_t)scopeTgt;
            strcpy(tr->cond, curCond);
            strcpy(tr->outs, scopeOuts);
          }
          condDepth = -1;
          curCond[0] = 0;
          scopeOuts[0] = 0;
          scopeTgt = -1;
        }
        depth--;
      }
    }

    // "ring" = vertically adjacent in the SAME column of the 2x4
    // top-down layout (0..2 -> +1 in col0, 4..6 -> +1 in col1); the
    // cross-column hops 3->4 and the 7->0 wraparound route through the
    // center channel like any other connection
    for (int k = 0; k < c->nTr; k++)
      c->tr[k].ring = st->si != 3 && st->si != 7 &&
                      c->tr[k].tgt == st->si + 1;
  }
  pCtx->diag = d;
}

/*
==============================================================================
Draw the FSM in diagram format.

The ring layout: SI 0..3 down the left column, SI 4..7 up the right,
bright-yellow names, steady outputs under the name, transitions as
their bare condition with the activated outputs beneath; gutter
arrows (| v ^ - +) connect ring neighbors.
==============================================================================
*/
// Merge-aware plot into the routing composition grid: crossings of
// '|' and '-' become '+', arrowheads are sticky, corners win.
static void route_put(char *g, uint8_t *gc, int gw, int rows, int x0,
                      int x, int y, char ch, uint8_t col)
{
  int gx = x - x0;

  if (g == NULL || gx < 0 || gx >= gw || y < 0 || y >= rows)
    return;

  char *cp = &g[y * gw + gx];
  uint8_t *cc = &gc[y * gw + gx];
  char o = *cp;

  // "keep the glyph, keep its color": whenever the existing character
  // survives a merge, its net's color survives with it
  if (o == ' ')
  {
    *cp = ch;
    *cc = col;
  }
  else if (o == '<' || o == '>' || o == '*')
    ;                                   // arrowheads / net dots stay
  else if (ch == '<' || ch == '>' || ch == '*')
  {
    *cp = ch;
    *cc = col;
  }
  else if (o == '|' && ch == '-')
    ;                                   // the VERTICAL wins a crossing
  else if (o == '-' && ch == '|')
  {
    *cp = '|';
    *cc = col;
  }
  else if (o == '+')
    ;                                   // an existing corner stays
  else
  {
    *cp = ch;
    *cc = col;
  }
}

// Any horizontal routing already on this row span?  (Collision test
// for the jog: two connections wanting the same row.)
static int route_row_busy(const char *g, int gw, int rows, int x0,
                          int xa, int xb, int y)
{
  if (g == NULL || y < 0 || y >= rows)
    return 0;
  for (int x = xa; x <= xb; x++)
  {
    int gx = x - x0;

    if (gx >= 0 && gx < gw &&
        (g[y * gw + gx] == '-' || g[y * gw + gx] == '+' ||
         g[y * gw + gx] == '*'))
      return 1;
  }
  return 0;
}

// Word-wrap at SPACE boundaries into a cell: each line packs as many
// whole tokens as fit in w columns at column x; ln advances per line
// and is returned so following content flows below.
static int diag_wrap(WINDOW *pWnd, int ln, int lnMax, int x, int w,
                     const char *sp)
{
  while (*sp == ' ')
    sp++;
  while (*sp != 0 && ln < lnMax)
  {
    int len = (int)strlen(sp);

    if (len > w)
    {
      len = w;
      while (len > 0 && sp[len] != ' ')
        len--;                          // last space that still fits
      if (len == 0)
        len = w;                        // one oversize token: hard cut
    }
    mvwprintw(pWnd, ln++, x, "%.*s", len, sp);
    sp += len;
    while (*sp == ' ')
      sp++;
  }
  return ln;
}

/*
==============================================================================
Draw the FSM diagram
==============================================================================
*/
void CPrism::DrawFsmDiag(WINDOW *pWnd, ListingCtx *pCtx, int x0,
                         int width, int rows)
{
  FsmDiag *d = pCtx->diag;
  FsmInfo *f = pCtx->fsm;

  if (d == NULL || f == NULL || width < 40 || rows < 12)
    return;

  int cellH    = (rows - 1) / 4;
  int lmW      = 5;                     // col0->col0 margin: 2 spaced
                                        //   lanes + 2-col '->' stub zone
  int midW     = 16;                    // 3-col JOG margin (col0-side
                                        //   bends only) + 2-col stub +
                                        //   5 spaced lanes + 2-col stub
  int colW     = (width - lmW - midW - 1) / 2;
  int contentW = colW - 5;
  int cx0      = x0 + lmW;
  int mx0      = cx0 + colW;            // middle region [mx0, mx0+midW)
  int chan0    = mx0 + 3;               // routing channel after the jog
                                        //   margin
  int cx1      = mx0 + midW;
  int gl       = x0 + 1;
  int gr       = cx1 + colW;
  int if_ln;
  int else_ln;

  // Rows recorded while drawing, for the routed connection pass:
  // every transition's label line and every state's name line
  int nameRow[8], nameEnd[8], trRow[8][3], trEnd[8][3];

  for (int i = 0; i < 8; i++)
  {
    nameRow[i] = -1;
    nameEnd[i] = 0;
    for (int k = 0; k < 3; k++)
    {
      trRow[i][k] = -1;
      trEnd[i][k] = 0;
    }
  }

  // Draw for 8 states
  for (int si = 0; si < 8; si++)
  {
    int idx = -1;

    for (int j = 0; j < f->nStates; j++)
      if (f->st[j].si == si)
        idx = j;
    if (idx < 0)
      continue;

    FsmCell *c = &d->cell[si];
    int col = si < 4 ? 0 : 1;
    int r = si < 4 ? si : si - 4;
    int x = col ? cx1 : cx0;
    int y = 1 + r * cellH;
    int ln = y;

    nameRow[si] = y;

    {

      int nl = (int)strlen(f->st[idx].name);


      if (nl > colW)

        nl = colW;

      nameEnd[si] = x + nl;         // first column after the name

    }

    wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL) | A_BOLD);
    mvwprintw(pWnd, ln++, x, "%.*s", colW, f->st[idx].name);
    wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL) | A_BOLD);

    // Steady outputs wrap at contentW on SPACE boundaries: each line
    // packs as many whole name=val sets as fit, the rest flow to the
    // next line - and everything below (the if lines) moves down with
    // ln automatically.
    if (c->steady[0])
    {
      wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
      ln = diag_wrap(pWnd, ln, y + cellH, x + 2, contentW, c->steady);
      wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
    }

    // Save the line of the 'if'
    if_ln   = ln;
    else_ln = -1;
    for (int k = 0; k < c->nTr && ln < y + cellH - 1; k++)
    {
      FsmTrans *tr = &c->tr[k];
      char lbl[64];

      if (tr->ring)
      {
        if (tr->cond[0] && strcmp(tr->cond, "else") != 0)
          snprintf(lbl, sizeof(lbl), "if %s", tr->cond[0] ? tr->cond : "(always)");
        else
        {
          snprintf(lbl, sizeof(lbl), "%s", tr->cond[0] ? tr->cond : "(always)");
          else_ln = ln;
        }
      }
      else
      {
        int ti = -1;

        for (int j = 0; j < f->nStates; j++)
          if (f->st[j].si == tr->tgt)
            ti = j;
        if (tr->cond[0])
            snprintf(lbl, sizeof(lbl), "if %s", tr->cond);
        else
            snprintf(lbl, sizeof(lbl), "goto %s", ti >= 0 ? f->st[ti].name : "?");
      }
      trRow[si][k] = ln;
      {
        int ll = (int)strlen(lbl);

        if (ll > colW - 1)
          ll = colW - 1;
        trEnd[si][k] = x + 2 + ll;    // first column after the text
      }
      wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
      mvwprintw(pWnd, ln++, x + 2, "%.*s", colW - 1, lbl);
      wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
      if (tr->outs[0] && ln < y + cellH)
      {
        // conditional outputs: same word-wrap, at indent 3 (so two
        // columns narrower than the steady lines)
        wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
        ln = diag_wrap(pWnd, ln, y + cellH, x + 3, contentW - 2,
                       tr->outs);
        wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_COMMENT));
      }
    }

    // ========================================================================
    // Draw state connection lines.
    //
    // "ring" indicates the target state is the next numerical state, either
    // from the 'if' or the 'else' condition
    // ========================================================================
    int hasRing = 0;
    for (int k = 0; k < c->nTr; k++)
      if (c->tr[k].ring)
        hasRing = 1;

    if (hasRing)
    {
      int start_ln = else_ln == -1 ? if_ln : else_ln;

      wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL));
      if (r < 7 && r != 3)
      {
        mvwaddch(pWnd, start_ln, x, '+');
        mvwaddch(pWnd, start_ln, x+1, '-');
        for (int yy = start_ln+1; yy < y + cellH - 1; yy++)
          mvwaddch(pWnd, yy, x, '|');
        mvwaddch(pWnd, y + cellH - 1, x, 'v');
      }
      else if (col == 0 && r == 3)
      {
        int by = y + cellH - 1;
      
        if (by > rows - 1)
          by = rows - 1;
        mvwaddch(pWnd, by, gl, '+');
        for (int xx = gl + 1; xx < gr; xx++)
          mvwaddch(pWnd, by, xx, '-');
        mvwaddch(pWnd, by, gr, '+');
        mvwaddch(pWnd, by - 1, gr, '^');
      }
      else if (col == 1 && r > 0)
      {
        for (int yy = y - cellH + 2; yy < y; yy++)
          mvwaddch(pWnd, yy, gr, '|');
        mvwaddch(pWnd, y - cellH + 1, gr, '^');
      }
      else if (col == 1 && r == 0)
      {
        mvwaddch(pWnd, 0, gr, '+');
        for (int xx = gl + 1; xx < gr; xx++)
          mvwaddch(pWnd, 0, xx, '-');
        mvwaddch(pWnd, 0, gl, '+');
        mvwaddch(pWnd, 1 + cellH - 1 > 1 ? 1 : 1, gl, 'v');
      }
      wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL));
    }
  }

  // ========================================================================
  // Routed connections, composed into an off-screen grid first (merge
  // rules turn crossings into '+', arrowheads stay), then blitted.
  // Reserved lanes are SPACED (every other column); the two columns
  // beside each cell column are a stub zone so every entry draws as
  // '->' (or '<-' into col0).  col1 sources route first; a col0 source
  // whose row is already taken jogs: '--', '+', one row up/down, on.
  // ========================================================================
  {
    int gw = gr - x0 + 1;
    char *g = (char *)malloc((size_t)rows * gw * 2);
    uint8_t *gc = (uint8_t *)(g + (size_t)rows * gw);

    if (g != NULL)
    {
      memset(g, ' ', (size_t)rows * gw);
      memset(gc, 0, (size_t)rows * gw);

      // One color per TARGET state, cycled: yellow green blue red orange
      uint8_t colorOf[8] = { 0 };
      int colorNext = 0;

      int midSlots = (midW - 3 - 4 + 1) / 2; // spaced lanes in the channel
      int lmSlots = (lmW - 2 + 1) / 2;      // spaced lanes in the margin
      int midL = 0, midR = 0, lmUsed = 0;

      // Shared lanes: ONE reserved column per TARGET state; every
      // source joining an already-running vertical gets a '*' net dot,
      // a source beyond the current span extends the vertical and
      // turns the end with '+'.
      int midLane[8], midLo[8], midHi[8];
      int lmLane[8], lmLo[8], lmHi[8];

      for (int i = 0; i < 8; i++)
      {
        midLane[i] = -1;
        lmLane[i] = -1;
        midLo[i] = midHi[i] = lmLo[i] = lmHi[i] = 0;
      }

      for (int pass = 0; pass < 2; pass++)
      for (int si = pass == 0 ? 4 : 0; si < (pass == 0 ? 8 : 4); si++)
      {
        FsmCell *c = &d->cell[si];

        for (int k2 = 0; k2 < c->nTr; k2++)
        {
          FsmTrans *tr = &c->tr[k2];
          int srcRow = trRow[si][k2];
          int dstRow = tr->tgt < 8 ? nameRow[tr->tgt] : -1;

          if (tr->ring || srcRow < 0 || dstRow < 0)
            continue;

          int srcCol = si < 4 ? 0 : 1;
          int dstCol = tr->tgt < 4 ? 0 : 1;
          int t = tr->tgt;

          if (srcCol == 0 && dstCol == 0)
          {
            // left-margin route; lane shared per target
            int rx = lmLane[t];

            if (rx < 0)
            {
              if (lmUsed < lmSlots)
                rx = cx0 - 2 - 1 - 2 * lmUsed++;
              else
              {
                int lo = srcRow < dstRow ? srcRow : dstRow;
                int hi = srcRow > dstRow ? srcRow : dstRow;

                for (int s2 = 0; s2 < lmSlots && rx < 0; s2++)
                {
                  int rxs = cx0 - 2 - 1 - 2 * s2;
                  int clear = 1;

                  for (int t2 = 0; t2 < 8; t2++)
                    if (lmLane[t2] == rxs &&
                        !(hi < lmLo[t2] - 1 || lo > lmHi[t2] + 1))
                      clear = 0;
                  if (clear)
                    rx = rxs;
                }
              }
              if (rx < 0)
                continue;
              lmLane[t] = rx;
              if (colorOf[t] == 0)
                colorOf[t] = (uint8_t)(1 + colorNext++ % 5);
              for (int xx = rx + 1; xx < cx0 - 1; xx++)
                route_put(g, gc, gw, rows, x0, xx, dstRow, '-', colorOf[t]);
              route_put(g, gc, gw, rows, x0, cx0 - 1, dstRow, '>', colorOf[t]);
              route_put(g, gc, gw, rows, x0, rx, dstRow, '+', colorOf[t]);
              lmLo[t] = lmHi[t] = dstRow;
            }
            for (int xx = rx + 1; xx <= cx0; xx++)
              route_put(g, gc, gw, rows, x0, xx, srcRow, '-', colorOf[t]);
            if (srcRow >= lmLo[t] && srcRow <= lmHi[t])
              route_put(g, gc, gw, rows, x0, rx, srcRow, '*', colorOf[t]);
            else
            {
              int lo = srcRow < lmLo[t] ? srcRow : lmLo[t];
              int hi = srcRow > lmHi[t] ? srcRow : lmHi[t];

              for (int yy = lo; yy <= hi; yy++)
                if (yy != srcRow && (yy < lmLo[t] || yy > lmHi[t]))
                  route_put(g, gc, gw, rows, x0, rx, yy, '|', colorOf[t]);
              route_put(g, gc, gw, rows, x0, rx, srcRow, '+', colorOf[t]);
              lmLo[t] = lo;
              lmHi[t] = hi;
            }
            continue;
          }

          // middle-channel route; lane shared per target.  Fresh slots
          // allocate by side (col0 targets left, col1 right); when they
          // run out, REUSE a column whose occupied vertical spans stay
          // clear of this net (channel routing) - five nets fit three
          // columns that way.
          int rx = midLane[t];

          if (rx < 0)
          {
            int slot = -1;

            if (midL + midR < midSlots)
              slot = dstCol == 0 ? midL++ : midSlots - 1 - midR++;
            else
            {
              int lo = srcRow < dstRow ? srcRow : dstRow;
              int hi = srcRow > dstRow ? srcRow : dstRow;

              for (int s2 = 0; s2 < midSlots && slot < 0; s2++)
              {
                int rxs = chan0 + 2 + 2 * s2;
                int clear = 1;

                for (int t2 = 0; t2 < 8; t2++)
                  if (midLane[t2] == rxs &&
                      !(hi < midLo[t2] - 1 || lo > midHi[t2] + 1))
                    clear = 0;
                if (clear)
                  slot = s2;
              }
            }
            if (slot < 0)
              continue;
            rx = chan0 + 2 + 2 * slot;
            midLane[t] = rx;
            if (colorOf[t] == 0)
              colorOf[t] = (uint8_t)(1 + colorNext++ % 5);
            if (dstCol == 0)
            {
              // the arrow reaches the NAME text: '<' one space after
              // it, the old channel-edge position becomes '-'
              int ax = nameEnd[t] + 1;

              if (ax > mx0 || ax < 1)
                ax = mx0;
              for (int xx = ax + 1; xx < rx; xx++)
                route_put(g, gc, gw, rows, x0, xx, dstRow, '-', colorOf[t]);
              route_put(g, gc, gw, rows, x0, ax, dstRow, '<', colorOf[t]);
            }
            else
            {
              for (int xx = rx + 1; xx < mx0 + midW - 1; xx++)
                route_put(g, gc, gw, rows, x0, xx, dstRow, '-', colorOf[t]);
              route_put(g, gc, gw, rows, x0, mx0 + midW - 1, dstRow, '>', colorOf[t]);
            }
            route_put(g, gc, gw, rows, x0, rx, dstRow, '+', colorOf[t]);
            midLo[t] = midHi[t] = dstRow;
          }

          int row = srcRow;

          if (srcCol == 0)
          {
            // start the dash one space after the label text (it owns
            // the rest of the cell now that target names are gone)
            int srcX = trEnd[si][k2] + 1;

            if (srcX > mx0 || srcX < 1)
              srcX = mx0;

            // conflict in the channel? jog in the dedicated margin
            if (route_row_busy(g, gw, rows, x0, chan0, rx, row))
            {
              int jog = row + (dstRow > row ? 1 : -1);

              if (jog >= 0 && jog < rows &&
                  !route_row_busy(g, gw, rows, x0, chan0, rx, jog))
              {
                for (int xx = srcX; xx <= mx0; xx++)
                  route_put(g, gc, gw, rows, x0, xx, srcRow, '-', colorOf[t]);
                route_put(g, gc, gw, rows, x0, mx0 + 1, srcRow, '+', colorOf[t]);
                route_put(g, gc, gw, rows, x0, mx0 + 1, jog, '+', colorOf[t]);
                for (int xx = mx0 + 2; xx < rx; xx++)
                  route_put(g, gc, gw, rows, x0, xx, jog, '-', colorOf[t]);
                row = jog;
              }
              else
                for (int xx = srcX; xx < rx; xx++)
                  route_put(g, gc, gw, rows, x0, xx, row, '-', colorOf[t]);
            }
            else
              for (int xx = srcX; xx < rx; xx++)
                route_put(g, gc, gw, rows, x0, xx, row, '-', colorOf[t]);
          }
          else
            for (int xx = rx + 1; xx <= cx1; xx++)
              route_put(g, gc, gw, rows, x0, xx, row, '-', colorOf[t]);

          if (row >= midLo[t] && row <= midHi[t])
            route_put(g, gc, gw, rows, x0, rx, row, '*', colorOf[t]);
          else
          {
            int lo = row < midLo[t] ? row : midLo[t];
            int hi = row > midHi[t] ? row : midHi[t];

            for (int yy = lo; yy <= hi; yy++)
              if (yy != row && (yy < midLo[t] || yy > midHi[t]))
                route_put(g, gc, gw, rows, x0, rx, yy, '|', colorOf[t]);
            route_put(g, gc, gw, rows, x0, rx, row, '+', colorOf[t]);
            midLo[t] = lo;
            midHi[t] = hi;
          }
        }
      }

      // blit the composition, each cell in its net's color
      for (int yy = 0; yy < rows; yy++)
        for (int gx = 0; gx < gw; gx++)
          if (g[yy * gw + gx] != ' ')
          {
            uint8_t c8 = gc[yy * gw + gx];
            int pr = c8 ? ROUTE_PAIR_BASE + c8 - 1
                        : SYNTAX_PAIR_NOTES_VOCAL;

            mvwaddch(pWnd, yy, x0 + gx,
                     (chtype)g[yy * gw + gx] | COLOR_PAIR(pr));
          }
      free(g);
    }
  }
}

/*
==============================================================================
Calculate the width of the FSM Diagram
==============================================================================
*/
static int diag_width(int cols)
{
  // The diagram takes what it needs and the split-pane CODE region
  // gives it up - 5 routing lanes cost real columns, and there is
  // plenty of screen width
  int w = cols / 2;

  if (w < 48) w = 48;
  if (w > 92) w = 92;
  return w;
}

/*
==============================================================================
Make a tab for the FSM diagram on narrow-terminals
==============================================================================
*/
CTab *CPrism::EnsureFsmTab(ListingCtx *pSrc)
{
  CTab *pTab;

  for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
       pTab = pTab->GetNextTab())
  {
    void *pc = pTab->SourceContext();

    if (pc != NULL && pc != (void *)&s_NotesCtxMarker &&
        pc != (void *)&s_WaveCtxMarker && *(const int *)pc == CTX_FSMDIAG)
    {
      ((DiagCtx *)pc)->src = pSrc;
      m_pParent->MakeTabActive(pTab);
      m_pParent->DrawSourceWindow();
      return pTab;
    }
  }

  DiagCtx *dc = (DiagCtx *)malloc(sizeof(DiagCtx));

  if (dc == NULL)
    return NULL;
  dc->kind = CTX_FSMDIAG;
  dc->src = pSrc;
  pTab = m_pParent->CreateNewTab("FSM");
  if (pTab == NULL)
  {
    free(dc);
    return NULL;
  }
  pTab->AttachTuiSource(this, dc);
  m_pParent->MakeTabActive(pTab);
  m_pParent->DrawSourceWindow();
  return pTab;
}

/*
==============================================================================
Determine why LoadedFsm() found nothing - point at the actual missing step
==============================================================================
*/
void CPrism::NoFsmHint(void)
{
  CTab *pTab;

  for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
       pTab = pTab->GetNextTab())
  {
    ListingCtx *p = (ListingCtx *)pTab->SourceContext();

    if (p != NULL && (void *)p != (void *)&s_NotesCtxMarker &&
        *(const int *)p == CTX_LISTING && p->verilog && p->fsm != NULL)
    {
      if (prism_chroma_loaded() < 0)
        CmdPrintf("%s is parsed, but no chroma is loaded - 'load' one"
                  " first\n", pTab->GetName());
      else
        CmdPrintf("%s is parsed, but it is not the LOADED chroma - load"
                  " it (or open the loaded chroma's .v)\n",
                  pTab->GetName());
      return;
    }
  }
  CmdPrintf("no parsed .v open ('open <chroma>.v' first)\n");
}

/*
==============================================================================
Handler for 'print [signal]': current value of a parsed in/out pin, read from
the PRISM control registers and bit-masked.  Bare form prints every
parsed signal.
==============================================================================
*/
int CPrism::Print(int argc, char *argv[])
{
  FsmInfo *f = LoadedFsm(NULL);

  if (f == NULL)
  {
    NoFsmHint();
    return -1;
  }
  if (f->nVars == 0)
  {
    CmdPrintf("no in/out assigns parsed\n");
    return -1;
  }

  uint32_t in = prism_read32(PRISM_REG_IN_DATA);
  uint32_t out = prism_read32(PRISM_REG_OUT_DATA);
  int shown = 0;

  for (int i = 0; i < f->nVars; i++)
  {
    FsmVar *v = &f->var[i];

    if (argc >= 2 && strcasecmp(argv[1], v->name) != 0)
      continue;
    CmdPrintf("%-14s %s_data[%d] = %d\n", v->name,
              v->dir == 'o' ? "out" : "in", v->num,
              (int)((v->dir == 'o' ? out : in) >> v->num) & 1);
    shown++;
  }
  if (!shown)
    CmdPrintf("no signal '%s' in the parsed .v ('print' lists them)\n",
              argv[1]);
  return shown ? OK : -1;
}

/*
==============================================================================
Handler for 'show <state>': bring a state's code into the loaded chroma's .v
view, centered, unless it is already fully visible
==============================================================================
*/
int CPrism::Show(int argc, char *argv[])
{
  ListingCtx *pCtx = NULL;
  FsmInfo    *f = LoadedFsm(&pCtx);

  (void)argc;
  if (f == NULL)
  {
    NoFsmHint();
    return -1;
  }
  // 'show fsm': the state-diagram view - a split pane when the tab is
  // wider than 120 columns (toggled by repeating the command), its own
  // FSM tab otherwise
  if (strcasecmp(argv[1], "fsm") == 0)
  {
    // 'show fsm tab': its own FSM tab even when the window is wide
    // enough for the split pane
    int forceTab = argc > 2 && strcasecmp(argv[2], "tab") == 0;

    ParseFsmDiag(pCtx);
    if (pCtx->diag == NULL)
    {
      CmdPrintf("could not parse the FSM structure\n");
      return -1;
    }

    CTab *pT;

    for (pT = m_pParent->GetFirstTab(); pT != NULL; pT = pT->GetNextTab())
      if (pT->SourceContext() == pCtx)
        break;

    int wy = 0, wx = 0;

    if (pT != NULL && pT->GetWindow() != NULL)
      getmaxyx(pT->GetWindow(), wy, wx);
    (void)wy;
    if (wx > 120 && !forceTab)
    {
      pCtx->diagShow ^= 1;
      if (pT != NULL)
        m_pParent->MakeTabActive(pT);
      m_pParent->DrawSourceWindow();
    }
    else
    {
      pCtx->diagShow = 0;         // never both the split pane and a tab
      EnsureFsmTab(pCtx);
    }
    return OK;
  }

  int si = FsmStateByName(argv[1]);
  int idx = -1;

  for (int i = 0; i < f->nStates; i++)
    if (f->st[i].si == si)
      idx = i;
  if (si < 0 || idx < 0 || f->st[idx].caseLine < 0)
  {
    CmdPrintf("no state '%s' in the parsed .v\n", argv[1]);
    return -1;
  }

  // Activate the .v tab if something else is up front
  CTab *pTab;

  for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
       pTab = pTab->GetNextTab())
    if (pTab->SourceContext() == pCtx)
      break;
  if (pTab == NULL)
    return -1;
  m_pParent->MakeTabActive(pTab);

  int first = pTab->SourceFirstLine();
  int visible = pTab->SourceWindowLineCount();
  int lo = f->st[idx].caseLine;
  int hi = f->st[idx].assignLine > lo ? f->st[idx].assignLine : lo;

  if (lo < first || hi >= first + visible)
  {
    int top = lo - (visible - (hi - lo)) / 2;   // center the state's span

    if (top > pCtx->count - visible)
      top = pCtx->count - visible;
    if (top < 0)
      top = 0;
    pTab->SourceFirstLine(top);
  }
  m_pParent->DrawSourceWindow();
  return OK;
}


/*
==============================================================================
'hide fsm': takes down whatever 'show fsm' put up - the split-pane
diagram on any .v tab and/or the standalone FSM tab
==============================================================================
*/
int CPrism::Hide(int argc, char *argv[])
{
  CTab *pT;
  int   hid = 0;

  (void)argc;
  if (strcasecmp(argv[1], "fsm") != 0)
  {
    CmdPrintf("only 'hide fsm' is supported\n");
    return -1;
  }

  // Close a standalone FSM tab wherever it is (active or not)
  for (pT = m_pParent->GetFirstTab(); pT != NULL; )
  {
    CTab *pNext = pT->GetNextTab();
    void *pc = pT->SourceContext();

    if (pc != NULL && pc != (void *)&s_NotesCtxMarker &&
        pc != (void *)&s_WaveCtxMarker && *(const int *)pc == CTX_FSMDIAG)
    {
      CloseTab(pT);
      hid = 1;
    }
    pT = pNext;
  }

  // Drop the split-pane diagram from every .v tab; the parsed diagram
  // stays cached so 'show fsm' brings it straight back
  for (pT = m_pParent->GetFirstTab(); pT != NULL; pT = pT->GetNextTab())
  {
    void *pc = pT->SourceContext();

    if (pc != NULL && pc != (void *)&s_NotesCtxMarker &&
        pc != (void *)&s_WaveCtxMarker && *(const int *)pc == CTX_LISTING &&
        ((ListingCtx *)pc)->diagShow)
    {
      ((ListingCtx *)pc)->diagShow = 0;
      hid = 1;
    }
  }

  if (!hid)
  {
    CmdPrintf("no FSM diagram is up ('show fsm' opens one)\n");
    return -1;
  }
  m_pParent->DrawSourceWindow();
  return OK;
}

/*
==============================================================================
Creates or switches to the Wave tab: an empty canvas 'play -w' draws its
strip into.  Created on first use, refocused after that.
==============================================================================
*/
CTab *CPrism::EnsureWaveTab(void)
{
  CTab *pTab;

  for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
       pTab = pTab->GetNextTab())
  {
    if (pTab->SourceContext() == &s_WaveCtxMarker)
    {
      m_pParent->MakeTabActive(pTab);
      m_pParent->DrawSourceWindow();
      return pTab;
    }
  }

  pTab = m_pParent->CreateNewTab("Wave");
  if (pTab == NULL)
    return NULL;
  pTab->AttachTuiSource(this, &s_WaveCtxMarker);
  m_pParent->MakeTabActive(pTab);
  m_pParent->DrawSourceWindow();
  return pTab;
}

/*
==============================================================================
Creates or switches to the NOTES tab
==============================================================================
*/
CTab *CPrism::EnsureNotesTab(void)
{
  CTab *pTab;

  // Focus the existing Notes tab if one is already open
  for (pTab = m_pParent->GetFirstTab(); pTab != NULL; pTab = pTab->GetNextTab())
  {
    if (pTab->SourceContext() == &s_NotesCtxMarker)
    {
      m_pParent->MakeTabActive(pTab);
      m_pParent->DrawSourceWindow();
      return pTab;
    }
  }

  pTab = m_pParent->CreateNewTab("Notes");
  if (pTab == NULL)
    return NULL;
  pTab->AttachTuiSource(this, &s_NotesCtxMarker);
  m_pParent->MakeTabActive(pTab);
  m_pParent->DrawSourceWindow();
  return pTab;
}

/*
==============================================================================
Opens / shows the NOTES tab
==============================================================================
*/
int CPrism::Notes(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  return EnsureNotesTab() != NULL ? OK : -1;
}

/*
==============================================================================
Close command handler.
==============================================================================
*/
int CPrism::Close(int argc, char *argv[])
{
  CTab *pTab = m_pParent->GetActiveSrcTab();

  (void)argc;
  (void)argv;

  if (pTab == NULL)
  {
    CmdPrintf("close: no tab open\n");
    return -1;
  }
  CloseTab(pTab);
  return OK;
}

/*
==============================================================================
Get the number of lines in the tab to be drawn based on it's tab height and
the number of actual lines to display based on the source content.
==============================================================================
*/
int CPrism::GetSourceLineCount(void *pCtx)
{
  if (pCtx == NULL)
    return 0;
  if (pCtx == &s_NotesCtxMarker)
  {
    int total = s_NoteCount + 1;        // completed lines + the open one
    return total > NOTE_LINES ? NOTE_LINES : total;
  }
  if (*(const int *)pCtx == CTX_SOUND)
    return ((SoundCtx *)pCtx)->count + 1;   // + the status row, so the
                                            // framework's paging clamp can
                                            // bring the last entry into view
  if (*(const int *)pCtx == CTX_MIDI)
    return MIDI_HEAD_LINES +
           ((CMidiFile *)pCtx)->m_Tracks * MIDI_TRK_LINES + MIDI_FOOT_LINES;
  return ((ListingCtx *)pCtx)->count;
}


/*
==============================================================================
MIDI tab (ported from pwl-tui): 'open <x>.mid' loads a CMidiFile whose
pointer IS the tab context (kind-tagged CTX_MIDI).  The tab shows the
track table + braille pitch grids; map/automap/inst/trim edit the
conversion, autosaving "<basename>.cfg" beside the .mid on the host.
==============================================================================
*/


// braille dot bits: [dot column 0/1][dot row 0..3]
static const uint8_t s_BrailleBit[2][4] =
{
  { 0x01, 0x02, 0x04, 0x40 },
  { 0x08, 0x10, 0x20, 0x80 },
};

static int midi_view_cells(int cols)
{
  int width = cols - MIDI_TAB_GUTTER - 1;

  return width < 8 ? 8 : width;
}

bool CPrism::IsMidiCtx(void *pCtx)
{
  return pCtx != NULL && pCtx != (void *)&s_NotesCtxMarker &&
         pCtx != (void *)&s_WaveCtxMarker &&
         *(const int *)pCtx == CTX_MIDI;
}

CMidiFile *CPrism::ActiveMidi(bool complain)
{
  CTab *pTab = (m_pParent != NULL) ? m_pParent->GetActiveSrcTab() : NULL;
  void *pCtx = (pTab != NULL) ? pTab->SourceContext() : NULL;

  if (IsMidiCtx(pCtx))
    return (CMidiFile *)pCtx;

  // Not the active tab: fall back to the only open MIDI tab; with
  // several the user must pick.
  {
    CMidiFile *pOnly = NULL;
    int nMidi = 0;

    for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
         pTab = pTab->GetNextTab())
    {
      if (IsMidiCtx(pTab->SourceContext()))
      {
        pOnly = (CMidiFile *)pTab->SourceContext();
        nMidi++;
      }
    }
    if (nMidi == 1)
      return pOnly;
    if (complain)
      CmdPrintf(nMidi == 0
                  ? "no MIDI tab ('open <file>.mid' first)\n"
                  : "several MIDI tabs open: switch to the one you mean\n");
  }
  return NULL;
}

void CPrism::ActivateMidiTab(CMidiFile *pMidi)
{
  CTab *pTab;

  for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
       pTab = pTab->GetNextTab())
  {
    if (pTab->SourceContext() == pMidi)
    {
      m_pParent->MakeTabActive(pTab);
      m_pParent->DrawSourceWindow();
      return;
    }
  }
}

// Every settings change autosaves the .cfg and drops the stale
// conversion so the next 'play' rebuilds it
void CPrism::CvtChanged(CMidiFile *pMidi)
{
  pMidi->SaveCfg();
  free(pMidi->m_Seq);
  pMidi->m_Seq = NULL;
  pMidi->m_SeqCount = 0;
  m_pParent->DrawSourceWindow();        // footer shows the new settings
}

int CPrism::LoadMidi(const char *arg)
{
  CMidiFile *pMidi;
  CTab      *pTab;
  char       path[96], err[80];
  FILE      *probe;

  // songs/<name> convenience on top of a literal path
  snprintf(path, sizeof(path), "%s", arg);
  if ((probe = fopen(path, "rb")) == NULL)
    snprintf(path, sizeof(path), "songs/%s", arg);
  else
    fclose(probe);

  CmdPrintf("reading %s over the host link...\n", path);
  doupdate();
  fflush(stdout);

  pMidi = new CMidiFile;
  if (pMidi == NULL)
    return -1;
  err[0] = 0;
  if (!pMidi->Load(path, err, (int)sizeof(err)))
  {
    CmdPrintf("%s\n", err[0] ? err : "load failed");
    delete pMidi;
    return -1;
  }

  CmdPrintf("%s: format %d, %d track%s, %d ticks/beat", pMidi->m_Title,
            pMidi->m_Format, pMidi->m_Tracks,
            pMidi->m_Tracks == 1 ? "" : "s", pMidi->m_Division);
  if (pMidi->TempoBpm() > 0)
    CmdPrintf(", %d bpm", pMidi->TempoBpm());
  CmdPrintf("\n");

  // Conversion defaults, then whatever the file's .cfg remembers
  pMidi->m_Cvt.inst[M2P_ROLE_MEL] = POLY_I_PIANO;
  pMidi->m_Cvt.inst[M2P_ROLE_BASS] = POLY_I_BRITE;
  pMidi->m_Cvt.inst[M2P_ROLE_PAD] = POLY_I_STRINGS;
  if (pMidi->LoadCfg())
    CmdPrintf("loaded .cfg: mel=%s bass=%s pad=%s\n",
              poly_inst_name(pMidi->m_Cvt.inst[M2P_ROLE_MEL]),
              poly_inst_name(pMidi->m_Cvt.inst[M2P_ROLE_BASS]),
              poly_inst_name(pMidi->m_Cvt.inst[M2P_ROLE_PAD]));
  CmdPrintf("('automap' guesses the mix, 'play' converts and plays)\n");

  pTab = m_pParent->CreateNewTab(pMidi->m_Title);
  if (pTab == NULL)
  {
    delete pMidi;
    return -1;
  }
  pTab->AttachTuiSource(this, pMidi);
  pTab->SourceFirstLine(0);
  m_pParent->MakeTabActive(pTab);
  m_pParent->DrawSourceWindow();
  return OK;
}

void CPrism::DrawMidiTab(CMidiFile *pMidi, WINDOW *pWnd, int topLine,
                         int lineCount)
{
  char line[160];
  int  rows, cols, width, t, y, winRows;

  getmaxyx(pWnd, winRows, cols);
  rows = winRows;
  if (lineCount < rows)
    rows = lineCount;
  if (topLine < 0)
    topLine = 0;
  width = midi_view_cells(cols);

  // The bottom rows belong to the conversion footer; tracks clip above
  if (winRows > MIDI_FOOT_LINES + 4)
    rows = (rows < winRows - MIDI_FOOT_LINES) ? rows
                                              : winRows - MIDI_FOOT_LINES;

  werase(pWnd);

  y = 0 - topLine;
  if (y >= 0 && y < rows)
  {
    int beat0 = (pMidi->m_ScrollCell * 2) / MIDI_DOTS_PER_BEAT + 1;
    int beats = (width * 2) / MIDI_DOTS_PER_BEAT;
    int total = (int)(pMidi->m_GridCols / MIDI_DOTS_PER_BEAT);

    snprintf(line, sizeof(line),
             "%s  %d %s %d/beat  beats %d-%d of %d",
             pMidi->m_Title, pMidi->m_Tracks,
             pMidi->m_RowsAreChans ? "ch" : "trk", pMidi->m_Division,
             beat0, beat0 + beats - 1, total);
    wattron(pWnd, A_BOLD);
    mvwprintw(pWnd, y, 0, "%.*s", cols - 1, line);
    wattroff(pWnd, A_BOLD);
  }

  for (t = 0; t < pMidi->m_Tracks; t++)
  {
    const MidiTrack_t *pTrk = &pMidi->m_Track[t];
    int base = MIDI_HEAD_LINES + t * MIDI_TRK_LINES - topLine;
    int pair = MIDI_PAIR(t);
    int l, x;

    y = base;
    if (y >= 0 && y < rows)
    {
      char chan[6];

      if (pTrk->channel == 0xFF)
        strcpy(chan, "-");
      else
        snprintf(chan, sizeof(chan), "%d%s", pTrk->channel,
                 pTrk->drums ? "*" : "");

      if (cols - MIDI_TAB_GUTTER >= 62)
        snprintf(line, sizeof(line),
                 "%-14.14s %-18.18s ch%-4s %5d beats %6lu notes",
                 pTrk->name[0] ? pTrk->name : "(unnamed)",
                 pMidi->Instrument(t), chan,
                 pMidi->Beats(t), (unsigned long)pTrk->notes);
      else
        snprintf(line, sizeof(line), "%-11.11s %-15.15s c%-3s %4db %5lun",
                 pTrk->name[0] ? pTrk->name : "(unnamed)",
                 pMidi->Instrument(t), chan,
                 pMidi->Beats(t), (unsigned long)pTrk->notes);
      wattron(pWnd, COLOR_PAIR(pair) | A_BOLD);
      mvwprintw(pWnd, y, 0, "%2d ", t + 1);
      wattroff(pWnd, A_BOLD);
      mvwprintw(pWnd, y, MIDI_TAB_GUTTER, "%.*s",
                cols - MIDI_TAB_GUTTER - 1, line);
      wattroff(pWnd, COLOR_PAIR(pair));
    }

    for (l = 0; l < 3; l++)
    {
      y = base + 1 + l;
      if (y < 0 || y >= rows)
        continue;
      if (pTrk->grid == NULL)
        continue;

      wattron(pWnd, COLOR_PAIR(pair));
      for (x = 0; x < width; x++)
      {
        unsigned bits = 0;
        int dx;

        for (dx = 0; dx < 2; dx++)
        {
          uint32_t dot = (uint32_t)((pMidi->m_ScrollCell + x) * 2 + dx);
          uint16_t acc;
          int r;

          if (dot >= pMidi->m_GridCols)
            break;
          acc = pTrk->grid[dot];
          for (r = 0; r < 4; r++)
            if (acc & (1u << (l * 4 + r)))
              bits |= s_BrailleBit[dx][r];
        }
        if (bits != 0)
          mvwaddch(pWnd, y, MIDI_TAB_GUTTER + x, (chtype)(0x2800 + bits));
      }
      wattroff(pWnd, COLOR_PAIR(pair));
    }
  }

  // ---- conversion footer: pinned to the window bottom, never scrolls
  if (winRows > MIDI_FOOT_LINES + 4)
  {
    const MidiCvt_t *cvt = &pMidi->m_Cvt;
    int fy = winRows - MIDI_FOOT_LINES;
    int n, i;

    wattron(pWnd, A_BOLD);
    mvwhline(pWnd, fy, 0, '-', cols > 1 ? cols - 1 : 1);
    mvwprintw(pWnd, fy, 2, " mid2prism ");

    if (cvt->satb >= 0)
      snprintf(line, sizeof(line), "map: satb=%d  drums=%s", cvt->satb,
               cvt->drums >= 0 ? "on" : "off");
    else
    {
      char mel[24];

      n = 0;
      mel[0] = 0;
      for (i = 0; i < MIDI_MEL_SRCS && cvt->melody[i] >= 0; i++)
        n += snprintf(&mel[n], sizeof(mel) - n, "%s%d", i ? "," : "",
                      cvt->melody[i]);
      n = snprintf(line, sizeof(line), "map: melody=%s",
                   mel[0] ? mel : "-");
      n += snprintf(&line[n], sizeof(line) - n, " bass=%d pad=%d drums=%d",
                    cvt->bass, cvt->pad, cvt->drums);
    }
    n = (int)strlen(line);
    for (i = 0; i < 16; i++)
      if (cvt->chan_inst[i] >= 0)
        n += snprintf(&line[n], sizeof(line) - n, " ch%d:%s", i,
                      poly_inst_name(cvt->chan_inst[i]));
    wmove(pWnd, fy + 1, 0);
    wclrtoeol(pWnd);
    mvwprintw(pWnd, fy + 1, 1, "%.*s", cols - 2, line);

    snprintf(line, sizeof(line),
             "inst: mel=%s bass=%s pad=%s  xpose=%+d trim=%ub",
             poly_inst_name(cvt->inst[M2P_ROLE_MEL]),
             poly_inst_name(cvt->inst[M2P_ROLE_BASS]),
             poly_inst_name(cvt->inst[M2P_ROLE_PAD]),
             cvt->transpose, cvt->trim_beats);
    wmove(pWnd, fy + 2, 0);
    wclrtoeol(pWnd);
    mvwprintw(pWnd, fy + 2, 1, "%.*s", cols - 2, line);

    if (pMidi->m_Seq != NULL)
      snprintf(line, sizeof(line),
               "conv: %lu events in RAM   automap map inst trim play",
               (unsigned long)pMidi->m_SeqCount);
    else
      snprintf(line, sizeof(line),
               "conv: none yet   automap map inst trim play");
    wmove(pWnd, fy + 3, 0);
    wclrtoeol(pWnd);
    mvwprintw(pWnd, fy + 3, 1, "%.*s", cols - 2, line);
    wattroff(pWnd, A_BOLD);
  }
}


/*
==============================================================================
MIDI conversion commands (map / inst / automap / trim), the converter,
and the automap heuristics - the pwl-tui pipeline sized for the poly
engine: roles resolve to poly instruments, drums to the kit.
==============================================================================
*/

typedef struct
{
  uint32_t notes, overlaps;
  uint32_t pitch_sum, dur_sum;
  uint32_t first_on, last_off;
  uint8_t  low, high;
  int      mean;
  int      ovl_pct;
  int      prog;
  uint8_t  melodic;
  int8_t   dup_of;
} ChanStat_t;

static void chan_stats(const CMidiFile *pMidi, ChanStat_t *cs)
{
  uint32_t last_off[16] = { 0 };
  uint32_t i;
  int c;

  memset(cs, 0, sizeof(ChanStat_t) * 16);
  for (c = 0; c < 16; c++)
  {
    cs[c].prog = pMidi->m_ChanProg[c] == 0xFF ? -1 : pMidi->m_ChanProg[c];
    cs[c].dup_of = -1;
    cs[c].first_on = 0xFFFFFFFFu;
  }
  for (i = 0; i < pMidi->m_NoteCount; i++)
  {
    const MidiNote_t *n = &pMidi->m_Notes[i];
    ChanStat_t *s = &cs[n->chan];

    s->notes++;
    s->pitch_sum += n->note;
    s->dur_sum += n->off_ms - n->on_ms;
    if (n->on_ms < last_off[n->chan])
      s->overlaps++;
    if (n->off_ms > last_off[n->chan])
      last_off[n->chan] = n->off_ms;
    if (n->on_ms < s->first_on)
      s->first_on = n->on_ms;
    if (n->off_ms > s->last_off)
      s->last_off = n->off_ms;
    if (s->low == 0 || n->note < s->low)
      s->low = n->note;
    if (n->note > s->high)
      s->high = n->note;
  }
  for (c = 0; c < 16; c++)
  {
    if (cs[c].notes == 0)
      continue;
    cs[c].mean = (int)(cs[c].pitch_sum / cs[c].notes);
    cs[c].ovl_pct = (int)(cs[c].overlaps * 100 / cs[c].notes);
    cs[c].melodic = (c != 9 && cs[c].notes >= 8);
  }
}

static int prior_melody(int p)
{
  if (p < 0) return 0;
  if (p >= 80 && p <= 87) return 80;
  if (p >= 72 && p <= 79) return 70;
  if (p >= 64 && p <= 71) return 60;
  if (p >= 40 && p <= 42) return 60;
  if (p >= 56 && p <= 63) return 50;
  if (p >= 8 && p <= 15) return 30;
  if (p >= 24 && p <= 31) return 25;
  if (p >= 0 && p <= 7) return 25;
  if (p >= 88 && p <= 95) return -40;
  if (p >= 32 && p <= 39) return -60;
  return 0;
}

static int prior_bass(int p)
{
  if (p < 0) return 0;
  if (p >= 32 && p <= 39) return 100;
  if (p == 43) return 60;
  if (p >= 0 && p <= 7) return 15;
  return 0;
}

static int prior_pad(int p)
{
  if (p < 0) return 0;
  if (p >= 88 && p <= 95) return 80;
  if (p >= 48 && p <= 54) return 70;
  if (p >= 16 && p <= 23) return 50;
  if (p >= 40 && p <= 47) return 30;
  return 0;
}

int CPrism::Automap(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  ChanStat_t cs[16];
  MidiCvt_t *cvt;
  int score[3][16];
  int8_t role_of[16];
  uint32_t span, total_notes = 0;
  int c, r, i, n_melodic = 0, busiest = -1;

  (void)argc;
  (void)argv;
  if (pMidi == NULL)
    return -1;
  if (pMidi->m_NoteCount == 0)
  {
    CmdPrintf("no notes to map\n");
    return -1;
  }
  cvt = &pMidi->m_Cvt;

  chan_stats(pMidi, cs);
  span = pMidi->TickToMs(pMidi->m_MaxTicks);
  if (span == 0)
    span = 1;

  // Doubled parts (same count, same register) drop out
  for (c = 0; c < 16; c++)
  {
    if (!cs[c].melodic)
      continue;
    total_notes += cs[c].notes;
    n_melodic++;
    if (busiest < 0 || cs[c].notes > cs[busiest].notes)
      busiest = c;
    for (i = 0; i < c; i++)
    {
      if (!cs[i].melodic || cs[i].dup_of >= 0)
        continue;
      uint32_t d = cs[c].notes > cs[i].notes ? cs[c].notes - cs[i].notes
                                             : cs[i].notes - cs[c].notes;

      if (d * 20 <= cs[i].notes &&
          (cs[c].mean > cs[i].mean ? cs[c].mean - cs[i].mean
                                   : cs[i].mean - cs[c].mean) <= 1)
      {
        cs[c].dup_of = (int8_t)i;
        cs[c].melodic = 0;
        n_melodic--;
        total_notes -= cs[c].notes;
        break;
      }
    }
  }

  for (i = 0; i < MIDI_MEL_SRCS; i++)
    cvt->melody[i] = -1;
  for (c = 0; c < 16; c++)
  {
    cvt->chan_gain[c] = 100;
    cvt->chan_inst[c] = -1;
    role_of[c] = -1;
  }
  cvt->bass = cvt->pad = cvt->satb = -1;
  cvt->drums = (int8_t)(cs[9].notes > 0 ? 9 : -1);

  // A single polyphonic performance is a satb split, not a role map
  if (n_melodic == 1 ||
      (busiest >= 0 && cs[busiest].notes * 100 >= total_notes * 60 &&
       cs[busiest].ovl_pct >= 35))
  {
    for (c = 0; c < 16; c++)
      if (cs[c].melodic && (n_melodic == 1 || c == busiest))
      {
        cvt->satb = (int8_t)c;
        break;
      }
  }

  if (cvt->satb < 0)
  {
    for (c = 0; c < 16; c++)
    {
      score[0][c] = score[1][c] = score[2][c] = -1000;
      if (!cs[c].melodic)
        continue;

      int mono = 100 - cs[c].ovl_pct;
      int cover = (int)((uint64_t)(cs[c].last_off - cs[c].first_on) * 100
                        / span);
      int density = (int)(cs[c].notes * 60 /
                          (total_notes ? total_notes : 1));
      int mean_dur = (int)(cs[c].dur_sum / cs[c].notes);

      score[M2P_ROLE_MEL][c] = prior_melody(cs[c].prog)
          + (cs[c].mean > 55 ? (cs[c].mean - 55 > 25 ? 25
                                                     : cs[c].mean - 55)
                             : (cs[c].mean - 55)) * 2
          + mono / 2
          + (density > 50 ? 50 : density)
          + cover / 4;
      score[M2P_ROLE_BASS][c] = prior_bass(cs[c].prog)
          + (cs[c].mean < 60 ? (60 - cs[c].mean > 30 ? 30
                                                     : 60 - cs[c].mean)
                             : (60 - cs[c].mean)) * 3
          + mono / 2;
      score[M2P_ROLE_PAD][c] = prior_pad(cs[c].prog)
          + (cs[c].ovl_pct > 60 ? 60 : cs[c].ovl_pct)
          + (mean_dur / 40 > 40 ? 40 : mean_dur / 40)
          + (cs[c].mean >= 48 && cs[c].mean <= 72 ? 20 : 0);
    }

    // Greedy: repeatedly take the best (role, channel) pair
    for (r = 0; r < 3; r++)
    {
      int bestRole = -1, bestChan = -1, best = 49;

      for (i = 0; i < 3; i++)
      {
        bool taken = (i == M2P_ROLE_MEL && cvt->melody[0] >= 0) ||
                     (i == M2P_ROLE_BASS && cvt->bass >= 0) ||
                     (i == M2P_ROLE_PAD && cvt->pad >= 0);

        if (taken)
          continue;
        for (c = 0; c < 16; c++)
          if (role_of[c] < 0 && score[i][c] > best)
          {
            best = score[i][c];
            bestRole = i;
            bestChan = c;
          }
      }
      if (bestRole < 0)
        break;
      role_of[bestChan] = (int8_t)bestRole;
      if (bestRole == M2P_ROLE_MEL)
        cvt->melody[0] = (int8_t)bestChan;
      else if (bestRole == M2P_ROLE_BASS)
        cvt->bass = (int8_t)bestChan;
      else
        cvt->pad = (int8_t)bestChan;
    }

    // Leftovers that still read as melodies fill the lead's rests
    if (cvt->melody[0] >= 0)
    {
      int lead = score[M2P_ROLE_MEL][(int)cvt->melody[0]];
      int slot = 1;

      for (c = 0; c < 16 && slot < MIDI_MEL_SRCS; c++)
        if (cs[c].melodic && role_of[c] < 0 &&
            score[M2P_ROLE_MEL][c] * 10 >= lead * 7)
        {
          cvt->melody[slot++] = (int8_t)c;
          role_of[c] = M2P_ROLE_MEL;
        }
    }
  }

  // Role instruments from the winners' GM programs
  if (cvt->satb >= 0)
    cvt->inst[M2P_ROLE_MEL] =
        (uint8_t)midi_gm_inst(cs[(int)cvt->satb].prog);
  else if (cvt->melody[0] >= 0)
    cvt->inst[M2P_ROLE_MEL] =
        (uint8_t)midi_gm_inst(cs[(int)cvt->melody[0]].prog);
  if (cvt->bass >= 0)
    cvt->inst[M2P_ROLE_BASS] =
        (uint8_t)midi_gm_inst(cs[(int)cvt->bass].prog);
  if (cvt->pad >= 0)
    cvt->inst[M2P_ROLE_PAD] =
        (uint8_t)midi_gm_inst(cs[(int)cvt->pad].prog);

  // The reasoning, one line per sounding channel
  for (c = 0; c < 16; c++)
  {
    const char *verdict = "unused";
    char extra[40];

    if (cs[c].notes == 0)
      continue;
    extra[0] = 0;
    if (c == 9)
      verdict = cvt->drums == 9 ? "drums (kit)" : "drums (unused)";
    else if (cs[c].dup_of >= 0)
    {
      snprintf(extra, sizeof(extra), "doubles ch%d", cs[c].dup_of);
      verdict = extra;
    }
    else if (cvt->satb == c)
    {
      snprintf(extra, sizeof(extra), "satb, %s",
               poly_inst_name(cvt->inst[M2P_ROLE_MEL]));
      verdict = extra;
    }
    else if (role_of[c] == M2P_ROLE_MEL || cvt->melody[0] == c)
    {
      snprintf(extra, sizeof(extra), "%s = %s",
               cvt->melody[0] == c ? "melody" : "melody (fills)",
               poly_inst_name(cvt->inst[M2P_ROLE_MEL]));
      verdict = extra;
    }
    else if (role_of[c] == M2P_ROLE_BASS)
    {
      snprintf(extra, sizeof(extra), "bass = %s",
               poly_inst_name(cvt->inst[M2P_ROLE_BASS]));
      verdict = extra;
    }
    else if (role_of[c] == M2P_ROLE_PAD)
    {
      snprintf(extra, sizeof(extra), "pad = %s",
               poly_inst_name(cvt->inst[M2P_ROLE_PAD]));
      verdict = extra;
    }
    CmdPrintf("  ch%-2d %5lu notes  mean %-3d  %s\n", c,
              (unsigned long)cs[c].notes, cs[c].mean, verdict);
  }
  CmdPrintf("('map'/'inst' adjust, then 'play')\n");
  CvtChanged(pMidi);
  return OK;
}

int CPrism::Map(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  MidiCvt_t *cvt;
  int ch;

  if (pMidi == NULL)
    return -1;
  cvt = &pMidi->m_Cvt;

  if (argc < 3)
  {
    CmdPrintf("usage: map melody <ch[,ch..]|+ch|-ch> | map"
              " bass|pad|drums|satb <ch|off>\n");
    return -1;
  }

  ch = strcmp(argv[2], "off") == 0 ? -1 : atoi(argv[2]);
  if (ch > 15)
  {
    CmdPrintf("MIDI channels are 0-15\n");
    return -1;
  }

  switch (argv[1][0])
  {
    case 'm':
    {
      int i, j;

      if (argv[2][0] == '+' || argv[2][0] == '-')
      {
        int c = atoi(&argv[2][1]);

        if (c < 0 || c > 15)
        {
          CmdPrintf("MIDI channels are 0-15\n");
          return -1;
        }
        for (i = 0; i < MIDI_MEL_SRCS; i++)
          if (cvt->melody[i] == c)
            break;
        if (argv[2][0] == '+')
        {
          if (i < MIDI_MEL_SRCS)
          {
            CmdPrintf("ch%d is already a melody source\n", c);
            return -1;
          }
          for (i = 0; i < MIDI_MEL_SRCS; i++)
            if (cvt->melody[i] < 0)
            {
              cvt->melody[i] = (int8_t)c;
              break;
            }
          if (i == MIDI_MEL_SRCS)
          {
            CmdPrintf("melody list is full (%d sources)\n", MIDI_MEL_SRCS);
            return -1;
          }
        }
        else
        {
          if (i == MIDI_MEL_SRCS)
          {
            CmdPrintf("ch%d is not in the melody list\n", c);
            return -1;
          }
          for (j = i; j < MIDI_MEL_SRCS - 1; j++)
            cvt->melody[j] = cvt->melody[j + 1];
          cvt->melody[MIDI_MEL_SRCS - 1] = -1;
        }
      }
      else
      {
        char *tok = strtok(argv[2], ",");

        for (i = 0; i < MIDI_MEL_SRCS; i++)
          cvt->melody[i] = -1;
        for (i = 0; tok != NULL && i < MIDI_MEL_SRCS; i++)
        {
          cvt->melody[i] = (int8_t)(strcmp(tok, "off") == 0 ? -1
                                                            : atoi(tok));
          tok = strtok(NULL, ",");
        }
      }
      break;
    }
    case 'b': cvt->bass = (int8_t)ch; break;
    case 'p': cvt->pad = (int8_t)ch; break;
    case 'd': cvt->drums = (int8_t)ch; break;
    case 's': cvt->satb = (int8_t)ch; break;
    default:
      CmdPrintf("roles: melody bass pad drums satb\n");
      return -1;
  }
  CvtChanged(pMidi);
  return OK;
}

int CPrism::Inst(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  int role, idx;

  if (pMidi == NULL)
    return -1;

  if (argc < 3)
  {
    char line[160];
    int n = 0;

    line[0] = 0;
    for (idx = 0; idx < poly_inst_count(); idx++)
      n += snprintf(&line[n], sizeof(line) - n, "%s%s", idx ? " " : "",
                    poly_inst_name(idx));
    CmdPrintf("usage: inst melody|bass|pad <name> | inst ch<N>"
              " <name|off>\ninstruments: %s\n", line);
    return -1;
  }

  if (argv[1][0] == 'c' && argv[1][1] == 'h' &&
      argv[1][2] >= '0' && argv[1][2] <= '9')
  {
    char list[100];
    int c = atoi(&argv[1][2]), i, n;

    if (c > 15)
    {
      CmdPrintf("MIDI channels are 0-15\n");
      return -1;
    }
    if (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "-") == 0)
      pMidi->m_Cvt.chan_inst[c] = -1;
    else
    {
      idx = poly_inst_find(argv[2]);
      if (idx < 0)
      {
        CmdPrintf("unknown instrument '%s' ('inst' lists them)\n",
                  argv[2]);
        return -1;
      }
      pMidi->m_Cvt.chan_inst[c] = (int8_t)idx;
    }
    n = 0;
    list[0] = 0;
    for (i = 0; i < 16; i++)
      if (pMidi->m_Cvt.chan_inst[i] >= 0)
        n += snprintf(&list[n], sizeof(list) - n, " ch%d:%s", i,
                      poly_inst_name(pMidi->m_Cvt.chan_inst[i]));
    CmdPrintf("channel instruments:%s\n", list[0] ? list : " (none)");
    CvtChanged(pMidi);
    return OK;
  }

  role = argv[1][0] == 'b' ? M2P_ROLE_BASS
       : argv[1][0] == 'p' ? M2P_ROLE_PAD : M2P_ROLE_MEL;
  idx = poly_inst_find(argv[2]);
  if (idx < 0)
  {
    CmdPrintf("unknown instrument '%s' ('inst' lists them)\n", argv[2]);
    return -1;
  }
  pMidi->m_Cvt.inst[role] = (uint8_t)idx;
  CvtChanged(pMidi);
  return OK;
}

int CPrism::Trim(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  int n;

  if (pMidi == NULL)
    return -1;
  if (argc < 2)
  {
    CmdPrintf("usage: trim <n> [bars]  (cut the first n beats/bars;"
              " 'trim 0' restores)\n");
    return -1;
  }
  n = atoi(argv[1]);
  if (argc > 2 && argv[2][0] == 'b')
    n *= pMidi->m_TimeSigNum ? pMidi->m_TimeSigNum : 4;
  if (n < 0)
    n = 0;
  pMidi->m_Cvt.trim_beats = (uint16_t)n;
  CmdPrintf("trim: first %d beats cut\n", n);
  CvtChanged(pMidi);
  return OK;
}

// Build the play list from the mapping: melody channels get the
// stealing protection, bass/pad their role instruments, the drums
// channel the kit; unmapped channels are LEFT OUT - that is the whole
// point of the editor.
int CPrism::MidiConvert(CMidiFile *pMidi)
{
  const MidiCvt_t *cvt = &pMidi->m_Cvt;
  uint32_t i, n = 0, trim_ms = 0;

  free(pMidi->m_Seq);
  pMidi->m_Seq = (PrismEv_t *)malloc(pMidi->m_NoteCount
                                     * sizeof(PrismEv_t));
  pMidi->m_SeqCount = 0;
  if (pMidi->m_Seq == NULL)
    return -1;
  if (cvt->trim_beats)
    trim_ms = pMidi->BeatToMs(cvt->trim_beats);

  for (i = 0; i < pMidi->m_NoteCount; i++)
  {
    const MidiNote_t *nn = &pMidi->m_Notes[i];
    PrismEv_t *e = &pMidi->m_Seq[n];
    int c = nn->chan, vocal = 0, inst = -1, note = nn->note;
    uint32_t dur = nn->off_ms - nn->on_ms;
    uint32_t vel = nn->vel;

    if (nn->on_ms < trim_ms)
      continue;
    if (c == cvt->drums)
    {
      uint32_t g = 0;

      inst = midi_drum_inst(note, &g);
      dur = g;                          // 0 = the kit piece's own gate
    }
    else if (c == cvt->satb)
      inst = cvt->inst[M2P_ROLE_MEL];
    else if (c == cvt->bass)
      inst = cvt->inst[M2P_ROLE_BASS];
    else if (c == cvt->pad)
      inst = cvt->inst[M2P_ROLE_PAD];
    else
    {
      int k;

      for (k = 0; k < MIDI_MEL_SRCS; k++)
        if (cvt->melody[k] == c)
        {
          vocal = 1;
          inst = cvt->inst[M2P_ROLE_MEL];
          break;
        }
    }
    if (inst < 0)
      continue;                         // unmapped channel: left out
    if (cvt->chan_inst[c] >= 0 && c != cvt->drums)
      inst = cvt->chan_inst[c];

    vel = vel * cvt->chan_gain[c] / 100u;
    if (vel > 127)
      vel = 127;
    if (vel < 1)
      vel = 1;
    if (c != cvt->drums)
      note += cvt->transpose;

    e->on_ms = nn->on_ms - trim_ms;
    e->dur_ms = (uint16_t)(dur > 65000 ? 65000 : dur < 45 && dur ? 45
                                                                 : dur);
    e->note = (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
    e->vel = (uint8_t)vel;
    e->inst = (uint8_t)inst;
    e->vocal = (uint8_t)vocal;
    n++;
  }
  pMidi->m_SeqCount = n;
  return (int)n;
}

/*
==============================================================================
Pan the text area of a listing tab.  Verilog tabs pan only the code
(line numbers and the FSM gutter stay put); .lst tabs pan the whole
line.  Returns the window column where panning text starts, -1 when
this tab cannot pan or is already at the end stop.
==============================================================================
*/
int CPrism::HScrollSource(void *pCtx, int delta)
{
  // MIDI tab: pan the braille score window (4 beats a step)
  if (IsMidiCtx(pCtx))
  {
    CMidiFile *pMidi = (CMidiFile *)pCtx;
    CTab *pTab = m_pParent->GetActiveSrcTab();

    if (pTab != NULL && pTab->GetWindow() != NULL)
    {
      int wy, wx;

      getmaxyx(pTab->GetWindow(), wy, wx);
      (void)wy;
      if (pMidi->Scroll(delta * 8, midi_view_cells(wx)))
        m_pParent->DrawSourceWindow();
    }
    return -1;
  }
  if (pCtx == NULL || pCtx == &s_NotesCtxMarker ||
      *(const int *)pCtx != CTX_LISTING)
    return -1;

  ListingCtx *p = (ListingCtx *)pCtx;
  int gutter = (p->verilog && p->fsm != NULL) ? 4 : 0;
  int tcol = p->verilog ? 5 + gutter : 0;
  int lc = p->leftCol + delta * 8;

  if (lc < 0)
    lc = 0;
  if (lc > 400)
    lc = 400;
  if (lc == p->leftCol)
    return -1;
  p->leftCol = lc;
  return tcol;
}

/*
==============================================================================
For Verilog Source TAB, when showing FSM split-pane diagram, the diagram
owns the right edge: exclude it from pans
==============================================================================
*/
int CPrism::HScrollClip(void *pCtx)
{
  if (pCtx == NULL || pCtx == (void *)&s_NotesCtxMarker ||
      *(const int *)pCtx != CTX_LISTING)
    return 0;

  ListingCtx *p = (ListingCtx *)pCtx;

  if (!p->diagShow || p->diag == NULL)
    return 0;

  CTab *pTab = m_pParent->GetActiveSrcTab();
  int wy = 0, wx = 0;

  if (pTab == NULL || pTab->GetWindow() == NULL)
    return 0;
  getmaxyx(pTab->GetWindow(), wy, wx);
  (void)wy;
  return wx > 80 ? diag_width(wx) : 0;
}

/*
==============================================================================
Draw the source window.  This is the main drawing routine and will call
various other draw functions base on the current tab type.
==============================================================================
*/
void CPrism::DrawSourceWindow(void *pCtx, WINDOW *pWnd, int topLine,
                              int lineCount)
{
  ListingCtx *pList = (ListingCtx *)pCtx;
  int         rows, cols;
  int         y, lineNo;
  int         hotLine = -1;

  if (pCtx == NULL || pWnd == NULL)
    return;

  // ===================================================================
  // Wave tab: an empty canvas the 'play -w' strip draws into with raw
  // escapes; curses only ever clears it, so the finished waveform stays
  // on screen until the tab itself redraws
  // ===================================================================
  if (pCtx == &s_WaveCtxMarker)
  {
    werase(pWnd);
    return;
  }

  // ===================================================================
  // FSM diagram tab (narrow terminals): the whole window is the diagram
  // ===================================================================
  if (pCtx != &s_NotesCtxMarker && *(const int *)pCtx == CTX_FSMDIAG)
  {
    DiagCtx *dc = (DiagCtx *)pCtx;

    getmaxyx(pWnd, rows, cols);
    werase(pWnd);
    if (dc->src != NULL)
      DrawFsmDiag(pWnd, (ListingCtx *)dc->src, 1, cols - 2, rows);
    return;
  }

  // ===================================================================
  // MIDI tab: track table + braille pitch grids + conversion footer
  // ===================================================================
  if (pCtx != &s_NotesCtxMarker && *(const int *)pCtx == CTX_MIDI)
  {
    DrawMidiTab((CMidiFile *)pCtx, pWnd, topLine, lineCount);
    return;
  }

  // ===================================================================
  // Notes tab: lines occupy fixed circular rows (abs index % visible
  // rows) so playback updates never repaint more than one line - see
  // NoteLineUpdate.  This full draw only runs on tab switches.
  // ===================================================================
  if (pCtx == &s_NotesCtxMarker)
  {
    int vis, total, j, idx, abs_line;

    getmaxyx(pWnd, rows, cols);
    vis = lineCount;
    if (vis > rows)
      vis = rows;
    if (vis <= 0)
      return;

    total = s_NoteCount + 1;            // completed lines + open head
    if (total > NOTE_LINES)
      total = NOTE_LINES;
    if (total > vis)
      total = vis;

    werase(pWnd);
    for (j = 0; j < total; j++)
    {
      // j lines back from the head, at its stable circular row
      idx      = (s_NoteHead - j + NOTE_LINES) % NOTE_LINES;
      abs_line = s_NoteAbs - j;
      if (abs_line < 0)
        break;
      notes_draw_span(pWnd, abs_line % vis, s_NoteRing[idx], 0, cols - 1);
    }
    // resync the delta-drawing state with what is now on screen
    s_DrawnAbs = s_NoteAbs;
    s_DrawnLen = s_NoteLen;
    s_DrawnVis = s_NoteVis;
    return;
  }

  // ======================================================
  // Sound Pack Tab
  // ======================================================
  if (*(const int *)pCtx == CTX_SOUND)
  {
    DrawSoundPack((SoundCtx *)pCtx, pWnd, topLine, lineCount);
    return;
  }

  // ===================================================================
  // Verilog source tab
  // ===================================================================
  getmaxyx(pWnd, rows, cols);
  if (lineCount > rows)
    lineCount = rows;

  // While halted, highlight the current state's row - .lst tabs only (the
  // .v source has no SI-to-line mapping), and only when this listing is
  // the chroma actually in the FSM.  Any chroma file the host serves can
  // be opened now, and a state row highlighted in some other chroma's
  // listing would be pointing at nothing.
  if (!pList->verilog && pList->chromaIdx >= 0 &&
      pList->chromaIdx == prism_chroma_loaded() && prism_dbg_is_halted())
    hotLine = LST_STATE_LINE((int)PRISM_DBG_STAT_CURR_SI(prism_dbg_status()));

  if (topLine < 0)
    topLine = 0;

  werase(pWnd);

  if (pList->verilog)
  {
    // Comments opened above the visible window render uncolored (same
    // behavior as the original pico16 viewer).  A parsed FSM tab gets a
    // gutter carrying the halted-state "-->" arrow.
    int hotV = FsmArrowLine(pList);
    int gutter = pList->fsm != NULL ? 4 : 0;
    int diagW = (pList->diagShow && pList->diag != NULL && cols > 80)
                ? diag_width(cols) : 0;

    m_cstyle_comment = 0;
    for (y = 0; y < lineCount; y++)
    {
      lineNo = topLine + y;
      if (lineNo >= pList->count)
        break;
      DrawVerilogLine(pWnd, y, lineNo, pList->starts[lineNo],
                      pList->lens[lineNo], cols - diagW, gutter,
                      lineNo == hotV, pList->leftCol);
    }
    if (diagW)
      DrawFsmDiag(pWnd, pList, cols - diagW, diagW, rows);
    return;
  }

  for (y = 0; y < lineCount; y++)
  {
    lineNo = topLine + y;
    if (lineNo >= pList->count)
      break;

    int lc = pList->leftCol;
    int len = pList->lens[lineNo] - lc;
    const char *pText = pList->starts[lineNo] + lc;

    if (len > cols - 1)
      len = cols - 1;

    if (lineNo == hotLine)
      wattron(pWnd, A_REVERSE | A_BOLD);
    if (len > 0)
      mvwprintw(pWnd, y, 0, "%.*s", len, pText);
    if (lineNo == hotLine)
      wattroff(pWnd, A_REVERSE | A_BOLD);
  }
}

/*
==============================================================================
Draw the splash screen
==============================================================================
*/
void CPrism::DrawSplash(WINDOW *pWnd)
{
  int rows, cols, y;

  if (pWnd == NULL)
    return;
  // The splash belongs to the empty tab window only: with a tab open the
  // framework repaints that tab instead (splashing over a live tab left
  // it invisible after every redraw)
  if (m_pParent && m_pParent->GetFirstTab() != NULL)
    return;

  getmaxyx(pWnd, rows, cols);
  y = rows / 2 - 6;
  if (y < 0)
    y = 0;

  werase(pWnd);
  wattron(pWnd, A_BOLD);
  wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  mvwprintw(pWnd, y,     (cols - 26) / 2, "P R I S M   D E B U G G E R");
  mvwprintw(pWnd, y+2,     (cols - 22) / 2, "         /\\");
  mvwprintw(pWnd, y+3,     (cols - 22) / 2, "        /  \\");
  mvwprintw(pWnd, y+4,     (cols - 22) / 2, "    ..-/----\\-..");
  mvwprintw(pWnd, y+5,     (cols - 22) / 2, "--''  /      \\  ''--");
  mvwprintw(pWnd, y+6,     (cols - 22) / 2, "     /________\\");
  wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_NORMAL));
  wattroff(pWnd, A_BOLD);
  mvwprintw(pWnd, y + 8, (cols - 34) / 2, "TinyQV / TT Sky 25a  peripheral 8");
  mvwprintw(pWnd, y + 9, (cols - 34) / 2, "8-state Mealy FSM, 44-bit STEWs");
  mvwprintw(pWnd, y + 10, (cols - 30) / 2, "type 'help' for commands");
  wrefresh(pWnd);
}

/*
==============================================================================
Closes the specified tab and free's resources
==============================================================================
*/
void CPrism::CloseTab(CTab *pTab)
{
  if (pTab != NULL)
  {
    void *pCtx = pTab->SourceContext();
    m_pParent->DeleteTab(pTab);
    if (pCtx != NULL && pCtx != &s_NotesCtxMarker &&
        pCtx != &s_WaveCtxMarker)   // both markers are statics, not heap
    {
      if (*(const int *)pCtx == CTX_SOUND)
      {
        SoundCtx  *pSnd = (SoundCtx *)pCtx;
        SoundBlob *pBlob = pSnd->pLoaded;

        adpcm_carrier_release();  // ramp the parked carrier down, PWM off

        while (pBlob != NULL)     // every sound this tab had fetched
        {
          SoundBlob *pNext = pBlob->pNext;
          free(pBlob);
          pBlob = pNext;
        }
        delete[] pSnd->pEntries;
        delete pSnd;
      }
      else if (*(const int *)pCtx == CTX_FSMDIAG)
        free(pCtx);               // the diagram data belongs to the .v tab
      else if (*(const int *)pCtx == CTX_MIDI)
        delete (CMidiFile *)pCtx; // frees grids/notes/tempos/conversion
      else
      {
        ListingCtx *pList = (ListingCtx *)pCtx;

        // a diagram tab mirroring this listing dies with it
        CTab *pD;

        for (pD = m_pParent->GetFirstTab(); pD != NULL;
             pD = pD->GetNextTab())
        {
          void *pc = pD->SourceContext();

          if (pc != NULL && pc != (void *)&s_NotesCtxMarker &&
              pc != (void *)&s_WaveCtxMarker &&
              *(const int *)pc == CTX_FSMDIAG &&
              ((DiagCtx *)pc)->src == (void *)pList)
          {
            m_pParent->DeleteTab(pD);
            free(pc);
            break;
          }
        }
        delete[] pList->starts;
        delete[] pList->lens;
        free(pList->text);        // the host file this tab was reading
        free(pList->fsm);         // parsed FSM facts, if a .v
        free(pList->diag);        // 'show fsm' diagram, if built
        delete pList;
      }
    }
  }
}

/*
==============================================================================
Watch window: live PRISM state, refreshed ~5x/sec by the CTui main loop
==============================================================================
*/
void CPrism::DrawWatchWindow(WINDOW *pWnd, int topLine)
{
  uint32_t ctrl  = prism_get_ctrl();
  uint32_t stat  = prism_dbg_status();
  uint32_t dbgc  = prism_dbg_get_ctrl();
  uint32_t cnt   = prism_read32(PRISM_REG_COUNT_VAL);
  uint32_t in    = prism_read32(PRISM_REG_IN_DATA);
  uint32_t out   = prism_read32(PRISM_REG_OUT_DATA);
  uint8_t  comm  = prism_read8(PRISM_REG_COMM_DATA);
  uint8_t  cmp   = prism_read8(PRISM_REG_COMPARE);
  uint8_t  fifo  = prism_read8(PRISM_REG_FIFO_STAT);
  int      line  = -topLine;

  (void)dbgc;

  if (pWnd == NULL)
    return;

  // One item per row: NAME in bright yellow at column 0, VALUE in white
  // at m_WatchValCol
#define WATCH_LINE(name, fmt, ...)                             \
  do {                                                         \
    if (line >= 0) {                                           \
      wmove(pWnd, line, 0);                                    \
      wclrtoeol(pWnd);                                         \
      wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL));      \
      mvwprintw(pWnd, line, 0, "%s", name);                    \
      wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_NOTES_VOCAL));     \
      wattron(pWnd, COLOR_PAIR(SYNTAX_PAIR_NORMAL));           \
      mvwprintw(pWnd, line, m_WatchValCol, fmt, ##__VA_ARGS__);\
      wattroff(pWnd, COLOR_PAIR(SYNTAX_PAIR_NORMAL));          \
    }                                                          \
    line++;                                                    \
  } while (0)

  WATCH_LINE("en",       "%d", prism_is_enabled());
  WATCH_LINE("irq",      "%d", (int)((ctrl & PRISM_CTRL_INTERRUPT) != 0));
  WATCH_LINE("state",    "%s", prism_dbg_is_halted()
                                 ? (prism_dbg_break_active() ? "BREAK"
                                                             : "HALTED")
                                 : "running");
  WATCH_LINE("SI curr",  "%lu", (unsigned long)PRISM_DBG_STAT_CURR_SI(stat));
  WATCH_LINE("SI next",  "%lu", (unsigned long)PRISM_DBG_STAT_NEXT_SI(stat));
  WATCH_LINE("count1",   "0x%06lx", (unsigned long)(cnt & 0xFFFFFF));
  WATCH_LINE("count2",   "0x%02lx", (unsigned long)(cnt >> 24));
  WATCH_LINE("cmp",      "0x%02x", cmp);
  WATCH_LINE("preload",  "0x%06lx",
             (unsigned long)prism_get_count1_preload());
  WATCH_LINE("comm",     "0x%02x", comm);
  WATCH_LINE("in",       "0x%04lx", (unsigned long)(in & 0xFFFF));
  WATCH_LINE("out",      "0x%03lx", (unsigned long)(out & 0x7FF));
  WATCH_LINE("fifo",     "%lu%s%s",
             (unsigned long)PRISM_FIFO_STAT_COUNT(fifo),
             (fifo & PRISM_FIFO_STAT_EMPTY) ? " empty" : "",
             (fifo & PRISM_FIFO_STAT_FULL) ? " FULL" : "");
  WATCH_LINE("out_pins", "0x%02lx",
             (unsigned long)PRISM_CTRL_GET_OUT_PINS(ctrl));
  WATCH_LINE("ui7",      "%d", (int)((ctrl & PRISM_CTRL_UI_IN7) != 0));

#undef WATCH_LINE

  wnoutrefresh(pWnd);
}

/*
==============================================================================
Debugger hot keys (F8 = step, F5 = continue, from CTui's key handler)
==============================================================================
*/
void CPrism::SingleStep(void)
{
  char step[8];
  char *argv[2];

  strcpy(step, "step");
  argv[0] = step;
  argv[1] = NULL;
  Legacy(1, argv);
}

void CPrism::Cont(void)
{
  char go[4];
  char *argv[2];

  strcpy(go, "go");
  argv[0] = go;
  argv[1] = NULL;
  Legacy(1, argv);
}
