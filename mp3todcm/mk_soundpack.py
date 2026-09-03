#!/usr/bin/env python3
"""Build a .spk sound pack from a directory of audio files.

Every file the system decoder understands (.mp3, .m4a, .wav, .aiff, ...)
is decoded, peak-normalized, resampled to the PCM chroma's sample rate
and IMA-ADPCM encoded, then packed into one indexed file the TinyQV
firmware can browse and play entry by entry.

    ./mk_soundpack.py SoundPack1 -o tqvfs/sounds/pack1.spk

Entry names come from the file name: split on '-' / '_', drop trailing
segments that are pure numbers (the stock-library id and take number),
drop the extension, and join what is left with spaces --

    silly-trumpet-11-187806.mp3   -> "silly trumpet"
    mad-goblin-laughter-1-543569  -> "mad goblin laughter"
    eh-13143.mp3                  -> "eh"

File layout (all little-endian; offsets are from the start of file):

    0   char     magic[4]      "SPK1"
    4   uint32   count         number of entries
    8   uint32   rate          sample rate of every entry, Hz
    12  uint32   block         ADPCM block size in samples
    16  entry[count], 52 bytes each:
            char   name[40]    NUL padded
            uint32 offset      start of this entry's ADPCM blob
            uint32 length      blob length in bytes
            uint32 samples     sample count (duration = samples / rate)
    ... the blobs, each 4-byte aligned.  A blob is exactly what
    mp3_to_dsm.py --format adpcm writes: {uint32 samples, uint32 rate,
    uint32 block} followed by the ADPCM blocks, so the firmware plays one
    with the same code path as a downloaded song.
"""

import argparse
import os
import struct
import sys
import tempfile

import numpy as np
import scipy.signal as sig

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mp3_to_dsm import (adpcm_decode_check, adpcm_encode, decode_to_wav,
                        read_wav_channel)

MAGIC = b"SPK1"
NAME_LEN = 40
ENTRY_LEN = NAME_LEN + 12
HEADER_LEN = 16
BLOCK = 1024
DEFAULT_RATE = 31250          # 64MHz / 2048, the PCM chroma sample clock

AUDIO_EXT = (".mp3", ".m4a", ".wav", ".aif", ".aiff", ".mp4", ".caf",
             ".flac", ".ogg", ".wma", ".aac")


def entry_name(filename: str) -> str:
    """Title for one source file (see the module docstring)."""
    stem = os.path.splitext(os.path.basename(filename))[0]
    parts = [p for p in stem.replace("_", "-").split("-") if p]
    while parts and parts[-1].isdigit():
        parts.pop()
    if not parts:                          # a name that was all numbers
        parts = [os.path.splitext(os.path.basename(filename))[0]]
    name = " ".join(parts)
    if len(name.encode()) > NAME_LEN - 1:
        name = name.encode()[:NAME_LEN - 1].decode("utf-8", "ignore")
    return name


def trim_silence(audio: np.ndarray, rate: int, floor_db: float,
                 pad_ms: float) -> np.ndarray:
    """Drop leading / trailing near-silence, keeping `pad_ms` around it.

    Stock sound-effect files often carry a fraction of a second of room
    tone at each end; in a pack that is dead air before every hit (and
    ADPCM bytes nobody hears).
    """
    if floor_db is None:
        return audio
    thresh = np.max(np.abs(audio)) * (10.0 ** (floor_db / 20.0))
    loud = np.nonzero(np.abs(audio) > thresh)[0]
    if len(loud) == 0:
        return audio
    pad = int(pad_ms * rate / 1000)
    return audio[max(0, loud[0] - pad):min(len(audio), loud[-1] + pad + 1)]


def fade_edges(audio: np.ndarray, rate: int, ms: float) -> np.ndarray:
    """Short raised-cosine in/out so an entry cannot click on/off."""
    n = min(int(ms * rate / 1000), len(audio) // 2)
    if n <= 0:
        return audio
    w = 0.5 - 0.5 * np.cos(np.linspace(0.0, np.pi, n))
    audio = audio.copy()
    audio[:n] *= w
    audio[-n:] *= w[::-1]
    return audio


def encode_one(path: str, args) -> tuple[bytes, int, float]:
    """Return (adpcm blob, sample count, peak gain applied)."""
    tmp = None
    if path.lower().endswith(".wav"):
        wav_path = path
    else:
        fd, tmp = tempfile.mkstemp(suffix=".wav")
        os.close(fd)
        decode_to_wav(path, tmp)
        wav_path = tmp
    try:
        audio, in_rate = read_wav_channel(wav_path, args.channel)
    finally:
        if tmp:
            os.unlink(tmp)

    audio = audio.astype(np.float64)
    audio = trim_silence(audio, in_rate, args.trim, args.trim_pad)

    peak = float(np.max(np.abs(audio))) if len(audio) else 0.0
    gain = args.gain
    if args.normalize and peak > 0.0:
        gain = min(args.ceiling / peak, args.max_gain) * args.gain
    audio = np.clip(audio * gain, -1.0, 1.0)
    audio = fade_edges(audio, in_rate, args.fade)

    from math import gcd
    g = gcd(args.rate, in_rate)
    pcm = sig.resample_poly(audio, args.rate // g, in_rate // g)
    pcm16 = np.clip(np.round(pcm * 32767.0), -32768, 32767).astype(np.int16)

    # pad the tail to a whole block: blocks are independently decodable
    # and the player always consumes whole ones
    if len(pcm16) % BLOCK:
        pcm16 = np.concatenate(
            [pcm16, np.zeros(BLOCK - len(pcm16) % BLOCK, dtype=np.int16)])

    blob = bytearray(adpcm_encode(pcm16))
    blob[4:8] = int(args.rate).to_bytes(4, "little")
    return bytes(blob), len(pcm16), gain


def main() -> None:
    p = argparse.ArgumentParser(
        description="Pack a directory of sounds into a .spk sound pack "
                    "for the TinyQV PRISM PCM chroma.")
    p.add_argument("directory", help="directory of source audio files")
    p.add_argument("-o", "--output", help="output .spk "
                   "(default: <directory>.spk)")
    p.add_argument("channel", nargs="?", choices=["left", "right"],
                   default="left", help="which channel to take (default left)")
    p.add_argument("-r", "--rate", type=int, default=DEFAULT_RATE,
                   help=f"sample rate in Hz; the firmware derives the PCM "
                        f"chroma preload from it (default {DEFAULT_RATE})")
    p.add_argument("-g", "--gain", type=float, default=1.0,
                   help="extra scale applied after normalization (default 1)")
    p.add_argument("--ceiling", type=float, default=0.95,
                   help="peak each entry is normalized to (default 0.95)")
    p.add_argument("--max-gain", type=float, default=8.0,
                   help="most a quiet entry may be lifted (default 8)")
    p.add_argument("--no-normalize", dest="normalize", action="store_false",
                   help="keep the source levels instead of peak matching")
    p.add_argument("--trim", type=lambda v: None if v == "none" else float(v),
                   default=-45.0, metavar="DB",
                   help="trim leading/trailing audio below this level "
                        "relative to the entry's peak (default -45; "
                        "'--trim none' disables)")
    p.add_argument("--trim-pad", type=float, default=15.0, metavar="MS",
                   help="audio kept either side of the trim (default 15)")
    p.add_argument("--fade", type=float, default=5.0, metavar="MS",
                   help="raised-cosine fade in/out (default 5)")
    p.add_argument("--max-seconds", type=float, default=0.0, metavar="S",
                   help="skip entries longer than this (0 = no limit)")
    p.add_argument("--verify", action="store_true",
                   help="decode every entry back and report its SNR")
    args = p.parse_args()

    if args.trim is not None and args.trim >= 0:
        sys.exit("error: --trim must be negative (dB below the peak)")
    if not os.path.isdir(args.directory):
        sys.exit(f"error: not a directory: {args.directory}")

    files = sorted(f for f in os.listdir(args.directory)
                   if f.lower().endswith(AUDIO_EXT) and
                   not f.startswith("."))
    if not files:
        sys.exit(f"error: no audio files in {args.directory}")

    out = args.output or (os.path.normpath(args.directory) + ".spk")
    names, blobs, counts = [], [], []
    skipped = []

    for i, f in enumerate(files):
        src = os.path.join(args.directory, f)
        name = entry_name(f)
        print(f"[{i + 1:3d}/{len(files)}] {name:<40s}", end="", flush=True)
        try:
            blob, samples, gain = encode_one(src, args)
        except SystemExit as e:                # decode_to_wav failures
            print(f"  SKIPPED ({e})")
            skipped.append(f)
            continue
        secs = samples / args.rate
        if args.max_seconds and secs > args.max_seconds:
            print(f"  skipped ({secs:.1f}s > {args.max_seconds:.1f}s)")
            skipped.append(f)
            continue
        note = ""
        if args.verify:
            # the reference decoder is the firmware's decode, byte for byte
            dec = adpcm_decode_check(blob)
            note = f"  decoded {len(dec)} samples"
        print(f"  {secs:5.2f}s  {len(blob) / 1024:6.1f} KiB  "
              f"gain {gain:4.2f}{note}")
        names.append(name)
        blobs.append(blob)
        counts.append(samples)

    if not blobs:
        sys.exit("error: nothing encoded")

    # Index first, then the blobs, each 4-byte aligned
    offset = HEADER_LEN + ENTRY_LEN * len(blobs)
    index = bytearray()
    payload = bytearray()
    for name, blob, samples in zip(names, blobs, counts):
        pad = (-len(blob)) % 4
        index += struct.pack(f"<{NAME_LEN}sIII", name.encode()[:NAME_LEN - 1],
                             offset, len(blob), samples)
        payload += blob + b"\0" * pad
        offset += len(blob) + pad

    with open(out, "wb") as fd:
        fd.write(struct.pack("<4sIII", MAGIC, len(blobs), args.rate, BLOCK))
        fd.write(index)
        fd.write(payload)

    total = sum(counts) / args.rate
    size = HEADER_LEN + len(index) + len(payload)
    print(f"\nwrote {out}: {len(blobs)} sounds, {total:.1f}s of audio, "
          f"{size / 1024:.0f} KiB at {args.rate} Hz")
    if skipped:
        print(f"skipped {len(skipped)}: {', '.join(skipped)}")


if __name__ == "__main__":
    main()
