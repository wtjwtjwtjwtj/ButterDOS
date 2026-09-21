#!/usr/bin/env python3
"""
resample_music.py - Preprocess Undertale music/audio for the DOS port.

Converts OGG or WAV files to 8000 Hz mono 16-bit WAV, in place.
The file keeps its original name (.ogg), but its contents become WAV.
The DOS port detects format by content, so this works transparently.

Usage:
    python resample_music.py                 # every .ogg in this dir
    python resample_music.py mus_story.ogg
    python resample_music.py *.ogg *.wav

Requires:
    pip install soundfile
"""

import glob
import os
import sys
import tempfile

try:
    import numpy as np
    import soundfile as sf
except ImportError:
    print("Missing dependency. Run:\n    pip install soundfile", file=sys.stderr)
    sys.exit(1)

TARGET_RATE = 8000
IMMUNE_NAME = 'resample_music.py'  # never touch ourselves


def to_mono(data):
    if data.ndim == 1:
        return data
    if data.shape[1] == 1:
        return data[:, 0]
    return data.mean(axis=1)


def resample_linear(data, src_rate, dst_rate):
    if src_rate == dst_rate:
        return data
    ratio = src_rate / dst_rate
    n_out = max(1, int(round(len(data) / ratio)))
    src_idx = np.arange(n_out, dtype=np.float64) * ratio
    s0 = np.floor(src_idx).astype(np.int64)
    s1 = np.minimum(s0 + 1, len(data) - 1)
    frac = (src_idx - s0).astype(np.float32)
    return data[s0] * (1.0 - frac) + data[s1] * frac


def process_file(path):
    if os.path.basename(path) == IMMUNE_NAME:
        return False

    ext = os.path.splitext(path)[1].lower()
    if ext not in ('.ogg', '.wav'):
        print(f"  skip  {path}  (not .ogg/.wav)")
        return False

    try:
        data, src_rate = sf.read(path, dtype='float32', always_2d=False)
    except Exception as e:
        # A file that's already been converted to raw WAV but has an .ogg
        # name will still open fine (soundfile sniffs content). If it truly
        # fails, skip.
        print(f"  fail  {os.path.basename(path)}  ({e})")
        return False

    if src_rate == TARGET_RATE:
        # Already at target rate; check if it's already mono/16-bit.
        if data.ndim == 1 or data.shape[1] == 1:
            print(f"  skip  {os.path.basename(path)}  (already 8000 Hz mono)")
            return False

    mono = to_mono(data)
    resampled = resample_linear(mono, src_rate, TARGET_RATE)

    # Write to a temp file in the same directory, then atomically replace.
    d = os.path.dirname(path) or '.'
    fd, tmp_path = tempfile.mkstemp(prefix='.resample-', suffix='.tmp', dir=d)
    os.close(fd)
    try:
        sf.write(tmp_path, resampled, TARGET_RATE,
                 subtype='PCM_16', format='WAV')
        os.replace(tmp_path, path)
    except Exception as e:
        try: os.unlink(tmp_path)
        except OSError: pass
        print(f"  fail  {os.path.basename(path)}  (write: {e})")
        return False

    print(f"  ok    {os.path.basename(path)}  "
          f"{src_rate} Hz -> {TARGET_RATE} Hz  "
          f"({len(mono)} -> {len(resampled)} samples)")
    return True


def main():
    files = sys.argv[1:]
    if not files:
        files = sorted(glob.glob('*.ogg'))
        if not files:
            print('no .ogg files in current directory and none specified',
                  file=sys.stderr)
            return 1
        print(f'(no files given, using {len(files)} .ogg in current dir)')

    print(f'Converting to {TARGET_RATE} Hz mono 16-bit WAV (in place)')
    ok = fail = 0
    for path in files:
        if process_file(path):
            ok += 1
        else:
            fail += 1

    print(f'\n{ok} converted, {fail} skipped/failed')
    return 0 if fail == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
