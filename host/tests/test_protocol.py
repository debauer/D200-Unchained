from __future__ import annotations

import struct

import pytest

from d200_deck.constants import PROTOCOL_VERSION
from d200_deck.constants import MAX_PAYLOAD
from d200_deck.device.protocol import (
    Color,
    Msg,
    PixelFormat,
    decode_header,
    encode_frame,
    fill_key_payload,
    key_image_payload,
    parse_info,
    parse_key_event,
    parse_sysinfo,
    rgb565,
)
from d200_deck.errors import InvalidColorError, ProtocolError

from .conftest import INFO_PAYLOAD


def test_color_from_hex() -> None:
    assert Color.from_hex("#ff8800") == Color(255, 136, 0)
    assert Color.from_hex("00FF00") == Color(0, 255, 0)
    with pytest.raises(InvalidColorError):
        Color.from_hex("red")
    with pytest.raises(InvalidColorError):
        Color(256, 0, 0)


def test_rgb565_matches_device_decoding() -> None:
    assert rgb565([Color(255, 0, 0), Color(0, 255, 0), Color(255, 255, 255)]) == bytes(
        [0x00, 0xF8, 0xE0, 0x07, 0xFF, 0xFF]
    )


def test_frame_roundtrip_and_limit() -> None:
    frame = encode_frame(Msg.CLEAR, b"\x01\x02\x03")
    assert decode_header(frame[:5]) == (Msg.CLEAR, 3)
    with pytest.raises(ProtocolError):
        decode_header(struct.pack("<BI", Msg.OK, MAX_PAYLOAD + 1))
    with pytest.raises(ProtocolError):
        encode_frame(Msg.CLEAR, bytes(MAX_PAYLOAD + 1))


def test_parse_info() -> None:
    info = parse_info(INFO_PAYLOAD)
    assert (info.version, info.width, info.height) == (PROTOCOL_VERSION, 1280, 720)
    assert info.key(1).width == 456
    assert info.key(0).code == 29
    with pytest.raises(KeyError):
        info.key(99)
    with pytest.raises(ProtocolError):
        parse_info(INFO_PAYLOAD[:-1])


def test_key_event_and_payloads() -> None:
    assert parse_key_event(bytes([30, 0, 1])).pressed is True
    with pytest.raises(ProtocolError):
        parse_key_event(b"\x00")
    assert fill_key_payload(9, Color(1, 2, 3)) == bytes([9, 0, 1, 2, 3])
    payload = key_image_payload(7, 2, 1, b"\x00" * 4, PixelFormat.RGB565)
    assert payload[:7] == bytes([7, 0, 0, 2, 0, 1, 0])
    with pytest.raises(ProtocolError, match="needs 6 bytes"):
        key_image_payload(7, 2, 1, b"\x00" * 4, PixelFormat.RGB888)


def test_parse_sysinfo() -> None:
    info = parse_sysinfo(b"cpu_cores=4\nmem_total_kb=118520\nserial=D200TEST\n")
    assert info == {"cpu_cores": "4", "mem_total_kb": "118520", "serial": "D200TEST"}
    assert parse_sysinfo(b"") == {}
    with pytest.raises(ProtocolError, match="malformed"):
        parse_sysinfo(b"no equals sign")
    with pytest.raises(ProtocolError, match="not UTF-8"):
        parse_sysinfo(b"\xff\xfe")
