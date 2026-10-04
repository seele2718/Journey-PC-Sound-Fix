#!/usr/bin/env python3
"""Verify the localized PC hook contracts used by the sound repair.

This is deliberately a read-only audit.  It does not patch Journey.exe.  The
full-file SHA-256 is reported as evidence, while acceptance is based on the
exact local instruction and a unique surrounding byte window.  That is the
contract used by the patcher to coexist with unrelated EXE/Lua changes
without requiring an identical whole-file hash.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path


# Preferred base from Journey's PE32+ optional header. Hook records below use
# virtual addresses (VA); subtracting this value gives the PE-relative RVA.
IMAGE_BASE = 0x140000000


@dataclass(frozen=True)
class Hook:
    """One machine-code boundary and the semantics its wrapper must preserve.

    original_hex is the exact displaced instruction sequence, not an opaque
    patch payload. continuation_va is the first untouched original instruction.
    The prose contract documents live registers, stack locations, and flags
    that cannot be inferred safely from the bytes alone.
    """
    name: str
    va: int
    original_hex: str
    continuation_va: int
    wrapper_contract: str


HOOKS = (
    Hook(
        'audio_system_post_init',
        0x1402be21b,
        'ff153f502900',
        0x1402be221,
        'Seed under the native semaphore; forward ReleaseSemaphore arguments.',
    ),
    Hook(
        'fmod_system_update',
        0x1402be429,
        'ff15195b2900',
        0x1402be42f,
        'Mirror live cue/category/master gains and reclaim ended routes, then call the original FMOD System::update IAT entry.',
    ),
    Hook(
        'reverb_barn_post_update',
        0x14012517f,
        '488b9c2458010000',
        0x140125187,
        'Publish SFX/music preset mailboxes and SFX transition gain while RBX is the ReverbBarn, then reproduce mov rbx,[original rsp+0x158].',
    ),
    Hook(
        'tone_descriptor_capture',
        0x1402c0dd4,
        '410fb75212',
        0x1402c0dd9,
        'Save the live v12 Tone descriptor from R10 in the dispatcher case-local slot [RBP+0x48], then reproduce movzx edx,word ptr [r10+0x12].',
    ),
    Hook(
        'tone_channel_create',
        0x1402c0eec,
        'e81ff4ffff',
        0x1402c0ef1,
        'Call original Tone creator with RCX=cue, RDX=SoundRecord, R8=voice params. Carry cue and the v12 Tone descriptor from [RBP+0x48] in wrapper-owned stack arguments for attachment at the pre-unpause hook; descriptor dry/wet are +0x18/+0x1c. Preserve the original RAX return.',
    ),
    Hook(
        'stream_main_create',
        0x1402c1399,
        'e8223b0000',
        0x1402c139e,
        'Snapshot caller descriptor [RBP-0x50] and cue R14 before the original Stream factory can reuse its pool. Carry that snapshot to the pre-unpause hook, retaining Stream+0x48 as the parent group; preserve the factory RAX return.',
    ),
    Hook(
        'stream_predecessor_channel_create',
        0x1402c42fc,
        'ff15fefb2800',
        0x1402c4302,
        'Snapshot the originating Stream route from job RBP (+0x08 Stream, +0x18 cue); call original FMOD playSound with all five arguments intact. Publish the returned paused predecessor and its END-only retirement callback before native seek/delay/fades/unpause continue.',
    ),
    Hook(
        'stream_successor_create',
        0x1402c43bb,
        'e8000b0000',
        0x1402c43c0,
        'Snapshot the origin route from job RBP (+0x08 Stream, +0x18 cue) before pool reuse; carry it through the original Stream factory to the pre-unpause hook. Preserve the factory RAX return and actual successor Stream parent group.',
    ),
    Hook(
        'ducker_positive_operation',
        0x1402c2718,
        'e853e0ffff',
        0x1402c271d,
        'Configure the native global-ID state from R14/RDX, falling back to the original helper when unavailable.',
    ),
    Hook(
        'ducker_negative_operation',
        0x1402c268d,
        'e8dee0ffff',
        0x1402c2692,
        'Retire the identity associated with R14 and RDX=3*local-index, with stock fallback when unresolved.',
    ),
    Hook(
        'ducker_reset_forget',
        0x1402bfa0e,
        'e85d0d0000',
        0x1402bfa13,
        'With a healthy adapter, suppress the original reset fade and defer cue-local association cleanup to protected whole-use retirement. RDI/R14D carry cue/local index; retain the original helper fallback if the adapter is unavailable.',
    ),
    Hook(
        'native_cue_admission',
        0x1402c3158,
        'ff15b20d2900',
        0x1402c315e,
        'Native ownership adapter: journey_event_cue_unpause',
    ),
    Hook(
        'native_cue_retirement',
        0x1402bf011,
        'ff1509502900',
        0x1402bf017,
        'Native ownership adapter: journey_event_cue_stop',
    ),
    Hook(
        'tone_before_unpause',
        0x1402c06b0,
        'ff155a382900',
        0x1402c06b6,
        'Native ownership adapter: journey_event_tone_pre_unpause',
    ),
    Hook(
        'stream_before_unpause',
        0x1402c4a49,
        'ff15c1f42800',
        0x1402c4a4f,
        'Native ownership adapter: journey_event_stream_pre_unpause',
    ),
    Hook(
        'tone_callback_registration',
        0x1402c04e1,
        'ff15b1392900',
        0x1402c04e7,
        'Native ownership adapter: journey_event_register_tone_callback',
    ),
    Hook(
        'stream_callback_registration',
        0x1402c49ea,
        'ff15a8f42800',
        0x1402c49f0,
        'Native ownership adapter: journey_event_register_stream_callback',
    ),
)


class PeImage:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe : pe + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        machine, count = struct.unpack_from("<HH", self.data, pe + 4)
        if machine != 0x8664:
            raise ValueError(f"expected AMD64 PE, got machine 0x{machine:04x}")
        optional_size = struct.unpack_from("<H", self.data, pe + 20)[0]
        optional = pe + 24
        if struct.unpack_from("<H", self.data, optional)[0] != 0x20B:
            raise ValueError("expected PE32+ optional header")
        self.image_base = struct.unpack_from("<Q", self.data, optional + 24)[0]
        section_table = optional + optional_size
        self.sections: list[tuple[str, int, int, int, int]] = []
        for index in range(count):
            off = section_table + index * 40
            name = self.data[off : off + 8].rstrip(b"\0").decode("ascii")
            virtual_size, rva, raw_size, raw_off = struct.unpack_from(
                "<IIII", self.data, off + 8
            )
            self.sections.append((name, rva, virtual_size, raw_off, raw_size))

    def va_to_offset(self, va: int) -> int:
        rva = va - self.image_base
        for _name, base, virtual_size, raw_off, raw_size in self.sections:
            if base <= rva < base + max(virtual_size, raw_size):
                return raw_off + (rva - base)
        raise ValueError(f"VA 0x{va:x} is outside mapped sections")

    def read_va(self, va: int, size: int) -> bytes:
        off = self.va_to_offset(va)
        return self.data[off : off + size]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--context", type=int, default=16)
    args = parser.parse_args()

    pe = PeImage(args.image)
    if pe.image_base != IMAGE_BASE:
        raise SystemExit(
            f"wrong image base 0x{pe.image_base:x}; expected 0x{IMAGE_BASE:x}"
        )

    rows = []
    all_ok = True
    for hook in HOOKS:
        expected = bytes.fromhex(hook.original_hex)
        actual = pe.read_va(hook.va, len(expected))
        context_va = hook.va - args.context
        context = pe.read_va(context_va, args.context + len(expected) + args.context)
        occurrence_count = pe.data.count(context)
        exact = actual == expected
        unique = occurrence_count == 1
        all_ok &= exact and unique
        rows.append(
            {
                "name": hook.name,
                "va": f"0x{hook.va:x}",
                "file_offset": f"0x{pe.va_to_offset(hook.va):x}",
                "original_hex": actual.hex(),
                "expected_hex": expected.hex(),
                "exact_instruction_match": exact,
                "context_va": f"0x{context_va:x}",
                "context_hex": context.hex(),
                "context_occurrences_in_file": occurrence_count,
                "unique_context": unique,
                "continuation_va": f"0x{hook.continuation_va:x}",
                "wrapper_contract": hook.wrapper_contract,
            }
        )

    report = {
        "schema": "journey.pc-reverb-hook-contracts.v1",
        "image": str(args.image.resolve()),
        "sha256": hashlib.sha256(pe.data).hexdigest(),
        "image_base": f"0x{pe.image_base:x}",
        "acceptance_policy": "Report the whole-file identity, but require every exact local instruction and unique context. The patcher does not require a full-file hash match.",
        "all_contracts_pass": all_ok,
        "hooks": rows,
    }
    print(json.dumps(report, indent=2))
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
