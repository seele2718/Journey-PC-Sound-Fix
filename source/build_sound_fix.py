#!/usr/bin/env python3
"""Build the Journey PC sound repair from an original Journey.exe.

This is the readable source path. It applies each independently validated
repair stage and compiles the included x86-64 C/assembly implementation. The
input file is never overwritten and whole-file hashes are provenance only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


HERE = Path(__file__).resolve().parent
VERSION = (HERE / "VERSION").read_text().strip()
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "tone"))
sys.path.insert(0, str(HERE / "sound_runtime"))

import animation_registers
import patch_tone_cache_native_a as tone_cache
import patch_pc_reverb_native as sound_runtime
import soft_coo
import native_routes

native_routes.configure(sound_runtime)


SOUND_RUNTIME_OPTIONS = {
    "native_pre_hadamard_late_output": True,
    "native_renderer_state_corrections": True,
    "native_stereo_path": True,
    "native_mono_aux_input": True,
    "native_lr1_cadence": True,
    "wet_onset_guard": True,
    "spatial_publication": True,
    "native_master_standard": True,
    "spatial_ancestors": True,
    "batched_ducker_catchup": True,
    "optimized_reverb_control": True,
    "publication_api_scope": True,
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stage(name: str, before: bytes, after: bytes, details: object) -> dict[str, object]:
    return {
        "name": name,
        "inputSha256": sha256(before),
        "outputSha256": sha256(after),
        "inputBytes": len(before),
        "outputBytes": len(after),
        "details": details,
    }


def finalize_checksum(data: bytes) -> tuple[bytes, dict[str, int]]:
    """Update PE header metadata after every instruction edit is complete."""
    offset = sound_runtime.JourneyPe(data).optional + 64
    before = struct.unpack_from("<I", data, offset)[0]
    checksum = sound_runtime.pe_checksum(data, offset)
    output = bytearray(data)
    struct.pack_into("<I", output, offset, checksum)
    return bytes(output), {"offset": offset, "before": before, "after": checksum}


def build(original: bytes) -> tuple[bytes, dict[str, object]]:
    stages: list[dict[str, object]] = []

    animated, animation_report = animation_registers.patch_image(original)
    stages.append(stage("animation sound selector registers", original, animated,
                        animation_report))

    toned, tone_plan = tone_cache.make_patch(animated)
    tone_report = tone_cache.verify(toned)
    if tone_cache.unpatch(toned) != animated:
        raise RuntimeError("Tone-cache stage did not undo to its exact input")
    stages.append(stage("Tone cache ownership and lookup", animated, toned, {
        "verification": tone_report,
        "plannedNativeBlobBytes": tone_plan["blob_bytes"],
    }))

    runtime_options = dict(SOUND_RUNTIME_OPTIONS)
    runtime, runtime_report = sound_runtime.make_patch(
        toned, **runtime_options
    )
    runtime_verify = sound_runtime.verify(runtime)
    recovered_tone, _, _ = sound_runtime.recover_base(runtime)
    if recovered_tone != toned:
        raise RuntimeError("sound-runtime stage did not undo to its exact input")
    runtime_stage_name = "room, spatial, pause, and category ducking behavior"
    stages.append(stage(runtime_stage_name, toned, runtime, {
        "build": runtime_report,
        "verification": runtime_verify,
    }))

    final, coo_report = soft_coo.transform(runtime)
    recovered_native, _ = soft_coo.transform(final, undo=True)
    if recovered_native != runtime:
        raise RuntimeError("PlayerCoo selection stage did not undo to its exact input")
    stages.append(stage("softer PlayerCoo player-chirp selection",
                        runtime, final, coo_report))
    finalized, checksum_report = finalize_checksum(final)
    stages.append(stage("final PE checksum", final, finalized, checksum_report))
    final = finalized

    # Determinism is checked by rebuilding every generated stage once.
    animated2, _ = animation_registers.patch_image(original)
    toned2, _ = tone_cache.make_patch(animated2)
    runtime2, _ = sound_runtime.make_patch(toned2, **runtime_options)
    final2, _ = soft_coo.transform(runtime2)
    final2, _ = finalize_checksum(final2)
    if final2 != final:
        raise RuntimeError("source build is not deterministic")

    report = {
        "schema": 1,
        "version": VERSION,
        "status": "built-and-verified",
        "inputSha256Provenance": sha256(original),
        "outputSha256": sha256(final),
        "inputBytes": len(original),
        "outputBytes": len(final),
        "wholeFileHashRequiredForCompatibility": False,
        "deterministic": True,
        "implementation": "native-control-owned sound repair",
        "runtimeOptions": runtime_options,
        "duckerRepairEnabled": True,
        "duckerCatchupBatched": True,
        "reverbControlWritesCached": True,
        "stages": stages,
    }
    return final, report


def write_exclusive(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if path.read_bytes() == data:
            return
        raise RuntimeError(f"refusing to overwrite a different file: {path}")
    path.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="original Journey.exe")
    parser.add_argument("--output", type=Path,
                        help="default: Journey.sound-fix.exe beside the input")
    parser.add_argument("--report", type=Path,
                        help="default: <output>.report.json")
    args = parser.parse_args()

    default_name = "Journey.sound-fix.exe"
    output = args.output or args.input.with_name(default_name)
    report_path = args.report or output.with_suffix(output.suffix + ".report.json")
    if output.resolve() == args.input.resolve():
        raise RuntimeError("refusing to overwrite the input executable")

    final, report = build(args.input.read_bytes())
    report["input"] = str(args.input)
    report["output"] = str(output)
    write_exclusive(output, final)
    write_exclusive(report_path, (json.dumps(report, indent=2) + "\n").encode())
    print(json.dumps({
        "status": report["status"],
        "output": str(output),
        "outputSha256": report["outputSha256"],
        "report": str(report_path),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
