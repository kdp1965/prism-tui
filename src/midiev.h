#pragma once

// One converted MIDI play event for the poly engine - shared between
// the C player (midi.c) and the C++ MIDI tab (tui/MidiFile, Prism).
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Poly instrument indexes (poly_presets[] order in synth.c)
#define POLY_I_STRINGS  0
#define POLY_I_BRITE    1
#define POLY_I_NES      2
#define POLY_I_ORGAN    3
#define POLY_I_PIANO    4
#define POLY_I_BRASS    5
#define POLY_I_FLUTE    6
#define POLY_I_KICK     7
#define POLY_I_SNARE    8
#define POLY_I_HAT      9

typedef struct
{
  uint32_t on_ms;
  uint16_t dur_ms;
  uint8_t  note, vel;
  uint8_t  inst;            // poly_presets index (kit >= POLY_I_KICK)
  uint8_t  vocal;           // melody voice-stealing protection
} PrismEv_t;

// midi.c: fire a converted event list at the poly engine (any key
// stops); has_drums reserves the last voice for the kit
void midi_play_events(const PrismEv_t *ev, uint32_t n, int has_drums);

// midi.c: GM program -> poly instrument, GM drum note -> kit
// instrument (+ gate override ms, 0 = the instrument's own)
int midi_gm_inst(int prog);
int midi_drum_inst(int note, uint32_t *gate_ms);

#ifdef __cplusplus
}
#endif
