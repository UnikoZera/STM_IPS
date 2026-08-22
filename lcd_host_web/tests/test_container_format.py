import struct

from lcd_host_web.container_format import pack_mjpeg, pack_raw


def test_raw5_header_and_payload():
    payload = pack_raw([b"\x01\x02"], 1, 1)
    assert payload[:4] == b"RAW5"
    assert struct.unpack_from("<HHH", payload, 4) == (1, 1, 1)
    assert payload[14:] == b"\x01\x02"


def test_mjpeg_has_length_prefixed_frames():
    payload = pack_mjpeg([b"jpeg"], 160, 80)
    assert payload[:4] == b"MJPG"
    assert struct.unpack_from("<I", payload, 14)[0] == 4
    assert payload[18:] == b"jpeg"
