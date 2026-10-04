#!/usr/bin/env python3
"""Repair Journey PC's lost animation-sound selector registers.

WHAT THIS FIXES

Journey's stock DudeAnimation.lua supplies two values whenever an animation
marker requests one of the shared movement sounds:

  register 0: surface selector
              0 Sand, 1 Stone, 2 Snow, 3 IcyStone, 4 DrySnow, 5 Metal
  register 1: Mountain cold-stage selector, values 0 through 5

The original PC executable stores both bytes in its active SoundBarn slot and
marks them present, but drops them while creating the live SCREAM/SBlk voice.
The stock sound-bank graph consequently reads zero for both registers: affected
movement sounds always choose Sand, and Mountain frozen-step sweeteners for
stages 1 through 5 never start.

This patch carries the already-authored two-byte selector through an unused
request-tail word and copies it into live voice registers 0 and 1 before the
bank graph begins. It changes no sound media, BNK graph, or Lua behavior.

The 17 affected shared roots are:

  ClimbUp
  FootstepDeepLeft, FootstepDeepRight
  FootstepRunLeft, FootstepRunRight
  FootstepWalkLeft, FootstepWalkRight
  Jump-InPlace, Jump-Moving, Jump-Surfing
  JumpLand-InPlace, JumpLand-Moving, JumpLand-Surfing
  SitDown, StandUp, TripAndRoll, TurnSharp

The six footstep roots and three landing roots also consume register 1 and can
start Mountain's five progressive frozen-step sweetener families.

THIS DOES NOT FIX

This is not a score/music, reverb, attenuation, Ducker, emitter-lifecycle,
scarf-recharge, frozen-cloth, Cave-drone, or general media-quality patch. Normal use keeps the stock SharSfxShared-SFX.bnk and DudeAnimation.lua.

PATCH SAFETY

The input is accepted from local machine-code structure, not a whole-file
hash. Harmless PE metadata or unrelated-byte differences are therefore allowed.
An incompatible code build is refused unless both authored patch sites are
found exactly once and their original call target and executable code cave pass
local validation.

The patcher writes a separate output file and never overwrites its input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path


REQUEST_EXPECTED = bytes.fromhex(
    "48 c7 44 24 28 00 00 00 00 66 c7 44 24 30 00 00"
)
REQUEST_PREFIX = bytes.fromhex(
    "10 05 b6 41 3d 00 48 8d 53 21 4c 8d 44 24 20 f3 "
    "0f 11 44 24 20 49 8b cb"
)
REQUEST_SUFFIX = bytes.fromhex(
    "40 88 7c 24 24 e8 0e fb ff ff 89 83 a0 01 00 00 "
    "85 c0 75 55 c7 83 a0 01"
)

VOICE_EXPECTED = bytes.fromhex("e8 6c 90 00 00")
VOICE_PREFIX = bytes.fromhex(
    "8b c4 48 89 44 24 30 48 8b cd 48 89 5c 24 38 48 "
    "c7 44 24 20 00 00 00 00"
)
VOICE_SUFFIX = bytes.fromhex(
    "84 c0 0f 84 e4 00 00 00 f3 41 0f 10 16 0f 57 db "
    "33 d2 48 8b cd e8 32 8d"
)

GRAPH_START_PROLOGUE = bytes.fromhex(
    "48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 "
    "41 54 41 55 41 56 41 57 48 81 ec a0 00 00 00 49"
)

REQUEST_STUB_SIZE = 22
VOICE_STUB_SIZE = 17
VOICE_STUB_DELTA = 0x20
MIN_CAVE_SIZE = VOICE_STUB_DELTA + VOICE_STUB_SIZE


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def rel32(source_rva: int, target_rva: int) -> bytes:
    displacement = target_rva - (source_rva + 5)
    if not -(1 << 31) <= displacement < (1 << 31):
        raise RuntimeError("relative branch is out of signed 32-bit range")
    return struct.pack("<i", displacement)


@dataclass(frozen=True)
class Section:
    name: str
    header_offset: int
    virtual_size: int
    virtual_address: int
    raw_size: int
    raw_offset: int
    characteristics: int

    def file_to_rva(self, file_offset: int) -> int:
        if not self.raw_offset <= file_offset < self.raw_offset + self.raw_size:
            raise RuntimeError(f"file offset 0x{file_offset:X} is outside {self.name}")
        return self.virtual_address + file_offset - self.raw_offset

    def rva_to_file(self, rva: int) -> int:
        delta = rva - self.virtual_address
        if not 0 <= delta < self.raw_size:
            raise RuntimeError(f"RVA 0x{rva:X} is outside {self.name}")
        return self.raw_offset + delta


@dataclass(frozen=True)
class PeImage:
    pe_offset: int
    image_base: int
    sections: tuple[Section, ...]

    def section(self, name: str) -> Section:
        matches = [section for section in self.sections if section.name == name]
        if len(matches) != 1:
            raise RuntimeError(f"expected exactly one {name} section")
        return matches[0]


def parse_pe(data: bytes) -> PeImage:
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise RuntimeError("input is not a DOS/PE executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe : pe + 4] != b"PE\0\0":
        raise RuntimeError("input has no valid PE header")
    machine, section_count, _, _, _, optional_size, _ = struct.unpack_from(
        "<HHIIIHH", data, pe + 4
    )
    if machine != 0x8664:
        raise RuntimeError(f"unsupported PE machine 0x{machine:04X}; expected x86-64")
    optional = pe + 24
    if optional + optional_size > len(data):
        raise RuntimeError("truncated PE optional header")
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise RuntimeError("unsupported optional-header format; expected PE32+")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    section_table = optional + optional_size
    sections: list[Section] = []
    for index in range(section_count):
        offset = section_table + index * 40
        if offset + 40 > len(data):
            raise RuntimeError("truncated PE section table")
        name = data[offset : offset + 8].rstrip(b"\0").decode("ascii", "strict")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        characteristics = struct.unpack_from("<I", data, offset + 36)[0]
        if raw_offset + raw_size > len(data):
            raise RuntimeError(f"section {name} extends beyond the input")
        sections.append(Section(
            name=name,
            header_offset=offset,
            virtual_size=virtual_size,
            virtual_address=virtual_address,
            raw_size=raw_size,
            raw_offset=raw_offset,
            characteristics=characteristics,
        ))
    return PeImage(pe_offset=pe, image_base=image_base, sections=tuple(sections))


def unique_signature_offset(haystack: bytes, signature: bytes, label: str) -> int:
    offsets: list[int] = []
    start = 0
    while True:
        found = haystack.find(signature, start)
        if found < 0:
            break
        offsets.append(found)
        start = found + 1
    if len(offsets) != 1:
        formatted = ", ".join(f"0x{value:X}" for value in offsets[:8]) or "none"
        raise RuntimeError(
            f"{label} signature must occur exactly once; found {len(offsets)} ({formatted})"
        )
    return offsets[0]


def decode_call_target_rva(data: bytes, section: Section, call_offset: int) -> int:
    if data[call_offset] != 0xE8:
        raise RuntimeError(f"expected CALL rel32 at file offset 0x{call_offset:X}")
    displacement = struct.unpack_from("<i", data, call_offset + 1)[0]
    return section.file_to_rva(call_offset) + 5 + displacement


def patch_image(source: bytes) -> tuple[bytes, dict[str, object]]:
    pe = parse_pe(source)
    text = pe.section(".text")
    if not (text.characteristics & 0x20000000):
        raise RuntimeError(".text is not marked executable")
    text_bytes = source[text.raw_offset : text.raw_offset + text.raw_size]

    request_signature = REQUEST_PREFIX + REQUEST_EXPECTED + REQUEST_SUFFIX
    voice_signature = VOICE_PREFIX + VOICE_EXPECTED + VOICE_SUFFIX
    request_match = unique_signature_offset(text_bytes, request_signature, "request adapter")
    voice_match = unique_signature_offset(text_bytes, voice_signature, "voice startup")
    request_offset = text.raw_offset + request_match + len(REQUEST_PREFIX)
    voice_offset = text.raw_offset + voice_match + len(VOICE_PREFIX)

    graph_rva = decode_call_target_rva(source, text, voice_offset)
    graph_offset = text.rva_to_file(graph_rva)
    actual_prologue = source[graph_offset : graph_offset + len(GRAPH_START_PROLOGUE)]
    if actual_prologue != GRAPH_START_PROLOGUE:
        raise RuntimeError(
            "voice-start CALL target does not match the expected graph initializer"
        )

    cave_delta = align_up(text.virtual_size, 0x10)
    cave_request_offset = text.raw_offset + cave_delta
    cave_voice_offset = cave_request_offset + VOICE_STUB_DELTA
    cave_end = cave_voice_offset + VOICE_STUB_SIZE
    if cave_end > text.raw_offset + text.raw_size:
        raise RuntimeError(".text has insufficient mapped raw tail for the two stubs")
    if any(source[cave_request_offset:cave_end]):
        raise RuntimeError("selected .text tail is not zero-filled; refusing code-cave overwrite")

    request_rva = text.file_to_rva(request_offset)
    voice_rva = text.file_to_rva(voice_offset)
    cave_request_rva = text.file_to_rva(cave_request_offset)
    cave_voice_rva = text.file_to_rva(cave_voice_offset)

    request_stub = bytes.fromhex(
        "48 c7 44 24 30 00 00 00 00 "
        "0f b7 83 60 01 00 00 "
        "66 89 44 24 38 "
        "c3"
    )
    voice_stub = (
        bytes.fromhex("41 0f b7 46 10 66 89 85 f8 02 00 00 e9")
        + rel32(cave_voice_rva + 12, graph_rva)
    )
    if len(request_stub) != REQUEST_STUB_SIZE or len(voice_stub) != VOICE_STUB_SIZE:
        raise AssertionError("internal stub-size mismatch")

    result = bytearray(source)
    result[request_offset : request_offset + len(REQUEST_EXPECTED)] = (
        b"\xE8" + rel32(request_rva, cave_request_rva) + b"\x90" * 11
    )
    result[voice_offset : voice_offset + len(VOICE_EXPECTED)] = (
        b"\xE8" + rel32(voice_rva, cave_voice_rva)
    )
    result[cave_request_offset : cave_request_offset + len(request_stub)] = request_stub
    result[cave_voice_offset : cave_voice_offset + len(voice_stub)] = voice_stub

    # Map the already-present raw tail containing the stubs. No section bytes
    # are added and unrelated headers/metadata remain byte-identical.
    struct.pack_into("<I", result, text.header_offset + 8, text.raw_size)

    changed = [index for index, (before, after) in enumerate(zip(source, result)) if before != after]
    allowed_ranges = (
        (text.header_offset + 8, text.header_offset + 12),
        (request_offset, request_offset + len(REQUEST_EXPECTED)),
        (voice_offset, voice_offset + len(VOICE_EXPECTED)),
        (cave_request_offset, cave_request_offset + len(request_stub)),
        (cave_voice_offset, cave_voice_offset + len(voice_stub)),
    )
    if not all(any(start <= index < end for start, end in allowed_ranges) for index in changed):
        raise AssertionError("internal verifier found a change outside declared ranges")

    report: dict[str, object] = {
        "status": "patched",
        "acceptance": "unique local code signatures and PE invariants; no whole-file hash gate",
        "input_sha256_provenance_only": sha256(source),
        "output_sha256_provenance_only": sha256(result),
        "input_size": len(source),
        "output_size": len(result),
        "image_base": f"0x{pe.image_base:X}",
        "text": {
            "rva": f"0x{text.virtual_address:X}",
            "raw_offset": f"0x{text.raw_offset:X}",
            "raw_size": f"0x{text.raw_size:X}",
            "virtual_size_before": f"0x{text.virtual_size:X}",
            "virtual_size_after": f"0x{text.raw_size:X}",
        },
        "patches": [
            {"label": "request adapter", "file_offset": f"0x{request_offset:X}"},
            {"label": "voice startup", "file_offset": f"0x{voice_offset:X}"},
            {"label": "request stub", "file_offset": f"0x{cave_request_offset:X}"},
            {"label": "voice stub", "file_offset": f"0x{cave_voice_offset:X}"},
        ],
        "changed_byte_count": len(changed),
    }
    return bytes(result), report


def default_output(source: Path) -> Path:
    return source.with_name(f"{source.stem}.animation-registers-patched{source.suffix}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("source", type=Path, help="unpatched Journey.exe-compatible input")
    parser.add_argument("--output", type=Path, help="separate patched output path")
    parser.add_argument(
        "--report",
        type=Path,
        help="optional JSON report path; hashes are provenance, never acceptance gates",
    )
    args = parser.parse_args()

    source_path = args.source.resolve()
    output_path = (args.output or default_output(source_path)).resolve()
    if output_path == source_path:
        raise SystemExit("refusing to overwrite the input executable")
    if output_path.exists():
        raise SystemExit(f"refusing to overwrite existing output: {output_path}")
    if args.report and args.report.resolve().exists():
        raise SystemExit(f"refusing to overwrite existing report: {args.report.resolve()}")

    patched, report = patch_image(source_path.read_bytes())
    output_path.write_bytes(patched)
    report["source_path"] = str(source_path)
    report["output_path"] = str(output_path)
    if args.report:
        report_path = args.report.resolve()
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        report["report_path"] = str(report_path)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
