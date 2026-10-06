# Journey PC Sound Fix

Journey PC Sound Fix is a mod by Seele that brings Journey’s PC audio closer
to the original PlayStation sound experience.

Version **0.3.1**.
This folder contains the Journey PC sound fix,
its installer, and the source used to build it.

### Windows compatibility in 0.3.1

Version 0.3.0 had a gap in the executable's PE memory layout that prevented
Windows from loading it. Version 0.3.1 corrects the section layout to meet
Windows' requirements. The sound-processing code and behavior are unchanged.

## What it changes

- Restores environmental echoes and room response, including pipes and goo.
- Makes child sounds inherit the appropriate distance, position and pause
  behavior—for example, distant companion footsteps and cape movement.
- Restores animation sound selectors used for different surfaces and states—for
  example, different footstep sounds on sand, snow, and stone.
- Keeps embedded sounds with different authored playback rates distinct.
- Makes category ducking affect the repaired room response as well as dry sound.
- Enables the softer player coo for the PC player-chirp input path.

The implementation follows the game's existing sound ownership and update
boundaries. Shared spatial processing and reduced redundant control writes
limit the extra work required for room response.

## Install

1. Keep a backup of your original `Journey.exe`.
2. Install Python 3.10 or newer if needed.
3. Drag your original `Journey.exe` onto `Install.bat`, or run:

   ```text
   python install.py "C:\path\to\Journey.exe"
   ```

4. The script creates `Journey.sound-fix.exe` beside the original. With the
   game closed, replace the game's `Journey.exe` with that file, retaining
   your backup. Restore the backup to undo the executable repair.

The installer needs only standard Python. It never overwrites the input.
It checks the executable layout, affected instructions and payload integrity;
it does not require an identical whole-file hash. Unrelated same-size embedded
Lua edits can remain intact, provided the checked areas are unchanged.

Use the game's original `fmod64.dll`. The repair targets the supported Windows
x64 PC build; a different executable layout is not automatically compatible.

## Audio files

Music and SFX bank/media patches are separate, manually installed downloads.
They are not included or installed by this folder. Existing level-music fixes
can be used alongside this executable repair. Follow the selected SFX package's
pairing instructions when choosing between native-Tone and standalone variants.

## Status and limits

This fix does not claim perfect console parity.

The repaired room path supports mono and stereo output. Surround room response
(such as 5.1/7.1) is not supported; unsupported layouts retain the original path.
The reconstructed processing expects a 48 kHz runtime mix. Existing game,
device or compatibility-layer audio problems may remain.

## Source

The three small files under `payload/` are compiled repair components, not a
complete game executable. The readable implementation and rebuild instructions
are in [source/README.md](source/README.md). [Technical notes](docs/TECHNICAL.md)
explain how the pieces fit together. No game executable or original bank/media
files are supplied here.
