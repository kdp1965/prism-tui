/************************************************************************************
 * tui/MidiFile.cxx
 *
 * Standard MIDI file parsing.  See MidiFile.h.
 *
 * Everything here treats the file as hostile: it arrives from the host
 * filesystem and a truncated or malformed chunk must end the parse, not
 * walk off the buffer.  Every read is bounds checked against the chunk
 * end, and unknown events are skipped by their declared length.
 ************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MidiFile.h"

#define PRISM_CTX_MIDI 4  // == CTX_MIDI in Prism.h's CtxKind

// g++'s freestanding <stdlib.h> shadows newlib's and omits qsort (same
// story as malloc in cxxrt.cpp); newlib's implementation links fine.
extern "C" void qsort(void *base, size_t nmemb, size_t size,
                      int (*compar)(const void *, const void *));


#define MIDI_MAX_BYTES  (512 * 1024)    // refuse anything sillier

// General MIDI program names, program 0-127
static const char *const s_GmNames[128] =
{
  "Acoustic Grand", "Bright Acoustic", "Electric Grand", "Honky-tonk",
  "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavi",
  "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
  "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
  "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
  "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
  "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar",
  "Muted Guitar", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
  "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass",
  "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
  "Violin", "Viola", "Cello", "Contrabass",
  "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
  "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2",
  "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
  "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
  "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
  "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
  "Oboe", "English Horn", "Bassoon", "Clarinet",
  "Piccolo", "Flute", "Recorder", "Pan Flute",
  "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
  "Lead Square", "Lead Sawtooth", "Lead Calliope", "Lead Chiff",
  "Lead Charang", "Lead Voice", "Lead Fifths", "Lead Bass+Lead",
  "Pad New Age", "Pad Warm", "Pad Polysynth", "Pad Choir",
  "Pad Bowed", "Pad Metallic", "Pad Halo", "Pad Sweep",
  "FX Rain", "FX Soundtrack", "FX Crystal", "FX Atmosphere",
  "FX Brightness", "FX Goblins", "FX Echoes", "FX Sci-fi",
  "Sitar", "Banjo", "Shamisen", "Koto",
  "Kalimba", "Bag pipe", "Fiddle", "Shanai",
  "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
  "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
  "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
  "Telephone Ring", "Helicopter", "Applause", "Gunshot"
};

CMidiFile::CMidiFile()
{
  memset(this, 0, sizeof(*this));
  m_Kind = PRISM_CTX_MIDI;
  m_Division = 480;
  m_TicksPerDot = 1;

  // Conversion defaults, matching mid2pwl.py's flag defaults.  Roles
  // start unmapped (the footer shows that) except GM drums.
  for (int i = 0; i < MIDI_MEL_SRCS; i++)
    m_Cvt.melody[i] = -1;
  m_Cvt.bass = -1;
  m_Cvt.pad = -1;
  m_Cvt.drums = 9;
  m_Cvt.satb = -1;
  m_Cvt.transpose = 0;
  m_Cvt.inst[0] = 0;                // set to real defaults by Mid2Pwl
  m_Cvt.inst[1] = 0;
  m_Cvt.inst[2] = 0;
  m_Cvt.legato_ms = 120;
  m_Cvt.roll_ms = 280;
  m_Cvt.mel_floor = 60;
  m_Cvt.bass_ceil = 55;
  m_Cvt.pad_lo = 45;
  m_Cvt.pad_hi = 59;
  m_Cvt.amp_base[0] = 30; m_Cvt.amp_span[0] = 22;   // melody
  m_Cvt.amp_base[1] = 24; m_Cvt.amp_span[1] = 18;   // bass
  m_Cvt.amp_base[2] = 18; m_Cvt.amp_span[2] = 14;   // pad
  m_Cvt.trim_beats = 0;
  m_Cvt.arp_ms = 0;
  for (int c = 0; c < 16; c++)
  {
    m_Cvt.chan_gain[c] = 100;
    m_Cvt.chan_inst[c] = -1;
    m_Cvt.skip_cnt[c] = 0;
  }
  for (int r = 0; r < 3; r++)
    m_Cvt.adsr_ovr[r][0] = -1;      // no envelope override
  m_TimeSigNum = 4;
  m_TimeSigDen = 4;
  memset(m_ChanProg, 0xFF, sizeof(m_ChanProg));
}

CMidiFile::~CMidiFile()
{
  for (int t = 0; t < MIDI_MAX_TRACKS; t++)
    free(m_Track[t].grid);
  free(m_Notes);
  free(m_Tempos);
  free(m_Seq);
}

// Scroll by whole character cells, clamped so the last screen still has
// content in it.  visibleCells is what the window can show right now.
bool CMidiFile::Scroll(int cells, int visibleCells)
{
  int last = (int)(m_GridCols / 2);     // 2 dot columns per cell
  int want;

  if (visibleCells < 1)
    visibleCells = 1;
  last -= visibleCells - 1;
  if (last < 0)
    last = 0;

  want = m_ScrollCell + cells;
  if (want > last)
    want = last;
  if (want < 0)
    want = 0;
  if (want == m_ScrollCell)
    return false;

  m_ScrollCell = want;
  return true;
}

static uint32_t rd32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t rd16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// MIDI variable length quantity; advances *pp, stops at end
static uint32_t rdvar(const uint8_t **pp, const uint8_t *end)
{
  const uint8_t *p = *pp;
  uint32_t v = 0;
  int i;

  for (i = 0; i < 4 && p < end; i++)
  {
    uint8_t b = *p++;

    v = (v << 7) | (b & 0x7F);
    if ((b & 0x80) == 0)
      break;
  }
  *pp = p;
  return v;
}

static uint8_t s_TimeSigNum;    // set by walk_track pass 1 (meta 0x58)
static uint8_t s_TimeSigDen;    //   with its denominator (1 << p[1])

// Walk one MTrk chunk.  Two passes are made over every track: the first
// (fill == false) collects the metadata and the tick span, the second
// fills the grid, which needs the whole file's span to scale time.
static void walk_track(MidiTrack_t *trk, const uint8_t *p, const uint8_t *end,
                       uint32_t ticksPerDot, uint32_t gridCols,
                       uint32_t *pTempo, bool fill)
{
  uint32_t tick = 0;
  uint8_t  status = 0;

  while (p < end)
  {
    uint32_t delta = rdvar(&p, end);
    uint8_t  ev;

    tick += delta;
    if (p >= end)
      break;

    if (*p & 0x80)
      status = *p++;            // new status
    // else: running status, the byte is already data

    ev = status & 0xF0;

    if (status == 0xFF)         // meta event
    {
      uint8_t  type;
      uint32_t len;

      if (p >= end)
        break;
      type = *p++;
      len = rdvar(&p, end);
      if (len > (uint32_t)(end - p))
        break;

      if (!fill && type == 0x03 && trk->name[0] == 0)
      {
        uint32_t n = len < sizeof(trk->name) - 1 ? len : sizeof(trk->name) - 1;
        uint32_t i;

        for (i = 0; i < n; i++)
          trk->name[i] = (p[i] >= ' ' && p[i] < 0x7F) ? (char)p[i] : ' ';
        trk->name[n] = 0;
      }
      else if (!fill && type == 0x51 && len >= 3 && *pTempo == 0)
        *pTempo = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
      else if (!fill && type == 0x58 && len >= 1 && p[0] >= 1 && p[0] <= 16)
      {
        s_TimeSigNum = p[0];    // beats per bar, for 'trim N bars'
        s_TimeSigDen = 4;
        if (len >= 2 && p[1] <= 6)
          s_TimeSigDen = (uint8_t)(1u << p[1]);
      }

      p += len;
      if (type == 0x2F)         // end of track
        break;
      continue;
    }

    if (status == 0xF0 || status == 0xF7)       // sysex: skip by length
    {
      uint32_t len = rdvar(&p, end);

      if (len > (uint32_t)(end - p))
        break;
      p += len;
      continue;
    }

    // Channel voice message: 2 data bytes except program change and
    // channel pressure, which carry 1
    {
      int need = (ev == 0xC0 || ev == 0xD0) ? 1 : 2;
      uint8_t d1, d2 = 0;

      if (end - p < need)
        break;
      d1 = *p++;
      if (need == 2)
        d2 = *p++;

      if (!fill)
      {
        if (trk->channel == 0xFF)
          trk->channel = status & 0x0F;
        if ((status & 0x0F) == 9)
          trk->drums = 1;
        if (ev == 0xC0 && trk->program == 0xFF)
          trk->program = d1 & 0x7F;
      }

      if (ev == 0x90 && d2 != 0)        // note on
      {
        if (!fill)
        {
          trk->notes++;
          if (trk->low == 0 || d1 < trk->low)
            trk->low = d1;
          if (d1 > trk->high)
            trk->high = d1;
        }
        else if (trk->grid != NULL)
        {
          // Fixed musical scale: the column is simply where the note
          // falls on the time axis, so simultaneous notes share a
          // column and sequential ones never do
          uint32_t col = tick / ticksPerDot;
          int pitch = d1;
          int row;

          if (pitch < MIDI_LOW_NOTE)
            pitch = MIDI_LOW_NOTE;
          if (pitch > MIDI_HIGH_NOTE)
            pitch = MIDI_HIGH_NOTE;
          row = (MIDI_GRID_ROWS - 1) -
                ((pitch - MIDI_LOW_NOTE) * (MIDI_GRID_ROWS - 1)) /
                (MIDI_HIGH_NOTE - MIDI_LOW_NOTE);
          if (col < gridCols && row >= 0 && row < MIDI_GRID_ROWS)
            trk->grid[col] |= (uint16_t)(1u << row);
        }
      }
    }
  }

  if (!fill && tick > trk->ticks)
    trk->ticks = tick;
}

/*
==============================================================================
Note retention.  The braille grid is a picture; the converter needs the
actual music.  One more walk over the chunks collects every note on/off
and tempo change with its tick, then the tempo map turns ticks into the
wall-clock ms mid2pwl.py got from mido - so the ported pipeline sees the
same numbers the Python one does.
==============================================================================
*/

typedef struct
{
  uint32_t tick;
  uint8_t  on;              // 1 = note on
  uint8_t  chan, note, vel;
} RawEv_t;

typedef struct
{
  uint32_t tick;
  uint32_t uspb;            // us per beat from this tick on
  uint64_t cum_us;          // us elapsed at .tick (filled after sort)
} TempoEnt_t;

#define MIDI_MAX_TEMPOS 256

// Walk one chunk for raw notes + tempos (append; bounds like walk_track)
static void collect_raw(const uint8_t *p, const uint8_t *end,
                        RawEv_t *ev, uint32_t *nEv, uint32_t evMax,
                        TempoEnt_t *tp, uint32_t *nTp, uint8_t *chanProg)
{
  uint32_t tick = 0;
  uint8_t  status = 0;

  while (p < end)
  {
    uint32_t delta = rdvar(&p, end);
    uint8_t  cmd;

    tick += delta;
    if (p >= end)
      break;
    if (*p & 0x80)
      status = *p++;
    cmd = status & 0xF0;

    if (status == 0xFF)
    {
      uint8_t  type;
      uint32_t len;

      if (p >= end)
        break;
      type = *p++;
      len = rdvar(&p, end);
      if (len > (uint32_t)(end - p))
        break;
      if (type == 0x51 && len >= 3 && *nTp < MIDI_MAX_TEMPOS)
      {
        tp[*nTp].tick = tick;
        tp[*nTp].uspb = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        (*nTp)++;
      }
      p += len;
      if (type == 0x2F)
        break;
      continue;
    }
    if (status == 0xF0 || status == 0xF7)
    {
      uint32_t len = rdvar(&p, end);

      if (len > (uint32_t)(end - p))
        break;
      p += len;
      continue;
    }

    {
      int need = (cmd == 0xC0 || cmd == 0xD0) ? 1 : 2;
      uint8_t d1, d2 = 0;

      if (end - p < need)
        break;
      d1 = *p++;
      if (need == 2)
        d2 = *p++;

      if (cmd == 0xC0 && chanProg[status & 0x0F] == 0xFF)
        chanProg[status & 0x0F] = d1 & 0x7F;    // first program per channel

      if ((cmd == 0x90 || cmd == 0x80) && *nEv < evMax)
      {
        ev[*nEv].tick = tick;
        ev[*nEv].on = (cmd == 0x90 && d2 != 0);
        ev[*nEv].chan = status & 0x0F;
        ev[*nEv].note = d1 & 0x7F;
        ev[*nEv].vel = d2 & 0x7F;
        (*nEv)++;
      }
    }
  }
}

// Sort keys: raw events by tick with offs ahead of ons at the same tick
// (close before reopen); tempos and notes by time
static int raw_cmp(const void *a, const void *b)
{
  const RawEv_t *x = (const RawEv_t *)a, *y = (const RawEv_t *)b;

  if (x->tick != y->tick)
    return x->tick < y->tick ? -1 : 1;
  return (int)x->on - (int)y->on;
}

static int tempo_cmp(const void *a, const void *b)
{
  const TempoEnt_t *x = (const TempoEnt_t *)a, *y = (const TempoEnt_t *)b;

  return x->tick < y->tick ? -1 : x->tick > y->tick ? 1 : 0;
}

static int note_cmp(const void *a, const void *b)
{
  const MidiNote_t *x = (const MidiNote_t *)a, *y = (const MidiNote_t *)b;

  return x->on_ms < y->on_ms ? -1 : x->on_ms > y->on_ms ? 1 : 0;
}

// tick -> ms through the tempo map (cum_us must be built)
static uint32_t tick_to_ms(uint32_t tick, const TempoEnt_t *tp, uint32_t nTp,
                           int division)
{
  const TempoEnt_t *seg = &tp[0];
  uint32_t i;

  for (i = 1; i < nTp && tp[i].tick <= tick; i++)
    seg = &tp[i];
  return (uint32_t)((seg->cum_us + (uint64_t)(tick - seg->tick) * seg->uspb
                                   / (uint32_t)division) / 1000u);
}

bool CMidiFile::Load(const char *path, char *err, int errLen)
{
  FILE     *f;
  uint8_t  *buf = NULL;
  long      size;
  const char *base;
  const uint8_t *p, *fileEnd;
  int       t;
  bool      ok = false;

  // Tab title is the basename; the full path stays for cfg/save names
  snprintf(m_Path, sizeof(m_Path), "%s", path);
  base = strrchr(path, '/');
  base = base ? base + 1 : path;
  snprintf(m_Title, sizeof(m_Title), "%s", base);

  if ((f = fopen(path, "rb")) == NULL)
  {
    snprintf(err, errLen, "cannot open %s", path);
    return false;
  }

  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0)
  {
    snprintf(err, errLen, "cannot size %s", base);
    fclose(f);
    return false;
  }
  if (size > MIDI_MAX_BYTES)
  {
    snprintf(err, errLen, "%s is %ld bytes (max %d)", base, size,
             MIDI_MAX_BYTES);
    fclose(f);
    return false;
  }
  fseek(f, 0, SEEK_SET);

  buf = (uint8_t *)malloc((size_t)size);
  if (buf == NULL)
  {
    snprintf(err, errLen, "out of memory for %ld bytes", size);
    fclose(f);
    return false;
  }

  m_Bytes = (uint32_t)fread(buf, 1, (size_t)size, f);
  fclose(f);
  if (m_Bytes < 22)             // header + an empty track at the least
  {
    snprintf(err, errLen, "%s: short read (%lu bytes)", base,
             (unsigned long)m_Bytes);
    free(buf);
    return false;
  }

  p = buf;
  fileEnd = buf + m_Bytes;

  if (memcmp(p, "MThd", 4) != 0 || rd32(p + 4) < 6)
  {
    snprintf(err, errLen, "%s is not a MIDI file", base);
    free(buf);
    return false;
  }
  s_TimeSigNum = 0;
  m_Format     = rd16(p + 8);
  m_FileTracks = rd16(p + 10);
  m_Division   = rd16(p + 12);
  if (m_Division & 0x8000)      // SMPTE timing: frames * ticks per frame
  {
    int fps = 256 - ((m_Division >> 8) & 0xFF);
    m_Division = fps * (m_Division & 0xFF);
  }
  if (m_Division <= 0)
    m_Division = 480;
  p += 8 + rd32(p + 4);

  // Pass one: metadata, note counts, tick spans
  for (t = 0; p + 8 <= fileEnd && m_Tracks < MIDI_MAX_TRACKS; t++)
  {
    uint32_t len = rd32(p + 4);
    const uint8_t *body = p + 8;

    if (memcmp(p, "MTrk", 4) != 0)
    {
      // Foreign chunk (sequencers write private ones): skip, not stop
      if (len > (uint32_t)(fileEnd - body))
        break;
      p = body + len;
      continue;
    }
    if (len > (uint32_t)(fileEnd - body))
      len = (uint32_t)(fileEnd - body);   // truncated file: use what is there

    MidiTrack_t *trk = &m_Track[m_Tracks];

    trk->program = 0xFF;
    trk->channel = 0xFF;
    walk_track(trk, body, body + len, 1, 0, &m_Tempo, false);
    if (trk->ticks > m_MaxTicks)
      m_MaxTicks = trk->ticks;
    m_Tracks++;
    p = body + len;
  }

  if (m_Tracks == 0)
  {
    snprintf(err, errLen, "%s has no tracks", base);
    free(buf);
    return false;
  }

  // Size the time axis: a dot column per sixteenth note, however long
  // the piece turns out to be (the view scrolls; it never rescales)
  m_TicksPerDot = (uint32_t)m_Division / MIDI_DOTS_PER_BEAT;
  if (m_TicksPerDot == 0)
    m_TicksPerDot = 1;
  m_GridCols = m_MaxTicks / m_TicksPerDot + 1;
  if (m_GridCols > MIDI_GRID_MAX)
  {
    // Absurdly long file: keep the head of it rather than the whole
    // thing squashed, and say so
    m_GridCols = MIDI_GRID_MAX;
    snprintf(err, errLen, "%s: showing the first %lu beats", base,
             (unsigned long)(m_GridCols / MIDI_DOTS_PER_BEAT));
  }

  for (t = 0; t < m_Tracks; t++)
    if (m_Track[t].notes > 0)
      m_Track[t].grid = (uint16_t *)calloc(m_GridCols, sizeof(uint16_t));

  // Pass two: the grids, now that the scale is known
  p = buf + 8 + rd32(buf + 4);
  for (t = 0; t < m_Tracks && p + 8 <= fileEnd; t++)
  {
    uint32_t len = rd32(p + 4);
    const uint8_t *body = p + 8;

    if (memcmp(p, "MTrk", 4) != 0)
    {
      if (len > (uint32_t)(fileEnd - body))
        break;
      p = body + len;
      t--;                              // foreign chunk: not a track
      continue;
    }
    if (len > (uint32_t)(fileEnd - body))
      len = (uint32_t)(fileEnd - body);
    walk_track(&m_Track[t], body, body + len, m_TicksPerDot, m_GridCols,
               &m_Tempo, true);
    p = body + len;
  }

  // Pass three: retain the music itself.  Collect every raw on/off and
  // tempo change, sort, then match pairs into ms-clocked notes.
  {
    uint32_t total = 0, nEv = 0, nTp = 0, i;
    RawEv_t *raw;
    TempoEnt_t *tp;

    for (t = 0; t < m_Tracks; t++)
      total += m_Track[t].notes;

    raw = (RawEv_t *)malloc((2 * total + 64) * sizeof(RawEv_t));
    tp = (TempoEnt_t *)malloc(MIDI_MAX_TEMPOS * sizeof(TempoEnt_t));
    if (raw != NULL && tp != NULL && total > 0)
    {
      uint32_t evMax = 2 * total + 64;

      p = buf + 8 + rd32(buf + 4);
      for (t = 0; t < m_Tracks && p + 8 <= fileEnd; t++)
      {
        uint32_t len = rd32(p + 4);
        const uint8_t *body = p + 8;

        if (memcmp(p, "MTrk", 4) != 0)
        {
          if (len > (uint32_t)(fileEnd - body))
            break;
          p = body + len;
          t--;                          // foreign chunk: not a track
          continue;
        }
        if (len > (uint32_t)(fileEnd - body))
          len = (uint32_t)(fileEnd - body);
        collect_raw(body, body + len, raw, &nEv, evMax, tp, &nTp,
                    m_ChanProg);
        p = body + len;
      }

      // Tempo map: default 120bpm at tick 0, then cumulative time
      if (nTp == 0 || tp[0].tick != 0)
      {
        memmove(&tp[1], &tp[0],
                (nTp < MIDI_MAX_TEMPOS - 1 ? nTp : MIDI_MAX_TEMPOS - 1)
                * sizeof(TempoEnt_t));
        tp[0].tick = 0;
        tp[0].uspb = 500000;
        if (nTp < MIDI_MAX_TEMPOS)
          nTp++;
      }
      qsort(tp, nTp, sizeof(TempoEnt_t), tempo_cmp);
      tp[0].cum_us = 0;
      for (i = 1; i < nTp; i++)
        tp[i].cum_us = tp[i - 1].cum_us +
                       (uint64_t)(tp[i].tick - tp[i - 1].tick)
                       * tp[i - 1].uspb / (uint32_t)m_Division;

      qsort(raw, nEv, sizeof(RawEv_t), raw_cmp);

      // Match ons to offs (FIFO per chan/note, like collect_notes)
      m_Notes = (MidiNote_t *)malloc(total * sizeof(MidiNote_t));
      if (m_Notes != NULL)
      {
        RawEv_t open[128];
        uint32_t nOpen = 0;

        for (i = 0; i < nEv; i++)
        {
          const RawEv_t *e = &raw[i];

          if (e->on)
          {
            if (nOpen < 128)
              open[nOpen++] = *e;
          }
          else
          {
            uint32_t j;

            for (j = 0; j < nOpen; j++)
              if (open[j].chan == e->chan && open[j].note == e->note)
                break;
            if (j < nOpen && m_NoteCount < total)
            {
              MidiNote_t *n = &m_Notes[m_NoteCount++];

              n->on_ms = tick_to_ms(open[j].tick, tp, nTp, m_Division);
              n->off_ms = tick_to_ms(e->tick, tp, nTp, m_Division);
              n->chan = e->chan;
              n->note = e->note;
              n->vel = open[j].vel;
              memmove(&open[j], &open[j + 1],
                      (nOpen - j - 1) * sizeof(RawEv_t));
              nOpen--;
            }
          }
        }
        // Notes never released run to the end of the piece
        for (i = 0; i < nOpen && m_NoteCount < total; i++)
        {
          MidiNote_t *n = &m_Notes[m_NoteCount++];

          n->on_ms = tick_to_ms(open[i].tick, tp, nTp, m_Division);
          n->off_ms = tick_to_ms(m_MaxTicks, tp, nTp, m_Division);
          n->chan = open[i].chan;
          n->note = open[i].note;
          n->vel = open[i].vel;
        }
        qsort(m_Notes, m_NoteCount, sizeof(MidiNote_t), note_cmp);
      }

      // Format 0 (and friends): one chunk carrying many channels shows
      // as one useless row.  Rebuild the rows per CHANNEL - the same
      // split GarageBand shows, and the shape the conversion commands
      // (map/automap take channels) actually want.  raw[] still has the
      // ticks, so the braille grids rebuild at the same fixed scale.
      {
        uint32_t chCnt[16] = { 0 }, chMax[16] = { 0 };
        uint8_t  chLow[16] = { 0 }, chHigh[16] = { 0 };
        int      nChans = 0, c;

        for (i = 0; i < nEv; i++)
        {
          const RawEv_t *e = &raw[i];

          if (e->tick > chMax[e->chan])
            chMax[e->chan] = e->tick;
          if (!e->on)
            continue;
          if (chCnt[e->chan]++ == 0)
            nChans++;
          if (chLow[e->chan] == 0 || e->note < chLow[e->chan])
            chLow[e->chan] = e->note;
          if (e->note > chHigh[e->chan])
            chHigh[e->chan] = e->note;
        }

        if (nChans > m_Tracks)
        {
          int8_t rowOf[16];

          for (t = 0; t < m_Tracks; t++)
          {
            free(m_Track[t].grid);
            memset(&m_Track[t], 0, sizeof(MidiTrack_t));
          }
          m_Tracks = 0;
          for (c = 0; c < 16; c++)
          {
            rowOf[c] = -1;
            if (chCnt[c] == 0 || m_Tracks >= MIDI_MAX_TRACKS)
              continue;

            MidiTrack_t *trk = &m_Track[m_Tracks];

            memset(trk, 0, sizeof(*trk));
            trk->channel = (uint8_t)c;
            trk->drums = (c == 9);
            trk->program = m_ChanProg[c];
            trk->notes = chCnt[c];
            trk->ticks = chMax[c];
            trk->low = chLow[c];
            trk->high = chHigh[c];
            trk->grid = (uint16_t *)calloc(m_GridCols, sizeof(uint16_t));
            rowOf[c] = (int8_t)m_Tracks;
            m_Tracks++;
          }

          for (i = 0; i < nEv; i++)
          {
            const RawEv_t *e = &raw[i];
            int row, pitch, col;

            if (!e->on || rowOf[e->chan] < 0 ||
                m_Track[(int)rowOf[e->chan]].grid == NULL)
              continue;
            col = (int)(e->tick / m_TicksPerDot);
            pitch = e->note;
            if (pitch < MIDI_LOW_NOTE)
              pitch = MIDI_LOW_NOTE;
            if (pitch > MIDI_HIGH_NOTE)
              pitch = MIDI_HIGH_NOTE;
            row = (MIDI_GRID_ROWS - 1) -
                  ((pitch - MIDI_LOW_NOTE) * (MIDI_GRID_ROWS - 1)) /
                  (MIDI_HIGH_NOTE - MIDI_LOW_NOTE);
            if ((uint32_t)col < m_GridCols)
              m_Track[(int)rowOf[e->chan]].grid[col] |=
                  (uint16_t)(1u << row);
          }
          m_RowsAreChans = 1;
        }
      }
    }
    free(raw);
    // Keep the tempo map: beat -> ms conversions (trim) need it later
    m_Tempos = (MidiTempo_t *)tp;
    m_TempoCount = nTp;
  }

  if (s_TimeSigNum != 0)
  {
    m_TimeSigNum = s_TimeSigNum;
    m_TimeSigDen = s_TimeSigDen >= 1 ? s_TimeSigDen : 4;
  }

  ok = true;
  free(buf);
  return ok;
}

/*
==============================================================================
Conversion settings <-> "<basename>.cfg" on the host, KEY=VALUE lines.
Hand-editable; unknown keys are ignored so the format can grow.
==============================================================================
*/

uint32_t CMidiFile::TickToMs(uint32_t tick) const
{
  const MidiTempo_t *seg;
  uint32_t i;

  if (m_Tempos == NULL || m_TempoCount == 0)
  {
    // No map retained: single-tempo fallback (120bpm when unstated)
    uint32_t uspb = m_Tempo != 0 ? m_Tempo : 500000u;

    return (uint32_t)((uint64_t)tick * uspb / (uint32_t)m_Division / 1000u);
  }

  seg = &m_Tempos[0];
  for (i = 1; i < m_TempoCount && m_Tempos[i].tick <= tick; i++)
    seg = &m_Tempos[i];
  return (uint32_t)((seg->cum_us + (uint64_t)(tick - seg->tick) * seg->uspb
                                   / (uint32_t)m_Division) / 1000u);
}

uint32_t CMidiFile::BeatToMs(uint32_t beat) const
{
  return TickToMs(beat * (uint32_t)m_Division);
}

void CMidiFile::CfgPath(char *buf, int len, const char *ext) const
{
  int n = (int)strlen(m_Path);

  if (n > 4 && m_Path[n - 4] == '.' && m_Path[n - 3] == 'm' &&
      m_Path[n - 2] == 'i' && m_Path[n - 1] == 'd')
    n -= 4;
  snprintf(buf, len, "%.*s%s", n, m_Path, ext);
}

bool CMidiFile::LoadCfg(void)
{
  char path[112], line[96];
  FILE *f;

  CfgPath(path, sizeof(path), ".cfg");
  if ((f = fopen(path, "r")) == NULL)
    return false;

  while (fgets(line, sizeof(line), f) != NULL)
  {
    char *val = strchr(line, '=');
    char *nl;

    if (val == NULL)
      continue;
    *val++ = 0;
    if ((nl = strchr(val, '\n')) != NULL)
      *nl = 0;

    if (strcmp(line, "MELODY") == 0)
    {
      char *tok = strtok(val, ",");
      int i = 0;

      for (; i < MIDI_MEL_SRCS; i++)
        m_Cvt.melody[i] = -1;
      for (i = 0; tok != NULL && i < MIDI_MEL_SRCS; i++)
      {
        m_Cvt.melody[i] = (int8_t)atoi(tok);
        tok = strtok(NULL, ",");
      }
    }
    else if (strcmp(line, "BASS") == 0)
      m_Cvt.bass = (int8_t)atoi(val);
    else if (strcmp(line, "PAD") == 0)
      m_Cvt.pad = (int8_t)atoi(val);
    else if (strcmp(line, "DRUMS") == 0)
      m_Cvt.drums = (int8_t)atoi(val);
    else if (strcmp(line, "SATB") == 0)
      m_Cvt.satb = (int8_t)atoi(val);
    else if (strcmp(line, "TRANSPOSE") == 0)
      m_Cvt.transpose = (int8_t)atoi(val);
    else if (strcmp(line, "LEGATO") == 0)
      m_Cvt.legato_ms = (uint16_t)atoi(val);
    else if (strcmp(line, "TRIM") == 0)
      m_Cvt.trim_beats = (uint16_t)atoi(val);
    else if (strncmp(line, "ADSR", 4) == 0 && line[4] >= '0' && line[4] <= '2')
    {
      // ADSRn=a,d,s,r or ADSRn=off
      int role = line[4] - '0';

      if (val[0] == 'o')
        m_Cvt.adsr_ovr[role][0] = -1;
      else
      {
        char *tok = strtok(val, ",");
        int i;

        for (i = 0; tok != NULL && i < 4; i++)
        {
          m_Cvt.adsr_ovr[role][i] = (int8_t)atoi(tok);
          tok = strtok(NULL, ",");
        }
      }
    }
    else if (strcmp(line, "ARP") == 0)
      m_Cvt.arp_ms = (uint8_t)atoi(val);
    else if (strcmp(line, "ROLL") == 0)
      m_Cvt.roll_ms = (uint16_t)atoi(val);
    else if (strcmp(line, "MEL_FLOOR") == 0)
      m_Cvt.mel_floor = (uint8_t)atoi(val);
    else if (strcmp(line, "BASS_CEIL") == 0)
      m_Cvt.bass_ceil = (uint8_t)atoi(val);
    else if (strcmp(line, "PAD_RANGE") == 0)
    {
      char *hi = strchr(val, ',');

      if (hi != NULL)
      {
        m_Cvt.pad_lo = (uint8_t)atoi(val);
        m_Cvt.pad_hi = (uint8_t)atoi(hi + 1);
      }
    }
    else if (strcmp(line, "INST") == 0)
    {
      // instrument INDEXES; Mid2Pwl translates names <-> indexes
      char *tok = strtok(val, ",");
      int i;

      for (i = 0; tok != NULL && i < 3; i++)
      {
        m_Cvt.inst[i] = (uint8_t)atoi(tok);
        tok = strtok(NULL, ",");
      }
    }
    else if (strncmp(line, "AMP", 3) == 0 && line[3] >= '0' && line[3] <= '2')
    {
      char *span = strchr(val, ',');

      if (span != NULL)
      {
        m_Cvt.amp_base[line[3] - '0'] = (uint8_t)atoi(val);
        m_Cvt.amp_span[line[3] - '0'] = (uint8_t)atoi(span + 1);
      }
    }
    else if (strncmp(line, "GAIN", 4) == 0 &&
             line[4] >= '0' && line[4] <= '9')
    {
      int c = atoi(&line[4]);

      if (c < 16)
        m_Cvt.chan_gain[c] = (uint8_t)atoi(val);
    }
    else if (strncmp(line, "SKIP", 4) == 0 &&
             line[4] >= '0' && line[4] <= '9')
    {
      int c = atoi(&line[4]);

      if (c < 16)
      {
        const char *v = val;

        m_Cvt.skip_cnt[c] = 0;
        while (*v != 0 && m_Cvt.skip_cnt[c] < 8)
        {
          m_Cvt.skip_bars[c][m_Cvt.skip_cnt[c]++] = (int16_t)atoi(v);
          while (*v != 0 && *v != ',')
            v++;
          if (*v == ',')
            v++;
        }
      }
    }
    else if (strncmp(line, "CINST", 5) == 0 &&
             line[5] >= '0' && line[5] <= '9')
    {
      // per-MIDI-channel instrument, stored by NAME so the cfg
      // survives instrument-table reshuffles
      int c = atoi(&line[5]);
      int idx = poly_inst_find(val);

      if (c < 16 && idx >= 0)
        m_Cvt.chan_inst[c] = (int8_t)idx;
    }
  }
  fclose(f);
  return true;
}

void CMidiFile::SaveCfg(void) const
{
  char path[112];
  FILE *f;
  int i;

  CfgPath(path, sizeof(path), ".cfg");
  if ((f = fopen(path, "w")) == NULL)
    return;

  fprintf(f, "# mid2pwl settings for %s\n", m_Title);   // format shared
                                                      // with pwl-tui
  fprintf(f, "MELODY=");
  for (i = 0; i < MIDI_MEL_SRCS && m_Cvt.melody[i] >= 0; i++)
    fprintf(f, "%s%d", i ? "," : "", m_Cvt.melody[i]);
  if (i == 0)
    fprintf(f, "-1");
  fprintf(f, "\n");
  fprintf(f, "BASS=%d\nPAD=%d\nDRUMS=%d\nSATB=%d\nTRANSPOSE=%d\n",
          m_Cvt.bass, m_Cvt.pad, m_Cvt.drums, m_Cvt.satb, m_Cvt.transpose);
  fprintf(f, "INST=%d,%d,%d\n", m_Cvt.inst[0], m_Cvt.inst[1], m_Cvt.inst[2]);
  fprintf(f, "LEGATO=%u\nROLL=%u\nTRIM=%u\nARP=%u\n", m_Cvt.legato_ms,
          m_Cvt.roll_ms, m_Cvt.trim_beats, m_Cvt.arp_ms);
  fprintf(f, "MEL_FLOOR=%u\nBASS_CEIL=%u\n", m_Cvt.mel_floor,
          m_Cvt.bass_ceil);
  fprintf(f, "PAD_RANGE=%u,%u\n", m_Cvt.pad_lo, m_Cvt.pad_hi);
  for (i = 0; i < 3; i++)
    fprintf(f, "AMP%d=%u,%u\n", i, m_Cvt.amp_base[i], m_Cvt.amp_span[i]);
  for (i = 0; i < 16; i++)
    if (m_Cvt.chan_gain[i] != 100)
      fprintf(f, "GAIN%d=%u\n", i, m_Cvt.chan_gain[i]);
  for (i = 0; i < 16; i++)
    if (m_Cvt.chan_inst[i] >= 0)
      fprintf(f, "CINST%d=%s\n", i, poly_inst_name(m_Cvt.chan_inst[i]));
  for (i = 0; i < 16; i++)
    if (m_Cvt.skip_cnt[i] > 0)
    {
      fprintf(f, "SKIP%d=", i);
      for (int k = 0; k < m_Cvt.skip_cnt[i]; k++)
        fprintf(f, "%s%d", k ? "," : "", m_Cvt.skip_bars[i][k]);
      fprintf(f, "\n");
    }
  for (i = 0; i < 3; i++)
  {
    if (m_Cvt.adsr_ovr[i][0] >= 0)
      fprintf(f, "ADSR%d=%d,%d,%d,%d\n", i, m_Cvt.adsr_ovr[i][0],
              m_Cvt.adsr_ovr[i][1], m_Cvt.adsr_ovr[i][2],
              m_Cvt.adsr_ovr[i][3]);
    else
      fprintf(f, "ADSR%d=off\n", i);
  }
  fclose(f);
}

const char *CMidiFile::Instrument(int track) const
{
  const MidiTrack_t *trk;

  if (track < 0 || track >= m_Tracks)
    return "";
  trk = &m_Track[track];

  if (trk->drums)
    return "Drums";
  if (trk->program == 0xFF)
    return trk->notes ? "(no program)" : "(no notes)";
  return s_GmNames[trk->program & 0x7F];
}

int CMidiFile::Beats(int track) const
{
  if (track < 0 || track >= m_Tracks || m_Division <= 0)
    return 0;
  return (int)(m_Track[track].ticks / (uint32_t)m_Division);
}

const char *MidiGmName(int prog)
{
  return (prog >= 0 && prog < 128) ? s_GmNames[prog] : "";
}

int CMidiFile::TempoBpm(void) const
{
  if (m_Tempo == 0)
    return 0;
  return (int)(60000000u / m_Tempo);
}
