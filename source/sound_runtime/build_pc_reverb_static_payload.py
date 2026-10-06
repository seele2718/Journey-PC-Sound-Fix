#!/usr/bin/env python3
"""Build the position-fixed native payload for Journey's PC reverb repair.

This builds the native payload and leaves PE editing to the patcher.  It compiles the maintained
portable renderer/runtime and native hook wrappers with GNU MinGW,
links them at RVAs chosen from the inspected input image, and returns the
section bytes plus unwind records to the caller.  It never edits the image.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
BUILD_ROOT = HERE
TABLE_HEX = HERE / "resampler_coefficients.f32le.hex"
TABLE_SHA256 = "feaf4b4362e6cbe48f68ee69643053e75e73520fd5e3fdef445600af6986387b"
IMAGE_BASE = 0x140000000
TEXT_DELTA = 0
RDATA_DELTA = 0x10000
XDATA_DELTA = 0x13000
PDATA_DELTA = 0x14000
DATA_DELTA = 0x60000


class BuildError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BuildError(message)


def align(value: int, amount: int) -> int:
    return (value + amount - 1) & -amount


class PeImage:
    def __init__(self, data: bytes) -> None:
        require(len(data) >= 0x100 and data[:2] == b"MZ", "Invalid DOS image.")
        self.data = data
        self.pe = struct.unpack_from("<I", data, 0x3C)[0]
        require(data[self.pe : self.pe + 4] == b"PE\0\0", "Invalid PE signature.")
        machine, self.section_count = struct.unpack_from("<HH", data, self.pe + 4)
        require(machine == 0x8664, "Only AMD64 PE images are supported.")
        self.optional_size = struct.unpack_from("<H", data, self.pe + 20)[0]
        self.optional = self.pe + 24
        require(struct.unpack_from("<H", data, self.optional)[0] == 0x20B,
                "Only PE32+ images are supported.")
        self.image_base = struct.unpack_from("<Q", data, self.optional + 24)[0]
        self.section_alignment = struct.unpack_from("<I", data, self.optional + 32)[0]
        self.file_alignment = struct.unpack_from("<I", data, self.optional + 36)[0]
        self.size_of_image = struct.unpack_from("<I", data, self.optional + 56)[0]
        self.size_of_headers = struct.unpack_from("<I", data, self.optional + 60)[0]
        self.section_table = self.optional + self.optional_size
        self.sections: dict[str, dict[str, int]] = {}
        for index in range(self.section_count):
            offset = self.section_table + index * 40
            name = data[offset : offset + 8].rstrip(b"\0").decode("ascii")
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from(
                "<IIII", data, offset + 8
            )
            self.sections[name] = {
                "virtual_size": virtual_size,
                "rva": rva,
                "raw_size": raw_size,
                "raw_offset": raw_offset,
                "characteristics": struct.unpack_from("<I", data, offset + 36)[0],
            }

    def validate_image_layout(self) -> None:
        """Check contiguous, aligned virtual sections, including zero-fill."""
        alignment = self.section_alignment
        require(alignment > 0 and alignment & (alignment - 1) == 0,
                "Invalid PE section alignment.")
        expected = align(self.size_of_headers, alignment)
        for name, section in self.sections.items():
            require(section["rva"] == expected,
                    f"Non-adjacent or overlapping PE section {name}: "
                    f"expected RVA 0x{expected:x}, got 0x{section['rva']:x}.")
            expected = align(section["rva"] + max(section["virtual_size"],
                                                   section["raw_size"]), alignment)
        require(expected == self.size_of_image,
                "PE SizeOfImage does not match the final section extent.")

    def section_data(self, name: str) -> bytes:
        section = self.sections[name]
        start = section["raw_offset"]
        size = min(section["virtual_size"], section["raw_size"])
        require(start + size <= len(self.data), f"Truncated payload section {name}.")
        return self.data[start : start + size]

    def section_image(self, name: str) -> bytes:
        """Return the complete in-memory section image, including zero-fill."""
        section = self.sections[name]
        initialized = self.section_data(name)
        require(len(initialized) <= section["virtual_size"],
                f"Payload section {name} has invalid initialized extent.")
        return initialized + bytes(section["virtual_size"] - len(initialized))

    def directory(self, index: int) -> tuple[int, int]:
        require(0 <= index < 16, "PE directory index is out of range.")
        return struct.unpack_from("<II", self.data,
                                  self.optional + 112 + index * 8)

    def rva_to_offset(self, rva: int, size: int = 1) -> int:
        require(size >= 0, "Negative RVA extent.")
        if rva < self.size_of_headers:
            require(rva + size <= self.size_of_headers,
                    f"RVA 0x{rva:x} crosses the PE headers.")
            return rva
        for section in self.sections.values():
            initialized = min(section["virtual_size"], section["raw_size"])
            if (section["rva"] <= rva and
                    rva + size <= section["rva"] + initialized):
                offset = section["raw_offset"] + rva - section["rva"]
                require(offset + size <= len(self.data),
                        f"RVA 0x{rva:x} is truncated.")
                return offset
        raise BuildError(f"RVA 0x{rva:x} is outside initialized PE data.")

    def relocation_entries(self) -> list[tuple[int, int]]:
        """Return (type, target RVA) entries from the base-relocation table."""
        rva, size = self.directory(5)
        require(rva != 0 and size >= 8,
                "Linked payload has no base-relocation directory.")
        offset = self.rva_to_offset(rva, size)
        end = offset + size
        result: list[tuple[int, int]] = []
        while offset < end:
            require(offset + 8 <= end, "Truncated relocation block header.")
            page_rva, block_size = struct.unpack_from("<II", self.data, offset)
            require(block_size >= 8 and block_size % 4 == 0 and
                    offset + block_size <= end,
                    "Malformed relocation block.")
            entry_count = (block_size - 8) // 2
            for index in range(entry_count):
                encoded = struct.unpack_from(
                    "<H", self.data, offset + 8 + index * 2)[0]
                kind = encoded >> 12
                if kind != 0:
                    result.append((kind, page_rva + (encoded & 0xFFF)))
            offset += block_size
        require(offset == end, "Relocation directory has a partial block.")
        return result


def section_for_rva(image: PeImage, rva: int, size: int = 1) -> str | None:
    for name, section in image.sections.items():
        if (section["rva"] <= rva and
                rva + size <= section["rva"] + section["virtual_size"]):
            return name
    return None


def audit_fixed_references(source: PeImage, payload: PeImage) -> dict[str, int]:
    """Prove that copied fixed-address payload references remain mapped.

    The patched Journey image is fixed at its original image base and has no
    relocation directory. GNU ld's relocation table therefore serves here as
    a build-time inventory of every absolute pointer. A pointer may address a
    verified part of the input Journey image or one of the payload sections we
    retain. References into linker-only `.idata`, `.reloc`, or omitted
    sections are rejected.
    """
    copied = {".text", ".rdata", ".xdata", ".data"}
    source_start = source.image_base
    source_end = source.image_base + source.size_of_image
    references = payload.relocation_entries()
    counts = {"total": 0, "journey": 0, "payload": 0}
    for kind, site_rva in references:
        require(kind == 10,
                f"Unsupported payload relocation type {kind} at RVA "
                f"0x{site_rva:x}.")
        site_section = section_for_rva(payload, site_rva, 8)
        require(site_section in copied,
                f"Absolute pointer site RVA 0x{site_rva:x} is in omitted "
                f"section {site_section!r}.")
        site_offset = payload.rva_to_offset(site_rva, 8)
        target_va = struct.unpack_from("<Q", payload.data, site_offset)[0]
        target_rva = target_va - payload.image_base
        target_section = section_for_rva(payload, target_rva)
        counts["total"] += 1
        if source_start <= target_va < source_end:
            counts["journey"] += 1
        elif target_section in copied:
            counts["payload"] += 1
        else:
            raise BuildError(
                f"Absolute pointer at RVA 0x{site_rva:x} targets omitted or "
                f"unmapped VA 0x{target_va:x} ({target_section!r})."
            )
    require(counts["total"] != 0,
            "Linked payload unexpectedly contains no absolute references.")
    return counts


def decode_table() -> bytes:
    chunks = []
    for line in TABLE_HEX.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            chunks.append(line)
    try:
        result = bytes.fromhex("".join(chunks))
    except ValueError as exc:
        raise BuildError(f"Invalid resampler coefficient data: {exc}") from exc
    require(len(result) == 4096, "Resampler coefficient table is not exactly 4096 bytes.")
    require(hashlib.sha256(result).hexdigest() == TABLE_SHA256,
            "Resampler coefficient digest changed.")
    return result


def run(command: list[str], cwd: Path) -> None:
    completed = subprocess.run(command, cwd=cwd, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise BuildError(f"Command failed ({command[0]}): {detail}")


def symbols(nm: str, image: Path, cwd: Path) -> dict[str, int]:
    completed = subprocess.run([nm, "-n", str(image)], cwd=cwd, check=True,
                               text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    result: dict[str, int] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) == 3:
            try:
                value = int(fields[0], 16)
            except ValueError:
                continue
            result[fields[2]] = value
    return result


def build(image_data: bytes, native_pre_hadamard_late_output: bool = False,
          native_renderer_state_corrections: bool = False,
          native_stereo_path: bool = False, native_mono_aux_input: bool = False,
          native_lr1_cadence: bool = False, wet_onset_guard: bool = False, spatial_publication: bool = False, native_master_standard: bool = False, spatial_ancestors: bool = False, batched_ducker_catchup: bool = False, optimized_reverb_control: bool = False, publication_api_scope: bool = False) -> dict[str, object]:
    require(optimized_reverb_control and publication_api_scope
            and batched_ducker_catchup,
            "This source requires category ducking, batched catch-up, optimized "
            "controls and native API-scope publication; use build_sound_fix.py.")
    require(not native_mono_aux_input or native_stereo_path,
            "Native mono auxiliary input requires the native stereo renderer path.")
    require(not native_lr1_cadence or (native_stereo_path and
            native_renderer_state_corrections and native_pre_hadamard_late_output),
            "Native LR1 cadence requires the proved native renderer and stereo paths.")
    require(not native_master_standard or spatial_publication, "Native Master Standard requires the validated spatial publication baseline.")
    require(not spatial_ancestors or (native_mono_aux_input and spatial_publication),
            "Spatial ancestors require mono auxiliary input and exact-DLL publication.")
    require(not spatial_publication or wet_onset_guard, "Spatial publication requires the wet onset guard.")
    require(not publication_api_scope or spatial_publication,
            "Native API-scope publication requires identity-gated spatial publication.")
    require(not wet_onset_guard or native_mono_aux_input,
            "Wet onset guard requires the per-cue auxiliary scalar callback.")
    source = PeImage(image_data)
    require(source.image_base == IMAGE_BASE, "Unsupported Journey image base.")
    require(source.section_alignment == 0x1000 and source.file_alignment == 0x200,
            "Unsupported Journey PE alignment.")
    origin = align(source.size_of_image, source.section_alignment)
    code_va = IMAGE_BASE + origin
    tools = {
        "cc": shutil.which("x86_64-w64-mingw32-gcc"),
        "ld": shutil.which("x86_64-w64-mingw32-ld"),
        "objcopy": shutil.which("x86_64-w64-mingw32-objcopy"),
        "nm": shutil.which("x86_64-w64-mingw32-nm"),
    }
    require(all(tools.values()), "GNU MinGW gcc/ld/objcopy/nm are required.")
    cflags = ["-DJOURNEY_GROUP_CAPACITY=4096",
        "-std=c11", "-O2", "-fno-builtin", "-ffunction-sections",
        "-fdata-sections", "-Wall", "-Wextra", "-Werror",
    ]
    if native_pre_hadamard_late_output:
        cflags.append(
            "-DJOURNEY_REVERB_PATCH_NATIVE_PRE_HADAMARD_LATE_OUTPUT")
    if native_renderer_state_corrections:
        cflags.extend([
            "-DJOURNEY_REVERB_PATCH_NATIVE_RENDERER_STATE_CORRECTIONS",
            "-ffp-contract=off",
        ])
    if native_stereo_path:
        cflags.extend(["-DJOURNEY_REVERB_PATCH_NATIVE_STEREO_PATH", "-ffp-contract=off"])
    if native_mono_aux_input:
        cflags.append("-DJOURNEY_REVERB_PATCH_NATIVE_MONO_AUX_INPUT")
    if native_lr1_cadence:
        cflags.append("-DJOURNEY_REVERB_PATCH_NATIVE_LR1_CADENCE")
    if native_master_standard:
        cflags.extend(["-DJOURNEY_REVERB_PATCH_NATIVE_MASTER_STANDARD", "-ffp-contract=off"])
    if spatial_publication:
        cflags.append("-DJOURNEY_REVERB_PATCH_SPATIAL_PUBLICATION")
    if publication_api_scope:
        cflags.append("-DJOURNEY_REVERB_PUBLICATION_API_SCOPE")
    if spatial_ancestors:
        cflags.append("-DJOURNEY_REVERB_PATCH_SPATIAL_ANCESTORS")
    if wet_onset_guard:
        cflags.append("-DJOURNEY_REVERB_PATCH_WET_ONSET_GUARD")
    if batched_ducker_catchup:
        cflags.append("-DJOURNEY_DUCKER_BATCHED_CATCHUP")
    if optimized_reverb_control:
        cflags.append("-DJOURNEY_REVERB_CONTROL_OPTIMIZED")
    c_sources = [
        "pc_reverb_patch_bootstrap.c",
        "pc_reverb_patch_runtime.c",
        "branch_spatial_mirror.c",
    ]
    c_sources.append("journey_ducker_adapter.c")
    c_sources.extend([
        "reverb_group_mirror.c",
        "reverb_runtime_control.c",
    ])
    if optimized_reverb_control:
        c_sources.append("reverb_control_cache.c")
    build_id = None
    with tempfile.TemporaryDirectory(prefix="journey-reverb-payload-") as temp_name:
        temp = Path(temp_name)
        objects: list[Path] = []
        for source_name in c_sources:
            output = temp / (Path(source_name).stem + ".o")
            run([str(tools["cc"]), *cflags, "-c", str(HERE / source_name),
                 "-o", str(output)], BUILD_ROOT)
            objects.append(output)
        for source_name in ("pc_reverb_patch_hooks.s",
                            "slapper_journey_import_thunks.s"):
            output = temp / (Path(source_name).stem + ".o")
            assembler_flags = []
            run([str(tools["cc"]), *assembler_flags, "-c",
                 str(HERE / source_name), "-o", str(output)], BUILD_ROOT)
            objects.append(output)

        table_path = temp / "resampler.bin"
        table_path.write_bytes(decode_table())
        table_object = temp / "resampler.o"
        run([str(tools["ld"]), "-r", "-b", "binary", table_path.name,
             "-o", table_object.name], temp)
        run([
            str(tools["objcopy"]),
            "--rename-section", ".data=.rdata,alloc,load,readonly,data,contents",
            "--redefine-sym",
            "_binary_resampler_bin_start=journey_reverb_resampler_table",
            table_object.name,
        ], temp)
        objects.append(table_object)

        output = temp / "payload.exe"
        run([
            str(tools["ld"]), "--gc-sections", "--image-base",
            hex(IMAGE_BASE), "--section-start", f".text={hex(code_va)}",
            "--section-start", f".rdata={hex(code_va + RDATA_DELTA)}",
            "--section-start", f".xdata={hex(code_va + XDATA_DELTA)}",
            "--section-start", f".pdata={hex(code_va + PDATA_DELTA)}",
            "--section-start", f".data={hex(code_va + DATA_DELTA)}",
            "--section-start", f".idata={hex(code_va + DATA_DELTA + 0x1000)}",
            "--section-start", f".reloc={hex(code_va + DATA_DELTA + 0x2000)}",
            "--entry", "journey_hook_audio_system_post_init", "-o", str(output),
            *map(str, objects),
        ], BUILD_ROOT)
        payload_data = output.read_bytes()
        payload = PeImage(payload_data)
        syms = symbols(str(tools["nm"]), output, BUILD_ROOT)
        reference_counts = audit_fixed_references(source, payload)


        allowed_sections = {
            ".text", ".rdata", ".xdata", ".pdata", ".data",
            ".idata", ".reloc",
        }
        require(set(payload.sections) <= allowed_sections,
                "Linked payload contains an unexpected section: " +
                ", ".join(sorted(set(payload.sections) - allowed_sections)))
        if ".idata" in payload.sections:
            require(not any(payload.section_data(".idata")),
                    "Linked payload .idata contains live linker data.")
        import_rva, import_size = payload.directory(1)
        if import_rva != 0 or import_size != 0:
            require(section_for_rva(payload, import_rva, import_size) == ".idata" and
                    not any(payload.data[payload.rva_to_offset(import_rva,
                                                               import_size):
                                         payload.rva_to_offset(import_rva,
                                                               import_size) +
                                         import_size]),
                    "Linked payload unexpectedly has live import descriptors.")

        expected_rvas = {
            ".text": origin,
            ".rdata": origin + RDATA_DELTA,
            ".xdata": origin + XDATA_DELTA,
            ".pdata": origin + PDATA_DELTA,
            ".data": origin + DATA_DELTA,
        }
        for name, rva in expected_rvas.items():
            require(name in payload.sections, f"Linked payload lacks {name}.")
            require(payload.sections[name]["rva"] == rva,
                    f"Linked payload moved {name}.")
        require(len(payload.section_data(".text")) <= RDATA_DELTA,
                "Payload text exceeded its reserved span.")
        require(len(payload.section_data(".rdata")) <=
                XDATA_DELTA - RDATA_DELTA,
                "Payload rdata exceeded its reserved span.")
        require(len(payload.section_data(".xdata")) <=
                PDATA_DELTA - XDATA_DELTA,
                "Payload unwind metadata exceeded its reserved span.")
        require(len(payload.section_data(".pdata")) <= 0x1000,
                "Payload runtime-function table exceeded its reserved span.")
        require(payload.sections[".data"]["virtual_size"] <= 0x1000,
                "Payload writable data exceeded one page.")

        raw_pdata = payload.section_data(".pdata")
        require(len(raw_pdata) % 12 == 0, "Malformed payload .pdata.")
        runtime_functions = list(struct.iter_unpack("<III", raw_pdata))
        require(all(begin < end for begin, end, _ in runtime_functions),
                "Malformed payload runtime-function range.")
        require(all(a[1] <= b[0] for a, b in zip(runtime_functions,
                                                  runtime_functions[1:])),
                "Unsorted payload runtime-function table.")
        require(all(origin <= begin < end <= origin + RDATA_DELTA
                    for begin, end, _ in runtime_functions),
                "Payload runtime-function range escaped .text.")
        require(all(origin + XDATA_DELTA <= unwind < origin + PDATA_DELTA
                    for _, _, unwind in runtime_functions),
                "Payload unwind record escaped .xdata.")

        hook_names = [
            "journey_hook_audio_system_post_init",
            "journey_hook_system_update",
            "journey_hook_reverb_barn_post_update",
            "journey_hook_tone_descriptor_capture",
            "journey_hook_tone_channel_create",
            "journey_hook_stream_main_create",
            "journey_hook_stream_predecessor_channel_create",
            "journey_hook_stream_successor_create",
        ]
        hook_names.extend([
            "journey_hook_ducker_configure",
            "journey_hook_ducker_retire",
            "journey_hook_ducker_reset_forget",
        ])
        hook_names.extend(['journey_event_cue_unpause', 'journey_event_cue_stop', 'journey_event_tone_pre_unpause', 'journey_event_stream_pre_unpause', 'journey_event_register_tone_callback', 'journey_event_register_stream_callback'])
        hooks = {
            name: syms[name] - IMAGE_BASE
            for name in hook_names
        }
        rx = bytearray(PDATA_DELTA)
        rx[TEXT_DELTA : TEXT_DELTA + len(payload.section_data(".text"))] = \
            payload.section_data(".text")
        rx[RDATA_DELTA : RDATA_DELTA + len(payload.section_data(".rdata"))] = \
            payload.section_data(".rdata")
        rx[XDATA_DELTA : XDATA_DELTA + len(payload.section_data(".xdata"))] = \
            payload.section_data(".xdata")
        data_image = payload.section_image(".data")
        return {
            "origin": origin,
            "build_id": build_id,
            "rx_prefix": bytes(rx),
            "data": data_image,
            "runtime_functions": runtime_functions,
            "hooks": hooks,
            "symbols": syms,
            "payload_sha256": hashlib.sha256(payload_data).hexdigest(),
            "text_size": len(payload.section_data(".text")),
            "rdata_size": len(payload.section_data(".rdata")),
            "xdata_size": len(payload.section_data(".xdata")),
            "data_initialized_size": len(payload.section_data(".data")),
            "data_virtual_size": len(data_image),
            "absolute_reference_counts": reference_counts,
        }
