/************************************************************************************
 * tui/MidiFile.h
 *
 * Standard MIDI file reader for the TUI's "open <file>.mid" tab.
 *
 * Reads the file off the host (tqv.py's served directory, through plain
 * stdio) and reduces it to what the tab shows: per track a name, the
 * instrument, the channel, how many notes and beats, and a coarse
 * pitch-over-time grid that draws as braille.  The file bytes are freed
 * as soon as the summary is built - only the summary lives in the tab.
 *
 * Nothing here touches curses: this is the model, PwlSynth.cxx draws it.
 * The tab's context pointer IS the CMidiFile, so the conversion settings
 * that mid2pwl.py will need when it moves on chip have a natural home
 * (see m_Cvt) with the right lifetime.
 ************************************************************************************/

#ifndef _PRISM_TUI_MIDIFILE_H
#define _PRISM_TUI_MIDIFILE_H

#include <stdint.h>
#include <stdbool.h>

// Poly instrument names <-> indexes (synth.c owns the table); used for
// the CINST cfg lines and the 'inst' command
extern "C" {
const char *poly_inst_name(int idx);
int         poly_inst_find(const char *name);
int         poly_inst_count(void);
}

// Conversion roles (mirrors pwl-tui's Mid2Pwl.h so the .cfg files and
// the map/inst command grammar stay identical)
#define M2P_ROLE_MEL   0
#define M2P_ROLE_BASS  1
#define M2P_ROLE_PAD   2

#include "../midiev.h"  // PrismEv_t: the conversion output format

#define MIDI_MAX_TRACKS  24
#define MIDI_GRID_ROWS   12     // 3 braille lines x 4 dot rows
#define MIDI_LOW_NOTE    24     // C1: the bottom of the drawn range
#define MIDI_HIGH_NOTE   96     // C7: the top

// Time is drawn at a FIXED musical scale, not scaled to the window: one
// dot column is a sixteenth note, so a quarter note is 4 dot columns (2
// character cells) and a 4/4 bar is 8 cells.  Notes share a column only
// when they really are played together, the way a score reads - the
// window scrolls along the piece instead of compressing it.
#define MIDI_DOTS_PER_BEAT 4
#define MIDI_GRID_MAX    16384  // dot columns kept (~1024 bars of 4/4)

typedef struct
{
  char     name[24];        // meta 0x03 track name, "" when absent
  uint8_t  program;         // first program change, 0xFF = none
  uint8_t  channel;         // first channel used, 0xFF = none
  uint8_t  drums;           // saw channel 10 (the GM drum channel)
  uint8_t  low, high;       // pitch range actually played
  uint32_t notes;
  uint32_t ticks;           // tick of the last event
  uint16_t *grid;           // [file's gridCols], bit per dot row
} MidiTrack_t;

// One matched note, in wall-clock ms (the tempo map applied).  The
// whole file's notes, all channels merged and sorted by onset - the
// same shape mid2pwl.py's collect_notes() hands its pipeline.
typedef struct
{
  uint32_t on_ms, off_ms;
  uint8_t  chan;            // MIDI channel 0-15
  uint8_t  note, vel;
} MidiNote_t;

// One tempo-map segment (kept after load so beats can be turned into
// wall-clock ms at any time - the trim command needs exactly that)
typedef struct
{
  uint32_t tick;
  uint32_t uspb;            // us per beat from this tick on
  uint64_t cum_us;          // us elapsed at .tick
} MidiTempo_t;

// mid2pwl conversion settings: which MIDI channels feed which synth
// role, what instrument each role plays, and the reduction knobs.
// Lives in the tab context and round-trips through <basename>.cfg on
// the host, so a file remembers its mix.  Mirrors mid2pwl.py's flags.
#define MIDI_MEL_SRCS 4
typedef struct
{
  int8_t   melody[MIDI_MEL_SRCS];   // priority list, -1 = unused slot
  int8_t   bass, pad, drums;        // MIDI channels, -1 = none
  int8_t   satb;                    // >= 0 replaces the role mapping
  int8_t   transpose;
  uint8_t  inst[3];                 // melody/bass/pad instrument index
  uint16_t legato_ms;
  uint16_t roll_ms;                 // satb chord-roll window
  uint8_t  mel_floor, bass_ceil;    // satb range guards
  uint8_t  arp_ms;                  // pad arpeggiator step, ms (0 = off):
                                    //   one voice cycles the chord tones
  uint8_t  pad_lo, pad_hi;          // pad octave-fold range
  uint8_t  amp_base[3], amp_span[3];// velocity -> amp, per role
  uint16_t trim_beats;              // beats cut from the front
  int8_t   chan_inst[16];           // per-MIDI-channel instrument index
                                    //   (-1 = the role's; 'inst ch5 violin')
  uint8_t  chan_gain[16];           // per-MIDI-channel volume, percent
                                    //   (100 = unity; 'amp ch7 50')
  int8_t   adsr_ovr[3][4];          // per-role A,D,S,R; A = -1 -> use the
                                    //   instrument's own envelope
  int16_t  skip_bars[16][8];        // per-channel rested bars, 1-based
  uint8_t  skip_cnt[16];            //   (negative = from the end;
                                    //   'skip ch0 1,-1'; notes in those
                                    //   bars become rests, timing kept)
} MidiCvt_t;

class CMidiFile
{
  public:
    // First member: prism tab contexts are told apart by a leading
    // kind int (CTX_MIDI in Prism.h's CtxKind)
    int           m_Kind;

    CMidiFile();
    ~CMidiFile();

    // Reads and summarises path; on failure fills err and returns false
    bool          Load(const char *path, char *err, int errLen);

    const char *  Instrument(int track) const;
    int           Beats(int track) const;
    int           TempoBpm(void) const;

    // Horizontal scroll, in character cells (2 dot columns each).
    // Returns true when the position actually moved.
    bool          Scroll(int cells, int visibleCells);

    // Wall-clock position of a tick / of the start of beat N (through
    // the retained tempo map)
    uint32_t      TickToMs(uint32_t tick) const;
    uint32_t      BeatToMs(uint32_t beat) const;

    // Conversion settings <-> "<basename>.cfg" beside the .mid on the
    // host.  Load returns false quietly when there is no cfg yet.
    void          CfgPath(char *buf, int len, const char *ext) const;
    bool          LoadCfg(void);
    void          SaveCfg(void) const;

  public:
    char          m_Title[40];      // basename: the tab's name
    int           m_Format;
    int           m_Division;       // ticks per quarter note
    int           m_Tracks;         // tracks summarised (<= MIDI_MAX_TRACKS)
    int           m_FileTracks;     // tracks the header claims
    uint32_t      m_Tempo;          // us per quarter note, 0 = unstated
    uint32_t      m_MaxTicks;
    uint32_t      m_Bytes;
    uint32_t      m_TicksPerDot;    // ticks in one dot column (a 16th)
    uint32_t      m_GridCols;       // dot columns held per track
    int           m_ScrollCell;     // leftmost cell shown (per tab)
    char          m_Path[96];       // as opened (cfg/save names derive)
    MidiTrack_t   m_Track[MIDI_MAX_TRACKS];

    // The retained music: every matched note in ms, onset-sorted
    MidiNote_t   *m_Notes;
    uint32_t      m_NoteCount;
    MidiTempo_t  *m_Tempos;         // the tempo map (>= 1 entry when loaded)
    uint32_t      m_TempoCount;
    uint8_t       m_TimeSigNum;     // beats per bar (meta 0x58; default 4)
    uint8_t       m_TimeSigDen;     // beat unit (meta 0x58 denominator)
    uint8_t       m_ChanProg[16];   // first program per CHANNEL (0xFF none)
    uint8_t       m_RowsAreChans;   // rows rebuilt per channel (format 0)

    // Conversion state (tui/Mid2Prism.cxx fills m_Seq)
    MidiCvt_t     m_Cvt;
    PrismEv_t    *m_Seq;            // converted song, malloc'd
    uint32_t      m_SeqCount;
    uint8_t       m_Saved;          // (kept for cfg parity; unused)
};

// General MIDI program name ("" when out of range)
const char *MidiGmName(int prog);

#endif /* _PWL_TEST_TUI_MIDIFILE_H */
