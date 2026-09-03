# mp3_to_dsm

Converts one channel of an MP3 (or WAV) file to a 1-bit second-order
delta-sigma modulated stream at 1024×1024 Hz (1,048,576 bits/s), emitted as
a C array in one of two formats (`--format`):

- `u32` (default): each `uint32_t` word holds 24 samples in its low three
  bytes (bit 0 plays first, then bits 1..23 in order); the top byte is
  always zero. One 32-bit load per 24 samples, at the cost of ~33% more
  storage than tight packing.
- `u8`: tightly packed `uint8_t` bytes, bit 0 of each byte first. 25%
  smaller in memory than `u32`.

MP3 decoding uses macOS's built-in `afconvert` (or `ffmpeg` if present),
so the only Python dependencies are numpy and scipy.

The conversion pipeline is built for low noise: the audio is upsampled in
two stages (a ×16 polyphase anti-imaging FIR, then fine linear
interpolation over the small remaining ratio) so that spectral images of
the audio band are suppressed instead of being fed to the modulator; the
second-order delta-sigma loop uses a stable CIFB topology with an ideal
(1 − z⁻¹)² noise transfer function; and a small quantizer dither breaks up
idle tones during quiet passages.

A soft-start/soft-stop envelope prevents speaker pop: the audio gets a
raised-cosine fade-in/out (`--fade`), and a DC ramp (`--ramp`) walks the
stream between the output pin's resting level (`--idle low` or `high`) and
the 50%-density zero level, so the stream begins and ends with an unbroken
run of idle-level bits and there is no DC step at the speaker when playback
starts or stops.

## Setup

```sh
python3 -m venv .venv
.venv/bin/pip install numpy scipy
```

## Usage

```sh
.venv/bin/python mp3_to_dsm.py song.mp3 left            # or: right
.venv/bin/python mp3_to_dsm.py song.mp3 right -o out.c -n song_dsm
.venv/bin/python mp3_to_dsm.py song.mp3 left --start 19 --end 46
.venv/bin/python mp3_to_dsm.py song.mp3 left --start 30 --duration 5
```

Options:

- `-o/--output` — output .c file (default `<input>_<channel>.c`)
- `-n/--name`   — C array name (default derived from the filename)
- `-r/--rate`   — DSM bit rate in Hz (default 1048576)
- `-f/--format` — `u32` (24 samples per word, default) or `u8` (packed
  bytes, 25% smaller)
- `-g/--gain`   — input scale, default 0.5; keep ≤ 0.7 or the second-order
  modulator loop goes unstable
- `--dither`    — quantizer dither amplitude (default 0.02, 0 disables)
- `--seed`      — dither RNG seed; same seed → bit-identical output
- `--fade`      — soft-start/stop audio fade-in/out in seconds
  (default 0.1, 0 disables)
- `--ramp`      — soft-start/stop DC ramp to/from the pin idle level in
  seconds, prepended and appended to the stream (default 0.05, 0 disables)
- `--idle`      — pin level when not playing: `low` (default) or `high`
- `--start`, `--end` — slice by absolute positions, e.g. `--start 19
  --end 46` converts 19s..46s (an `--end` past the file just plays to EOF)
- `--duration` — alternative to `--end`: length in seconds from `--start`

Output size at the default rate is **~171 KiB per second** of audio for
`u32` and **128 KiB per second** for `u8`, so slice with `--start`/`--end`
for small targets.

The generated file defines (array type matches `--format`):

```c
const uint32_t <name>_rate;  // bits per second (1048576)
const uint32_t <name>_bits;  // valid 1-bit samples (last element may be partial)
const uint32_t <name>[];     // u32: 24 samples/word, bit 0 first, top byte 0
const uint8_t  <name>[];     // u8:  8 samples/byte, bit 0 first
```

Playback: load an element, shift out bits LSB-first at the bit rate (24
bits per `u32` word, 8 per `u8` byte), into a low-pass filter (an RC is
enough).

Installing `numba` in the venv is optional and speeds up conversion of long
files considerably.
