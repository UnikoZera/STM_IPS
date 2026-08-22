import json
import struct
from pathlib import Path

CONTRACT = json.loads((Path(__file__).parents[1] / "protocol" / "contract.json").read_text())


def crc16_usb(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def encode_host(command: int, data: bytes = b"", total_size: int = 0) -> bytes:
    crc = crc16_usb(data) if data else CONTRACT["host_frame"]["empty_payload_crc"]
    payload = data + struct.pack("<H", crc)
    return bytes(CONTRACT["host_frame"]["header"]) + bytes([command]) + struct.pack(
        "<IH", total_size, len(payload)
    ) + payload


def test_contract_has_firmware_commands():
    assert CONTRACT["version"] == 1
    assert CONTRACT["host_frame"]["max_data_bytes"] == 1024
    assert CONTRACT["commands"]["download_large"] == 0x11
    assert CONTRACT["commands"]["continue"] == 0xA1


def test_empty_host_frame_vector():
    assert encode_host(CONTRACT["commands"]["query_file_list"]) == bytes.fromhex(
        "BB44200000000002000000"
    )


def test_data_host_frame_vector_and_crc_scope():
    frame = encode_host(0x45, b"abc", total_size=3)
    assert frame[:9] == bytes.fromhex("BB4445030000000500")
    assert frame[9:12] == b"abc"
    assert frame[-2:] == struct.pack("<H", crc16_usb(b"abc"))


def test_limits_are_explicit():
    assert CONTRACT["host_frame"]["max_data_bytes"] == 1024
    assert CONTRACT["device_frame"]["max_payload_bytes"] == 32768
