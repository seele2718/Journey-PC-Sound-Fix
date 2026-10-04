#!/usr/bin/env python3
"""Build the repaired executable and the three installer payloads from source."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import build_payloads
import build_sound_fix


HERE = Path(__file__).resolve().parent
VERSION = (HERE / "VERSION").read_text().strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="original Journey.exe")
    parser.add_argument("--build-dir", type=Path, default=HERE / "build")
    args = parser.parse_args()
    original = args.input.read_bytes()
    repaired, report = build_sound_fix.build(original)
    args.build_dir.mkdir(parents=True, exist_ok=True)
    repaired_path = args.build_dir / "Journey.sound-fix.exe"
    repaired_path.write_bytes(repaired)
    payload_dir = args.build_dir / "payload"
    manifest = build_payloads.build_payloads(
        original, repaired, payload_dir, VERSION, "normal"
    )
    (args.build_dir / "build-report.json").write_text(
        json.dumps({"executable": report, "payload": manifest}, indent=2) + "\n"
    )
    print(json.dumps({
        "status": "built-from-source",
        "executable": str(repaired_path),
        "payloadDirectory": str(payload_dir),
        "sha256": report["outputSha256"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
