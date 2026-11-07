#!/usr/bin/env python3
"""Reconcile full BNTX census rows with completed source-backed conversions."""
import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from native_asset_convert import FORMATS as CONVERTER_FORMATS
from native_asset_coverage import inventory as build_coverage


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("inventory", type=Path)
    ap.add_argument("batch_summary", type=Path)
    ap.add_argument("converted_dir", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    inv_bytes = args.inventory.read_bytes()
    inv = json.loads(inv_bytes)
    batch_bytes = args.batch_summary.read_bytes()
    batch = json.loads(batch_bytes)
    if batch.get("producer") != "tools/native_asset_convert_all.py" or batch.get("inventory_sha256") != sha(inv_bytes):
        raise SystemExit("batch summary does not match this inventory")
    if batch.get("eligible_conversion_complete") is not True:
        raise SystemExit("supported conversion batch is incomplete")
    out = args.output.resolve()
    if not any(out.is_relative_to((ROOT / base).resolve()) for base in ("build", "generated")):
        raise SystemExit("output must be under build/ or generated/")

    records = []
    converted = 0
    for asset in inv.get("assets", []):
        asset_id = asset["asset_id"]
        supported_profile = (asset.get("conversion_status") == "parsed" and
                             asset.get("format") in CONVERTER_FORMATS and
                             asset.get("dimension_name") == "2d" and asset.get("subresources_enumerated") is True and
                             asset.get("depth") == 1 and asset.get("tile_mode_name") == "block-linear")
        artifact = args.converted_dir / (asset_id + ".dkntex")
        sidecar_path = artifact.with_suffix(artifact.suffix + ".json")
        sidecar = None
        if supported_profile and sidecar_path.is_file() and artifact.is_file():
            sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
            if (sidecar.get("asset_id") != asset_id or sidecar.get("conversion_status") != "converted" or
                sidecar.get("round_trip_payload_validation") != "pass" or asset.get("channel_mapping") is None or
                sidecar.get("channel_mapping") != asset.get("channel_mapping")):
                raise SystemExit(f"invalid conversion sidecar for {asset_id}")
            converted += 1
        if supported_profile:
            layout = "complete-mip-layer-grid" if asset.get("layers", 1) > 1 else "switch2-block-linear-2d"
        elif asset.get("layers", 1) > 1:
            layout = "array"
        elif asset.get("depth", 1) > 1 or asset.get("dimension_name") == "3d":
            layout = "3d"
        else:
            layout = "unverified-source-layout"
        records.append({"asset_id": asset_id, "format": asset.get("format"), "layout": layout,
                        "converted": sidecar is not None,
                        "artifact_sha256": sidecar.get("output_sha256") if sidecar else None})

    if converted != batch.get("verified_artifacts_in_run") or len(records) != batch.get("inventory_records"):
        raise SystemExit("converted sidecars do not reconcile with batch summary counts")
    coverage = build_coverage(records)
    coverage.update({
        "game_build": inv.get("game_build"),
        "romfs_sha256": inv.get("romfs_sha256"),
        "inventory_sha256": sha(inv_bytes),
        "batch_summary_sha256": sha(batch_bytes),
        "source_inventory_records": len(records),
        "converted_supported_records": converted,
        "unsupported_records": batch.get("inventory_status_counts", {}).get("unsupported", 0),
        "unresolved_records": batch.get("inventory_status_counts", {}).get("unresolved", 0),
        "coverage_complete": (inv.get("coverage_complete") is True and converted == len(records) and not batch.get("inventory_status_counts", {}).get("unsupported")
                              and not batch.get("inventory_status_counts", {}).get("unresolved")),
        "coverage_basis": "complete BNTX census reconciled to the batch converter's full output hash scan",
    })
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(coverage, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    counts = {}
    for row in coverage["observed_assets"]:
        counts[row["status"]] = counts.get(row["status"], 0) + 1
    print(json.dumps({"output": str(out), "records": len(records), "status_counts": counts,
                      "coverage_complete": coverage["coverage_complete"]}, indent=2))
    return 0 if counts.get("converted", 0) == converted else 1


if __name__ == "__main__":
    raise SystemExit(main())
