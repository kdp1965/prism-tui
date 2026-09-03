#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <gpio.h>
#include <csr.h>
#include <uart.h>
#include <prism.h>
#include "prism_tui.h"
#include "synth.h"

/* ==========================================================================
 * midi.c - Standard MIDI file player for the poly engine.
 *
 * Reads a .mid off the host link (tqv.py serves the directory; the
 * songs/ subdir by convention), reduces it to a tempo-mapped, sorted
 * note list (the same collect/pair pipeline as pwl-tui's MidiFile), maps
 * GM programs onto the poly instruments - channel 10 onto the kick/
 * snare/hat kit - and fires the notes at the poly engine, each with its
 * real duration as the gate.  The highest-pitched busy channel is the
 * melody and gets the voice-stealing protection.
 * ========================================================================== */

#define MIDI_MAX_BYTES   (192 * 1024)
#define MIDI_MAX_TEMPOS  64
#define MIDI_MAX_NOTES   12000

typedef struct { uint32_t tick; uint8_t on, chan, note, vel; } RawEv_t;
typedef struct { uint32_t tick, uspb; uint64_t cum_us; } TempoEnt_t;
typedef struct { uint32_t on_ms; uint16_t dur_ms;
                 uint8_t chan, note, vel; } MNote_t;

static uint32_t mrd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t mrd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rdvar(const uint8_t **pp, const uint8_t *end)
{
    uint32_t v = 0;
    const uint8_t *p = *pp;

    while (p < end) {
        v = (v << 7) | (*p & 0x7F);
        if (!(*p++ & 0x80))
            break;
    }
    *pp = p;
    return v;
}

// One track's raw note events + tempo changes + first program per channel
static void collect_raw(const uint8_t *p, const uint8_t *end,
                        RawEv_t *ev, uint32_t *nEv, uint32_t evMax,
                        TempoEnt_t *tp, uint32_t *nTp, uint8_t *chanProg)
{
    uint32_t tick = 0;
    uint8_t  status = 0;

    while (p < end) {
        uint32_t delta = rdvar(&p, end);
        uint8_t  cmd;

        tick += delta;
        if (p >= end)
            break;
        if (*p & 0x80)
            status = *p++;
        cmd = status & 0xF0;

        if (status == 0xFF) {
            uint8_t  type;
            uint32_t len;

            if (p >= end)
                break;
            type = *p++;
            len = rdvar(&p, end);
            if (len > (uint32_t)(end - p))
                break;
            if (type == 0x51 && len >= 3 && *nTp < MIDI_MAX_TEMPOS) {
                tp[*nTp].tick = tick;
                tp[*nTp].uspb = ((uint32_t)p[0] << 16) |
                                ((uint32_t)p[1] << 8) | p[2];
                (*nTp)++;
            }
            p += len;
            if (type == 0x2F)
                break;
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {
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
                chanProg[status & 0x0F] = d1 & 0x7F;
            if ((cmd == 0x90 || cmd == 0x80) && *nEv < evMax) {
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

static int raw_cmp(const void *a, const void *b)
{
    const RawEv_t *x = (const RawEv_t *)a, *y = (const RawEv_t *)b;

    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    return (int)x->on - (int)y->on;     // offs before ons at the same tick
}

static int tempo_cmp(const void *a, const void *b)
{
    const TempoEnt_t *x = (const TempoEnt_t *)a, *y = (const TempoEnt_t *)b;

    return x->tick < y->tick ? -1 : x->tick > y->tick ? 1 : 0;
}

static int note_cmp(const void *a, const void *b)
{
    const MNote_t *x = (const MNote_t *)a, *y = (const MNote_t *)b;

    return x->on_ms < y->on_ms ? -1 : x->on_ms > y->on_ms ? 1 : 0;
}

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

// GM program -> the nearest poly instrument
int midi_gm_inst(int prog)
{
    if (prog < 0)   return POLY_I_ORGAN;
    if (prog < 16)  return POLY_I_PIANO;    // pianos, chromatic perc
    if (prog < 24)  return POLY_I_ORGAN;    // organs
    if (prog < 40)  return POLY_I_BRITE;    // guitars, basses
    if (prog < 56)  return POLY_I_STRINGS;  // strings, ensemble
    if (prog < 64)  return POLY_I_BRASS;    // brass
    if (prog < 80)  return POLY_I_FLUTE;    // reeds, pipes
    if (prog < 96)  return POLY_I_BRITE;    // synth leads
    return POLY_I_STRINGS;                  // pads, the rest
}

// GM drum note -> kit instrument (+ a gate override for open/cymbals)
int midi_drum_inst(int note, uint32_t *gate_ms)
{
    *gate_ms = 0;                           // 0 = the instrument default
    if (note == 35 || note == 36)
        return POLY_I_KICK;
    if (note >= 37 && note <= 40)
        return POLY_I_SNARE;
    if (note == 46 || note == 26) {         // open hat
        *gate_ms = 90;
        return POLY_I_HAT;
    }
    if (note == 49 || note == 51 || note == 52 || note == 55 ||
        note == 57 || note == 59) {         // cymbals ride/crash
        *gate_ms = 160;
        return POLY_I_HAT;
    }
    return POLY_I_HAT;
}

static const char *const inst_names[] = {
    "strings", "brite", "nes", "organ", "piano", "brass", "flute",
    "kick", "snare", "hat"
};

// Fire a converted event list at the poly engine.  Any key stops.
// The TUI's MIDI-tab 'play' and the console 'midi' command both land
// here so the two paths can never drift apart.
void midi_play_events(const PrismEv_t *ev, uint32_t n, int has_drums)
{
    if (poly_engine_start() != 0)
        return;
    if (has_drums)
        poly_drum_voice = poly_nvoices - 1;

    uint32_t t0 = read_time(), idx = 0;

    for (;;) {
        int c2 = uart_getc();

        if (c2 != -1)
            break;
        while (idx < n &&
               (int32_t)(read_time() - t0 -
                         us_ticks(ev[idx].on_ms * 1000u)) >= 0) {
            const PrismEv_t *e = &ev[idx++];

            if (e->inst >= POLY_I_KICK) {
                int vi = poly_note_on_ex(e->inst == POLY_I_KICK ? 9 : 2,
                                         e->inst == POLY_I_KICK ? 1 : 3,
                                         0, e->vel, e->inst);

                if (vi >= 0 && e->dur_ms)
                    poly_voice_gate(vi, e->dur_ms);
            } else {
                int m = e->note;

                while (m < 24)
                    m += 12;
                while (m > 95)
                    m -= 12;
                int vi = poly_note_on_ex(m % 12, m / 12 - 1, e->vocal,
                                         e->vel, e->inst);

                if (vi >= 0)
                    poly_voice_gate(vi, e->dur_ms);
            }
            poly_engine_service();      // a chord burst must not starve
        }                               // the refill
        poly_engine_service();
        if (idx >= n && !poly_active())
            break;
    }
    poly_engine_stop();
    printf("midi: %lu/%lu notes\n", (unsigned long)idx, (unsigned long)n);
}

void cmd_midi(int argc, char *argv[])
{
    // '-i' anywhere: the 2x interpolated output (85MHz only)
    poly_interp = 0;
    for (int a = 1; a < argc; a++)
        if (!strcmp(argv[a], "-i")) {
            poly_interp = 1;
            for (int b = a; b < argc - 1; b++)
                argv[b] = argv[b + 1];
            argc--;
            a--;
        }
    if (poly_interp && clock_hz < 85000000u) {
        printf("-i needs the 85MHz clock (run --freq 85) - playing plain\n");
        poly_interp = 0;
    }
    if (argc < 2) {
        printf("usage: midi [-i] <file>  (reads songs/<file>[.mid] off the"
               " served host dir)\n");
        return;
    }

    // songs/<name>[.mid] conveniences on top of a literal path
    char path[96];
    FILE *f = fopen(argv[1], "rb");

    if (f == NULL) {
        snprintf(path, sizeof(path), "songs/%s", argv[1]);
        f = fopen(path, "rb");
    }
    if (f == NULL && strstr(argv[1], ".mid") == NULL) {
        snprintf(path, sizeof(path), "songs/%s.mid", argv[1]);
        f = fopen(path, "rb");
    }
    if (f == NULL) {
        printf("midi: cannot open %s (served songs/ dir; 'fs probe' if the"
               " link is down)\n", argv[1]);
        return;
    }

    long size;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 22 || size > MIDI_MAX_BYTES) {
        printf("midi: unusable size %ld\n", size);
        fclose(f);
        return;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)size);

    if (buf == NULL || (long)fread(buf, 1, (size_t)size, f) != size) {
        printf("midi: read failed\n");
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);

    if (memcmp(buf, "MThd", 4) != 0 || mrd32(buf + 4) < 6) {
        printf("midi: not a MIDI file\n");
        free(buf);
        return;
    }

    int division = mrd16(buf + 12);

    if (division & 0x8000) {
        int fps = 256 - ((division >> 8) & 0xFF);
        division = fps * (division & 0xFF);
    }
    if (division <= 0)
        division = 480;

    // Collect every raw on/off + tempo across all tracks
    RawEv_t    *raw = (RawEv_t *)malloc(2u * MIDI_MAX_NOTES * sizeof(RawEv_t));
    TempoEnt_t *tp = (TempoEnt_t *)malloc(MIDI_MAX_TEMPOS * sizeof(TempoEnt_t));
    MNote_t    *notes = (MNote_t *)malloc(MIDI_MAX_NOTES * sizeof(MNote_t));
    uint8_t     chanProg[16];
    uint32_t    nEv = 0, nTp = 0, nNotes = 0, i;

    if (raw == NULL || tp == NULL || notes == NULL) {
        printf("midi: out of memory\n");
        goto out;
    }
    memset(chanProg, 0xFF, sizeof(chanProg));

    {
        const uint8_t *p = buf + 8 + mrd32(buf + 4);
        const uint8_t *fileEnd = buf + size;

        while (p + 8 <= fileEnd) {
            uint32_t len = mrd32(p + 4);
            const uint8_t *body = p + 8;

            if (len > (uint32_t)(fileEnd - body))
                len = (uint32_t)(fileEnd - body);
            if (memcmp(p, "MTrk", 4) == 0)
                collect_raw(body, body + len, raw, &nEv,
                            2u * MIDI_MAX_NOTES, tp, &nTp, chanProg);
            p = body + len;
        }
    }
    if (nEv == 0) {
        printf("midi: no notes found\n");
        goto out;
    }

    // Tempo map: default 120bpm at tick 0, then cumulative us
    if (nTp == 0 || tp[0].tick != 0) {
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
                       * tp[i - 1].uspb / (uint32_t)division;

    qsort(raw, nEv, sizeof(RawEv_t), raw_cmp);

    // Pair ons to offs (FIFO per chan/note)
    {
        RawEv_t  open[96];
        uint32_t nOpen = 0, j;

        for (i = 0; i < nEv; i++) {
            const RawEv_t *e = &raw[i];

            if (e->on) {
                if (nOpen < 96)
                    open[nOpen++] = *e;
                continue;
            }
            for (j = 0; j < nOpen; j++)
                if (open[j].chan == e->chan && open[j].note == e->note)
                    break;
            if (j < nOpen && nNotes < MIDI_MAX_NOTES) {
                MNote_t *n = &notes[nNotes++];
                uint32_t on = tick_to_ms(open[j].tick, tp, nTp, division);
                uint32_t off = tick_to_ms(e->tick, tp, nTp, division);
                uint32_t dur = off > on ? off - on : 0;

                n->on_ms = on;
                n->dur_ms = (uint16_t)(dur < 45 ? 45 :
                                       dur > 65000 ? 65000 : dur);
                n->chan = open[j].chan;
                n->note = open[j].note;
                n->vel = open[j].vel;
                memmove(&open[j], &open[j + 1],
                        (nOpen - j - 1) * sizeof(RawEv_t));
                nOpen--;
            }
        }
    }
    qsort(notes, nNotes, sizeof(MNote_t), note_cmp);
    free(raw);
    raw = NULL;
    free(tp);
    tp = NULL;

    // Map channels: GM program -> instrument; the busiest high channel
    // is the melody (voice-stealing protection)
    {
        uint32_t cnt[16] = { 0 }, sum[16] = { 0 };
        int      melody = -1, inst[16], c;
        uint32_t best = 0;

        for (i = 0; i < nNotes; i++) {
            cnt[notes[i].chan]++;
            sum[notes[i].chan] += notes[i].note;
        }
        for (c = 0; c < 16; c++) {
            inst[c] = midi_gm_inst(chanProg[c] == 0xFF ? -1 : chanProg[c]);
            if (c != 9 && cnt[c] >= 12) {
                uint32_t mean = sum[c] / cnt[c];

                if (mean > best) {
                    best = mean;
                    melody = c;
                }
            }
        }

        poly_nvoices = clock_hz >= 85000000u ? 4 : 3;   // banner truth:
                                        // engine start would set it after
        printf("%lu notes", (unsigned long)nNotes);
        if (nNotes)
            printf(", %lu:%02lu",
                   (unsigned long)(notes[nNotes - 1].on_ms / 60000u),
                   (unsigned long)(notes[nNotes - 1].on_ms / 1000u % 60u));
        printf(" (%d voices%s)\n", poly_nvoices,
               poly_interp ? ", 2x interp" : "");
        for (c = 0; c < 16; c++)
            if (cnt[c])
                printf("  ch%-2d %-8s %5lu notes%s\n", c + 1,
                       c == 9 ? "drums" : inst_names[inst[c]],
                       (unsigned long)cnt[c],
                       c == melody ? "  << melody" : "");
        printf("(any key stops)\n");

        if (poly_engine_start() != 0)
            goto out;
        if (cnt[9])                     // the song has drums: the kit
            poly_drum_voice = poly_nvoices - 1;     // owns the last voice

        uint32_t t0 = read_time(), idx = 0;

        for (;;) {
            int c2 = uart_getc();

            if (c2 != -1)
                break;
            while (idx < nNotes &&
                   (int32_t)(read_time() - t0 -
                             us_ticks(notes[idx].on_ms * 1000u)) >= 0) {
                MNote_t *nn = &notes[idx++];

                if (nn->chan == 9) {
                    uint32_t g = 0;
                    int di = midi_drum_inst(nn->note, &g);
                    int vi = poly_note_on_ex(di == POLY_I_KICK ? 9 : 2,
                                             di == POLY_I_KICK ? 1 : 3,
                                             0, nn->vel, di);

                    if (vi >= 0 && g)
                        poly_voice_gate(vi, g);
                } else {
                    int m = nn->note;

                    while (m < 24)
                        m += 12;
                    while (m > 95)
                        m -= 12;
                    int vi = poly_note_on_ex(m % 12, m / 12 - 1,
                                             nn->chan == melody, nn->vel,
                                             inst[nn->chan]);

                    if (vi >= 0)
                        poly_voice_gate(vi, nn->dur_ms);
                }
                poly_engine_service();  // a chord burst must not starve
            }                           // the refill
            poly_engine_service();
            if (idx >= nNotes && !poly_active())
                break;
        }
        poly_engine_stop();
        printf("midi: %lu/%lu notes\n", (unsigned long)idx,
               (unsigned long)nNotes);
    }

out:
    free(raw);
    free(tp);
    free(notes);
    free(buf);
}
