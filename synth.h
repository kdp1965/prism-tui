#pragma once

// Synth engine internals, shared between synth.c (engine + interactive
// keyboard) and synth_demos.c (song tables + demo sequencer).

#include <stdint.h>

#define SYNTH_M         62          // count2 compare: carrier period - 2
#define SYNTH_CENTER    31          // comm value that gives silence
#define SYNTH_LVL_MAX   3840        // envelope level for amp 30 (level>>7)
#define SYNTH_MIN_HALF  130u        // clamp: >= 2 carrier periods
#define SYNTH_ARP_MS    35          // default arp rate: audible arpeggio
#define SYNTH_DUO_MS    10          // alternation rate reading as "2 notes"

// WAVE_LASER = square with an exponential downward pitch dive (one-shot);
// WAVE_GUN / WAVE_BOOM = LFSR noise whose pitch darkens as it decays.
// For these three the preset's pw field is the sweep rate (base_p grows
// by base_p >> pw every ms - smaller pw = faster dive).
enum { WAVE_SQUARE, WAVE_PULSE, WAVE_SWEEP, WAVE_NOISE, WAVE_FLUTE,
       WAVE_LASER, WAVE_GUN, WAVE_BOOM };

// The noise generator serves the drum kit and the gun/explosion effects
#define WAVE_IS_NOISE(w) \
    ((w) == WAVE_NOISE || (w) == WAVE_GUN || (w) == WAVE_BOOM)

enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    const char *name;
    uint8_t  wave;
    uint8_t  pw;         // pulse width in /256 of the note period; for the
                         //   laser/gun/boom effects: pitch sweep shift
    uint8_t  vib_depth;  // 0 = off .. 8 = ~+/-0.8% pitch
    uint8_t  vib_rate;   // ms per vibrato table step (16 step table)
    uint16_t vib_delay;  // ms before vibrato fades in
    uint16_t attack;     // level units per ms (0 = instant)
    uint16_t decay;      // level units per ms towards sustain
    uint16_t sustain;    // 0..3840
    uint16_t release;    // level units per ms
} synth_preset_t;

extern const synth_preset_t synth_presets[];

// Preset indexes (must match synth_presets[] order in synth.c)
#define PRE_LEAD    0
#define PRE_BRASS   1
#define PRE_PULSE   2
#define PRE_THIN    3
#define PRE_PWM     4
#define PRE_BELL    5
#define PRE_DRUM    6
#define PRE_SHAKU   7
#define PRE_LASER   8
#define PRE_GUN     9
#define PRE_BOOM    10

// Synth engine state
typedef struct {
    const synth_preset_t *pre;
    int      preset_idx;
    int      octave;         // base octave for the bottom key row
    uint8_t  env_state;
    uint16_t level;          // envelope level 0..3840
    uint32_t base_p;         // half period of the sounding note
    uint32_t t_a, t_b;       // alternating half periods (pulse modes)
    uint8_t  swap_phase;
    uint8_t  pw_cur;         // current pulse width (sweep mode)
    int8_t   pw_dir;
    uint16_t gate_ms;        // ms since note on
    uint8_t  vib_phase;
    uint16_t vib_div;
    uint16_t lfsr;
    uint8_t  vel16;          // velocity 1..16: scales the envelope output
    // Arpeggiator: cycles the pitch through up to 3 notes every arp_ms
    // without retriggering the envelope.  At the default 35ms this is the
    // classic chip arpeggio; at ~10ms two notes fuse into one thicker
    // "polyphonic" voice (the Model 100 MINUET trick).
    uint8_t  arp_note[3];    // absolute notes; [0] is the base note
    uint8_t  arp_n;          // notes in the cycle (<2 = static pitch)
    uint8_t  arp_ms;         // ms per alternation
    uint8_t  arp_idx;
    uint8_t  arp_tmr;
    uint8_t  duo;            // live keyboard duo interval index, 0 = off
    // Flute voice (WAVE_FLUTE): the FSM already flips output polarity at
    // every phase swap, so shaping the amplitude through a half-sine lobe
    // between swaps turns the square wave into a sine.  pitch_target
    // carries the scoop glide (notes start slightly flat, breath style).
    uint32_t pitch_target;   // where base_p is gliding to
    uint32_t flute_next;     // us deadline of the next amplitude sample
    uint8_t  flute_step;     // 0..15 position within the half-sine lobe
} synth_state_t;

extern synth_state_t syn;

// Engine (synth.c)
void synth_set_arp(uint8_t base, uint8_t intervals, uint8_t ms);
void synth_note_on(int semi, int oct);
void synth_note_off(void);
void synth_service(void);       // phase swap / flute servicing, call often
void synth_tick(void);          // 1ms housekeeping: envelope, vibrato, ...

// Note-progression output ("C#4 ", "[organ] ", "oct3 " ...).  The TUI
// installs a sink so the stream lands in its Notes tab; on the plain
// console it stays ordinary printf output.
extern int (*synth_note_out)(const char *fmt, ...);
#define note_out(...) \
    (synth_note_out ? synth_note_out(__VA_ARGS__) : printf(__VA_ARGS__))

// Note numbers for song tables: nX(octave), 0 = rest
#define nC(o)   ((uint8_t)((o) * 12 + 0))
#define nCs(o)  ((uint8_t)((o) * 12 + 1))
#define nD(o)   ((uint8_t)((o) * 12 + 2))
#define nDs(o)  ((uint8_t)((o) * 12 + 3))
#define nE(o)   ((uint8_t)((o) * 12 + 4))
#define nF(o)   ((uint8_t)((o) * 12 + 5))
#define nFs(o)  ((uint8_t)((o) * 12 + 6))
#define nG(o)   ((uint8_t)((o) * 12 + 7))
#define nGs(o)  ((uint8_t)((o) * 12 + 8))
#define nA(o)   ((uint8_t)((o) * 12 + 9))
#define nAs(o)  ((uint8_t)((o) * 12 + 10))
#define nB(o)   ((uint8_t)((o) * 12 + 11))

// Poly engine (synth.c) - shared with the MIDI player (midi.c).
// Instrument indexes live in midiev.h (the C++ MIDI tab needs them too).
#include "midiev.h"

int  poly_engine_start(void);   // chroma + carrier + buffers + ISR
void poly_engine_stop(void);    // fade down, PWM off, stats print
int  poly_engine_service(void); // buffer refill housekeeping (call often)
int  poly_note_on_ex(int semi, int oct, int vocal, int vel, int inst);
void poly_voice_gate(int vi, uint32_t ms);  // override auto note-off
int  poly_active(void);         // any voice still sounding
const char *poly_inst_name(int idx);        // poly_presets[] names
int  poly_inst_find(const char *name);      // -1 unknown
int  poly_inst_count(void);
extern int poly_interp;         // '-i' 2x interpolated output
extern int poly_nvoices;        // 3 voices; 4 at the 85MHz clock
extern int poly_preset;         // keyboard-selected instrument
extern int poly_drum_voice;     // reserved kit voice, -1 = none

// Per-voice note stream (TUI voice-track Notes tab); NULL = the plain
// synth_note_out stream is used instead
extern void (*poly_note_out2)(int voice, const char *text);

void cmd_midi(int argc, char *argv[]);      // midi.c

// Demo songs (synth_demos.c)
void synth_demo(int which);     // play demo song 1..synth_demo_count()
int synth_demo_count(void);
const char *synth_demo_name(int i);
