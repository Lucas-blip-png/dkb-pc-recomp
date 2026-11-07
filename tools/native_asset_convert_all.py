#!/usr/bin/env python3
"""Convert every census record that satisfies the verified native BNTX profile.

Outputs are resumable and restricted to build/ or generated/. Unsupported and
unresolved census records remain explicit in the coverage summary.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from native_asset_convert import FORMATS as CONVERTER_FORMATS, MAGIC, parse_asset
from native_asset_convert_bntx import convert_inventory_asset


def valid_existing(path, asset):
    sidecar = path.with_suffix(path.suffix + ".json")
    try:
        report = json.loads(sidecar.read_text(encoding="utf-8"))
        blob = path.read_bytes()
        digest = hashlib.sha256(blob).hexdigest()
        meta, _ = parse_asset(blob)
        channels = asset.get("channel_mapping")
        return (channels is not None and meta.get("channel_mapping") == channels and
                report.get("channel_mapping") == channels and report.get("asset_id") == asset["asset_id"] and
                report.get("source_sha256") == asset.get("source_sha256") and
                report.get("descriptor_sha256") == asset.get("descriptor_sha256") and
                report.get("output_sha256") == digest and report.get("round_trip_payload_validation") == "pass")
    except (OSError, json.JSONDecodeError, ValueError):
        return False


def upgrade_channel_metadata(path, asset):
    """Attach source-verified channel mapping without changing an existing payload."""
    sidecar = path.with_suffix(path.suffix + ".json")
    try:
        report = json.loads(sidecar.read_text(encoding="utf-8"))
        old_blob = path.read_bytes()
        old_hash = hashlib.sha256(old_blob).hexdigest()
        meta, payload = parse_asset(old_blob)
        mapping = asset.get("channel_mapping")
        if (not isinstance(mapping, list) or len(mapping) != 4 or
            any(type(c) is not int or c < 0 or c > 5 for c in mapping) or
            meta.get("version") != 2 or "channel_mapping" in meta or
            report.get("asset_id") != asset.get("asset_id") or
            report.get("source_sha256") != asset.get("source_sha256") or
            report.get("descriptor_sha256") != asset.get("descriptor_sha256") or
            report.get("conversion_status") != "converted" or
            report.get("output_sha256") != old_hash or report.get("round_trip_payload_validation") != "pass" or
            any(meta.get(k) != asset.get(k) for k in ("format", "color_space", "width", "height", "mip_levels", "layers"))):
            return False
        meta["channel_mapping"] = mapping
        header = json.dumps(meta, sort_keys=True, separators=(",", ":")).encode("utf-8")
        blob = MAGIC + struct.pack("<II", 2, len(header)) + header + payload
        if parse_asset(blob)[1] != payload:
            raise ValueError("metadata upgrade changed the texture payload")
        report.update({"channel_mapping": mapping, "prior_artifact_sha256": old_hash,
                       "metadata_upgrade": "channel-mapping-from-matched-descriptor",
                       "output_size": len(blob), "output_sha256": hashlib.sha256(blob).hexdigest()})
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_bytes(blob)
        tmp.replace(path)
        sidecar.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return valid_existing(path, asset)
    except (OSError, json.JSONDecodeError, ValueError, KeyError):
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("romfs", type=Path)
    ap.add_argument("inventory", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--limit", type=int, help="process at most this many eligible records")
    args = ap.parse_args()
    out = args.output.resolve()
    if not any(out.is_relative_to((ROOT / base).resolve()) for base in ("build", "generated")):
        raise SystemExit("output must be under build/ or generated/")
    out.mkdir(parents=True, exist_ok=True)
    inventory_bytes = args.inventory.read_bytes()
    inventory = json.loads(inventory_bytes)
    source_manifest = json.loads((args.romfs / "index.json").read_text(encoding="utf-8"))
    if source_manifest.get("status") != "pass" or source_manifest.get("romfs_sha256") != inventory.get("romfs_sha256"):
        raise SystemExit("RomFS manifest does not match the inventory source")
    rows = inventory.get("assets", [])
    eligible = [row for row in rows if row.get("conversion_status") == "parsed" and row.get("format") in CONVERTER_FORMATS]
    if args.limit is not None:
        if args.limit < 1:
            raise SystemExit("--limit must be positive")
        eligible = eligible[:args.limit]

    ledger = out / "conversion-results.jsonl"
    converted = upgraded = skipped = failed = 0
    with ledger.open("a", encoding="utf-8", newline="\n") as log:
        for index, row in enumerate(eligible, 1):
            asset_id = row["asset_id"]
            artifact = out / (asset_id + ".dkntex")
            if valid_existing(artifact, row):
                skipped += 1
                continue
            try:
                if upgrade_channel_metadata(artifact, row):
                    upgraded += 1
                    continue
                result = convert_inventory_asset(args.romfs, args.inventory, asset_id, artifact)
                if not valid_existing(artifact, row):
                    raise ValueError("written artifact failed output-hash or round-trip sidecar verification")
                result["artifact"] = artifact.name
                log.write(json.dumps(result, sort_keys=True) + "\n")
                log.flush()
                converted += 1
            except Exception as exc:
                log.write(json.dumps({"asset_id": asset_id, "conversion_status": "failed", "reason": str(exc)}, sort_keys=True) + "\n")
                log.flush()
                failed += 1
            if index % 100 == 0 or index == len(eligible):
                print(f"processed {index}/{len(eligible)}: converted={converted}, already-valid={skipped}, failed={failed}", flush=True)

    valid_count = skipped + upgraded + converted
    status_counts = {}
    for row in rows:
        status = row.get("conversion_status", "missing")
        status_counts[status] = status_counts.get(status, 0) + 1
    summary = {
        "schema": "dk-native-asset-batch-conversion", "version": 1,
        "producer": "tools/native_asset_convert_all.py",
        "inventory_sha256": hashlib.sha256(inventory_bytes).hexdigest(),
        "inventory_records": len(rows), "inventory_status_counts": status_counts,
        "inventory_coverage_complete": inventory.get("coverage_complete") is True,
        "eligible_records_in_run": len(eligible), "verified_artifacts_in_run": valid_count,
        "converted_this_run": converted, "metadata_upgraded_this_run": upgraded,
        "skipped_existing": skipped, "failed_this_run": failed,
        "complete_for_run_scope": valid_count == len(eligible),
        "eligible_conversion_complete": valid_count == len(eligible) and args.limit is None,
        "full_inventory_conversion_complete": len(eligible) == len(rows) and valid_count == len(eligible) and args.limit is None,
        "unresolved_inventory_records": inventory.get("unresolved"),
        "unsupported_categories": inventory.get("unresolved_categories"),
        "results_ledger": ledger.name,
    }
    summary_path = out / "coverage-summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["complete_for_run_scope"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
