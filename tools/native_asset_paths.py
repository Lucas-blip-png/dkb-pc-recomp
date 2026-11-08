"""Shared output rules of the native asset tools.

The inventory holds game file paths and hashes, a converted texture holds deswizzled game pixels, and the coverage
files and ledgers describe both. None of that may reach a tracked folder, so every tool that writes goes through
require_generated_output (build/ or generated/ of this repository only, with `..` and symlinks resolved), and every
file it writes carries GENERATED_MARKER, which tools/check_content.py refuses to let into a commit.
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The same value as GENERATED_MARKER in tools/check_content.py, built in two pieces so that this file does not contain it.
GENERATED_MARKER = "TOTK_RECOMP_" + "GENERATED_FROM_GAME_CODE"
ASSET_ID = re.compile(r"^[0-9a-f]{64}$")


def require_generated_output(path, root=None):
    """The resolved path, if it lies inside build/ or generated/ of the repository (or of `root`, for tests)."""
    base = ROOT if root is None else Path(root).resolve()
    resolved = Path(path).resolve()
    for folder in ("build", "generated"):
        try:
            resolved.relative_to((base / folder).resolve())
            return resolved
        except ValueError:
            continue
    raise ValueError("generated asset output must be under build/ or generated/")


def require_asset_id(value):
    """An asset id is a lowercase SHA-256 hex digest; anything else could name a path outside the output folder."""
    if not isinstance(value, str) or not ASSET_ID.match(value):
        raise ValueError("asset id must be 64 lowercase hex digits")
    return value


def marked(report):
    """A copy of a JSON report with the generated-data marker."""
    out = dict(report)
    out["generated_marker"] = GENERATED_MARKER
    return out


def write_report(path, report, root=None):
    """Write a JSON report (marker included) to a guarded output path and return the resolved path."""
    out = require_generated_output(path, root)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(marked(report), indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return out
