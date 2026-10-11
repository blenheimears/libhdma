#!/usr/bin/env python3
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
"""Exercise and independently inspect the decoder-compatibility controls."""
from __future__ import annotations
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SYNC = b"\x41\xa2\x95\x47"

class Bits:
    def __init__(self, data: bytes, bit: int = 0):
        self.data = data
        self.bit = bit
    def get(self, width: int) -> int:
        if width < 0 or self.bit + width > len(self.data) * 8:
            raise ValueError("truncated bitstream")
        value = 0
        for _ in range(width):
            value = (value << 1) | ((self.data[self.bit >> 3] >> (7 - (self.bit & 7))) & 1)
            self.bit += 1
        return value


def parse_xll(data: bytes) -> dict[str, int]:
    frames = reuse = undecor = 0
    max_fixed = max_segments = 0
    start = 0
    while True:
        off = data.find(SYNC, start)
        if off < 0:
            break
        start = off + 4
        r = Bits(data, off * 8)
        if r.get(32) != 0x41A29547 or r.get(4) != 0:
            continue
        common_bytes = r.get(8) + 1
        frame_bits = r.get(5) + 1
        frame_bytes = r.get(frame_bits) + 1
        if off + frame_bytes > len(data):
            raise ValueError(f"XLL frame at {off} exceeds file")
        if r.get(4) != 0:
            raise ValueError("test supports one channel set")
        grid_log = r.get(4)
        segment_log = r.get(4)
        navi_bits = r.get(5) + 1
        r.get(2)  # band CRC mode
        scalable = r.get(1)
        mask_bits = r.get(5) + 1
        if scalable:
            r.get(4)
        segments = 1 << grid_log
        if grid_log > 4 or segment_log + grid_log not in (9, 10):
            raise ValueError("invalid segment grid")
        max_segments = max(max_segments, segments)

        sub = Bits(data, (off + common_bytes) * 8)
        sub_bytes = sub.get(10) + 1
        channels = sub.get(4) + 1
        sub.get(channels)  # residual/full mask
        depth = sub.get(5) + 1
        sub.get(5)  # storage depth
        sub.get(4)  # sample rate
        sub.get(2)  # rate modifier
        sub.get(2)  # replacement set
        primary = sub.get(1)
        dmix = sub.get(1)
        if dmix:
            sub.get(1)
            if primary:
                sub.get(3)
            else:
                raise ValueError("unexpected non-primary downmix set in 5.1 test")
        sub.get(1)  # hierarchical flag
        if not sub.get(1):
            raise ValueError("speaker mask absent")
        sub.get(mask_bits)
        decor = sub.get(1)
        if not decor:
            undecor += 1
        if decor:
            order_bits = 1 if channels <= 2 else 2 if channels <= 4 else 3
            for _ in range(channels):
                sub.get(order_bits)
            for _ in range(channels // 2):
                if sub.get(1):
                    sub.get(7)
        adaptive = [sub.get(4) for _ in range(channels)]
        for order in adaptive:
            if order == 0:
                max_fixed = max(max_fixed, sub.get(2))
        for order in adaptive:
            for _ in range(order):
                sub.get(8)

        nav = Bits(data, (off + common_bytes + sub_bytes) * 8)
        sizes = [nav.get(navi_bits) + 1 for _ in range(segments)]
        segpos = off + common_bytes + sub_bytes + ((segments * navi_bits + 7) // 8) + 2
        allocbits = 4 if depth == 16 else 5
        for seg, size in enumerate(sizes):
            if segpos + size > off + frame_bytes:
                raise ValueError("segment exceeds XLL frame")
            sr = Bits(data, segpos * 8)
            is_reuse = bool(seg and sr.get(1))
            if is_reuse:
                reuse += 1
            else:
                common_alloc = sr.get(1)
                coded = 1 if common_alloc else channels
                rice = []
                for _ in range(coded):
                    value = sr.get(1)
                    rice.append(value)
                    if value and not common_alloc and sr.get(1):
                        sr.get(allocbits)
                for _ in range(coded):
                    if seg == 0:
                        sr.get(allocbits)
                    sr.get(allocbits)
            segpos += size
        frames += 1
    if not frames:
        raise ValueError("no XLL frames found")
    return {"frames": frames, "reuse": reuse, "undecor": undecor,
            "max_fixed": max_fixed, "max_segments": max_segments}


def make_pcm(path: Path, frames: int = 16) -> bytes:
    out = bytearray()
    samples = frames * 512
    history = [0] * 6
    for n in range(samples):
        # Correlated but non-periodic six-channel signal. The LFE channel has a
        # smooth third-order trend, which exercises fixed predictor selection.
        base = ((n * 7919 + (n * n * 17) + ((n >> 3) * 104729)) & 0x7FFFFF) - 0x400000
        values = [
            base,
            base // 2 + ((n * 31337) & 0x1FFFF) - 0x10000,
            -base // 3,
            (history[3] * 3 + ((n * 97) & 0x3FFF) - 0x2000) // 4,
            base - ((n * 1237) & 0x3FFFF),
            -base + ((n * 3469) & 0x1FFFF),
        ]
        for c, v in enumerate(values):
            v = max(-0x800000, min(0x7FFFFF, int(v)))
            history[c] = v
            out.extend((v & 0xFFFFFF).to_bytes(3, "little"))
    path.write_bytes(out)
    return bytes(out)


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, **kwargs)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: xll_decoder_controls_runtime.py HDMAENC FFMPEG", file=sys.stderr)
        return 2
    encoder, ffmpeg = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="libhdma-controls-") as td:
        tmp = Path(td)
        original = make_pcm(tmp / "input.pcm")
        variants = {
            "default": [],
            "decor-auto": ["--xll-decorrelation", "auto"],
            "decor-off": ["--xll-decorrelation", "off"],
            "default-1024": ["--xll-frame-samples", "1024"],
            "on-1024": ["--xll-frame-samples", "1024", "--xll-decorrelation", "on"],
            "fixed0": ["--max-fixed-prediction-order", "0"],
            "noreuse": ["--xll-parameter-reuse", "off"],
            "decor": ["--xll-decorrelation", "on"],
            "max8": ["--xll-max-segments", "8"],
            "combined": ["--xll-hardware-test"],
            "explicit-all": ["--max-fixed-prediction-order", "0",
                             "--xll-parameter-reuse", "off",
                             "--xll-decorrelation", "on",
                             "--xll-max-segments", "8"],
        }
        stats: dict[str, dict[str, int]] = {}
        streams: dict[str, bytes] = {}
        for name, extra in variants.items():
            stream = tmp / f"{name}.dts"
            decoded = tmp / f"{name}.pcm"
            cmd = [encoder, str(tmp / "input.pcm"), "--raw-pcm", "--sample-rate", "48000",
                   "--channels", "6", "--bit-depth", "24", "--channel-layout", "5.1",
                   "--lossless", "--xll-reference", "full", "--no-core-lookahead",
                   "--threads", "1", "--no-progress", "--overwrite", "-o", str(stream)] + extra
            run(cmd)
            streams[name] = stream.read_bytes()
            stats[name] = parse_xll(streams[name])
            run([ffmpeg, "-v", "error", "-y", "-i", str(stream), "-map", "0:a:0",
                 "-f", "s24le", "-acodec", "pcm_s24le", str(decoded)])
            got = decoded.read_bytes()
            if got != original:
                raise RuntimeError(f"{name}: decoded PCM mismatch: {len(got)} vs {len(original)}")

        # A missing option must behave identically to explicit ON, across both
        # compile-time XLL frame instantiations. OFF and AUTO remain accessible.
        if stats["default"]["undecor"] or stats["default-1024"]["undecor"]:
            raise RuntimeError("default XLL header did not always signal decorrelation")
        if streams["default"] != streams["decor"]:
            raise RuntimeError("default XLL output differs from --xll-decorrelation on")
        if streams["default-1024"] != streams["on-1024"]:
            raise RuntimeError("1024-sample default XLL output differs from explicit on")
        if stats["decor-off"]["undecor"] != stats["decor-off"]["frames"]:
            raise RuntimeError("explicit --xll-decorrelation off no longer works")
        if stats["default"]["max_fixed"] != 0 or stats["fixed0"]["max_fixed"] != 0:
            raise RuntimeError(f"reference fixed-order policy failed: default={stats['default']} explicit={stats['fixed0']}")
        for invalid in ("1", "2", "3"):
            bad = subprocess.run([encoder, str(tmp / "input.pcm"), "--raw-pcm",
                                  "--sample-rate", "48000", "--channels", "6",
                                  "--bit-depth", "24", "--channel-layout", "5.1",
                                  "--max-fixed-prediction-order", invalid,
                                  "--no-progress", "--overwrite", "-o", str(tmp / f"bad-{invalid}.dts")],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if bad.returncode == 0:
                raise RuntimeError(f"unsafe fixed order {invalid} was accepted")
        if stats["noreuse"]["reuse"]:
            raise RuntimeError(f"reuse disable failed: {stats['noreuse']}")
        if stats["decor"]["undecor"]:
            raise RuntimeError(f"forced decorrelation failed: {stats['decor']}")
        if stats["max8"]["max_segments"] > 8:
            raise RuntimeError(f"segment cap failed: {stats['max8']}")
        combined = stats["combined"]
        # 0.0.43 turns the hardware diagnostic into a deliberately minimal
        # baseline: no prediction/reuse/decorrelation and the minimum legal
        # 48-kHz segment count. Raw-linear forcing is an internal worker
        # control, so it intentionally has no equivalent four-option CLI form.
        if (combined["max_fixed"] != 0 or combined["reuse"] or
            combined["undecor"] != combined["frames"] or
            combined["max_segments"] > 2):
            raise RuntimeError(f"combined profile failed: {combined}")
        print("PASS XLL decoder controls:")
        for name in variants:
            print(name, stats[name])
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
