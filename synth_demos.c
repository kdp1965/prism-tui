// ==========================================================================
// Demo sequencer.  A song is a list of events; each event optionally
// switches preset, then plays a note (or a rest) for len steps, releasing
// after gate steps (gate 0 = legato hold into the next event).  arp packs
// two chord intervals as nibbles (lo first, 0 = unused); the sounding
// pitch cycles base / base+lo / base+hi every SYNTH_ARP_MS without
// retriggering the envelope - the classic chiptune chord.  vel gives each
// note a dynamic weight: 1 (whisper) .. 15, with 0 meaning full scale
// (16/16), so zero-filled entries play at full volume.
// The song loops until any key is pressed.
// ==========================================================================

#include <stdio.h>
#include <stdint.h>
#include <uart.h>
#include <csr.h>
#include "synth.h"

typedef struct {
    uint8_t note;      // 0 = rest (release), else nX(octave) below
    uint8_t preset;    // PRE_* to switch, 0xFF = keep current
    uint8_t len;       // steps until the next event (0 = end of song)
    uint8_t gate;      // steps until release, 0 = legato
    uint8_t arp;       // chord intervals, lo/hi nibble (0 = plain note)
    uint8_t tempo;     // new step time in ms from this event on, 0 = keep
    uint8_t vel;       // velocity: 1..15 scale the envelope, 0 = full (16/16)
    uint8_t arp_ms;    // ms per arp alternation, 0 = default (35).  ~10ms
                       //   fuses two notes into one "polyphonic" voice
} demo_ev_t;

// Note number macros (nC(o)..nB(o)) come from synth.h

#define ARP_MIN  0x73   // minor chord: +3, +7
#define ARP_MAJ  0x74   // major chord: +4, +7
#define KEEP     0xFF
// Demo 1 "chip": uptempo chiptune groove, Am F C G, kick/hat + bass +
// arpeggio chords.  One bar = 16 steps of 110ms (~136 BPM).
// Dynamics: full kick, soft hats, walking bass slightly accented on the
// downbeat, upper arps brighter than the low ones.
static const demo_ev_t demo_chip[] = {
    //  note   preset     len gate arp     tmpo vel
    // Am
    { nC(2), PRE_DRUM,  2, 1, 0,       0,  0 },  // kick
    { nA(2), PRE_PULSE, 2, 1, 0,       0, 13 },  // bass (accent)
    { nA(3), PRE_THIN,  4, 3, ARP_MIN, 0, 11 },  // Am arp
    { nC(6), PRE_DRUM,  2, 1, 0,       0,  7 },  // hat (soft)
    { nA(2), PRE_PULSE, 2, 1, 0,       0, 11 },
    { nA(4), PRE_THIN,  4, 3, ARP_MIN, 0, 13 },  // upper arp (bright)
    // F
    { nC(2), PRE_DRUM,  2, 1, 0,       0,  0 },
    { nF(2), PRE_PULSE, 2, 1, 0,       0, 13 },
    { nF(3), PRE_THIN,  4, 3, ARP_MAJ, 0, 11 },
    { nC(6), PRE_DRUM,  2, 1, 0,       0,  7 },
    { nF(2), PRE_PULSE, 2, 1, 0,       0, 11 },
    { nF(4), PRE_THIN,  4, 3, ARP_MAJ, 0, 13 },
    // C
    { nC(2), PRE_DRUM,  2, 1, 0,       0,  0 },
    { nC(3), PRE_PULSE, 2, 1, 0,       0, 13 },
    { nC(4), PRE_THIN,  4, 3, ARP_MAJ, 0, 11 },
    { nC(6), PRE_DRUM,  2, 1, 0,       0,  7 },
    { nC(3), PRE_PULSE, 2, 1, 0,       0, 11 },
    { nC(5), PRE_THIN,  4, 3, ARP_MAJ, 0, 13 },
    // G
    { nC(2), PRE_DRUM,  2, 1, 0,       0,  0 },
    { nG(2), PRE_PULSE, 2, 1, 0,       0, 13 },
    { nG(3), PRE_THIN,  4, 3, ARP_MAJ, 0, 11 },
    { nC(6), PRE_DRUM,  2, 1, 0,       0,  7 },
    { nG(2), PRE_PULSE, 2, 1, 0,       0, 11 },
    { nG(4), PRE_THIN,  4, 3, ARP_MAJ, 0, 14 },  // last arp lifts into loop
    { 0, 0, 0, 0, 0, 0, 0 },
};

// Demo 2 "wave": slow synthwave ballad, shimmering PWM pads, a brass
// lead with vibrato, bell outro.  Step = 176ms (~85 BPM sixteenths).
// Dynamics: soft pads, lead phrases swell towards their peak notes and
// taper at the ends, bells fade away.
static const demo_ev_t demo_wave[] = {
    //  note   preset     len gate arp     tmpo vel
    // Pads (PWM sweep arpeggios)
    { nA(3), PRE_PWM,   8, 7, ARP_MIN, 0, 10 },
    { nF(3), KEEP,      8, 7, ARP_MAJ, 0, 10 },
    { nC(3), KEEP,      8, 7, ARP_MAJ, 0, 11 },
    { nG(3), KEEP,      8, 7, ARP_MAJ, 0, 11 },
    // Lead melody over Am F C G
    { nA(3), PRE_BRASS, 4, 0, 0,       0, 11 },
    { nC(4), KEEP,      2, 0, 0,       0, 12 },
    { nB(3), KEEP,      2, 0, 0,       0, 11 },
    { nA(3), KEEP,      4, 0, 0,       0, 10 },
    { nE(4), KEEP,      4, 3, 0x04,    0, 15, 10 },  // phrase peak, octave duo
    { nF(4), KEEP,      4, 0, 0,       0, 14 },
    { nE(4), KEEP,      2, 0, 0,       0, 12 },
    { nD(4), KEEP,      2, 0, 0,       0, 11 },
    { nC(4), KEEP,      8, 6, 0x04,    0, 12, 12 },  // held note, fifth duo
    { nE(4), KEEP,      4, 0, 0,       0, 12 },
    { nG(4), KEEP,      2, 0, 0,       0, 15 },  // phrase peak
    { nF(4), KEEP,      2, 0, 0,       0, 13 },
    { nE(4), KEEP,      4, 0, 0,       0, 12 },
    { nD(4), KEEP,      4, 3, 0,       0, 11 },
    { nD(4), KEEP,      4, 0, 0,       0, 12 },
    { nB(3), KEEP,      2, 0, 0,       0, 10 },
    { nC(4), KEEP,      2, 0, 0,       0, 11 },
    { nA(3), KEEP,      6, 5, 0x04,    0, 13, 10 },  // closing note sings, duo
    { 0,     KEEP,      2, 0, 0,       0,  0 },
    // Bell outro
    { nA(4), PRE_BELL,  8, 6, ARP_MIN, 0, 12 },
    { nA(3), KEEP,      8, 6, ARP_MIN, 0,  9 },  // echo, quieter
    { 0,     KEEP,      4, 0, 0,       0,  0 },
    { 0, 0, 0, 0, 0, 0, 0 },
};

// Demo 3 "megamix": a full-length arrangement combining both styles.
// Drum kit (noise preset by pitch): nC(1) = kick, nB(4) = snare,
// nC(7) = hat.  The tempo field varies the step time as the song
// progresses: dreamy 176ms intro -> accelerating build -> 110ms chip
// groove -> lead verse -> breakdown -> drum break with an accelerando
// fill -> bright 95ms finale -> rallentando bell outro, then loops.
static const demo_ev_t demo_mega[] = {
    //  note   preset     len gate arp     tmpo vel
    // --- Intro: bells over nothing, then pads swell (176ms) ---
    { nA(4), PRE_BELL,  8, 6, ARP_MIN, 176, 11 },
    { nE(4), KEEP,      8, 6, 0,       0,    9 },
    { nA(3), PRE_PWM,   8, 7, ARP_MIN, 0,   10 },
    { nG(3), KEEP,      8, 7, ARP_MAJ, 0,   11 },
    // --- Build: bass heartbeat crescendo, tempo climbing, drums in ---
    { nA(2), PRE_PULSE, 4, 3, 0,       160,  8 },
    { nA(2), KEEP,      4, 3, 0,       150,  9 },
    { nA(2), KEEP,      2, 1, 0,       140, 10 },
    { nA(2), KEEP,      2, 1, 0,       130, 11 },
    { nC(1), PRE_DRUM,  2, 1, 0,       120, 12 },
    { nA(2), PRE_PULSE, 2, 1, 0,       0,   12 },
    { nC(1), PRE_DRUM,  2, 1, 0,       110, 14 },
    { nA(2), PRE_PULSE, 2, 1, 0,       0,   13 },
    // --- Chip groove: kick/snare + bass + arps, Am F C G (110ms) ---
    { nC(1), PRE_DRUM,  2, 1, 0,       0,    0 },
    { nA(2), PRE_PULSE, 2, 1, 0,       0,   13 },
    { nA(3), PRE_THIN,  4, 3, ARP_MIN, 0,   11 },
    { nB(4), PRE_DRUM,  2, 1, 0,       0,   13 },
    { nA(2), PRE_PULSE, 2, 1, 0,       0,   11 },
    { nA(4), PRE_THIN,  4, 3, ARP_MIN, 0,   13 },
    { nC(1), PRE_DRUM,  2, 1, 0,       0,    0 },
    { nF(2), PRE_PULSE, 2, 1, 0,       0,   13 },
    { nF(3), PRE_THIN,  4, 3, ARP_MAJ, 0,   11 },
    { nB(4), PRE_DRUM,  2, 1, 0,       0,   13 },
    { nF(2), PRE_PULSE, 2, 1, 0,       0,   11 },
    { nF(4), PRE_THIN,  4, 3, ARP_MAJ, 0,   13 },
    { nC(1), PRE_DRUM,  2, 1, 0,       0,    0 },
    { nC(3), PRE_PULSE, 2, 1, 0,       0,   13 },
    { nC(4), PRE_THIN,  4, 3, ARP_MAJ, 0,   11 },
    { nB(4), PRE_DRUM,  2, 1, 0,       0,   13 },
    { nC(3), PRE_PULSE, 2, 1, 0,       0,   11 },
    { nC(5), PRE_THIN,  4, 3, ARP_MAJ, 0,   13 },
    { nC(1), PRE_DRUM,  2, 1, 0,       0,    0 },
    { nG(2), PRE_PULSE, 2, 1, 0,       0,   13 },
    { nG(3), PRE_THIN,  4, 3, ARP_MAJ, 0,   11 },
    { nB(4), PRE_DRUM,  2, 1, 0,       0,   13 },
    { nG(2), PRE_PULSE, 2, 1, 0,       0,   11 },
    { nG(4), PRE_THIN,  4, 3, ARP_MAJ, 0,   13 },
    // --- Lead verse: call and response; phrases swell to their peaks ---
    { nC(1), PRE_DRUM,  1, 1, 0,       0,    0 },
    { nA(3), PRE_LEAD,  3, 0, 0,       0,   12 },
    { nC(4), KEEP,      2, 0, 0,       0,   13 },
    { nE(4), KEEP,      4, 3, 0x0C,    0,   15, 10 },  // phrase peak, octave duo
    { nB(4), PRE_DRUM,  2, 1, 0,       0,   13 },
    { nA(2), PRE_PULSE, 4, 3, 0,       0,   11 },
    { nC(1), PRE_DRUM,  1, 1, 0,       0,    0 },
    { nF(4), PRE_LEAD,  3, 0, 0,       0,   14 },
    { nE(4), KEEP,      2, 0, 0,       0,   12 },
    { nC(4), KEEP,      4, 3, 0,       0,   11 },  // answer, softer
    { nC(7), PRE_DRUM,  2, 1, 0,       0,    7 },
    { nF(2), PRE_PULSE, 4, 3, 0,       0,   11 },
    { nC(1), PRE_DRUM,  1, 1, 0,       0,    0 },
    { nG(4), PRE_LEAD,  3, 0, 0,       0,   15 },  // phrase peak
    { nE(4), KEEP,      2, 0, 0,       0,   13 },
    { nD(4), KEEP,      4, 3, 0,       0,   12 },
    { nC(7), PRE_DRUM,  2, 1, 0,       0,    7 },
    { nC(3), PRE_PULSE, 4, 3, 0,       0,   11 },
    { nC(1), PRE_DRUM,  1, 1, 0,       0,    0 },
    { nB(3), PRE_LEAD,  3, 0, 0,       0,   11 },
    { nD(4), KEEP,      2, 0, 0,       0,   12 },
    { nG(4), KEEP,      4, 3, 0x07,    0,   14, 10 },  // lifts into breakdown, duo
    { nB(4), PRE_DRUM,  2, 1, 0,       0,   13 },
    { nG(2), PRE_PULSE, 4, 3, 0,       0,   11 },
    // --- Breakdown: pads, everything slows and hushes ---
    { nD(3), PRE_PWM,   8, 7, ARP_MIN, 150,  9 },
    { nA(3), KEEP,      8, 7, ARP_MIN, 165,  8 },
    // --- Drum break (100ms): ghost hats, backbeat snares, then an
    // --- accelerando roll that crescendos into the finale ---
    { nC(1), PRE_DRUM,  2, 1, 0,       100,  0 },
    { nC(7), KEEP,      1, 1, 0,       0,    7 },
    { nC(7), KEEP,      1, 1, 0,       0,    5 },
    { nB(4), KEEP,      2, 1, 0,       0,   13 },
    { nC(1), KEEP,      2, 1, 0,       0,    0 },
    { nC(7), KEEP,      1, 1, 0,       0,    7 },
    { nB(4), KEEP,      1, 1, 0,       0,    9 },  // ghost snare
    { nB(4), KEEP,      2, 1, 0,       0,   13 },
    { nB(4), KEEP,      1, 1, 0,       95,   9 },
    { nB(4), KEEP,      1, 1, 0,       85,  11 },
    { nB(4), KEEP,      1, 1, 0,       75,  13 },
    { nB(4), KEEP,      1, 1, 0,       65,  15 },
    { nC(1), KEEP,      2, 1, 0,       55,   0 },
    // --- Finale (95ms): arps up an octave, lead reprise, full tilt ---
    { nC(1), PRE_DRUM,  2, 1, 0,       95,   0 },
    { nA(4), PRE_THIN,  4, 3, ARP_MIN, 0,   13 },
    { nA(2), PRE_PULSE, 2, 1, 0,       0,   14 },
    { nA(4), PRE_LEAD,  2, 0, 0,       0,   13 },
    { nC(5), KEEP,      2, 0, 0,       0,   14 },
    { nE(5), KEEP,      4, 3, 0x0C,    0,    0, 10 },  // full-scale peak, duo
    { nC(1), PRE_DRUM,  2, 1, 0,       0,    0 },
    { nF(4), PRE_THIN,  4, 3, ARP_MAJ, 0,   13 },
    { nF(2), PRE_PULSE, 2, 1, 0,       0,   14 },
    { nF(4), PRE_LEAD,  2, 0, 0,       0,   13 },
    { nA(4), KEEP,      2, 0, 0,       0,   14 },
    { nC(5), KEEP,      4, 3, 0,       0,   15 },
    { nC(1), PRE_DRUM,  2, 1, 0,       0,    0 },
    { nC(4), PRE_THIN,  4, 3, ARP_MAJ, 0,   13 },
    { nC(3), PRE_PULSE, 2, 1, 0,       0,   14 },
    { nG(4), PRE_LEAD,  2, 0, 0,       0,   14 },
    { nE(5), KEEP,      2, 0, 0,       0,   15 },
    { nD(5), KEEP,      4, 3, 0,       0,   13 },
    { nC(1), PRE_DRUM,  2, 1, 0,       0,    0 },
    { nG(4), PRE_THIN,  4, 3, ARP_MAJ, 0,   13 },
    { nG(2), PRE_PULSE, 2, 1, 0,       0,   14 },
    { nB(4), PRE_LEAD,  2, 0, 0,       0,   14 },
    { nD(5), KEEP,      2, 0, 0,       0,   15 },
    { nG(5), KEEP,      4, 3, 0x0C,    0,    0, 10 },  // climax, full scale, duo
    // --- Outro: rallentando pad into one last bell, dying away ---
    { nG(3), PRE_PWM,   8, 7, ARP_MAJ, 120, 10 },
    { nA(3), PRE_BELL, 10, 8, ARP_MIN, 176, 12 },
    { 0,     KEEP,      4, 0, 0,       0,    0 },
    { 0, 0, 0, 0, 0, 0, 0 },
};

// Demo 4 "minuet": Bach's Minuet in G (BWV Anh. 114), first strain, as a
// tribute to the TRS-80 Model 100 MINUET.BA - one square wave voice made
// to sound polyphonic by rapidly alternating between two notes.  Here the
// quarter and half notes carry a lower harmony voice (the arp interval)
// alternated every 10ms, which the ear fuses into a two-part texture; the
// eighth note runs stay solo.  The event's note is the HARMONY, the
// interval reaches up to the melody.  One step = an eighth note (250ms).
static const demo_ev_t demo_minuet[] = {
    //  note   preset     len gate arp   tmpo vel arp_ms
    // Bar 1: D5 | G4 A4 B4 C5
    { nB(4), PRE_LEAD,  2, 1, 0x03, 250, 13, 10 },   // D5 over B4
    { nG(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nA(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nB(4), KEEP,      1, 0, 0,    0,   12, 0 },
    { nC(5), KEEP,      1, 0, 0,    0,   12, 0 },
    // Bar 2: D5 G4 G4
    { nB(4), KEEP,      2, 1, 0x03, 0,   13, 10 },   // D5 over B4
    { nB(3), KEEP,      2, 1, 0x08, 0,   12, 10 },   // G4 over B3
    { nD(4), KEEP,      2, 1, 0x05, 0,   11, 10 },   // G4 over D4
    // Bar 3: E5 | C5 D5 E5 F#5
    { nC(5), KEEP,      2, 1, 0x04, 0,   13, 10 },   // E5 over C5
    { nC(5), KEEP,      1, 0, 0,    0,   11, 0 },
    { nD(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nE(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nFs(5), KEEP,     1, 0, 0,    0,   13, 0 },
    // Bar 4: G5 G4 G4
    { nB(4), KEEP,      2, 1, 0x08, 0,   14, 10 },   // G5 over B4 (peak)
    { nB(3), KEEP,      2, 1, 0x08, 0,   12, 10 },
    { nD(4), KEEP,      2, 1, 0x05, 0,   11, 10 },
    // Bar 5: C5 | D5 C5 B4 A4
    { nA(4), KEEP,      2, 1, 0x03, 0,   13, 10 },   // C5 over A4
    { nD(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nC(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nB(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nA(4), KEEP,      1, 0, 0,    0,   11, 0 },
    // Bar 6: B4 | C5 B4 A4 G4
    { nG(4), KEEP,      2, 1, 0x04, 0,   12, 10 },   // B4 over G4
    { nC(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nB(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nA(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nG(4), KEEP,      1, 0, 0,    0,   10, 0 },
    // Bar 7: F#4 | G4 A4 B4 G4
    { nD(4), KEEP,      2, 1, 0x04, 0,   12, 10 },   // F#4 over D4
    { nG(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nA(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nB(4), KEEP,      1, 0, 0,    0,   12, 0 },
    { nG(4), KEEP,      1, 0, 0,    0,   11, 0 },
    // Bar 8: A4 (dotted half, half cadence)
    { nD(4), KEEP,      6, 4, 0x07, 0,   13, 10 },   // A4 over D4
    // Bars 9-12 repeat bars 1-4
    { nB(4), KEEP,      2, 1, 0x03, 0,   13, 10 },
    { nG(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nA(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nB(4), KEEP,      1, 0, 0,    0,   12, 0 },
    { nC(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nB(4), KEEP,      2, 1, 0x03, 0,   13, 10 },
    { nB(3), KEEP,      2, 1, 0x08, 0,   12, 10 },
    { nD(4), KEEP,      2, 1, 0x05, 0,   11, 10 },
    { nC(5), KEEP,      2, 1, 0x04, 0,   13, 10 },
    { nC(5), KEEP,      1, 0, 0,    0,   11, 0 },
    { nD(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nE(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nFs(5), KEEP,     1, 0, 0,    0,   13, 0 },
    { nB(4), KEEP,      2, 1, 0x08, 0,   14, 10 },
    { nB(3), KEEP,      2, 1, 0x08, 0,   12, 10 },
    { nD(4), KEEP,      2, 1, 0x05, 0,   11, 10 },
    // Bars 13-14 repeat bars 5-6
    { nA(4), KEEP,      2, 1, 0x03, 0,   13, 10 },
    { nD(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nC(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nB(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nA(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nG(4), KEEP,      2, 1, 0x04, 0,   12, 10 },
    { nC(5), KEEP,      1, 0, 0,    0,   12, 0 },
    { nB(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nA(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nG(4), KEEP,      1, 0, 0,    0,   10, 0 },
    // Bar 15: A4 | B4 A4 G4 F#4
    { nD(4), KEEP,      2, 1, 0x07, 0,   12, 10 },   // A4 over D4
    { nB(4), KEEP,      1, 0, 0,    0,   12, 0 },
    { nA(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nG(4), KEEP,      1, 0, 0,    0,   11, 0 },
    { nFs(4), KEEP,     1, 0, 0,    0,   10, 0 },
    // Bar 16: G4 (dotted half) over G3 - full close
    { nG(3), KEEP,      6, 5, 0x0C, 0,   14, 10 },   // G4 over G3 (octave)
    { 0,     KEEP,      2, 0, 0,    0,    0, 0 },    // breath, then da capo
    { 0, 0, 0, 0, 0, 0, 0, 0 },
};

// ==========================================================================
// "Afterglow"
//
// A melodic PRISM synth piece built specifically around the limitations of
// the single-voice sequencer.
//
// Tonal center: A minor
//
// Primary motif:
//      A C E D C B A
//
// Harmonic movement:
//      Am | F | C | E
//
// Velocity:
//      0    = full volume (16/16)
//      15   = strong accent
//      11-14 = normal melodic range
//      8-10 = soft
//      5-7  = distant / ghosted
//
// The arrangement deliberately avoids treating PRISM like a multichannel
// synthesizer.  Melody, harmony, bass and percussion take turns occupying
// the one available voice.
//
// The primary motif is introduced quietly, developed, then returns an
// octave higher at the climax.
// ==========================================================================

static const demo_ev_t demo_afterglow[] = {

    // ======================================================================
    // INTRO
    //
    // Bare bell melody.  Establish the motif clearly enough that its return
    // at the climax is recognizable.
    // ======================================================================

    // Main motif: A C E D C B A
    { nA(4), PRE_BELL,   2, 0, 0,       156,  8 },
    { nC(5), KEEP,       2, 0, 0,         0, 10 },
    { nE(5), KEEP,       4, 0, 0,         0, 12 },
    { nD(5), KEEP,       2, 0, 0,         0, 11 },
    { nC(5), KEEP,       2, 0, 0,         0, 10 },
    { nB(4), KEEP,       2, 0, 0,         0,  9 },
    { nA(4), KEEP,       2, 2, 0,         0,  8 },

    // Quiet answer
    { nE(4), KEEP,       2, 0, 0,         0,  7 },
    { nA(4), KEEP,       2, 0, 0,         0,  9 },
    { nC(5), KEEP,       4, 0, 0,         0, 11 },
    { nB(4), KEEP,       2, 0, 0,         0, 10 },
    { nA(4), KEEP,       2, 0, 0,         0,  9 },
    { nG(4), KEEP,       2, 0, 0,         0,  8 },
    { nE(4), KEEP,       2, 2, 0,         0,  7 },

    // Breath
    { 0,     KEEP,       4, 0, 0,         0,  0 },


    // ======================================================================
    // HARMONY
    //
    // Am - F - C - E
    //
    // PWM arpeggios establish the harmonic world.  The second pass grows
    // slightly louder so the music feels as though it is waking up.
    // ======================================================================

    { nA(3), PRE_PWM,    8, 7, ARP_MIN, 140,  8 },
    { nF(3), KEEP,       8, 7, ARP_MAJ,   0,  8 },
    { nC(4), KEEP,       8, 7, ARP_MAJ,   0,  9 },
    { nE(3), KEEP,       8, 7, ARP_MAJ,   0, 10 },

    { nA(3), KEEP,       8, 7, ARP_MIN,   0,  9 },
    { nF(3), KEEP,       8, 7, ARP_MAJ,   0,  9 },
    { nC(4), KEEP,       8, 7, ARP_MAJ,   0, 10 },
    { nE(4), KEEP,       8, 7, ARP_MAJ,   0, 11 },


    // ======================================================================
    // FIRST THEME
    //
    // The opening motif returns on the lead voice, stronger and faster.
    // ======================================================================

    // Am phrase
    { nA(4), PRE_LEAD,   2, 0, 0,       112, 11 },
    { nC(5), KEEP,       2, 0, 0,         0, 13 },
    { nE(5), KEEP,       4, 0, 0,         0, 15 },
    { nD(5), KEEP,       2, 0, 0,         0, 14 },
    { nC(5), KEEP,       2, 0, 0,         0, 13 },
    { nB(4), KEEP,       2, 0, 0,         0, 12 },
    { nA(4), KEEP,       2, 2, 0,         0, 10 },

    // F phrase
    { nF(4), KEEP,       2, 0, 0,         0, 11 },
    { nA(4), KEEP,       2, 0, 0,         0, 13 },
    { nC(5), KEEP,       4, 0, 0,         0, 15 },
    { nB(4), KEEP,       2, 0, 0,         0, 13 },
    { nA(4), KEEP,       2, 0, 0,         0, 12 },
    { nG(4), KEEP,       2, 0, 0,         0, 11 },
    { nF(4), KEEP,       2, 2, 0,         0, 10 },

    // C phrase
    { nE(4), KEEP,       2, 0, 0,         0, 11 },
    { nG(4), KEEP,       2, 0, 0,         0, 12 },
    { nC(5), KEEP,       4, 0, 0,         0, 14 },
    { nB(4), KEEP,       2, 0, 0,         0, 13 },
    { nG(4), KEEP,       2, 0, 0,         0, 12 },
    { nE(4), KEEP,       4, 3, 0,         0, 10 },

    // E phrase -- hold back the resolution
    { nE(4), KEEP,       2, 0, 0,         0, 11 },
    { nB(4), KEEP,       2, 0, 0,         0, 13 },
    { nE(5), KEEP,       4, 0, 0,         0, 15 },
    { nD(5), KEEP,       2, 0, 0,         0, 13 },
    { nB(4), KEEP,       2, 0, 0,         0, 12 },
    { nE(5), KEEP,       4, 3, 0,         0, 14 },


    // ======================================================================
    // BREATH / RECOLLECTION
    //
    // Return to the harmony instead of introducing a new musical idea.
    // Thin pulse makes it feel more distant than the first PWM statement.
    // ======================================================================

    { nA(3), PRE_THIN,   8, 7, ARP_MIN,   0,  8 },
    { nF(3), KEEP,       8, 7, ARP_MAJ,   0,  8 },
    { nC(4), KEEP,       8, 7, ARP_MAJ,   0,  9 },
    { nE(3), KEEP,       8, 7, ARP_MAJ,   0, 10 },


    // ======================================================================
    // SECOND STATEMENT
    //
    // Begin with the recognizable motif, then let it escape upward into
    // new material.
    // ======================================================================

    { nA(4), PRE_LEAD,   2, 0, 0,       108, 11 },
    { nC(5), KEEP,       2, 0, 0,         0, 13 },
    { nE(5), KEEP,       4, 0, 0,         0, 15 },
    { nD(5), KEEP,       2, 0, 0,         0, 14 },
    { nC(5), KEEP,       2, 0, 0,         0, 13 },
    { nB(4), KEEP,       2, 0, 0,         0, 12 },
    { nA(4), KEEP,       2, 2, 0,         0, 10 },

    // First variation -- climb
    { nC(5), KEEP,       2, 0, 0,         0, 11 },
    { nE(5), KEEP,       2, 0, 0,         0, 13 },
    { nG(5), KEEP,       4, 0, 0,         0, 15 },
    { nF(5), KEEP,       2, 0, 0,         0, 14 },
    { nE(5), KEEP,       2, 0, 0,         0, 13 },
    { nD(5), KEEP,       2, 0, 0,         0, 12 },
    { nC(5), KEEP,       2, 2, 0,         0, 10 },

    // Second variation
    { nF(4), KEEP,       2, 0, 0,         0, 11 },
    { nA(4), KEEP,       2, 0, 0,         0, 12 },
    { nC(5), KEEP,       2, 0, 0,         0, 13 },
    { nE(5), KEEP,       2, 0, 0,         0, 15 },
    { nD(5), KEEP,       4, 0, 0,         0, 13 },
    { nC(5), KEEP,       4, 3, 0,         0, 11 },

    // Dominant cadence
    { nB(4), KEEP,       2, 0, 0,         0, 11 },
    { nE(5), KEEP,       2, 0, 0,         0, 13 },
    { nB(5), KEEP,       4, 0, 0,         0, 15 },
    { nE(5), KEEP,       4, 0, 0,         0, 13 },
    { nB(4), KEEP,       4, 3, 0,         0, 10 },


    // ======================================================================
    // HEARTBEAT
    //
    // Percussion appears for the first time.
    //
    // Kick  = strong
    // Snare = medium/strong
    // Hat   = quiet
    // Bass  = medium
    //
    // Because PRISM has only one voice, drums and bass answer each other.
    // ======================================================================

    // Am
    { nC(1), PRE_DRUM,   2, 1, 0,       104, 15 },
    { nA(2), PRE_PULSE,  6, 5, 0,         0, 11 },

    { nC(7), PRE_DRUM,   1, 1, 0,         0,  7 },
    { nA(2), PRE_PULSE,  3, 2, 0,         0, 10 },
    { nB(4), PRE_DRUM,   2, 1, 0,         0, 13 },
    { nE(3), PRE_PULSE,  2, 1, 0,         0,  9 },

    // F
    { nC(1), PRE_DRUM,   2, 1, 0,         0, 15 },
    { nF(2), PRE_PULSE,  6, 5, 0,         0, 11 },

    { nC(7), PRE_DRUM,   1, 1, 0,         0,  6 },
    { nF(2), PRE_PULSE,  3, 2, 0,         0, 10 },
    { nB(4), PRE_DRUM,   2, 1, 0,         0, 13 },
    { nG(2), PRE_PULSE,  2, 1, 0,         0,  9 },

    // C
    { nC(1), PRE_DRUM,   2, 1, 0,         0, 15 },
    { nC(3), PRE_PULSE,  6, 5, 0,         0, 11 },

    { nC(7), PRE_DRUM,   1, 1, 0,         0,  7 },
    { nC(3), PRE_PULSE,  3, 2, 0,         0, 10 },
    { nB(4), PRE_DRUM,   2, 1, 0,         0, 14 },
    { nE(3), PRE_PULSE,  2, 1, 0,         0,  9 },

    // E -- increase intensity
    { nC(1), PRE_DRUM,   2, 1, 0,         0, 15 },
    { nE(3), PRE_PULSE,  4, 3, 0,         0, 11 },

    { nC(7), PRE_DRUM,   1, 1, 0,         0,  8 },
    { nE(3), PRE_PULSE,  2, 1, 0,         0, 11 },

    { nB(4), PRE_DRUM,   2, 1, 0,         0, 14 },
    { nE(4), PRE_PULSE,  4, 3, 0,         0, 12 },

    // Small drum pickup into the climax
    { nC(1), PRE_DRUM,   1, 1, 0,         0, 14 },
    { nC(7), KEEP,       1, 1, 0,         0,  7 },
    { nB(4), KEEP,       1, 1, 0,         0, 13 },
    { nB(4), KEEP,       1, 1, 0,         0, 15 },


    // ======================================================================
    // CLIMAX
    //
    // The original seven-note motif returns one octave higher.
    // Full-scale velocity is reserved for the highest emotional peaks.
    // ======================================================================

    { nA(5), PRE_LEAD,   2, 0, 0,        98, 13 },
    { nC(6), KEEP,       2, 0, 0,         0, 14 },
    { nE(6), KEEP,       4, 0, 0,         0,  0 },
    { nD(6), KEEP,       2, 0, 0,         0, 15 },
    { nC(6), KEEP,       2, 0, 0,         0, 14 },
    { nB(5), KEEP,       2, 0, 0,         0, 13 },
    { nA(5), KEEP,       2, 2, 0,         0, 12 },

    // F answer
    { nF(5), KEEP,       2, 0, 0,         0, 13 },
    { nA(5), KEEP,       2, 0, 0,         0, 14 },
    { nC(6), KEEP,       4, 0, 0,         0,  0 },
    { nB(5), KEEP,       2, 0, 0,         0, 15 },
    { nA(5), KEEP,       2, 0, 0,         0, 14 },
    { nG(5), KEEP,       2, 0, 0,         0, 13 },
    { nF(5), KEEP,       2, 2, 0,         0, 12 },

    // Push upward
    { nC(6), KEEP,       2, 0, 0,         0, 13 },
    { nE(6), KEEP,       2, 0, 0,         0, 14 },
    { nG(6), KEEP,       4, 0, 0,         0,  0 },
    { nF(6), KEEP,       2, 0, 0,         0, 15 },
    { nE(6), KEEP,       2, 0, 0,         0, 14 },
    { nD(6), KEEP,       2, 0, 0,         0, 13 },
    { nC(6), KEEP,       2, 2, 0,         0, 12 },

    // One final melodic climb
    { nA(5), KEEP,       2, 0, 0,         0, 13 },
    { nB(5), KEEP,       2, 0, 0,         0, 14 },
    { nC(6), KEEP,       2, 0, 0,         0, 15 },
    { nE(6), KEEP,       2, 0, 0,         0,  0 },
    { nD(6), KEEP,       2, 0, 0,         0, 15 },
    { nC(6), KEEP,       2, 0, 0,         0, 14 },
    { nB(5), KEEP,       2, 0, 0,         0, 13 },
    { nA(5), KEEP,       4, 3, 0,         0, 12 },

    // Dominant tension: E major = E G# B
    { nE(5), PRE_THIN,  12,11, ARP_MAJ,   0, 11 },

    // Resolution into A minor
    { nA(4), PRE_PWM,   16,15, ARP_MIN,   0, 13 },


    // ======================================================================
    // AFTERGLOW
    //
    // Let the energy drain away gradually rather than ending immediately.
    // ======================================================================

    { nF(3), KEEP,       8, 7, ARP_MAJ, 120, 11 },
    { nC(4), KEEP,       8, 7, ARP_MAJ,   0, 10 },
    { nA(3), KEEP,       8, 7, ARP_MIN,   0,  9 },
    { nE(3), KEEP,       8, 7, ARP_MAJ,   0,  8 },


    // ======================================================================
    // OUTRO
    //
    // The melody returns almost as it sounded at the beginning, but now
    // steadily fades.  The final A is deliberately very quiet.
    // ======================================================================

    { nA(4), PRE_BELL,   2, 0, 0,       156,  9 },
    { nC(5), KEEP,       2, 0, 0,         0, 10 },
    { nE(5), KEEP,       4, 0, 0,         0, 11 },
    { nD(5), KEEP,       2, 0, 0,         0, 10 },
    { nC(5), KEEP,       2, 0, 0,         0,  9 },
    { nB(4), KEEP,       2, 0, 0,         0,  8 },
    { nA(4), KEEP,       2, 2, 0,         0,  7 },

    // Fragment of the answer, slowing down
    { nE(4), KEEP,       2, 0, 0,       168,  7 },
    { nA(4), KEEP,       2, 0, 0,         0,  8 },
    { nC(5), KEEP,       4, 0, 0,         0,  9 },
    { nB(4), KEEP,       2, 0, 0,       180,  7 },
    { nA(4), KEEP,       2, 0, 0,         0,  6 },

    // Suspended E, then silence
    { nE(4), KEEP,       4, 3, 0,       192,  6 },
    { 0,     KEEP,       3, 0, 0,         0,  0 },

    // One last bell
    { nA(4), PRE_BELL,  12, 8, 0,       210,  5 },

    // Let the release disappear completely before looping
    { 0,     KEEP,      10, 0, 0,         0,  0 },

    // End marker
    { 0, 0, 0, 0, 0, 0, 0 },
};

// Note definitions (Octave * 12 + NoteIndex + 1)
// Standard pitch encoding assuming 1-based indexing for note byte
#define NOTE(oct, idx) ((oct) * 12 + (idx) + 1)

// Arpeggiator helper: PACK_ARP(third, fifth) -> lo/hi nibble chord intervals
#define PACK_ARP(m1, m2) (((m1) & 0x0F) | (((m2) & 0x0F) << 4))

static const demo_ev_t synth_dance_song[] = {
// =========================================================================
    // SECTION 1: INTRO (Bell Arpeggios & Atmosphere) - ~120ms step time (~125 BPM)
    // =========================================================================
    // Am (A3) bell arpeggio
    { NOTE(3, 9),  PRE_BELL,  2, 2, PACK_ARP(3, 7), 120, 0 }, 
    { NOTE(3, 9),  0xFF,      2, 2, PACK_ARP(3, 7),   0, 0 },
    { NOTE(3, 9),  0xFF,      2, 2, PACK_ARP(3, 7),   0, 0 },
    { NOTE(3, 9),  0xFF,      2, 2, PACK_ARP(3, 7),   0, 0 },
    // Fmaj (F3) bell arpeggio
    { NOTE(3, 5),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 }, 
    { NOTE(3, 5),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 },
    { NOTE(3, 5),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 },
    { NOTE(3, 5),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 },
    // Cmaj (C3) bell arpeggio
    { NOTE(3, 0),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 }, 
    { NOTE(3, 0),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 },
    { NOTE(3, 0),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 },
    { NOTE(3, 0),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 },
    // Gmaj (G3) bell arpeggio
    { NOTE(3, 7),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 }, 
    { NOTE(3, 7),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 },
    { NOTE(3, 7),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 },
    { NOTE(3, 7),  0xFF,      2, 2, PACK_ARP(4, 7),   0, 0 },

    // =========================================================================
    // SECTION 2: DRIVE & BASSLINE (Pulsing Octaves)
    // =========================================================================
    { NOTE(2, 9),  PRE_PWM,   2, 1, 0, 0, 0 }, // A2
    { NOTE(3, 9),  0xFF,      2, 1, 0, 0, 0 }, // A3
    { NOTE(2, 9),  0xFF,      2, 1, 0, 0, 0 }, // A2
    { NOTE(3, 9),  0xFF,      2, 1, 0, 0, 0 }, // A3

    { NOTE(2, 5),  0xFF,      2, 1, 0, 0, 0 }, // F2
    { NOTE(3, 5),  0xFF,      2, 1, 0, 0, 0 }, // F3
    { NOTE(2, 5),  0xFF,      2, 1, 0, 0, 0 }, // F2
    { NOTE(3, 5),  0xFF,      2, 1, 0, 0, 0 }, // F3

    { NOTE(2, 0),  0xFF,      2, 1, 0, 0, 0 }, // C2
    { NOTE(3, 0),  0xFF,      2, 1, 0, 0, 0 }, // C3
    { NOTE(2, 7),  0xFF,      2, 1, 0, 0, 0 }, // G2
    { NOTE(3, 7),  0xFF,      2, 1, 0, 0, 0 }, // G3

    // =========================================================================
    // SECTION 3: MAIN LEAD HOOK (Brassy 80s Theme)
    // =========================================================================
    // Measure 1
    { NOTE(4, 9),  PRE_BRASS, 4, 3, 0, 0, 0 }, // A4
    { NOTE(4, 7),  0xFF,      2, 2, 0, 0, 0 }, // G4
    { NOTE(4, 9),  0xFF,      2, 2, 0, 0, 0 }, // A4
    { NOTE(5, 0),  0xFF,      4, 4, 0, 0, 0 }, // C5
    { NOTE(4, 11), 0xFF,      4, 3, 0, 0, 0 }, // B4

    // Measure 2
    { NOTE(4, 5),  0xFF,      4, 3, 0, 0, 0 }, // F4
    { NOTE(4, 7),  0xFF,      2, 2, 0, 0, 0 }, // G4
    { NOTE(4, 9),  0xFF,      2, 2, 0, 0, 0 }, // A4
    { NOTE(4, 7),  0xFF,      8, 7, 0, 0, 0 }, // G4 (Sustained)

    // Measure 3 (Variant)
    { NOTE(4, 9),  0xFF,      4, 3, 0, 0, 0 }, // A4
    { NOTE(4, 7),  0xFF,      2, 2, 0, 0, 0 }, // G4
    { NOTE(4, 9),  0xFF,      2, 2, 0, 0, 0 }, // A4
    { NOTE(5, 3),  0xFF,      4, 4, 0, 0, 0 }, // D#5 / Eb5 pitch bend/accent
    { NOTE(5, 4),  0xFF,      4, 3, 0, 0, 0 }, // E5

    // Measure 4
    { NOTE(5, 0),  0xFF,      4, 3, 0, 0, 0 }, // C5
    { NOTE(4, 11), 0xFF,      4, 3, 0, 0, 0 }, // B4
    { NOTE(4, 9),  0xFF,      8, 8, 0, 0, 0 }, // A4 (Hold)

    // =========================================================================
    // SECTION 4: BRIDGE (Syncopated D-Minor Modulation)
    // =========================================================================
    { NOTE(3, 2),  PRE_THIN,  3, 2, 0, 0, 0 }, // D3
    { NOTE(3, 2),  0xFF,      3, 2, 0, 0, 0 }, 
    { NOTE(3, 5),  0xFF,      2, 2, 0, 0, 0 }, // F3
    { NOTE(3, 9),  0xFF,      4, 3, 0, 0, 0 }, // A3
    { NOTE(3, 7),  0xFF,      4, 3, 0, 0, 0 }, // G3

    { NOTE(3, 2),  0xFF,      3, 2, 0, 0, 0 }, // D3
    { NOTE(3, 2),  0xFF,      3, 2, 0, 0, 0 }, 
    { NOTE(3, 5),  0xFF,      2, 2, 0, 0, 0 }, // F3
    { NOTE(3, 10), 0xFF,      4, 3, 0, 0, 0 }, // A#3
    { NOTE(3, 9),  0xFF,      4, 3, 0, 0, 0 }, // A3

    // =========================================================================
    // SECTION 5: NOISE PERCUSSION BREAKDOWN (Simulated Drum Pattern)
    // =========================================================================
    { NOTE(1, 0),  PRE_DRUM,  2, 1, 0, 0, 0 }, // Kick pulse
    { 0,           0xFF,      2, 0, 0, 0, 0 }, // Rest
    { NOTE(4, 0),  PRE_DRUM,  2, 2, 0, 0, 0 }, // Snare noise blast
    { 0,           0xFF,      2, 0, 0, 0, 0 }, // Rest
    { NOTE(1, 0),  0xFF,      2, 1, 0, 0, 0 }, // Kick
    { NOTE(1, 0),  0xFF,      2, 1, 0, 0, 0 }, // Kick
    { NOTE(4, 0),  0xFF,      2, 2, 0, 0, 0 }, // Snare
    { 0,           0xFF,      2, 0, 0, 0, 0 }, // Rest

    // =========================================================================
    // SECTION 6: REPRISE & FINALE (Square Lead + High Arp)
    // =========================================================================
    { NOTE(4, 9),  PRE_LEAD,  2, 2, PACK_ARP(3, 7), 0, 0 }, // Fast Am Arp Lead
    { NOTE(4, 5),  0xFF,      2, 2, PACK_ARP(4, 7), 0, 0 }, // Fast Fmaj Arp Lead
    { NOTE(4, 0),  0xFF,      2, 2, PACK_ARP(4, 7), 0, 0 }, // Fast Cmaj Arp Lead
    { NOTE(4, 7),  0xFF,      2, 2, PACK_ARP(4, 7), 0, 0 }, // Fast Gmaj Arp Lead

    // Resolving Final Chord Stabs
    { NOTE(3, 9),  PRE_BRASS, 4, 3, PACK_ARP(3, 7), 0, 0 }, // Am
    { NOTE(3, 5),  0xFF,      4, 3, PACK_ARP(4, 7), 0, 0 }, // F
    { NOTE(3, 9),  0xFF,      8, 8, PACK_ARP(3, 7), 0, 0 }, // Am (Final Sustained Chord)

    // End of track marker
    { 0, 0, 0, 0, 0, 0, 0 }
};

// ==========================================================================
// "Dark Signal"
//
// A mysterious / cosmic piece for the single-voice PRISM synth.
//
// Tonal center: E minor
//
// Main "signal" motif:
//
//      E  B  F#  G  D#  E
//
// The wide fifth, F#/G tension, and D# leading tone give the melody an
// unsettled, searching quality.
//
// Harmonic language:
//
//      Em | C | Am | B
//
// B is deliberately MAJOR (B-D#-F#), giving a strong and slightly alien
// pull back toward E minor.
//
// The arrangement treats the single voice as something moving through
// space: distant bell -> drifting arpeggios -> detected transmission ->
// pulse/noise activity -> weightless void -> accelerating event horizon ->
// luminous return -> fading beacon.
//
// Velocity:
//      0     = full (16/16)
//      13-15 = strong
//      9-12  = medium
//      5-8   = distant
//      1-4   = barely audible
// ==========================================================================

// Accidentals used by this song.
#define nDs(o)  ((uint8_t)((o) * 12 + 3))
#define nFs(o)  ((uint8_t)((o) * 12 + 6))

static const demo_ev_t demo_dark_signal[] = {

    // ======================================================================
    // I. DISTANT BEACON
    //
    // A faint signal appears out of silence.
    // Lots of empty space is intentional.
    // ======================================================================

    { 0,      KEEP,       6, 0, 0,         184,  0 },

    { nE(5),  PRE_BELL,   3, 2, 0,           0,  5 },
    { 0,      KEEP,       3, 0, 0,           0,  0 },

    { nB(5),  KEEP,       3, 2, 0,           0,  6 },
    { 0,      KEEP,       3, 0, 0,           0,  0 },

    { nFs(5), KEEP,       2, 2, 0,           0,  5 },
    { nG(5),  KEEP,       4, 3, 0,           0,  7 },

    { 0,      KEEP,       4, 0, 0,           0,  0 },

    { nDs(5), KEEP,       2, 2, 0,           0,  5 },
    { nE(5),  KEEP,       6, 4, 0,           0,  8 },

    { 0,      KEEP,       8, 0, 0,           0,  0 },


    // Second beacon: same identity, slightly closer.
    { nE(5),  PRE_BELL,   2, 2, 0,         172,  6 },
    { nB(5),  KEEP,       2, 2, 0,           0,  7 },
    { nFs(5), KEEP,       2, 2, 0,           0,  6 },
    { nG(5),  KEEP,       4, 3, 0,           0,  8 },
    { nDs(5), KEEP,       2, 2, 0,           0,  7 },
    { nE(5),  KEEP,       6, 4, 0,           0,  9 },

    { 0,      KEEP,       6, 0, 0,           0,  0 },


    // ======================================================================
    // II. NEBULA
    //
    // Slow arpeggios establish a huge harmonic space.
    //
    // Em - C - Am - B
    // ======================================================================

    { nE(3),  PRE_PWM,   12,11, ARP_MIN,   154,  6 },
    { nC(4),  KEEP,      12,11, ARP_MAJ,     0,  7 },
    { nA(3),  KEEP,      12,11, ARP_MIN,     0,  7 },
    { nB(3),  KEEP,      12,11, ARP_MAJ,     0,  8 },

    // Repeat higher and slightly louder.
    { nE(4),  PRE_THIN,  12,11, ARP_MIN,     0,  7 },
    { nC(4),  KEEP,      12,11, ARP_MAJ,     0,  8 },
    { nA(3),  KEEP,      12,11, ARP_MIN,     0,  8 },
    { nB(3),  KEEP,      12,11, ARP_MAJ,     0,  9 },


    // ======================================================================
    // III. TRANSMISSION
    //
    // The distant beacon resolves into an intelligible melody.
    //
    // PRE_BRASS is useful here because long notes allow the delayed
    // vibrato to appear naturally, making the "signal" seem unstable.
    // ======================================================================

    { nE(4),  PRE_BRASS,  3, 0, 0,         136,  9 },
    { nB(4),  KEEP,       3, 0, 0,           0, 11 },
    { nFs(4), KEEP,       2, 0, 0,           0, 10 },
    { nG(4),  KEEP,       5, 4, 0,           0, 12 },
    { nDs(4), KEEP,       2, 0, 0,           0, 10 },
    { nE(4),  KEEP,       6, 5, 0,           0, 13 },

    { 0,      KEEP,       2, 0, 0,           0,  0 },

    // Answer: descend into something less certain.
    { nB(4),  KEEP,       3, 0, 0,           0, 10 },
    { nA(4),  KEEP,       2, 0, 0,           0,  9 },
    { nG(4),  KEEP,       3, 0, 0,           0, 10 },
    { nFs(4), KEEP,       2, 0, 0,           0,  9 },
    { nE(4),  KEEP,       4, 0, 0,           0, 11 },

    // Chromatic "lock-on": D -> D# -> E
    { nD(4),  KEEP,       2, 0, 0,           0,  8 },
    { nDs(4), KEEP,       2, 0, 0,           0, 10 },
    { nE(4),  KEEP,       8, 6, 0,           0, 13 },


    // ======================================================================
    // IV. ORBITAL PULSE
    //
    // Not really a drum groove.  Think spacecraft machinery / telemetry.
    //
    // Low noise bursts act like impacts; high noise is faint static.
    // Bass notes provide the repeating orbital pulse.
    // ======================================================================

    // Em
    { nC(1),  PRE_DRUM,   1, 1, 0,         122, 11 },
    { nE(2),  PRE_PULSE,  5, 4, 0,           0, 10 },

    { nC(7),  PRE_DRUM,   1, 1, 0,           0,  4 },
    { nB(2),  PRE_PULSE,  3, 2, 0,           0,  8 },

    { nC(1),  PRE_DRUM,   1, 1, 0,           0, 12 },
    { nE(3),  PRE_PULSE,  5, 4, 0,           0, 11 },

    // C
    { nC(7),  PRE_DRUM,   1, 1, 0,           0,  5 },
    { nC(3),  PRE_PULSE,  5, 4, 0,           0, 10 },

    { nB(4),  PRE_DRUM,   1, 1, 0,           0,  8 },
    { nG(2),  PRE_PULSE,  3, 2, 0,           0,  9 },

    { nC(1),  PRE_DRUM,   1, 1, 0,           0, 12 },
    { nC(3),  PRE_PULSE,  5, 4, 0,           0, 11 },

    // Am
    { nC(7),  PRE_DRUM,   1, 1, 0,           0,  4 },
    { nA(2),  PRE_PULSE,  5, 4, 0,           0, 10 },

    { nB(4),  PRE_DRUM,   1, 1, 0,           0,  8 },
    { nE(3),  PRE_PULSE,  3, 2, 0,           0,  9 },

    // B major -- tension
    { nC(1),  PRE_DRUM,   1, 1, 0,           0, 13 },
    { nB(2),  PRE_PULSE,  4, 3, 0,           0, 11 },

    { nC(7),  PRE_DRUM,   1, 1, 0,           0,  6 },
    { nFs(3), PRE_PULSE,  2, 1, 0,           0, 10 },

    { nB(4),  PRE_DRUM,   1, 1, 0,           0, 10 },
    { nB(3),  PRE_PULSE,  4, 3, 0,           0, 12 },


    // ======================================================================
    // V. ZERO GRAVITY
    //
    // Everything suddenly falls away.
    // ======================================================================

    { 0,      KEEP,       5, 0, 0,         168,  0 },

    { nE(3),  PRE_PWM,   14,13, ARP_MIN,     0,  6 },

    { nB(4),  PRE_BELL,   2, 2, 0,           0,  5 },
    { 0,      KEEP,       2, 0, 0,           0,  0 },

    { nC(4),  PRE_PWM,   12,11, ARP_MAJ,     0,  6 },

    { nFs(5), PRE_BELL,   2, 2, 0,           0,  5 },
    { nG(5),  KEEP,       4, 3, 0,           0,  6 },

    { nA(3),  PRE_PWM,   12,11, ARP_MIN,     0,  7 },

    { nDs(5), PRE_BELL,   2, 2, 0,           0,  6 },
    { nE(5),  KEEP,       5, 4, 0,           0,  8 },

    // Hold on the dominant chord as if approaching something enormous.
    { nB(3),  PRE_THIN,  16,15, ARP_MAJ,     0,  8 },


    // ======================================================================
    // VI. EVENT HORIZON
    //
    // The pulse returns and progressively accelerates.
    // ======================================================================

    { nE(3),  PRE_PULSE,  4, 3, 0,         124, 10 },
    { nB(3),  KEEP,       4, 3, 0,           0, 11 },
    { nE(4),  KEEP,       4, 3, 0,         116, 11 },
    { nFs(4), KEEP,       4, 3, 0,           0, 12 },

    { nC(1),  PRE_DRUM,   1, 1, 0,         108, 13 },
    { nE(3),  PRE_PULSE,  3, 2, 0,           0, 11 },
    { nC(7),  PRE_DRUM,   1, 1, 0,           0,  6 },
    { nB(3),  PRE_PULSE,  3, 2, 0,           0, 12 },

    { nC(1),  PRE_DRUM,   1, 1, 0,         100, 14 },
    { nC(4),  PRE_PULSE,  3, 2, 0,           0, 12 },
    { nB(4),  PRE_DRUM,   1, 1, 0,           0, 11 },
    { nA(3),  PRE_PULSE,  3, 2, 0,           0, 13 },

    { nC(1),  PRE_DRUM,   1, 1, 0,          94, 15 },
    { nB(3),  PRE_PULSE,  2, 1, 0,           0, 13 },
    { nC(7),  PRE_DRUM,   1, 1, 0,           0,  8 },
    { nFs(4), PRE_PULSE,  2, 1, 0,           0, 14 },

    { nB(4),  PRE_DRUM,   1, 1, 0,          88, 13 },
    { nDs(4), PRE_PULSE,  2, 1, 0,           0, 14 },
    { nC(1),  PRE_DRUM,   1, 1, 0,           0,  0 },


    // ======================================================================
    // VII. THE SIGNAL REVEALED
    //
    // Main motif returns one octave higher.
    //
    // This is the emotional payoff rather than simply another feature demo.
    // ======================================================================

    { nE(5),  PRE_LEAD,   3, 0, 0,          96, 13 },
    { nB(5),  KEEP,       3, 0, 0,           0, 15 },
    { nFs(5), KEEP,       2, 0, 0,           0, 14 },
    { nG(5),  KEEP,       5, 4, 0,           0,  0 },
    { nDs(5), KEEP,       2, 0, 0,           0, 14 },
    { nE(5),  KEEP,       6, 5, 0,           0,  0 },

    // Answer rises rather than falls.
    { nB(5),  KEEP,       2, 0, 0,           0, 13 },
    { nD(6),  KEEP,       2, 0, 0,           0, 14 },
    { nE(6),  KEEP,       4, 0, 0,           0,  0 },
    { nFs(6), KEEP,       2, 0, 0,           0, 15 },
    { nG(6),  KEEP,       5, 4, 0,           0,  0 },

    // Fall back through the signal tones.
    { nFs(6), KEEP,       2, 0, 0,           0, 15 },
    { nE(6),  KEEP,       2, 0, 0,           0, 14 },
    { nB(5),  KEEP,       3, 0, 0,           0, 13 },
    { nG(5),  KEEP,       2, 0, 0,           0, 12 },
    { nFs(5), KEEP,       2, 0, 0,           0, 11 },

    // Chromatic return: D -> D# -> E
    { nD(5),  KEEP,       2, 0, 0,           0, 12 },
    { nDs(5), KEEP,       2, 0, 0,           0, 14 },
    { nE(5),  KEEP,       8, 6, 0,           0,  0 },


    // ======================================================================
    // VIII. COSMIC EXPANSE
    //
    // The melody dissolves back into enormous arpeggiated harmony.
    // ======================================================================

    { nE(4),  PRE_THIN,  12,11, ARP_MIN,   112, 11 },
    { nC(4),  KEEP,      12,11, ARP_MAJ,     0, 10 },
    { nA(3),  KEEP,      12,11, ARP_MIN,     0,  9 },

    // B major dominant -- bright, strange, unresolved.
    { nB(3),  KEEP,      16,15, ARP_MAJ,     0, 12 },

    // Huge E-minor arrival.
    { nE(3),  PRE_PWM,   20,18, ARP_MIN,   128, 13 },


    // ======================================================================
    // IX. THE SIGNAL FADES
    //
    // Return to the exact sound world of the opening.
    // ======================================================================

    { nC(4),  PRE_THIN,  10, 9, ARP_MAJ,   144,  8 },
    { nA(3),  KEEP,      10, 9, ARP_MIN,   156,  7 },
    { nB(3),  KEEP,      12,11, ARP_MAJ,   168,  6 },

    { 0,      KEEP,       4, 0, 0,           0,  0 },


    // Beacon returns, now fading away.
    { nE(5),  PRE_BELL,   3, 2, 0,         184,  7 },
    { 0,      KEEP,       3, 0, 0,           0,  0 },

    { nB(5),  KEEP,       3, 2, 0,           0,  6 },
    { 0,      KEEP,       3, 0, 0,           0,  0 },

    { nFs(5), KEEP,       2, 2, 0,         196,  5 },
    { nG(5),  KEEP,       4, 3, 0,           0,  6 },

    { 0,      KEEP,       4, 0, 0,           0,  0 },

    { nDs(5), KEEP,       2, 2, 0,         208,  4 },
    { nE(5),  KEEP,       6, 4, 0,           0,  5 },

    { 0,      KEEP,       6, 0, 0,           0,  0 },


    // One extremely distant response.
    { nB(5),  PRE_BELL,   5, 3, 0,         220,  3 },

    { 0,      KEEP,      10, 0, 0,           0,  0 },

    // Final faint E: "the signal is still out there."
    { nE(5),  KEEP,       8, 5, 0,           0,  2 },

    { 0,      KEEP,      12, 0, 0,           0,  0 },

    // End marker
    { 0, 0, 0, 0, 0, 0, 0 },
};
static void synth_demo_event(const demo_ev_t *ev)
{
    if (ev->preset != KEEP) {
        syn.preset_idx = ev->preset;
        syn.pre = &synth_presets[ev->preset];
    }
    if (ev->note) {
        syn.vel16 = ev->vel ? ev->vel : 16;
        synth_note_on(ev->note % 12, ev->note / 12);
        synth_set_arp(ev->note, ev->arp, ev->arp_ms);
    } else
        synth_note_off();
}

static const struct {
    const demo_ev_t *song;
    uint16_t step_ms;       // initial step time (tempo events override)
    const char *name;
} demo_songs[] = {
    { demo_chip, 110, "chiptune" },
    { demo_wave, 176, "synthwave" },
    { demo_mega, 176, "megamix" },
    { demo_afterglow, 156, "afterglow" },
    { synth_dance_song, 125, "synth_dance" },
    { demo_dark_signal, 184, "dark signal" },
    { demo_minuet, 250, "minuet" },
};
#define NUM_DEMOS ((int)(sizeof(demo_songs) / sizeof(demo_songs[0])))

void synth_demo(int which)
{
    const demo_ev_t *song = demo_songs[which - 1].song;
    uint32_t step_us = demo_songs[which - 1].step_ms * 1000u;
    const demo_ev_t *ev = song;
    uint8_t step = 0;

    printf("~~~ PRISM synth demo %d: %s ~~~  (any key stops)\n\n",
           which, demo_songs[which - 1].name);

    uint32_t now = read_time();
    uint32_t next_tick = now + 1000;
    uint32_t next_step = now + step_us;

    synth_demo_event(ev);

    int stopped = 0;

    for (;;) {
        if (uart_getc() != -1) {
            stopped = 1;
            break;
        }

        synth_service();
        now = read_time();

        if ((int32_t)(now - next_tick) >= 0) {
            next_tick += 1000;
            synth_tick();       // envelope, vibrato, sweep and arpeggiator
        }

        if ((int32_t)(now - next_step) >= 0) {
            ++step;
            if (ev->gate && step == ev->gate)
                synth_note_off();
            if (step >= ev->len) {
                ++ev;
                if (ev->len == 0)
                    break;              // song finished: back to the prompt
                step = 0;
                if (ev->tempo)
                    step_us = ev->tempo * 1000u;
                synth_demo_event(ev);
            }
            next_step += step_us;       // new tempo applies immediately
        }
    }

    synth_note_off();
    printf(stopped ? "\ndemo stopped\n" : "\ndemo done\n");
}

int synth_demo_count(void)
{
    return NUM_DEMOS;
}

const char *synth_demo_name(int i)
{
    return demo_songs[i].name;
}
