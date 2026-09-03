/************************************************************************************
 * prism-test/tui/Prism.h
 *
 * CPrism: the CTuiSource behind the PRISM debug TUI on TinyQV.
 *
 * This is a new implementation for the TT Sky 25a PRISM (peripheral 8,
 * 8 states, 44-bit STEWs) against the tinyQV SDK - the 2017 F1000-era
 * CPrism in nuttx_riscv served only as the structural reference.  All
 * hardware access goes through prism.h; all legacy console commands are
 * forwarded to console.c's cli_execute with printf output redirected
 * into the TUI command window.
 *
 *   Author: Ken Pettit <pettitkd@gmail.com>  (original TUI framework)
 ************************************************************************************/

#ifndef _PRISM_TEST_TUI_PRISM_H
#define _PRISM_TEST_TUI_PRISM_H

#include "TuiSource.h"

class CMidiFile;

class CPrism;
typedef int (CPrism::*CPrismFunc_t)(int argc, char *argv[]);

class CPrism : public CTuiSource
{
  public:
    CPrism();
    ~CPrism();

  /* CTuiSource interface */

  public:
    int                 GetSourceLineCount(void *pCtx);
    void                DrawSourceWindow(void *pCtx, WINDOW *pWnd, int topLine,
                                         int lineCount);
    void                DrawWatchWindow(WINDOW *pWnd, int topLine);
    const TuiCmd_t *    GetCommandTable(void);
    int                 GetCommandTabList(char *pCmd, const char *pBuffer,
                                          TuiSortList_t *&pList);
    void                FreeTabList(TuiSortList_t *pList);
    void                DebugPrintf(const char *fmt, ...);
    int                 ProcessLine(char *line);
    void                DrawSplash(WINDOW *pWnd);
    int                 HandleCtrlC(void);
    void                CloseTab(CTab *pTab);
    void                SingleStep(void);
    void                Cont(void);

  /* Sound pack tab: WantKeys routing (CTRL-W focuses it) */

  public:
    bool                WantProcessKey(void);
    int                 ProcessKey(int key);
    int                 WantFocus(void);
    void                SetFocus(void *pCtx, WINDOW *pWnd, int topLine,
                                 int lineCount);

  /* Command handlers (called through the TuiCmd_t table) */

  public:
    int                 Legacy(int argc, char *argv[]);   // -> cli_execute()
    int                 Help(int argc, char *argv[]);
    int                 Show(int argc, char *argv[]);     // center a state's code
    int                 Print(int argc, char *argv[]);    // parsed pin value
    int                 HScrollSource(void *pCtx, int delta);
    int                 HScrollClip(void *pCtx);
    int                 Clear(int argc, char *argv[]);
    int                 Open(int argc, char *argv[]);     // chroma .lst tab
    int                 Close(int argc, char *argv[]);    // close active tab
    int                 Hide(int argc, char *argv[]);     // take down 'show fsm'
    int                 Notes(int argc, char *argv[]);    // note progression tab
    int                 Automap(int argc, char *argv[]);  // guess the MIDI map
    int                 Map(int argc, char *argv[]);      // channels -> roles
    int                 Inst(int argc, char *argv[]);     // role/chan instruments
    int                 Trim(int argc, char *argv[]);     // cut leading beats

  /* Stdout redirection: legacy printf output -> command window */

  public:
    void                InstallStdoutHook(void);
    void                RemoveStdoutHook(void);
    void                StdoutChunk(const char *buffer, int length);
    void                FlushStdoutLine(void);
    int                 CmdPrintf(const char *fmt, ...);
    void                NoteChunk(const char *text);      // -> Notes tab ring
    bool                IsMidiCtx(void *pCtx);
    CMidiFile *         ActiveMidi(bool complain);
    void                ActivateMidiTab(CMidiFile *pMidi);
    void                CvtChanged(CMidiFile *pMidi);
    int                 LoadMidi(const char *arg);
    int                 MidiConvert(CMidiFile *pMidi);    // -> m_Seq
    void                DrawMidiTab(CMidiFile *pMidi, WINDOW *pWnd,
                                    int topLine, int lineCount);

  /* Chroma listing / source tabs */

  public:
    // Every source tab's context starts with one of these, so a void *
    // handed back by the framework can be told apart (the Notes tab is a
    // static marker address, compared before any of this)
    enum CtxKind { CTX_LISTING = 1, CTX_SOUND, CTX_FSMDIAG, CTX_MIDI };

    // FSM facts parsed out of a chroma .v source when its tab opens:
    // states (name, SI, case-label line, first assignment line of the
    // state's begin block) and the variables wired to the in/out
    // vectors.  Powers name-based breakpoints and the halted "-->".
    struct FsmState
    {
      char         name[28];
      int          si;
      int          caseLine;
      int          assignLine;
    };
    struct FsmVar
    {
      char         name[28];
      char         dir;           // 'i' = in_data bit, 'o' = out_data bit
      int          num;
    };
    struct FsmInfo
    {
      int          nStates;
      int          nVars;
      FsmState     st[16];
      FsmVar       var[24];
    };
    struct FsmTrans { uint8_t tgt; uint8_t ring; char cond[30]; char outs[56]; };
    // The pad keeps tr[] (and so every cond/outs string) WORD-ALIGNED:
    // newlib's optimized str* misread odd-address strings on this core
    struct FsmCell  { char steady[72]; uint8_t nTr; uint8_t pad; FsmTrans tr[3]; };
    struct FsmDiag  { FsmCell cell[8]; };
    struct DiagCtx  { int kind; void *src; };   // FSM-diagram tab context

    struct ListingCtx
    {
      int          kind;          // CTX_LISTING
      int          chromaIdx;     // console.c chroma table index, -1 = none
      bool         verilog;       // .v source (no SI highlight) vs .lst
      int          count;         // line count
      char        *text;          // the file, read from the host FS (owned)
      const char **starts;        // start of each line in the text
      int         *lens;          // length of each line (no newline)
      FsmInfo     *fsm;           // parsed .v facts, NULL otherwise
      int         leftCol;      // horizontal pan (chars)
      FsmDiag    *diag;         // 'show fsm' parsed diagram
      uint8_t     diagShow;     // split-pane diagram visible
    };

    // One entry of a .spk sound pack, as stored in the file's index
    struct SoundEntry
    {
      char         name[40];
      uint32_t     offset;        // of the ADPCM blob, from file start
      uint32_t     length;        // blob bytes
      uint32_t     samples;
    };

    // One fetched sound, kept on the tab's loaded list (most recently
    // used first) so replays never refetch.  Node and ADPCM data are one
    // allocation; the list is released when the tab closes, or a tail
    // entry at a time if an allocation needs the room back.
    struct SoundBlob
    {
      SoundBlob     *pNext;
      int            idx;         // entry this blob belongs to
      uint32_t       length;
      uint8_t        data[];      // the ADPCM blob itself
    };

    // A sound pack tab.  The index lives here; the audio itself is
    // fetched an entry at a time - over the host filesystem link that is
    // ~10KB/s - and kept on pLoaded for instant replays.
    // A pack loaded into RAM B ('tqv.py load pack.spk') needs no fetch:
    // pBase points straight at it.
    struct SoundCtx
    {
      int            kind;        // CTX_SOUND
      char           path[72];    // host file, empty when pBase is set
      const uint8_t *pBase;       // in-memory pack, NULL for a host file
      int            bank;        // 0 host file, 1 RAM B, 2 flash
      int            count;
      uint32_t       rate;
      int            sel;         // highlighted entry
      int            wave;        // 'open ... -w': scope in cmd window
      SoundBlob     *pLoaded;     // fetched sounds, MRU first
      SoundEntry    *pEntries;
      WINDOW        *pWnd;        // window the framework last drew us in,
                                  //   so the status line can be repainted
                                  //   from a keystroke (there is no public
                                  //   source-window accessor on CTui)
    };

  private:
    void                AddTuiSortItem(TuiSortList_t *pList, const char *pStr);
    int                 HelpEmit(const char *usage, const char *help,
                                 int &onPage, int rows);
    char *              LoadHostFile(const char *path, int &len);
    void                ParseFsm(ListingCtx *pCtx);
    FsmInfo *           LoadedFsm(ListingCtx **ppCtx);
    int                 FsmStateByName(const char *name);
    int                 FsmArrowLine(ListingCtx *pCtx);
    void                FsmJumpToState(void);
    void                IdlePoll(void);
    bool                ChromaPath(const char *name, bool verilog,
                                   char *path, int size);
    int                 OpenSoundPack(const char *arg, int bank, int wave);
    void                DrawSoundPack(SoundCtx *pCtx, WINDOW *pWnd,
                                      int topLine, int lineCount);
    void                SoundRowText(SoundCtx *pCtx, int idx, char *line,
                                     int size);
    void                SoundRowRepaint(SoundCtx *pCtx, int idx, int topLine);
    void                SoundPlay(SoundCtx *pCtx, int idx);
    SoundBlob *         SoundFind(SoundCtx *pCtx, int idx);
    SoundBlob *         SoundFetch(SoundCtx *pCtx, int idx);
    void                SoundStatus(SoundCtx *pCtx, const char *fmt, ...);
    SoundCtx *          ActiveSoundTab(void);
    TuiSortList_t *     BuildListFromNames(const char *const *names);
    void                AddFatNames(TuiSortList_t *pList, const void *table,
                                    bool packs);
    void                AddStateNames(TuiSortList_t *pList);
    void                AddVarNames(TuiSortList_t *pList);
    void                NoFsmHint(void);
    void                ParseFsmDiag(ListingCtx *pCtx);
    void                DrawFsmDiag(WINDOW *pWnd, ListingCtx *pCtx,
                                    int x0, int width, int rows);
    CTab               *EnsureFsmTab(ListingCtx *pSrc);
    CTab *              EnsureNotesTab(void);
    CTab *              EnsureWaveTab(void);
    void                NoteLineUpdate(void);   // redraw just the head line
    char *              GetLineToken(char *pLine, int col, int &syntax);
    void                AppendWS(char *pLine);
    void                DrawVerilogLine(WINDOW *pWnd, int y, int lineNo,

                        const char *text, int len, int cols,

                        int gutter, bool hot, int lcol);

  private:
    TuiSortList_t     * m_pCmdTabList;      // persistent command-name list
    int                 m_BpState[2];       // state each bp slot targets
                                            //   (TUI shadow; -1 = unknown)
    int                 m_LastHalted;       // async-break edge detector
    uint32_t            m_HaltPollAt;       // next IdlePoll halt check
    int                 m_WatchValCol;      // column where watch VALUES
                                            //   start (names at column 0)
    char                m_OutLine[256];     // stdout hook line accumulator
    int                 m_OutLen;
    bool                m_OutOpen;          // partial line already printed
    char                m_token[256];       // syntax tokenizer scratch
    int                 m_cstyle_comment;   // inside /* */ (per draw pass)
};

#endif /* _PRISM_TEST_TUI_PRISM_H */
