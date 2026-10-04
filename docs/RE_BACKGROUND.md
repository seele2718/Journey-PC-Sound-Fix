# Reverse-engineering background for the source

The repair is intentionally written close to the machine contracts it changes.
That makes the build verifiable, but it also means that several kinds of
hexadecimal value appear in the source. They are not interchangeable.

## How to read the hexadecimal values

| Example | Meaning |
|---|---|
| `0x1402BE221` | A preferred virtual address (VA) in the supported 64-bit PC executable. |
| `0x2BE221` | A relative virtual address (RVA): the same location measured from image base `0x140000000`. |
| `[r10+0x12]` | A field at byte offset `0x12` in the object currently held in `r10`. |
| `4c8b642470` | Original x86-64 instruction bytes used as a local compatibility fingerprint. |
| `0x3f800000` | The exact IEEE-754 binary32 bit pattern for `1.0f`. |
| `0x44102550` | A bit set. Here, each set bit selects one authored Ducker category. |

The patcher reports a whole-file SHA-256 for provenance, but it accepts an
input by its PE layout, exact original instructions, and unique local context.
This lets unrelated metadata and embedded-data edits coexist with the repair
without letting a merely similar executable reach the wrong function.

## PC address model

The supported executable is a PE32+ x86-64 image with preferred image base
`0x140000000`. Source files use:

- a **VA** when an assembly bridge must call an existing Journey function;
- an **RVA** when parsing or editing a PE independently of its runtime load
  address;
- a **file offset** only while reading or writing bytes on disk.

An original-image hash in a source comment identifies the binary used to audit
that layout, not the resulting patched EXE or an installation allowlist. The
Tone signature file identifies the intermediate image after the animation
selector repair; the top-level builder applies those stages in that order.

This supported patched image uses the fixed preferred base; do not assume the
added code is generally relocatable. The linker relocation inventory is audited
at build time to check that fixed references target mapped Journey locations or
retained payload sections. Relative calls and jumps remain position-relative.

## Hook contracts

`sound_runtime/audit_pc_reverb_hook_contracts.py` records the seventeen-site
map: startup and native admission, retirement, and pre-unpause boundaries.
`sound_runtime/native_routes.py` adds the required import and callback checks.
Each hook entry records:

1. the semantic boundary, such as “a Stream channel was just created”;
2. the original bytes that must still be present;
3. the first untouched continuation address; and
4. the register/stack behavior the wrapper must preserve.

`sound_runtime/pc_reverb_patch_hooks.s` implements those contracts. A wrapper
does not merely call new code: it also reproduces the instruction displaced by
the hook and preserves any flags consumed by the following original branch.

Startup initializes the repair and seeds existing group identities before
forwarding the original semaphore release at `0x1402BE21B`. The following
instruction at `0x1402BE221` is the untouched continuation, not a post-release
initialization hook. Cue bookkeeping follows protected admission and later
control-side retirement; an END callback only retires the matching identity.

The `.inc` files contain C source included by the runtime, not separate scripts
executed by the game. `native_cue_release.inc` and `native_voice_release.inc`
clean up patch-owned routing objects, retaining unfinished cleanup for retry
and waiting for DSP destruction acknowledgments before slot reuse. They do not
choose the music's authored fade-out or transition timing.

The short byte strings in `EXPECTED_FIXED_TARGETS`, hook records, animation
signatures, and player-chirp signatures are compatibility guards. They are not
audio data and are not copied into a sound buffer.

## Tone-cache repair

Embedded Tone media is addressed by two authored descriptor fields:

- descriptor `+0x44`: signed payload-relative media offset;
- descriptor `+0x4c`: requested playback frequency, converted to an integer.

The repaired cache key is:

```text
(uint32 frequency << 32) | uint32 media_offset
```

The bank-owned red/black-tree node used by the original engine is:

| Offset | Meaning |
|---:|---|
| `+0x00`, `+0x08`, `+0x10` | left, parent, and right links |
| `+0x18` | red/black color byte |
| `+0x19` | sentinel/header flag |
| `+0x20` | repaired 64-bit identity key |
| `+0x28` | associated SoundRecord pointer |

The `.set` expressions at the start of `tone_cache_native_a.s` turn known
Journey RVAs into position-relative branch targets for the appended section.
Comments beside the fragments explain which original loader, creator, or
playback boundary each target resumes.

## Room-response and spatial runtime

The sound runtime keeps bootstrap state in appended writable `.jrvd` and its
larger routing/identity tables in a one-time heap allocation, rather than
borrowing undocumented padding from Journey objects.
Conceptually, it tracks:

- a Journey cue and its original FMOD source group;
- a separate wet group containing the reconstructed room path;
- the authored dry/wet send values from Tone or Stream descriptors;
- spatial ancestors whose distance/pan processing must also affect the wet
  input; and
- Ducker identities shared by all active cues in an authored category.

The original dry path is left in place. The new runtime reconstructs the
console-style wet path and applies the verified shared output stages to the
combined result.

`JOURNEY_DUCKER_TARGET_MASK` has bits 4, 6, 8, 10, 13, 20, 26, and 30 set.
Those are the authored categories observed to participate in the console
Ducker contract; it is not a volume scalar.

## Exact binary32 constants

Several native DSP operations are sensitive to binary32 rounding and
evaluation order. Writing `0.9636834f` in C can be readable, but a compiler may
round a decimal spelling or reassociate an expression differently. Where exact
native behavior matters, the source stores the original 32-bit representation
and converts it with `mk_f()`.

Useful reference values are:

| Bits | Value |
|---:|---:|
| `0x3f800000` | `1.0f` |
| `0xbf800000` | `-1.0f` |
| `0x3f000000` | `0.5f` |
| `0xc0c00000` | `-6.0f` |

In `reverb_native_master_fixed48.h`, the `words` table is an exact serialized
initial state for the one supported native Master configuration: 48 kHz,
256-frame stereo, unity upstream gain, and Journey's authored MasterBuss.
Its major regions are:

| State range | Semantic stage |
|---:|---|
| `0x00c0`–`0x06bf` | four-band, two-half filter bank |
| `0x06d0` onward | linked RMS compressor |
| `0x18e0` onward | 49-frame look-ahead limiter |
| `0x1ac0`/`0x1b00` | final per-slot gain and gain delta |

Repeated words normally represent the same parameter in parallel native
lanes. Keeping the table in exact bits prevents a documentation improvement
from changing the accepted output.

`resampler_coefficients.f32le.hex` holds 1,024 exact binary32 filter weights,
encoded as little-endian bytes in hexadecimal text. The room renderer uses
these to convert its 48 kHz input to 24 kHz internally and its output back to
48 kHz. The builder validates and embeds the decoded 4 KB table; it is not
recorded sound, and it does not rewrite game media or resample the whole mix.

## Generated manifests

`payload/manifest.json` is generated, so JSON comments would make it invalid.
Its `before`, `after`, and `context` hex strings describe localized changes in
the original-sized PE image. The human meaning of executable hooks lives in
the source-stage patchers and the hook-contract table; the manifest is the
installer's byte-level verification record.

The PE header checksum is finalized after all instruction edits. It is a
Windows executable metadata field, not the SHA-256 compatibility fingerprint
or a sound parameter. The manifest carries the reference image's final
checksum; the installer recalculates it for compatible inputs with unrelated
edits so those edits remain intact.

## Confidence boundary

Names in this public source describe behavior supported by static
decompilation, exact instruction execution, deterministic offline fixtures,
and the recorded gameplay tests used to select the current build. Numeric
addresses remain build-specific and are never treated as universal Journey
addresses. The installer/source patcher rejects incompatible local signatures,
imports, layout, or displaced instructions rather than guessing. Separately,
the running repair validates FMOD's identity before using its private
publication functions; that runtime gate is not an EXE installation check.
