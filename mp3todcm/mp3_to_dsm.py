#!/usr/bin/env python3
"""Convert one channel of an audio file to a 1-bit delta-sigma (DSM) stream.

The selected channel is decoded, upsampled to the DSM bit rate
(default 1024*1024 = 1,048,576 Hz), run through a delta-sigma modulator,
and written out as a C array.  Bits are packed LSB-first: the LSB of each
byte is the earliest sample, the MSB the latest.

Input can be anything the system decoder understands (.mp3, .m4a/AAC,
.aiff, ...); .wav input is read directly.  Decoding uses `afconvert`
(built into macOS) or `ffmpeg` if available.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

import numpy as np
import scipy.signal as sig

DEFAULT_RATE = 1024 * 1024  # 1,048,576 Hz


def decode_to_wav(src: str, dst_wav: str) -> None:
    """Decode an audio file to 16-bit LE PCM WAV using afconvert or ffmpeg."""
    if shutil.which("afconvert"):
        cmd = ["afconvert", "-f", "WAVE", "-d", "LEI16", src, dst_wav]
    elif shutil.which("ffmpeg"):
        cmd = ["ffmpeg", "-y", "-v", "error", "-i", src,
               "-acodec", "pcm_s16le", dst_wav]
    else:
        sys.exit("error: need either afconvert (macOS) or ffmpeg to decode "
                 "compressed audio (.mp3/.m4a/...)")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"error: decoding {src!r} failed:\n{res.stderr.strip()}")


def read_wav_channel(path: str, channel: str) -> tuple[np.ndarray, int]:
    """Return (float samples in [-1, 1], sample_rate) for one channel.

    Uses scipy.io.wavfile rather than the stdlib wave module: afconvert
    emits WAVE_FORMAT_EXTENSIBLE headers for some sources (e.g. mono
    .m4a), which stdlib wave rejects.
    """
    from scipy.io import wavfile
    rate, data = wavfile.read(path)
    if data.dtype != np.int16:
        sys.exit(f"error: expected 16-bit PCM after decode, got {data.dtype}")
    if data.ndim == 1:
        print("note: source is mono; using the only channel")
        col_data = data
    else:
        col_data = data[:, 0 if channel == "left" else 1]
    return col_data.astype(np.float32) / 32768.0, rate


def _dsm_core(chunk, dither, s1, s2, fb):
    """Second-order delta-sigma modulator, stable CIFB topology.

    Feedback coefficient 1.0 at both stages: with the second integrator
    consuming the *updated* s1 in the same step, this yields the ideal
    NTF (1 - z^-1)^2 with unity signal transfer.  `dither` is a small
    per-sample noise sequence added at the quantizer to break up idle
    tones / limit cycles.  Returns (bits, s1, s2, fb) so integrator state
    carries across chunks.
    """
    bits = np.empty(len(chunk), dtype=np.uint8)
    for i in range(len(chunk)):
        s1 += chunk[i] - fb
        s2 += s1 - fb
        # safety clamp: bounds recovery from any transient overload
        if s1 > 8.0:
            s1 = 8.0
        elif s1 < -8.0:
            s1 = -8.0
        if s2 > 8.0:
            s2 = 8.0
        elif s2 < -8.0:
            s2 = -8.0
        if s2 + dither[i] >= 0.0:
            bits[i] = 1
            fb = 1.0
        else:
            bits[i] = 0
            fb = -1.0
    return bits, s1, s2, fb


try:  # optional large speedup if numba is installed
    from numba import njit
    _dsm_core = njit(cache=True)(_dsm_core)
except ImportError:
    pass


def dsm_modulate(chunk: np.ndarray, dither: np.ndarray,
                 state: dict) -> np.ndarray:
    bits, s1, s2, fb = _dsm_core(chunk.astype(np.float64), dither,
                                 state["s1"], state["s2"], state["fb"])
    state.update(s1=s1, s2=s2, fb=fb)
    return bits


OVERSAMPLE = 16   # polyphase FIR upsample factor before fine interpolation
CTX = 256         # input samples of context per chunk edge for the FIR


def limit_peaks(audio: np.ndarray, rate: int, ceiling: float,
                look_ms: float, release_ms: float) -> np.ndarray:
    """Look-ahead peak limiter.

    Holds |audio| at or below `ceiling` with a gain that starts to fall
    `look_ms` before a peak arrives (so the attack is a short ramp, not a
    step) and recovers exponentially with a `release_ms` time constant.
    Used with --boost to raise the loudness of a song without clipping:
    the PWM DAC's quantization floor is fixed relative to full scale, so
    every dB of level is a dB of signal-to-noise.  Prints how much of the
    song it touched so the boost can be judged (a few percent of samples
    with a few dB of reduction is inaudible; tens of percent is squashed).
    """
    from scipy.ndimage import minimum_filter1d
    n = max(1, int(look_ms * rate / 1000))
    # gain each sample needs, spread over +-n samples (the look-ahead)
    need = minimum_filter1d(np.minimum(1.0, ceiling / np.maximum(np.abs(audio), 1e-9)),
                            size=2 * n + 1)
    # instant attack onto `need`, exponential release back toward unity
    g = np.empty_like(need)
    a = np.exp(-1000.0 / (release_ms * rate))
    cur = 1.0
    for i in range(len(need)):
        cur = need[i] if need[i] < cur else 1.0 - (1.0 - cur) * a
        g[i] = cur
    # smooth the attack into a ramp; the look-ahead margin makes this safe
    g = np.convolve(np.pad(g, n, mode="edge"), np.ones(n) / n,
                    mode="same")[n:-n]
    g = np.minimum(g, need)
    red = -20.0 * np.log10(np.maximum(g, 1e-6))
    touched = np.mean(red > 0.1) * 100.0
    print(f"limiter: ceiling {ceiling:.2f}, peak in {np.abs(audio).max():.2f} "
          f"-> out {np.abs(audio * g).max():.2f}; {touched:.2f}% of samples "
          f"reduced, {np.sum(red > 1.0) / rate:.1f}s by >1dB, max {red.max():.1f}dB")
    return audio * g


def apply_envelope(audio: np.ndarray, in_rate: int, fade_s: float,
                   ramp_s: float, idle: float) -> np.ndarray:
    """Soft-start / soft-stop.  `audio` must already be gain-scaled.

    Applies a raised-cosine fade-in/out of `fade_s` seconds to the audio
    itself, then prepends/appends a `ramp_s`-second DC ramp between the
    output pin's idle level (`idle` = -1.0 for a pin resting low, +1.0 for
    high) and the stream's zero level, so playback starts and ends without
    a DC step at the speaker.
    """
    audio = audio.copy()
    nf = min(int(fade_s * in_rate), len(audio) // 2)
    if nf > 0:
        w = 0.5 - 0.5 * np.cos(np.linspace(0.0, np.pi, nf))
        audio[:nf] *= w
        audio[-nf:] *= w[::-1]
    nr = int(ramp_s * in_rate)
    if nr > 0:
        r = 0.5 - 0.5 * np.cos(np.linspace(0.0, np.pi, nr))  # 0 -> 1
        audio = np.concatenate([idle * (1.0 - r), audio, idle * r])
    return audio


def modulate_all(audio: np.ndarray, in_rate: int, out_rate: int,
                 dither_amp: float, rng: np.random.Generator,
                 idle: float = 1.0) -> np.ndarray:
    """Upsample + modulate, chunked to bound memory.

    Two-stage upsampling: a polyphase anti-imaging FIR takes the audio to
    in_rate*OVERSAMPLE (suppressing the spectral images that plain linear
    interpolation would leave at multiples of in_rate), then linear
    interpolation covers the small remaining ratio to out_rate, where its
    own images are negligible.  `audio` must already be gain-scaled.
    """
    audio = audio.astype(np.float64)
    n_in = len(audio)
    n_out = int(np.floor((n_in - 1) * out_rate / in_rate)) + 1
    # anti-imaging FIR: pass to ~20 kHz, stop by in_rate - 20 kHz
    mid_rate = in_rate * OVERSAMPLE
    fir = sig.firwin(OVERSAMPLE * 64 + 1, min(20500.0, in_rate * 0.465),
                     fs=mid_rate, window=("kaiser", 9.0))
    # start the loop at its fixed point for a railed input at `idle`, so
    # the stream begins with an unbroken run of idle-level bits
    state = {"s1": idle, "s2": idle, "fb": idle}
    chunks = []
    step = in_rate  # one second of input per chunk
    j = 0
    for i0 in range(0, n_in, step):
        i1 = min(i0 + step, n_in)
        lo = max(i0 - CTX, 0)
        hi = min(i1 + CTX, n_in)
        mid = sig.resample_poly(audio[lo:hi], OVERSAMPLE, 1, window=fir)
        # output sample times (in input-sample units) that fall in [i0, i1)
        j_end = n_out if i1 == n_in else int(np.ceil((i1 - 1) * out_rate / in_rate))
        t = np.arange(j, j_end, dtype=np.float64) * (in_rate / out_rate)
        up = np.interp((t - lo) * OVERSAMPLE,
                       np.arange(len(mid), dtype=np.float64), mid)
        dither = (rng.random(len(up)) - 0.5) * dither_amp
        chunks.append(dsm_modulate(up, dither, state))
        j = j_end
        done = min(i1 / in_rate, n_in / in_rate)
        print(f"\r  modulating: {done:6.1f}s / {n_in / in_rate:.1f}s",
              end="", flush=True)
    print()
    return np.concatenate(chunks)



# ============================================================================
# IMA ADPCM output (--format adpcm): 4-bit samples for the PRISM PCM chroma.
#
# The audio is resampled to --pcm-rate (31250 Hz = 64MHz / 2048, the PCM
# chroma sample clock) and encoded as standard IMA ADPCM, packed in
# independently decodable blocks: the encoder runs continuously but dumps
# its state (predictor, step index) into each block header, so the player
# can stream blocks through small ping-pong buffers.
#
# Blob layout (little endian, matches play.c):
#   u32 total samples, u32 sample rate, u32 samples per block
#   per block: i16 predictor, u8 step index, u8 reserved,
#              block_samples/2 bytes of nibbles (low nibble plays first)
# ============================================================================

ADPCM_BLOCK = 1024   # samples per block (multiple of 2)

_IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
_IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(samples: np.ndarray) -> bytes:
    """Encode int16 samples to the block format above."""
    n = len(samples)
    pred, idx = int(samples[0]), 0
    out = bytearray()
    out += int(n).to_bytes(4, "little")
    out += b"\x00\x00\x00\x00"        # rate patched by caller
    out += int(ADPCM_BLOCK).to_bytes(4, "little")
    for b0 in range(0, n, ADPCM_BLOCK):
        blk = samples[b0:b0 + ADPCM_BLOCK]
        out += int(pred & 0xFFFF).to_bytes(2, "little")
        out.append(idx)
        out.append(0)
        byte = 0
        for i, s in enumerate(blk):
            step = _IMA_STEP[idx]
            diff = int(s) - pred
            nib = 0
            if diff < 0:
                nib = 8
                diff = -diff
            if diff >= step:
                nib |= 4
                diff -= step
            if diff >= step >> 1:
                nib |= 2
                diff -= step >> 1
            if diff >= step >> 2:
                nib |= 1
            # reconstruct exactly like the decoder
            delta = step >> 3
            if nib & 4:
                delta += step
            if nib & 2:
                delta += step >> 1
            if nib & 1:
                delta += step >> 2
            pred = pred - delta if nib & 8 else pred + delta
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + _IMA_INDEX[nib]))
            if i & 1:
                out.append(byte | (nib << 4))
            else:
                byte = nib
        if len(blk) & 1:                 # odd tail (last block only)
            out.append(byte)
    return bytes(out)


def adpcm_decode_check(blob: bytes) -> np.ndarray:
    """Reference decoder (logic-identical to play.c) for verification."""
    total = int.from_bytes(blob[0:4], "little")
    bs = int.from_bytes(blob[8:12], "little")
    o = 12
    outp = np.empty(total, dtype=np.int16)
    n = 0
    while n < total:
        pred = int.from_bytes(blob[o:o + 2], "little", signed=True)
        idx = blob[o + 2]
        o += 4
        cnt = min(bs, total - n)
        for i in range(cnt):
            nib = (blob[o + (i >> 1)] >> (4 * (i & 1))) & 0xF
            step = _IMA_STEP[idx]
            delta = step >> 3
            if nib & 4:
                delta += step
            if nib & 2:
                delta += step >> 1
            if nib & 1:
                delta += step >> 2
            pred = pred - delta if nib & 8 else pred + delta
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + _IMA_INDEX[nib]))
            outp[n + i] = pred
        o += (cnt + 1) // 2
        n += cnt
    return outp


def write_adpcm_c(path: str, blob: bytes, name: str, src: str,
                  channel: str, rate: int, gain: float,
                  n_samples: int) -> None:
    with open(path, "w") as f:
        f.write(
            f"// Generated by mp3_to_dsm.py --format adpcm\n"
            f"// Source: {os.path.basename(src)} ({channel} channel)\n"
            f"// IMA ADPCM, {rate} Hz, gain {gain:.2f}, "
            f"{n_samples} samples ({n_samples / rate:.2f} s)\n"
            f"// Layout: u32 samples, u32 rate, u32 block samples, then\n"
            f"// blocks of {{i16 predictor, u8 index, u8 pad, nibbles}}.\n"
            f"// Decoder: play.c adpcm_decode_block().\n\n"
            f"#include <stdint.h>\n\n"
            f"const uint32_t {name}_adpcm_rate = {rate}u;\n"
            f"const uint32_t {name}_adpcm_size = {len(blob)}u;\n"
            f"const uint8_t {name}_adpcm[] __attribute__((aligned(4))) = {{\n")
        for i in range(0, len(blob), 16):
            f.write("    " + " ".join(f"0x{b:02x}," for b in blob[i:i + 16])
                    + "\n")
        f.write("};\n")


def sanitize_name(name: str) -> str:
    name = re.sub(r"\W+", "_", name).strip("_")
    if not name or name[0].isdigit():
        name = "_" + name
    return name


def write_c_array(path: str, bits: np.ndarray, name: str, src: str,
                  channel: str, rate: int, gain: float, fmt: str) -> int:
    """Write the packed stream as a C array; returns its size in bytes.

    fmt 'u32': 3 bytes per uint32_t word (24 samples, top byte zero) for
    single 32-bit loads.  fmt 'u8': tightly packed bytes, 25% smaller.
    """
    packed = np.packbits(bits, bitorder="little")  # LSB = first sample
    header = (
        f"// Generated by mp3_to_dsm.py\n"
        f"// Source: {os.path.basename(src)} ({channel} channel)\n"
        f"// 1-bit second-order delta-sigma stream, {rate} Hz, "
        f"gain {gain:.2f}\n"
    )
    if fmt == "u32":
        # 3 bytes per uint32_t word: low byte plays first, top byte zero
        pad = (-len(packed)) % 3
        padded = np.concatenate([packed, np.zeros(pad, dtype=np.uint8)])
        b = padded.reshape(-1, 3).astype(np.uint32)
        words = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        header += (
            f"// Each uint32_t holds 24 samples: bit 0 plays first, then\n"
            f"// bits 1..23 in order.  Bits 24..31 are always zero.\n\n"
            f"#include <stdint.h>\n\n"
            f"const uint32_t {name}_rate = {rate}u;  // bits per second\n"
            f"const uint32_t {name}_bits = {len(bits)}u;  "
            f"// valid samples (last word may be partial)\n"
            f"const uint32_t {name}[{len(words)}] = {{\n"
        )
        values, per_line, size = [f"0x{w:08X}" for w in words], 8, 4 * len(words)
    else:
        header += (
            f"// Packed bytes: bit 0 of each byte plays first, bit 7 last.\n\n"
            f"#include <stdint.h>\n\n"
            f"const uint32_t {name}_rate = {rate}u;  // bits per second\n"
            f"const uint32_t {name}_bits = {len(bits)}u;  "
            f"// valid samples (last byte may be partial)\n"
            f"const uint8_t {name}[{len(packed)}] = {{\n"
        )
        values, per_line, size = [f"0x{b:02X}" for b in packed], 16, len(packed)
    with open(path, "w") as f:
        f.write(header)
        for i in range(0, len(values), per_line):
            f.write(f"    {', '.join(values[i:i + per_line])},\n")
        f.write("};\n")
    return size


def trim_silence_cap(audio: np.ndarray, rate: int, keep_s: float,
                     floor_db: float) -> tuple[np.ndarray, float, float]:
    """Cap leading/trailing silence at keep_s seconds.

    Silence is anything below floor_db relative to the clip's own peak.
    Everything between the first and last loud samples is untouched;
    each end keeps at most keep_s of its surrounding quiet, so a stock
    file's seconds of room tone stop wasting flash without clipping
    into the music."""
    peak = float(np.max(np.abs(audio))) if len(audio) else 0.0
    if peak <= 0.0:
        return audio, 0.0, 0.0
    thresh = peak * (10.0 ** (floor_db / 20.0))
    loud = np.nonzero(np.abs(audio) > thresh)[0]
    if len(loud) == 0:
        return audio, 0.0, 0.0
    keep = int(keep_s * rate)
    a = max(0, int(loud[0]) - keep)
    b = min(len(audio), int(loud[-1]) + keep + 1)
    return audio[a:b], a / rate, (len(audio) - b) / rate


def main() -> None:
    p = argparse.ArgumentParser(
        description="Convert one channel of an audio file to a 1-bit "
                    "delta-sigma C array (LSB of each byte plays first).")
    p.add_argument("input", help="input audio file (.mp3, .m4a, .wav, or "
                   "anything afconvert/ffmpeg decodes)")
    p.add_argument("channel", choices=["left", "right"],
                   help="which channel to convert")
    p.add_argument("-o", "--output", help="output .c file "
                   "(default: <input>_<channel>.c)")
    p.add_argument("-n", "--name", help="C array name "
                   "(default: derived from input filename)")
    p.add_argument("-r", "--rate", type=int, default=DEFAULT_RATE,
                   help=f"DSM bit rate in Hz (default {DEFAULT_RATE})")
    p.add_argument("-f", "--format", choices=["u32", "u8", "adpcm"],
                   default="u32",
                   help="C array format: u32 = 24 samples per uint32_t word "
                        "(single 32-bit loads, top byte zero); u8 = tightly "
                        "packed uint8_t bytes, 25%% smaller; adpcm = 4-bit "
                        "IMA ADPCM at --pcm-rate for the PRISM PCM chroma, "
                        "~8x smaller than DSM (default u32)")
    p.add_argument("--pcm-rate", type=int, default=31250,
                   help="ADPCM sample rate in Hz; 31250 = 64MHz PCM chroma "
                        "sample clock (default 31250)")
    p.add_argument("-g", "--gain", type=float, default=0.5,
                   help="input scale; keep <= 0.7 for modulator stability "
                        "(default 0.5)")
    bg = p.add_mutually_exclusive_group()
    bg.add_argument("--autoboost", type=float, nargs="?", const=5.0,
                    default=None, metavar="PCT",
                    help="find the maximum boost that leaves only PCT%% "
                         "of samples above the limiter ceiling (bare: "
                         "5%%) and enable --limit at its default; pairs "
                         "well with --trim so silence does not skew the "
                         "statistics")
    bg.add_argument("--boost", type=float, default=0.0,
                   help="extra gain in dB applied after --gain; use with "
                        "--limit to raise loudness without clipping "
                        "(default 0)")
    p.add_argument("--limit", type=float, nargs="?", const=-1.0,
                   default=None, metavar="CEILING",
                   help="look-ahead peak limiter holding the audio at or "
                        "below CEILING (default 0.98 for adpcm, 0.8 for "
                        "DSM where the modulator needs the headroom)")
    p.add_argument("--limit-lookahead", type=float, default=2.0,
                   metavar="MS", help="limiter look-ahead in ms (default 2)")
    p.add_argument("--limit-release", type=float, default=80.0,
                   metavar="MS", help="limiter release time in ms (default 80)")
    p.add_argument("--dither", type=float, default=0.02,
                   help="quantizer dither amplitude to suppress idle tones; "
                        "0 disables (default 0.02)")
    p.add_argument("--seed", type=int, default=0,
                   help="dither RNG seed, for reproducible output (default 0)")
    p.add_argument("--fade", type=float, default=0.1,
                   help="soft-start/stop: raised-cosine fade-in/out of the "
                        "audio, in seconds; 0 disables (default 0.1)")
    p.add_argument("--ramp", type=float, default=0.05,
                   help="soft-start/stop: DC ramp between the pin idle "
                        "level and the stream zero level, prepended and "
                        "appended, in seconds; 0 disables (default 0.05)")
    p.add_argument("--idle", choices=["low", "high"], default="low",
                   help="output pin level when not playing, sets the ramp "
                        "endpoints and how the stream starts/ends "
                        "(default low)")
    p.add_argument("--trim", type=float, nargs="?", const=0.5,
                   default=None, metavar="S",
                   help="cap leading/trailing silence at S seconds "
                        "(bare --trim: 0.5); silence is below "
                        "--trim-floor dB of the clip's peak")
    p.add_argument("--trim-floor", type=float, default=-45.0, metavar="DB",
                   help="level counted as silence for --trim, dB below "
                        "the peak (default -45)")
    p.add_argument("--start", type=float, default=0.0,
                   help="start offset in seconds (default 0)")
    end_group = p.add_mutually_exclusive_group()
    end_group.add_argument("--duration", type=float,
                           help="limit length in seconds "
                                "(default: to end of file)")
    end_group.add_argument("--end", type=float,
                           help="end point in seconds, e.g. --start 19 "
                                "--end 46 converts 19s..46s")
    args = p.parse_args()

    if not os.path.exists(args.input):
        sys.exit(f"error: no such file: {args.input}")
    max_gain = 1.0 if args.format == "adpcm" else 0.8
    if not 0.0 < args.gain <= max_gain:
        sys.exit(f"error: --gain must be in (0, {max_gain}]" +
                 ("" if args.format == "adpcm" else
                  "; higher values make the second-order modulator unstable"))
    if not 0.0 <= args.dither <= 0.2:
        sys.exit("error: --dither must be in [0, 0.2]")
    if args.autoboost is not None:
        if not 0.0 < args.autoboost < 50.0:
            sys.exit("error: --autoboost percent must be in (0, 50)")
        if args.limit is None:
            args.limit = -1.0           # bare --limit: the default ceiling
    if args.limit is not None:
        if args.limit < 0:
            args.limit = 0.98 if args.format == "adpcm" else max_gain
        if not 0.0 < args.limit <= max_gain or (args.format == "adpcm" and args.limit > 1.0):
            sys.exit(f"error: --limit ceiling must be in (0, {max_gain}]")
        if args.limit_lookahead <= 0 or args.limit_release <= 0:
            sys.exit("error: --limit-lookahead and --limit-release must be > 0")
    elif args.boost > 0:
        sys.exit("error: --boost raises peaks above --gain; add --limit "
                 "(or lower --boost) so they cannot clip")
    if args.boost < 0:
        sys.exit("error: --boost must be >= 0 (lower --gain instead)")
    if args.fade < 0 or args.ramp < 0:
        sys.exit("error: --fade and --ramp must be >= 0")
    if args.trim is not None and args.trim < 0:
        sys.exit("error: --trim must be >= 0 seconds")
    if args.trim_floor >= 0:
        sys.exit("error: --trim-floor must be negative (dB below the peak)")

    if args.input.lower().endswith(".wav"):
        wav_path, tmp = args.input, None
    else:
        fd, wav_path = tempfile.mkstemp(suffix=".wav")
        os.close(fd)
        tmp = wav_path
        print(f"decoding {args.input} ...")
        decode_to_wav(args.input, wav_path)
    try:
        audio, in_rate = read_wav_channel(wav_path, args.channel)
    finally:
        if tmp:
            os.unlink(tmp)

    if in_rate != 44100:
        print(f"note: source rate is {in_rate} Hz (not 44100); "
              f"resampling from {in_rate} Hz")

    if args.end is not None:
        if args.end <= args.start:
            sys.exit(f"error: --end ({args.end}s) must be after "
                     f"--start ({args.start}s)")
        end_s = args.end
    elif args.duration is not None:
        end_s = args.start + args.duration
    else:
        end_s = len(audio) / in_rate
    if args.start >= len(audio) / in_rate:
        sys.exit(f"error: --start ({args.start}s) is past the end of the "
                 f"file ({len(audio) / in_rate:.1f}s)")
    i0 = int(args.start * in_rate)
    i1 = min(int(end_s * in_rate), len(audio))
    audio = audio[i0:i1]
    if len(audio) < 2:
        sys.exit("error: selected range contains no audio")

    if args.trim is not None:
        audio, cut0, cut1 = trim_silence_cap(audio, in_rate, args.trim,
                                             args.trim_floor)
        if cut0 or cut1:
            print(f"trim: cut {cut0:.2f}s leading / {cut1:.2f}s trailing "
                  f"silence (kept <= {args.trim:g}s each end)")

    if args.autoboost is not None:
        # The boost that puts exactly PCT%% of samples at the ceiling is
        # a percentile: q = the level (100-PCT)%% of samples stay under.
        q = float(np.percentile(np.abs(audio), 100.0 - args.autoboost))
        if q > 0.0:
            args.boost = 20.0 * np.log10(args.limit / (args.gain * q))
        else:
            args.boost = 0.0
        over = float(np.mean(np.abs(audio) * args.gain
                             * 10.0 ** (args.boost / 20.0)
                             > args.limit)) * 100.0
        print(f"autoboost: {args.boost:+.1f} dB ({over:.1f}% of samples "
              f"above the {args.limit:g} limiter ceiling)")

    idle = -1.0 if args.idle == "low" else 1.0
    gain = args.gain * 10.0 ** (args.boost / 20.0)
    audio = audio * gain
    if args.limit is not None:
        audio = limit_peaks(audio, in_rate, args.limit,
                            args.limit_lookahead, args.limit_release)
    audio = apply_envelope(audio, in_rate, args.fade, args.ramp, idle)

    name = sanitize_name(args.name or
                         os.path.splitext(os.path.basename(args.input))[0])

    if args.format == "adpcm":
        dur = len(audio) / in_rate
        from math import gcd
        g = gcd(args.pcm_rate, in_rate)
        pcm = sig.resample_poly(audio.astype(np.float64),
                                args.pcm_rate // g, in_rate // g)
        pcm16 = np.clip(np.round(pcm * 32767.0), -32768, 32767).astype(np.int16)
        print(f"{dur:.2f}s of audio -> {len(pcm16)} samples at "
              f"{args.pcm_rate} Hz")
        blob = bytearray(adpcm_encode(pcm16))
        blob[4:8] = int(args.pcm_rate).to_bytes(4, "little")
        blob = bytes(blob)
        # verify against the reference decoder
        dec = adpcm_decode_check(blob)
        err = np.abs(dec.astype(np.int32) - pcm16.astype(np.int32))
        snr = 10 * np.log10(np.mean(pcm16.astype(np.float64) ** 2) /
                            max(np.mean((dec.astype(np.float64) - pcm16) ** 2),
                                1e-9))
        print(f"adpcm verify: max sample error {err.max()}, "
              f"quantization SNR {snr:.1f} dB")
        out = args.output or (os.path.splitext(args.input)[0] + "_adpcm.c")
        if out.endswith(".bin"):
            with open(out, "wb") as f:      # raw blob for tqv.py send
                f.write(blob)
        else:
            write_adpcm_c(out, blob, name, args.input, args.channel,
                          args.pcm_rate, gain, len(pcm16))
        dsm_bytes = int(dur * DEFAULT_RATE / 8)
        print(f"wrote {out}: {len(blob)} bytes "
              f"({len(blob) / 1024:.0f} KiB) - {dsm_bytes // 1024} KiB as "
              f"DSM, {dsm_bytes / len(blob):.1f}x smaller")
        return

    dur = len(audio) / in_rate
    if args.format == "u32":
        est_bytes = int(dur * args.rate / 24) * 4  # 24 samples per uint32_t
    else:
        est_bytes = int(dur * args.rate / 8)
    print(f"{dur:.2f}s of audio (incl. soft-start/stop) -> "
          f"~{est_bytes / 1024:.0f} KiB of DSM data at {args.rate} Hz")

    bits = modulate_all(audio, in_rate, args.rate,
                        args.dither, np.random.default_rng(args.seed),
                        idle=idle)

    out = args.output or (os.path.splitext(args.input)[0]
                          + f"_{args.channel}.c")
    size = write_c_array(out, bits, name, args.input, args.channel,
                         args.rate, gain, args.format)
    ctype, count = (("uint32_t", size // 4) if args.format == "u32"
                    else ("uint8_t", size))
    print(f"wrote {out}: const {ctype} {name}[{count}] "
          f"({size / 1024:.0f} KiB, {len(bits)} bits)")


if __name__ == "__main__":
    main()
