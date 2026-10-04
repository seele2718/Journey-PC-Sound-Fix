#!/usr/bin/env python3
"""Install the Journey PC sound fix into a separate executable.

The installer uses three source-reproducible payloads:

* tone-code.bin repairs ownership and source-format handling for embedded Tone
  audio graphs.
* sound-runtime.bin contains the executable room-response, spatial-routing,
  pause-propagation, Ducker, voice-association, and output-layout repair.  Its
  Windows unwind records are carried inside the same executable section.
* sound-state.bin contains the runtime's writable state.  Keeping it separate
  allows Windows to map code read/execute and state read/write without making
  any page simultaneously writable and executable.

The script verifies every payload, the PE layout, and the original bytes at
each changed location.  It never overwrites the supplied Journey.exe.
"""

from __future__ import annotations

from array import array
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys


HERE = Path(__file__).resolve().parent
DEFAULT_PAYLOAD = HERE / "payload"
SCHEMA = "journey-sound-fix-payload-v1"
IMAGE_BASE = 0x140000000


class InstallError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise InstallError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def pe_checksum(data: bytes, checksum_offset: int) -> int:
    padded = data + (b"\0" if len(data) & 1 else b"")
    words = array("H", padded)
    if sys.byteorder != "little":
        words.byteswap()
    total = sum(words) - words[checksum_offset // 2] - words[checksum_offset // 2 + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (total + len(data)) & 0xFFFFFFFF


def parse_pe(data: bytes) -> dict[str, object]:
    require(len(data) >= 0x100 and data[:2] == b"MZ",
            "input is not a Windows PE executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    require(pe + 24 <= len(data) and data[pe:pe + 4] == b"PE\0\0",
            "invalid PE header")
    machine, section_count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    require(machine == 0x8664, "only the x64 Journey executable is supported")
    require(optional + optional_size <= len(data), "truncated PE optional header")
    require(struct.unpack_from("<H", data, optional)[0] == 0x20B,
            "expected a 64-bit PE executable")
    require(struct.unpack_from("<Q", data, optional + 24)[0] == IMAGE_BASE,
            "unsupported Journey image base")
    sections = []
    section_table = optional + optional_size
    for index in range(section_count):
        offset = section_table + index * 40
        require(offset + 40 <= len(data), "truncated PE section table")
        name = data[offset:offset + 8].rstrip(b"\0").decode("ascii", "strict")
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        require(raw_offset + raw_size <= len(data), f"truncated {name} section")
        sections.append({
            "name": name,
            "rva": rva,
            "virtualSize": virtual_size,
            "rawSize": raw_size,
            "rawOffset": raw_offset,
        })
    exception_rva, exception_size = struct.unpack_from(
        "<II", data, optional + 112 + 3 * 8
    )

    def rva_to_offset(rva: int) -> int:
        for section in sections:
            extent = max(int(section["virtualSize"]), int(section["rawSize"]))
            if int(section["rva"]) <= rva < int(section["rva"]) + extent:
                return int(section["rawOffset"]) + rva - int(section["rva"])
        raise InstallError(f"RVA 0x{rva:X} is outside the executable image")

    return {
        "optional": optional,
        "sections": sections,
        "sectionCount": section_count,
        "checksumOffset": optional + 64,
        "exceptionOffset": rva_to_offset(exception_rva),
        "exceptionSize": exception_size,
    }


def read_payload(directory: Path) -> tuple[dict[str, object], dict[str, bytes]]:
    manifest_path = directory / "manifest.json"
    require(manifest_path.is_file(), f"missing payload manifest: {manifest_path}")
    manifest = json.loads(manifest_path.read_text())
    require(manifest.get("schema") == SCHEMA, "unsupported payload schema")
    payloads = {}
    for item in manifest["payloads"]:
        name = str(item["file"])
        path = directory / name
        require(path.is_file(), f"missing payload: {path}")
        data = path.read_bytes()
        require(len(data) == int(item["size"]), f"wrong payload size: {name}")
        require(sha256(data) == item["sha256"], f"payload checksum failed: {name}")
        payloads[name] = data
    return manifest, payloads


def apply(source: bytes, manifest: dict[str, object],
          payloads: dict[str, bytes]) -> tuple[bytes, dict[str, object]]:
    pe = parse_pe(source)
    require(len(source) == int(manifest["baseSize"]),
            f"unsupported input size {len(source)}; expected {manifest['baseSize']}")
    require(int(pe["sectionCount"]) == int(manifest["baseSectionCount"]),
            "unsupported original PE section layout")
    original_names = [item["name"] for item in pe["sections"]]
    for name in (".tna1", ".jrvx", ".jrvd"):
        require(name not in original_names, "input already contains sound-fix sections")
    require(int(pe["exceptionSize"]) == int(manifest["baseExceptionSize"]),
            "unsupported original exception-directory layout")
    exception_offset = int(pe["exceptionOffset"])
    exception_size = int(pe["exceptionSize"])
    original_exception = source[exception_offset:exception_offset + exception_size]
    require(sha256(original_exception) == manifest["baseExceptionSha256"],
            "original unwind records do not match the supported code build")

    require(bool(manifest.get("prerequisites")), "missing native compatibility prerequisites")
    for check in manifest["prerequisites"]:
        offset = int(check["offset"])
        expected = bytes.fromhex(check["bytes"])
        require(offset >= 0 and source[offset:offset + len(expected)] == expected,
                f"native compatibility check failed: {check['name']}")
        if check.get("unique"):
            require(source.count(expected) == 1,
                    f"ambiguous native prerequisite: {check['name']}")
    output = bytearray(source)

    # These records are the small changes made to the original Journey image;
    # they are generated from the source-built reference executable rather
    # than maintained as an unexplained list of offsets.  Collectively they:
    #
    # * extend the PE headers and section table for .tna1 (Tone repair), .jrvx
    #   (executable sound runtime), and .jrvd (writable runtime state), and
    #   publish the sound runtime's Windows unwind records;
    # * preserve the two authored animation-sound selector registers when an
    #   animation cue becomes a live voice;
    # * redirect the affected Tone, room-response, spatial-routing,
    #   pause-propagation, Ducker, voice-association, and output-layout paths
    #   to the source-built repair code; and
    # * select the softer player-chirp variant, PlayerCoo, that the original
    #   PC digital-input path cannot request.
    #
    # The substantial implementations live in the three payload files below;
    # these records are mostly PE metadata and localized call/jump sites.
    # Every site is checked against both its exact original bytes and nearby
    # context, allowing compatible data/Lua mods while rejecting a conflicting
    # native-code build.
    for record in manifest["records"]:
        offset = int(record["offset"])
        before = bytes.fromhex(record["before"])
        after = bytes.fromhex(record["after"])
        require(output[offset:offset + len(before)] == before,
                f"required original bytes changed at 0x{offset:X}: {record['name']}")
        context_offset = int(record["contextOffset"])
        context = bytes.fromhex(record["context"])
        require(source[context_offset:context_offset + len(context)] == context,
                f"local compatibility check failed at 0x{context_offset:X}")
        output[offset:offset + len(after)] = after

    target_size = int(manifest["targetSize"])
    require(target_size >= len(output), "payload target is smaller than the input")
    output.extend(b"\0" * (target_size - len(output)))
    occupied: list[tuple[int, int, str]] = []
    for item in manifest["payloads"]:
        name = str(item["file"])
        offset = int(item["rawOffset"])
        data = payloads[name]
        end = offset + len(data)
        require(len(source) <= offset < end <= len(output),
                f"payload lies outside the appended image: {name}")
        for left, right, other in occupied:
            require(end <= left or right <= offset,
                    f"payloads overlap: {name} and {other}")
        output[offset:end] = data
        occupied.append((offset, end, name))

    checksum_offset = int(pe["checksumOffset"])
    struct.pack_into("<I", output, checksum_offset, 0)
    if sha256(source) == manifest.get("baseSha256Provenance"):
        checksum = int(manifest["referenceChecksum"])
    else:
        checksum = pe_checksum(bytes(output), checksum_offset)
    struct.pack_into("<I", output, checksum_offset, checksum)
    result = bytes(output)
    installed_pe = parse_pe(result)
    installed_sections = {
        str(item["name"]): item for item in installed_pe["sections"]
    }
    for item in manifest["payloads"]:
        section_name = str(item["section"])
        require(section_name in installed_sections,
                f"installed PE is missing {section_name}")
        section = installed_sections[section_name]
        require(int(section["rawOffset"]) == int(item["rawOffset"]) and
                int(section["rawSize"]) == int(item["size"]),
                f"installed PE layout changed for {section_name}")
    reference_match = sha256(result) == manifest["targetSha256"]
    if sha256(source) == manifest.get("baseSha256Provenance"):
        require(reference_match,
                "exact reference input did not produce the reference output")
    return result, {
        "status": "patched",
        "version": manifest["version"],
        "inputSha256Provenance": sha256(source),
        "outputSha256": sha256(result),
        "referenceOutputSha256": manifest["targetSha256"],
        "referenceOutputMatch": reference_match,
        "wholeFileHashUsedForAcceptance": False,
        "changedRecordCount": len(manifest["records"]),
    }


def write_new(path: Path, data: bytes) -> None:
    require(not path.exists(), f"refusing to overwrite existing output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    created = False
    try:
        # Exclusive creation works on ordinary Windows, macOS and Linux
        # filesystems and does not require hard-link support from the Steam
        # library volume.  The input executable is never opened for writing.
        with path.open("xb") as handle:
            created = True
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
    except BaseException:
        if created:
            try:
                path.unlink()
            except OSError:
                pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", nargs="?", type=Path,
                        help="compatible original Journey.exe")
    parser.add_argument("--output", type=Path,
                        help="default: Journey.sound-fix.exe beside the input")
    parser.add_argument("--payload-dir", type=Path, default=DEFAULT_PAYLOAD,
                        help=argparse.SUPPRESS)
    parser.add_argument("--explain", action="store_true",
                        help="describe the payload without patching an executable")
    args = parser.parse_args()

    try:
        manifest, payloads = read_payload(args.payload_dir)
        print("Journey PC sound-fix components:")
        for item in manifest["payloads"]:
            print(f"  {item['file']}: {item['description']}")
        if args.explain:
            return 0
        require(args.input is not None,
                "provide an original Journey.exe, or use --explain")
        source = args.input.resolve()
        output = (args.output or source.with_name("Journey.sound-fix.exe")).resolve()
        require(source != output, "input and output must be different files")
        result, report = apply(source.read_bytes(), manifest, payloads)
        write_new(output, result)
        report["input"] = str(source)
        report["output"] = str(output)
        print(json.dumps(report, indent=2))
        return 0
    except (OSError, ValueError, KeyError, struct.error, json.JSONDecodeError) as exc:
        print(json.dumps({"status": "rejected", "error": str(exc)}, indent=2),
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
