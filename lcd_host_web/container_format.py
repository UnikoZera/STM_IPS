"""Pure helpers for the MCU RAW5 and MJPG container formats."""

from __future__ import annotations

import io
import struct

RAW_MAGIC = b"RAW5"
HEADER_SIZE = 14


def write_header(stream, magic: bytes, frame_count: int, width: int, height: int) -> None:
    stream.write(magic)
    stream.write(struct.pack("<H", frame_count & 0xFFFF))
    stream.write(struct.pack("<H", width & 0xFFFF))
    stream.write(struct.pack("<H", height & 0xFFFF))
    stream.write(b"\x00" * 4)


def pack_raw(frames: list[bytes], width: int, height: int) -> bytes:
    out = io.BytesIO()
    write_header(out, RAW_MAGIC, len(frames), width, height)
    for frame in frames:
        out.write(frame)
    return out.getvalue()


def pack_mjpeg(frames: list[bytes], width: int, height: int) -> bytes:
    out = io.BytesIO()
    write_header(out, b"MJPG", len(frames), width, height)
    for frame in frames:
        out.write(struct.pack("<I", len(frame)))
        out.write(frame)
    return out.getvalue()
