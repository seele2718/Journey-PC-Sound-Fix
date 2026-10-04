# Build from source

The build starts from your original `Journey.exe`. No previously patched
executable or external project files are required.

## Requirements

- Python 3.10 or newer.
- `python -m pip install -r requirements.txt` (Capstone and pefile).
- MinGW-w64 x86-64 tools on `PATH`: `x86_64-w64-mingw32-gcc`, `-ld`, `-as`,
  `-objcopy` and `-nm`.

On Windows, an MSYS2 environment can provide the MinGW-w64 tools. These are
source-build requirements only; the ordinary installer uses standard Python.

From this directory, run:

```text
python build_all.py "C:\path\to\Journey.exe"
```

This writes `build/Journey.sound-fix.exe`, a build report, and a `build/payload/`
folder containing the installer manifest and three BIN files. The input is
unchanged. Generated code is rebuilt twice to check determinism.

## Repair stages

1. `animation_registers.py` passes animation sound selectors into the bank graph.
2. `tone/` repairs the native media-cache key and ownership. A requested
   playback rate is part of the identity, not discarded when media is reused.
3. `sound_runtime/` provides room response, wet spatial ancestry, native
   voice lifecycle handling, child spatial controls, pause and category ducking.
4. `soft_coo.py` enables the softer `PlayerCoo` selection.

After all four repairs, the builder recalculates the final executable's PE
header checksum. This updates file metadata, not sound behavior.

`build_sound_fix.py` selects the sound-fix configuration. It retains the
optimized category updates and cached control writes. The hook-contract table
defines the native admission, publication and retirement sites; `native_routes.py`
adds their required import and callback checks.

`build_payloads.py` extracts the built sections and localized instruction edits
for the standard-Python installer. Tone code, sound-runtime code (including
unwind records), and non-executable writable state remain separate components.
They are applied together, not copied individually into the game folder.

See [RE_BACKGROUND.md](../docs/RE_BACKGROUND.md) for address/byte semantics and
[../docs/TECHNICAL.md](../docs/TECHNICAL.md) for current routing and limitations.
