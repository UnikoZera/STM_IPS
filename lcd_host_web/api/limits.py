"""Resource limits shared by HTTP validation and conversion workers."""

from dataclasses import dataclass

MAX_UPLOAD_BYTES = 256 * 1024 * 1024
MAX_OUTPUT_BYTES = 512 * 1024 * 1024
MAX_WIDTH = 1024
MAX_HEIGHT = 1024


@dataclass(frozen=True)
class ConversionLimits:
    upload_bytes: int = MAX_UPLOAD_BYTES
    output_bytes: int = MAX_OUTPUT_BYTES
    width: int = MAX_WIDTH
    height: int = MAX_HEIGHT
