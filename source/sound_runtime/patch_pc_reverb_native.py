#!/usr/bin/env python3
"""Apply, verify, or recover the base of Journey's static reverb repair.

Compatibility is deliberately local: every changed instruction, its unique
surrounding context, the fixed-address helper functions, and the exact imported
API identities are checked.  Whole-file hashes are provenance only, allowing
the patch to coexist with unrelated embedded-Lua and localized EXE
repairs. This module transforms bytes; the caller manages file output.
"""

from __future__ import annotations

from array import array
import hashlib
import json
from pathlib import Path
import struct
import sys

from audit_pc_reverb_hook_contracts import HOOKS
from build_pc_reverb_static_payload import (
    BuildError,
    DATA_DELTA,
    IMAGE_BASE,
    PDATA_DELTA,
    PeImage as PayloadPeImage,
    align,
    build,
)


# Embedded undo-format identifier, independent of the public package version.
VERSION = "0.1.0-alpha.1-ducker1"
RX_SECTION = b".jrvx"
DATA_SECTION = b".jrvd"
MAGIC = b"JOURNEY_NATIVE_REVERB_UNDO_V1".ljust(32, b"\0")
SCHEMA = "journey-native-reverb-undo-v1"
CAPSULE_LIMIT = 65536

EXPECTED_IMPORTS = {
    0x553A90: ("api-ms-win-crt-math-l1-1-0.dll", "cos"),
    0x553A98: ("api-ms-win-crt-math-l1-1-0.dll", "exp"),
    0x553AD8: ("api-ms-win-crt-math-l1-1-0.dll", "pow"),
    0x553AE8: ("api-ms-win-crt-math-l1-1-0.dll", "sin"),
    0x5539D0: ("api-ms-win-crt-heap-l1-1-0.dll", "malloc"),
    0x5539E8: ("api-ms-win-crt-heap-l1-1-0.dll", "free"),
    0x5537A0: ("VCRUNTIME140.dll", "memset"),
    0x5537C8: ("VCRUNTIME140.dll", "memcpy"),
    0x5530B8: ("KERNEL32.dll", "GetModuleHandleA"),
    0x553098: ("KERNEL32.dll", "GetProcAddress"),
    0x553F00: (
        "fmod64.dll",
        "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@"
        "PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z",
    ),
    0x553F48: (
        "fmod64.dll",
        "?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
    ),
}

# Absolute addresses embedded by the tiny assembly bridge.  These anchors are
# checked independently of the localized hook sites so a locally similar but
# incompatible Journey build cannot accidentally call the wrong constructor or
# read a different audio-system global.
EXPECTED_FIXED_TARGETS = {
    0x2C0310: bytes.fromhex(
        "4c8bdc55565741554881eca8000000488b05bad6a103488bf1498d4b08498bf8"
    ),
    0x2C4EC0: bytes.fromhex(
        "48895c241048896c2418488974242057415641574883ec60488bf10f29742450"
    ),
    0x2BDFCD: bytes.fromhex(
        "488b1d0cfaa1034533c0ba000400004c"
    ),
    0x2BE108: bytes.fromhex(
        "488b05d1f8a103488d35c28d3c004c8b"
    ),
}


class PatchError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PatchError(message)


def digest(data: bytes) -> str:
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


class JourneyPe(PayloadPeImage):
    def __init__(self, data: bytes) -> None:
        super().__init__(data)
        require(self.image_base == IMAGE_BASE, "Unsupported Journey image base.")
        require(self.section_alignment == 0x1000 and self.file_alignment == 0x200,
                "Unsupported Journey PE alignment.")
        require(len(self.sections) == self.section_count,
                "Duplicate or malformed PE section names.")

    def rva_to_offset(self, rva: int) -> int:
        if rva < self.size_of_headers:
            return rva
        for section in self.sections.values():
            extent = max(section["virtual_size"], section["raw_size"])
            if section["rva"] <= rva < section["rva"] + extent:
                offset = section["raw_offset"] + rva - section["rva"]
                require(offset < len(self.data), f"RVA 0x{rva:x} is truncated.")
                return offset
        raise PatchError(f"RVA 0x{rva:x} is outside mapped sections.")

    def read_rva(self, rva: int, size: int) -> bytes:
        offset = self.rva_to_offset(rva)
        require(offset + size <= len(self.data), f"RVA 0x{rva:x} is truncated.")
        return self.data[offset : offset + size]

    def c_string(self, rva: int) -> str:
        offset = self.rva_to_offset(rva)
        end = self.data.find(b"\0", offset)
        require(end >= 0, f"Unterminated string at RVA 0x{rva:x}.")
        try:
            return self.data[offset:end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise PatchError(f"Non-ASCII string at RVA 0x{rva:x}.") from exc

    def directory(self, index: int) -> tuple[int, int]:
        return struct.unpack_from("<II", self.data, self.optional + 112 + index * 8)

    def imports(self) -> dict[int, tuple[str, str]]:
        import_rva, import_size = self.directory(1)
        require(import_rva != 0 and import_size >= 20,
                "Journey import directory is missing.")
        offset = self.rva_to_offset(import_rva)
        result: dict[int, tuple[str, str]] = {}
        descriptors = 0
        while True:
            require(offset + 20 <= len(self.data), "Truncated import descriptor.")
            fields = struct.unpack_from("<IIIII", self.data, offset)
            if fields == (0, 0, 0, 0, 0):
                break
            original_thunk, _, _, name_rva, first_thunk = fields
            dll = self.c_string(name_rva)
            lookup = original_thunk or first_thunk
            index = 0
            while True:
                value = struct.unpack("<Q", self.read_rva(lookup + index * 8, 8))[0]
                if value == 0:
                    break
                if value >> 63:
                    name = f"#{value & 0xFFFF}"
                else:
                    name = self.c_string(value + 2)
                result[first_thunk + index * 8] = (dll, name)
                index += 1
                require(index < 65536, "Unterminated import thunk table.")
            descriptors += 1
            require(descriptors < 4096, "Unterminated import descriptor table.")
            offset += 20
        return result

    def exception_entries(self) -> list[tuple[int, int, int]]:
        rva, size = self.directory(3)
        require(rva != 0 and size != 0 and size % 12 == 0,
                "Malformed exception directory.")
        result = list(struct.iter_unpack("<III", self.read_rva(rva, size)))
        require(all(begin < end for begin, end, _ in result),
                "Malformed runtime-function range.")
        require(all(a[1] <= b[0] for a, b in zip(result, result[1:])),
                "Unsorted or overlapping exception directory.")
        return result


def validate_base(data: bytes) -> tuple[JourneyPe, dict[str, bytes]]:
    pe = JourneyPe(data)
    require(RX_SECTION.decode() not in pe.sections and
            DATA_SECTION.decode() not in pe.sections,
            "Image is already or partially native-reverb patched.")
    cert_rva, cert_size = pe.directory(4)
    require(cert_rva == 0 and cert_size == 0,
            "Authenticode-signed images are unsupported.")
    header_end = pe.section_table + pe.section_count * 40
    require(header_end + 80 <= pe.size_of_headers,
            "The PE has fewer than two free section-header slots.")
    require(data[header_end : header_end + 80] == bytes(80),
            "The required section-header slots are not empty.")
    imports = pe.imports()
    for rva, expected in EXPECTED_IMPORTS.items():
        require(imports.get(rva) == expected,
                f"Import at RVA 0x{rva:x} is not {expected[0]}!{expected[1]}.")

    for rva, expected in EXPECTED_FIXED_TARGETS.items():
        require(pe.read_rva(rva, len(expected)) == expected,
                f"Fixed native target at RVA 0x{rva:x} changed.")
        require(data.count(expected) == 1,
                f"Fixed native target at RVA 0x{rva:x} is ambiguous.")

    contexts: dict[str, bytes] = {}
    for hook in HOOKS:
        rva = hook.va - IMAGE_BASE
        expected = bytes.fromhex(hook.original_hex)
        require(pe.read_rva(rva, len(expected)) == expected,
                f"Hook {hook.name} instruction bytes changed.")
        context = pe.read_rva(rva - 16, 16 + len(expected) + 16)
        require(data.count(context) == 1,
                f"Hook {hook.name} context is missing or ambiguous.")
        contexts[hook.name] = context
    return pe, contexts


def section_header(name: bytes, virtual_size: int, rva: int,
                   raw_size: int, raw_offset: int, characteristics: int) -> bytes:
    require(len(name) <= 8, "PE section name is too long.")
    return struct.pack("<8sIIIIIIHHI", name.ljust(8, b"\0"), virtual_size,
                       rva, raw_size, raw_offset, 0, 0, 0, 0, characteristics)


def hook_bytes(site_rva: int, destination_rva: int, width: int) -> bytes:
    require(width >= 5, "Hook instruction is too short for CALL rel32.")
    displacement = destination_rva - (site_rva + 5)
    require(-(1 << 31) <= displacement < (1 << 31),
            "Hook target is outside CALL rel32 range.")
    return b"\xE8" + struct.pack("<i", displacement) + b"\x90" * (width - 5)


def make_patch(data: bytes, native_pre_hadamard_late_output: bool = False,
                   native_renderer_state_corrections: bool = False,
          native_stereo_path: bool = False, native_mono_aux_input: bool = False,
          native_lr1_cadence: bool = False, wet_onset_guard: bool = False, spatial_publication: bool = False, native_master_standard: bool = False, spatial_ancestors: bool = False, batched_ducker_catchup: bool = False, optimized_reverb_control: bool = False, publication_api_scope: bool = False) -> tuple[bytes, dict[str, object]]:
    pe, _ = validate_base(data)
    try:
        payload = build(data, native_pre_hadamard_late_output=
                        native_pre_hadamard_late_output,
                        native_renderer_state_corrections=
                        native_renderer_state_corrections,
                        native_stereo_path=native_stereo_path,
                        native_mono_aux_input=native_mono_aux_input,
                        native_lr1_cadence=native_lr1_cadence, wet_onset_guard=wet_onset_guard, spatial_publication=spatial_publication, native_master_standard=native_master_standard, spatial_ancestors=spatial_ancestors, batched_ducker_catchup=batched_ducker_catchup, optimized_reverb_control=optimized_reverb_control, publication_api_scope=publication_api_scope)
    except BuildError as exc:
        raise PatchError(str(exc)) from exc
    origin = int(payload["origin"])
    old_functions = pe.exception_entries()
    new_functions = list(payload["runtime_functions"])
    require(old_functions[-1][1] <= origin,
            "Existing exception ranges extend into the new payload.")
    combined_functions = old_functions + new_functions
    combined_pdata = b"".join(struct.pack("<III", *entry)
                              for entry in combined_functions)
    exception_rva = origin + PDATA_DELTA
    rx_core = bytes(payload["rx_prefix"]) + combined_pdata

    header_offset = pe.section_table + pe.section_count * 40
    checksum_offset = pe.optional + 64
    raw_start = align(len(data), pe.file_alignment)
    data_bytes = bytes(payload["data"])

    def produce(rx_virtual_size: int, rx_raw_size: int,
                data_raw_offset: int, data_raw_size: int) -> list[dict[str, object]]:
        writes: list[dict[str, object]] = []

        def add(name: str, offset: int, after: bytes) -> None:
            require(0 <= offset and offset + len(after) <= len(data),
                    f"Patch write {name} escapes the original image.")
            writes.append({
                "name": name,
                "offset": offset,
                "before": data[offset : offset + len(after)].hex(),
                "after": after.hex(),
            })

        hook_names = (
            "journey_hook_audio_system_post_init",
            "journey_hook_system_update",
            "journey_hook_reverb_barn_post_update",
            "journey_hook_tone_descriptor_capture",
            "journey_hook_tone_channel_create",
            "journey_hook_stream_main_create",
            "journey_hook_stream_predecessor_channel_create",
            "journey_hook_stream_successor_create",
            "journey_hook_ducker_configure",
            "journey_hook_ducker_retire",
            "journey_hook_ducker_reset_forget",
        )
        hook_names += ('journey_event_cue_unpause', 'journey_event_cue_stop', 'journey_event_tone_pre_unpause', 'journey_event_stream_pre_unpause', 'journey_event_register_tone_callback', 'journey_event_register_stream_callback')
        active_hooks = HOOKS
        require(len(active_hooks)==len(hook_names), "Hook/symbol map differs")
        for hook, symbol in zip(active_hooks, hook_names):
            site_rva = hook.va - IMAGE_BASE
            original = bytes.fromhex(hook.original_hex)
            patched = hook_bytes(site_rva, int(payload["hooks"][symbol]),
                                 len(original))
            add("hook:" + hook.name, pe.rva_to_offset(site_rva),
                patched)

        add("section count", pe.pe + 6,
            struct.pack("<H", pe.section_count + 2))
        old_code = struct.unpack_from("<I", data, pe.optional + 4)[0]
        old_init = struct.unpack_from("<I", data, pe.optional + 8)[0]
        add("SizeOfCode", pe.optional + 4,
            struct.pack("<I", old_code + rx_raw_size))
        add("SizeOfInitializedData", pe.optional + 8,
            struct.pack("<I", old_init + data_raw_size))
        image_end = origin + DATA_DELTA + max(len(data_bytes), data_raw_size)
        add("SizeOfImage", pe.optional + 56,
            struct.pack("<I", align(image_end, pe.section_alignment)))
        add("exception directory", pe.optional + 112 + 3 * 8,
            struct.pack("<II", exception_rva, len(combined_pdata)))
        add("RX section header", header_offset,
            section_header(RX_SECTION, rx_virtual_size, origin, rx_raw_size,
                           raw_start, 0x60000020))
        add("RW section header", header_offset + 40,
            section_header(DATA_SECTION, len(data_bytes), origin + DATA_DELTA,
                           data_raw_size, data_raw_offset, 0xC0000040))
        writes.append({
            "name": "checksum",
            "offset": checksum_offset,
            "before": data[checksum_offset : checksum_offset + 4].hex(),
            "after": None,
        })
        writes.sort(key=lambda item: int(item["offset"]))
        for left, right in zip(writes, writes[1:]):
            left_length = len(bytes.fromhex(str(left["before"])))
            require(int(left["offset"]) + left_length <= int(right["offset"]),
                    "Patch writes overlap.")
        return writes

    def capsule(records: list[dict[str, object]]) -> bytes:
        metadata = {
            "schema": SCHEMA,
            "version": VERSION,
            "base_size": len(data),
            "rx_core_size": len(rx_core),
            "runtime_function_count": len(new_functions),
            "records": records,
        }
        if native_pre_hadamard_late_output:
            metadata["native_pre_hadamard_late_output"] = True
        if native_stereo_path:
            metadata["native_stereo_path"] = True
        if native_mono_aux_input:
            metadata["native_mono_aux_input"] = True
        if native_lr1_cadence:
            metadata["native_lr1_cadence"] = True
        if native_master_standard:
            metadata["native_master_standard"] = True
        if spatial_ancestors:
            metadata["spatial_ancestors"] = True
        if spatial_publication:
            metadata["spatial_publication"] = True
        if wet_onset_guard:
            metadata["wet_onset_guard"] = True
        if batched_ducker_catchup:
            metadata["batched_ducker_catchup"] = True
        if optimized_reverb_control:
            metadata["optimized_reverb_control"] = True
        if publication_api_scope:
            metadata["publication_api_scope"] = True
        if native_renderer_state_corrections:
            metadata["native_renderer_state_corrections"] = True
        return json.dumps(metadata, sort_keys=True,
                          separators=(",", ":")).encode()

    trial_records = produce(0, 0, 0, 0)
    trial_capsule = capsule(trial_records)
    capsule_offset = align(len(rx_core), 16)
    rx_content_size = capsule_offset + 36 + len(trial_capsule)
    rx_raw_size = align(rx_content_size, pe.file_alignment)
    # The state has a fixed RVA used by the compiled code. Reserve the entire
    # span up to it, including loader-zero-filled tail pages: Windows image
    # sections must be adjacent, not merely non-overlapping. Raw file bytes
    # still end at the capsule's ordinary file-aligned extent.
    rx_virtual_size = DATA_DELTA
    require(rx_raw_size <= rx_virtual_size,
            "Native-reverb payload overlaps the fixed state section.")
    data_raw_offset = raw_start + rx_raw_size
    data_raw_size = align(len(data_bytes), pe.file_alignment)
    records = produce(rx_virtual_size, rx_raw_size,
                      data_raw_offset, data_raw_size)
    capsule_bytes = capsule(records)
    require(len(capsule_bytes) == len(trial_capsule),
            "Undo capsule size changed after layout finalization.")
    require(len(capsule_bytes) < CAPSULE_LIMIT, "Undo capsule is unexpectedly large.")

    output = bytearray(data)
    output += bytes(raw_start - len(output))
    output += rx_core
    output += bytes(capsule_offset - len(rx_core))
    output += MAGIC + struct.pack("<I", len(capsule_bytes)) + capsule_bytes
    output += bytes(raw_start + rx_raw_size - len(output))
    require(len(output) == data_raw_offset, "Internal RX layout mismatch.")
    output += data_bytes + bytes(data_raw_size - len(data_bytes))

    for record in records:
        offset = int(record["offset"])
        after = record["after"]
        value = bytes(4) if after is None else bytes.fromhex(str(after))
        output[offset : offset + len(value)] = value
    struct.pack_into("<I", output, checksum_offset,
                     pe_checksum(bytes(output), checksum_offset))
    JourneyPe(bytes(output)).validate_image_layout()
    report = {
        "patch_version": VERSION,
        "input_sha256_provenance": digest(data),
        "output_sha256_provenance": digest(bytes(output)),
        "origin_rva": hex(origin),
        "rx_virtual_size": rx_virtual_size,
        "rx_raw_size": rx_raw_size,
        "rw_virtual_size": len(data_bytes),
        "runtime_function_count_added": len(new_functions),
        "existing_runtime_function_count": len(old_functions),
        "payload_text_size": payload["text_size"],
        "payload_rdata_size": payload["rdata_size"],
        "payload_xdata_size": payload["xdata_size"],
        "hook_count": len(HOOKS),
        "fixed_native_target_count": len(EXPECTED_FIXED_TARGETS),
        "absolute_reference_counts": payload["absolute_reference_counts"],
        "payload_data_initialized_size": payload["data_initialized_size"],
        "payload_data_virtual_size": payload["data_virtual_size"],
        "native_stereo_path": native_stereo_path,
        "native_mono_aux_input": native_mono_aux_input,
        "native_lr1_cadence": native_lr1_cadence,
        "wet_onset_guard": wet_onset_guard,
        "spatial_publication": spatial_publication,
        "native_master_standard": native_master_standard,
        "spatial_ancestors": spatial_ancestors,
        "ducker_enabled": True,
        "batched_ducker_catchup": batched_ducker_catchup,
        "optimized_reverb_control": optimized_reverb_control,
        **({"publication_api_scope": True} if publication_api_scope else {}),
        "build_id": payload["build_id"],
        "native_pre_hadamard_late_output": native_pre_hadamard_late_output,
        "native_renderer_state_corrections":
            native_renderer_state_corrections,
    }
    return bytes(output), report


def recover_base(data: bytes) -> tuple[bytes, JourneyPe, dict[str, object]]:
    pe = JourneyPe(data)
    names = list(pe.sections)
    require(names[-2:] == [RX_SECTION.decode(), DATA_SECTION.decode()],
            "Native-reverb sections are absent or not the final two sections.")
    rx = pe.sections[RX_SECTION.decode()]
    rw = pe.sections[DATA_SECTION.decode()]
    require(rx["characteristics"] == 0x60000020 and
            rw["characteristics"] == 0xC0000040,
            "Native-reverb section permissions changed.")
    require(rw["raw_offset"] + rw["raw_size"] == len(data),
            "Unexpected bytes follow the native-reverb sections.")
    # VirtualSize includes zero-filled memory; only raw bytes contain the
    # undo capsule. Its length determines the end of initialized content.
    payload = data[rx["raw_offset"] : rx["raw_offset"] + rx["raw_size"]]
    require(payload.count(MAGIC) == 1, "Missing or ambiguous undo capsule.")
    capsule_at = payload.index(MAGIC)
    require(capsule_at + 36 <= len(payload), "Truncated undo capsule header.")
    length = struct.unpack_from("<I", payload, capsule_at + 32)[0]
    capsule_end = capsule_at + 36 + length
    require(length < CAPSULE_LIMIT and capsule_end <= len(payload) and
            capsule_end <= rx["virtual_size"],
            "Malformed undo capsule length.")
    require(payload[capsule_end:] == bytes(len(payload) - capsule_end),
            "Nonzero native-reverb RX padding.")
    try:
        metadata = json.loads(payload[capsule_at + 36 : capsule_end].decode())
    except (UnicodeDecodeError, ValueError) as exc:
        raise PatchError(f"Invalid undo capsule: {exc}") from exc
    require(metadata.get("schema") == SCHEMA and
            metadata.get("version") == VERSION,
            "Unsupported native-reverb patch version.")
    base_size = metadata.get("base_size")
    require(type(base_size) is int and 1024 <= base_size <= rx["raw_offset"],
            "Invalid original image size in undo capsule.")
    require(data[base_size : rx["raw_offset"]] == bytes(rx["raw_offset"] - base_size),
            "Nonzero alignment gap before native-reverb RX data.")
    require(data[rw["raw_offset"] + rw["virtual_size"] :
                 rw["raw_offset"] + rw["raw_size"]] ==
            bytes(rw["raw_size"] - rw["virtual_size"]),
            "Nonzero native-reverb RW padding.")

    records = metadata.get("records")
    require(isinstance(records, list) and 12 <= len(records) <= 32,
            "Malformed undo record collection.")
    restored = bytearray(data[:base_size])
    previous_end = 0
    checksum_records = 0
    for record in records:
        require(isinstance(record, dict), "Malformed undo record.")
        try:
            offset = record["offset"]
            before = bytes.fromhex(record["before"])
            after = (None if record["after"] is None
                     else bytes.fromhex(record["after"]))
        except (KeyError, TypeError, ValueError) as exc:
            raise PatchError(f"Malformed undo record: {exc}") from exc
        require(type(offset) is int and previous_end <= offset and
                0 < len(before) <= 80 and offset + len(before) <= base_size,
                "Overlapping or out-of-bounds undo record.")
        previous_end = offset + len(before)
        if after is None:
            require(record.get("name") == "checksum" and len(before) == 4,
                    "Malformed checksum undo record.")
            checksum_records += 1
        else:
            require(len(after) == len(before) and
                    data[offset : offset + len(after)] == after,
                    f"Patched field changed: {record.get('name', 'unknown')}.")
        restored[offset : offset + len(before)] = before
    require(checksum_records == 1, "Missing or duplicate checksum undo record.")
    return bytes(restored), pe, metadata


def verify(data: bytes) -> dict[str, object]:
    base, pe, metadata = recover_base(data)
    expected, report = make_patch(
        base,
        native_pre_hadamard_late_output=bool(
            metadata.get("native_pre_hadamard_late_output", False)),
        native_renderer_state_corrections=bool(
            metadata.get("native_renderer_state_corrections", False)),
        native_stereo_path=bool(metadata.get("native_stereo_path", False)),
        native_mono_aux_input=bool(metadata.get("native_mono_aux_input", False)),
        native_lr1_cadence=bool(metadata.get("native_lr1_cadence", False)),
        wet_onset_guard=bool(metadata.get("wet_onset_guard", False)),
        spatial_publication=bool(metadata.get("spatial_publication", False)),
        native_master_standard=bool(metadata.get("native_master_standard", False)),
        spatial_ancestors=bool(metadata.get("spatial_ancestors", False)),
        batched_ducker_catchup=bool(
            metadata.get("batched_ducker_catchup", False)),
        optimized_reverb_control=bool(
            metadata.get("optimized_reverb_control", False)),
        publication_api_scope=bool(metadata.get("publication_api_scope", False)))
    require(expected == data,
            "Patched image differs from the canonical patch rebuilt from its base.")
    checksum_offset = pe.optional + 64
    expected_checksum = pe_checksum(data, checksum_offset)
    actual_checksum = struct.unpack_from("<I", data, checksum_offset)[0]
    require(actual_checksum == expected_checksum, "Invalid PE checksum.")
    old = JourneyPe(base)
    new_functions = pe.exception_entries()
    old_functions = old.exception_entries()
    require(new_functions[:len(old_functions)] == old_functions,
            "Original exception entries changed.")
    require(len(new_functions) == len(old_functions) +
            metadata["runtime_function_count"],
            "Unexpected native runtime-function count.")
    for index in range(16):
        if index == 3:
            continue
        require(old.directory(index) == pe.directory(index),
                f"Unrelated PE directory {index} changed.")
    report.update({
        "status": "verified",
        "round_trip": "byte-identical to recovered input",
        "checksum": hex(actual_checksum),
        "compatibility": "localized instructions, contexts, functions, and IAT identities",
        "qualification": "static PE verification; does not run the game",
    })
    return report
