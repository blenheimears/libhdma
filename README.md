# libhdma
Libhdma is a lossless audio codec for Blu-ray, compatible with most Blu-ray players. It also produces a lossy core for players that don't support lossless audio. It supports up to 8 (7.1) channels, and will downmix to 5.1 for the lossy core.

The lossy core is an essential part of the encoded audio; the lossless coder encodes the difference between the lossy audio and the original, giving greater compression efficiency than other codecs with a lossy core.

The encoder for the lossy core is based on code from ffmpeg.

This project only implements an encoder; vanilla ffmpeg can already decode both the lossy core and the lossless audio.
