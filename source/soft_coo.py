#!/usr/bin/env python3
"""Select Journey PC's softer PlayerCoo player-chirp variant.

The PC input provider reduces controller input to a digital 0/1 value before
the sound selector receives it.  The selector therefore chooses PlayerChirp
for an ordinary controller tap, while the console provider supplies a graded
pressure value and can select PlayerCoo below 0.5.

This patch removes only the selector's final conditional PlayerCoo ->
PlayerChirp replacement.  PlayerCall/PlayerShout duration branches, state
updates, effect lookup, and the selected effect ID copied to the outgoing
network record are left untouched.

The input is identified by a unique local machine-code signature, not a
whole-file hash.  The patcher writes a separate output and never overwrites its
input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path


PREFIX = bytes.fromhex(
    "f3 0f 10 84 24 e0 00 00 00 "
    "48 8d 05 c0 5c 39 00 "
    "0f 2f 05 cd cd 43 00 "
    "48 8d 3d 6a 5c 39 00"
)
ORIGINAL = bytes.fromhex("48 0f 47 f8")  # cmova rdi, rax (PlayerCoo -> PlayerChirp)
PATCHED = bytes.fromhex("0f 1f 40 00")   # four-byte NOP; retain PlayerCoo
SUFFIX = bytes.fromhex("40 32 f6 eb 6e 48 8d 3d 82 5c 39 00")
EXPECTED_IMAGE_BASE = 0x140000000
EXPECTED_PATCH_RVA = 0x24E3B6


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class Section:
    name: str
    virtual_address: int
    raw_size: int
    raw_offset: int
    characteristics: int

    def file_to_rva(self, offset: int) -> int:
        if not self.raw_offset <= offset < self.raw_offset + self.raw_size:
            raise RuntimeError(f"file offset 0x{offset:X} is outside {self.name}")
        return self.virtual_address + offset - self.raw_offset


@dataclass(frozen=True)
class PeImage:
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
    machine, count, _, _, _, optional_size, _ = struct.unpack_from(
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
    table = optional + optional_size
    sections: list[Section] = []
    for index in range(count):
        offset = table + index * 40
        if offset + 40 > len(data):
            raise RuntimeError("truncated PE section table")
        name = data[offset : offset + 8].rstrip(b"\0").decode("ascii", "strict")
        _, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        characteristics = struct.unpack_from("<I", data, offset + 36)[0]
        if raw_offset + raw_size > len(data):
            raise RuntimeError(f"section {name} extends beyond the input")
        sections.append(Section(name, virtual_address, raw_size, raw_offset, characteristics))
    return PeImage(image_base, tuple(sections))


def offsets(data: bytes, signature: bytes) -> list[int]:
    found: list[int] = []
    start = 0
    while True:
        offset = data.find(signature, start)
        if offset < 0:
            return found
        found.append(offset)
        start = offset + 1


def transform(source: bytes, undo: bool = False) -> tuple[bytes, dict[str, object]]:
    pe = parse_pe(source)
    if pe.image_base != EXPECTED_IMAGE_BASE:
        raise RuntimeError(
            f"unexpected image base 0x{pe.image_base:X}; expected 0x{EXPECTED_IMAGE_BASE:X}"
        )
    text = pe.section(".text")
    if not (text.characteristics & 0x20000000):
        raise RuntimeError(".text is not executable")
    before, after = (PATCHED, ORIGINAL) if undo else (ORIGINAL, PATCHED)
    signature = PREFIX + before + SUFFIX
    matches = offsets(source, signature)
    if len(matches) != 1:
        state = "patched" if undo else "original"
        other = offsets(source, PREFIX + after + SUFFIX)
        detail = f"; opposite state occurs {len(other)} time(s)" if other else ""
        raise RuntimeError(
            f"{state} PlayerCoo-selection signature must occur exactly once; "
            f"found {len(matches)}{detail}"
        )
    patch_offset = matches[0] + len(PREFIX)
    if not text.raw_offset <= patch_offset < text.raw_offset + text.raw_size:
        raise RuntimeError("PlayerCoo-selection signature is outside executable .text")
    patch_rva = text.file_to_rva(patch_offset)
    if patch_rva != EXPECTED_PATCH_RVA:
        raise RuntimeError(
            f"PlayerCoo-selection instruction is at unexpected RVA 0x{patch_rva:X}; "
            f"expected 0x{EXPECTED_PATCH_RVA:X}"
        )
    result = bytearray(source)
    result[patch_offset : patch_offset + len(before)] = after
    output = bytes(result)
    report = {
        "operation": "undo" if undo else "patch",
        "input_sha256": sha256(source),
        "output_sha256": sha256(output),
        "patch_file_offset": patch_offset,
        "patch_rva": patch_rva,
        "before": before.hex(),
        "after": after.hex(),
        "changed_bytes": sum(a != b for a, b in zip(source, output)),
        "scope": "softer PlayerCoo player-chirp selection only",
    }
    return output, report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--undo", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    source = args.input.read_bytes()
    output, report = transform(source, undo=args.undo)
    default_suffix = ".soft-coo-unpatched.exe" if args.undo else ".soft-coo-patched.exe"
    destination = args.output or args.input.with_name(args.input.stem + default_suffix)
    if destination.resolve() == args.input.resolve():
        raise RuntimeError("refusing to overwrite the input executable")
    if destination.exists() and destination.read_bytes() != output:
        raise RuntimeError(f"refusing to overwrite different output: {destination}")
    destination.write_bytes(output)
    report["input"] = str(args.input)
    report["output"] = str(destination)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
