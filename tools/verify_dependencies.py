#!/usr/bin/env python3
"""Verify the checked-in runtime and headers used by both build and tests."""
from pathlib import Path
import hashlib
import json
import sys

root = Path(__file__).resolve().parents[1] / "deps"
manifest = json.loads((root / "manifest.json").read_text())
for name, expected in manifest["sha256"].items():
    path = root / name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        sys.exit(f"Dependency missing or changed: {name}")
print(f"Verified {len(manifest['sha256'])} pinned dependency files")
