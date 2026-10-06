# Implementation overview

The sound fix uses the game's native sound-control ownership boundaries. Its room
renderer, descriptor sends, output adaptation, animation selectors, native Tone
cache and player-coo selection retain the selected behavior. It does not add
new gain adjustments or change any bank/media files.

## What the repairs do

- **Animation selectors:** pass the game's existing surface and state choices
  into sound-bank graphs. This restores choices such as sand, snow and stone
  footsteps; it does not replace their recordings.
- **Tone cache:** keep embedded media requested at different playback rates
  distinct, even when those requests refer to the same stored sound. This
  preserves authored pitch variations instead of reusing the wrong cached rate.
- **Room response:** reconstruct the reverberant, or *wet*, signal alongside
  the direct, or *dry*, sound. Authored sends and ancestor distance, gain and
  pause behavior determine what reaches the room. Pausing an input is separate
  from cutting off echoes that have already entered the room.
- **Child spatial controls:** propagate the appropriate parent controls to
  child sounds, including the dry path. This prevents distant companion
  movement from remaining as loud as nearby movement.
- **Category ducking:** carry the game's temporary category-volume reductions
  into the repaired wet route. This is separate from a music bank's authored
  crossfades and transition schedule.
- **Player coo:** select the softer short player-chirp variant on the PC input
  path. This does not add controller-pressure sensing.

## Ownership and scheduling

Native cue admission runs under Journey's existing protected sound-worker
boundary. Identity tickets distinguish reused cues and groups. Tone and Stream
routes are prepared before their native unpause calls; Stream routes retain
their actual native parent rather than reconstructing it from a cue pointer.
Native END callbacks retire the matching generation while preserving original
callbacks and userdata. Reclamation waits for DSP destruction acknowledgment.

Sources may share an independently owned spatial-ancestor chain. Sharing is
qualified by owner generation and destination; source-specific descriptor gains
remain private. A stopped source cannot own storage still used by another source.
Branch-child spatial handling is distinct from physically nested Start Child
groups. Branch/category bookkeeping follows protected admission/update.

The runtime preserves the game's existing update sequencing, including the
required initial spatial publication. Cached writes suppress redundant
patch-owned controls without caching native control reads.

## Components

| Component | Purpose |
| --- | --- |
| `tone-code.bin` / `.tna1` | Bank-owned Tone-cache repair. |
| `sound-runtime.bin` / `.jrvx` | Runtime code, constants, and Windows unwind table. |
| `sound-state.bin` / `.jrvd` | Writable non-executable bootstrap state. Larger routing state is allocated once on the heap. |
| Manifest records | Localized original-image instructions and PE header updates. |

The code section's virtual size includes the zero-filled reserved span up to
the state section. Its raw file size contains only the actual payload and
recovery metadata, rounded to file alignment. Both builder and installer reject
gaps or overlaps between virtual sections; Windows requires adjacent sections.

Version 0.3.0 left a `0xa000`-byte virtual gap between `.jrvx` and `.jrvd`.
Version 0.3.1 extends `.jrvx`'s virtual size to cover that reserved span, which
Windows zero-fills. Section addresses, executable sound code, hooks and audio
constants are unchanged. The PE checksum and recovery metadata are updated;
recovery reads the file-backed metadata rather than the expanded virtual span.
This corrects Windows loader compatibility, not sound behavior.

The build omits timing, event and file-log instrumentation. Functional
identity/lifetime checks and the read-only FMOD compatibility check remain enabled.

## Verification and limits

Compatibility checks cover affected instruction bytes, native imports, helper
anchors and PE layout. Source-stage undo and deterministic reconstruction are
checked by the builder. Whole-file input hashes are provenance, not an allowlist.
Do not substitute a different FMOD binary or apply this to an unrecognized
Journey layout.

The custom room processing expects a 48 kHz mix and supports mono/stereo output.
Surround room processing is not implemented. Unsupported output layouts fall
back rather than being assigned guessed channel maps. A device change can still
exercise the game's own output-device behavior; this is not a device-switch fix.

Fixed-capacity association tables remain bounded. Offline lifecycle and routing
tests plus gameplay feedback support this implementation, not proof of every
possible workload.
