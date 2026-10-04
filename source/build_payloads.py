#!/usr/bin/env python3
"""Create the three public installer payloads from original and repaired EXEs.

The repair sources build a complete Journey.sound-fix.exe first.  This script
then extracts the three authored PE sections and records the localized changes
made inside the original-sized part of the image.  It does not compile repair
logic and it does not contain a Journey executable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


SCHEMA = "journey-sound-fix-payload-v1"
COMPONENTS = (
    (".tna1", "tone-code.bin",
     "Tone-cache ownership and source-format repair code."),
    (".jrvx", "sound-runtime.bin",
     "Executable room-response, spatial, pause, Ducker, and output repair; "
     "includes the combined Windows unwind table."),
    (".jrvd", "sound-state.bin",
     "Writable state used by the sound runtime; deliberately non-executable."),
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_pe(data: bytes) -> dict[str, object]:
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise RuntimeError("input is not a PE executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise RuntimeError("invalid PE signature")
    machine, count, _, _, _, optional_size, _ = struct.unpack_from(
        "<HHIIIHH", data, pe + 4
    )
    if machine != 0x8664:
        raise RuntimeError("expected an x86-64 executable")
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise RuntimeError("expected PE32+")
    table = optional + optional_size
    sections = []
    for index in range(count):
        offset = table + index * 40
        name = data[offset:offset + 8].rstrip(b"\0").decode("ascii")
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        sections.append({
            "name": name, "virtualSize": virtual_size, "rva": rva,
            "rawSize": raw_size, "rawOffset": raw_offset,
        })
    exception_rva, exception_size = struct.unpack_from(
        "<II", data, optional + 112 + 3 * 8
    )
    exception_offset = None
    for section in sections:
        delta = exception_rva - int(section["rva"])
        if 0 <= delta < int(section["rawSize"]):
            exception_offset = int(section["rawOffset"]) + delta
            break
    if exception_offset is None:
        raise RuntimeError("exception table is not backed by a section")
    return {
        "optional": optional,
        "sectionCount": count,
        "sections": sections,
        "checksumOffset": optional + 64,
        "exceptionOffset": exception_offset,
        "exceptionSize": exception_size,
    }


def patch_records(base: bytes, target: bytes,
                  checksum_offset: int) -> list[dict[str, object]]:
    changed = [
        index for index, (left, right) in enumerate(zip(base, target))
        if left != right and not checksum_offset <= index < checksum_offset + 4
    ]
    spans: list[list[int]] = []
    for index in changed:
        if not spans or index != spans[-1][1]:
            spans.append([index, index + 1])
        else:
            spans[-1][1] = index + 1
    records = []
    for number, (start, end) in enumerate(spans, 1):
        context_start = max(0, start - 12)
        context_end = min(len(base), end + 12)
        records.append({
            "name": f"localized original-image change {number}",
            "offset": start,
            "before": base[start:end].hex(),
            "after": target[start:end].hex(),
            "contextOffset": context_start,
            "context": base[context_start:context_end].hex(),
        })
    return records


def prerequisite_records(base: bytes) -> list[dict[str, object]]:
    """Carry unchanged native call targets and imports into the installer.

    Local prerequisites preserve the source builder's compatibility boundary;
    this deliberately does not hash unrelated embedded data or the whole EXE.
    """
    here = Path(__file__).resolve().parent
    sys.path.insert(0, str(here / "sound_runtime"))
    import patch_pc_reverb_native as runtime
    import native_routes
    native_routes.configure(runtime)
    pe = runtime.JourneyPe(base)
    records = []
    def add(name, offset, size, unique=False):
        data = base[offset:offset+size]
        if len(data) != size:
            raise RuntimeError("truncated prerequisite: " + name)
        records.append(dict(name=name, offset=offset, bytes=data.hex(), unique=unique))
    def rva(name, address, size, unique=False):
        add(name, pe.rva_to_offset(address), size, unique)
    add('original section layout and reserved payload slots', pe.section_table,
        (pe.section_count+3)*40)
    add('image base and alignments', pe.optional+24, 16)
    add('image size and header extent', pe.optional+56, 8)
    add('import-directory location', pe.optional+112+8, 8)
    add('unsigned-image contract', pe.optional+112+4*8, 8)
    for address, expected in runtime.EXPECTED_FIXED_TARGETS.items():
        if pe.read_rva(address, len(expected)) != expected:
            raise RuntimeError('native target differs: '+hex(address))
        rva('native target '+hex(address), address, len(expected), True)
    for site in json.loads((here/'tone/tone_cache_native_a_signatures.json').read_text())['contexts']:
        expected=bytes.fromhex(site['bytes'])
        if pe.read_rva(site['rva'],len(expected)) != expected:
            raise RuntimeError('Tone prerequisite differs: '+site['name'])
        rva('Tone: '+site['name'],site['rva'],len(expected),True)
    wanted=set(runtime.EXPECTED_IMPORTS) | {0x5539d8,0x5539e0,0x554050}
    actual=pe.imports()
    for address,expected in runtime.EXPECTED_IMPORTS.items():
        if actual.get(address)!=expected:
            raise RuntimeError('import differs: '+hex(address))
    if actual[0x5539d8][1]!='_aligned_malloc' or actual[0x5539e0][1]!='_aligned_free' or 'release' not in actual[0x554050][1]:
        raise RuntimeError('Tone imports differ')
    start,_=pe.directory(1)
    cursor=pe.rva_to_offset(start)
    found=set()
    while True:
        fields=struct.unpack_from('<IIIII',base,cursor)
        if fields==(0,0,0,0,0):break
        lookup,_,_,name,iat=fields
        lookup=lookup or iat
        dll=pe.c_string(name)
        count=0
        while struct.unpack('<Q',pe.read_rva(lookup+count*8,8))[0]: count+=1
        selected=False
        for address in sorted(wanted):
            if address<iat or (address-iat)%8 or (address-iat)//8>=count:continue
            index=(address-iat)//8
            if not selected:
                add('import descriptor: '+dll,cursor,20)
                rva('import DLL: '+dll,name,len(dll)+1)
                selected=True
            pointer=struct.unpack('<Q',pe.read_rva(lookup+index*8,8))[0]
            rva('import lookup '+hex(address),lookup+index*8,8)
            rva('import address '+hex(address),address,8)
            if not pointer>>63:
                rva('import name: '+actual[address][1],pointer,2+len(actual[address][1])+1)
            found.add(address)
        cursor+=20
    if found!=wanted:raise RuntimeError('missing required imports')
    return records


def build_payloads(base: bytes, target: bytes, output: Path,
                   version: str, label: str) -> dict[str, object]:
    base_pe = parse_pe(base)
    target_pe = parse_pe(target)
    target_sections = {str(item["name"]): item for item in target_pe["sections"]}
    output.mkdir(parents=True, exist_ok=True)
    payload_items = []
    for section_name, filename, description in COMPONENTS:
        if section_name not in target_sections:
            raise RuntimeError(f"repaired executable is missing {section_name}")
        section = target_sections[section_name]
        start = int(section["rawOffset"])
        size = int(section["rawSize"])
        data = target[start:start + size]
        path = output / filename
        if path.exists() and path.read_bytes() != data:
            raise RuntimeError(f"refusing to overwrite a different payload: {path}")
        path.write_bytes(data)
        payload_items.append({
            "file": filename,
            "section": section_name,
            "rawOffset": start,
            "size": size,
            "sha256": sha256(data),
            "description": description,
        })

    base_exception_offset = int(base_pe["exceptionOffset"])
    base_exception_size = int(base_pe["exceptionSize"])
    original_exception = base[
        base_exception_offset:base_exception_offset + base_exception_size
    ]
    runtime = next(item for item in payload_items if item["section"] == ".jrvx")
    exception_offset = int(target_pe["exceptionOffset"])
    runtime_start = int(runtime["rawOffset"])
    runtime_end = runtime_start + int(runtime["size"])
    if not runtime_start <= exception_offset < runtime_end:
        raise RuntimeError("sound-runtime payload does not contain the unwind table")
    combined_exception = target[
        exception_offset:exception_offset + int(target_pe["exceptionSize"])
    ]
    if not combined_exception.startswith(original_exception):
        raise RuntimeError("repaired unwind table does not preserve original records")

    manifest = {
        "schema": SCHEMA,
        "version": version,
        "label": label,
        "baseSize": len(base),
        "baseSha256Provenance": sha256(base),
        "baseSectionCount": int(base_pe["sectionCount"]),
        "baseExceptionSize": base_exception_size,
        "baseExceptionSha256": sha256(original_exception),
        "targetSize": len(target),
        "targetSha256": sha256(target),
        "referenceChecksum": struct.unpack_from(
            "<I", target, int(target_pe["checksumOffset"])
        )[0],
        "records": patch_records(base, target, int(base_pe["checksumOffset"])),
        "prerequisites": prerequisite_records(base),
        "payloads": payload_items,
        "wholeFileHashPolicy": "provenance-only",
    }
    manifest_path = output / "manifest.json"
    encoded = (json.dumps(manifest, indent=2) + "\n").encode()
    if manifest_path.exists() and manifest_path.read_bytes() != encoded:
        raise RuntimeError(f"refusing to overwrite a different manifest: {manifest_path}")
    manifest_path.write_bytes(encoded)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("repaired", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--label", default="normal")
    args = parser.parse_args()
    manifest = build_payloads(
        args.original.read_bytes(), args.repaired.read_bytes(), args.output_dir,
        args.version, args.label,
    )
    print(json.dumps({
        "status": "payloads-built",
        "output": str(args.output_dir),
        "targetSha256": manifest["targetSha256"],
        "payloadCount": len(manifest["payloads"]),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
