#!/usr/bin/env python3
"""Decodes an IEC 61937 dump written by `win51svc --dump` and prints which channels carry signal.

usage: analyze_dump.py <dump.spdif> [window_seconds]
Needs ffmpeg on PATH. Prints one line per window: the RMS level of each channel in dBFS.
"""
import array
import math
import subprocess
import sys

CHANNELS = ["FL", "FR", "FC", "LFE", "SL", "SR"]


def main():
    path = sys.argv[1]
    window = float(sys.argv[2]) if len(sys.argv) > 2 else 0.3

    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-f", "spdif", "-i", path, "-show_entries",
         "stream=codec_name,sample_rate,channels,channel_layout,bit_rate", "-of", "default=nw=1"],
        capture_output=True, text=True)
    print(probe.stdout.strip() or probe.stderr.strip())

    # Decode to raw 6-channel float in libavcodec's native 5.1 order (FL FR FC LFE SL SR).
    pcm = subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "spdif", "-i", path, "-f", "f32le", "-ac", "6", "-ar", "48000", "-"],
        capture_output=True, check=True).stdout
    frames = len(pcm) // (4 * 6)
    per_window = int(48000 * window)
    print(f"{frames / 48000:.2f} s decoded\n")
    print("  time  " + "".join(f"{c:>8}" for c in CHANNELS))

    values = array.array("f", pcm[:frames * 24])  # little-endian host assumed
    for start in range(0, frames - per_window + 1, per_window):
        sums = [0.0] * 6
        for i in range(start, start + per_window):
            base = i * 6
            for c in range(6):
                v = values[base + c]
                sums[c] += v * v
        levels = []
        for c in range(6):
            rms = math.sqrt(sums[c] / per_window)
            levels.append(20 * math.log10(rms) if rms > 1e-6 else -120.0)
        loudest = max(range(6), key=lambda c: levels[c])
        marks = "".join(f"{lv:8.1f}" for lv in levels)
        label = CHANNELS[loudest] if levels[loudest] > -60 else "-"
        print(f"{start / 48000:6.2f}{marks}   {label}")


if __name__ == "__main__":
    main()
