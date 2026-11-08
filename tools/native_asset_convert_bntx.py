#!/usr/bin/env python3
"""Convert inventory-selected Bananza BNTX textures into the native asset container."""
import argparse
import hashlib
import json
from pathlib import Path

import zstandard

from native_asset_convert import FORMATS as CONVERTER_FORMATS, make_asset_subresources, parse_asset
from native_asset_inventory import sarc_members
from native_asset_layout import FORMATS as BNTX_FORMATS, bntx_blob_offsets, parse_bntx
from native_asset_paths import require_asset_id, require_generated_output, write_report

_CONTEXTS = {}


def _context(romfs_dir, inventory_path):
    root = Path(romfs_dir).resolve()
    inventory_file = Path(inventory_path).resolve()
    key = (str(root), str(inventory_file))
    if key in _CONTEXTS:
        return _CONTEXTS[key]

    inventory = json.loads(inventory_file.read_text(encoding="utf-8"))
    assets = {row["asset_id"]: row for row in inventory.get("assets", [])}
    if len(assets) != len(inventory.get("assets", [])):
        raise ValueError("inventory contains duplicate asset ids")
    manifest = json.loads((root / "index.json").read_text(encoding="utf-8"))
    if manifest.get("status") != "pass" or manifest.get("romfs_sha256") != inventory.get("romfs_sha256"):
        raise ValueError("RomFS manifest does not match the inventory source")
    entries = manifest.get("files", [])
    resources = {}
    for row in entries:
        path = row.get("path", "")
        if path.endswith(".zs"):  # every indexed resource: inventory scope "all" names non-Tex/ resources too
            resources.setdefault(hashlib.sha256(path.encode("utf-8")).hexdigest(), []).append(row)
    dictionary_rows = [row for row in entries if row.get("path", "").lower().endswith("zsdic.pack.zs")]
    dictionaries = []
    if dictionary_rows:
        with (root / "romfs.bin").open("rb") as stream:
            dictionary_pack = _read(stream, dictionary_rows[0])
        dictionary_sarc = zstandard.ZstdDecompressor().decompress(dictionary_pack, max_output_size=512 << 20)
        dictionaries = [zstandard.ZstdCompressionDict(member) for member in sarc_members(dictionary_sarc)]
    by_id = {dictionary.dict_id(): dictionary for dictionary in dictionaries if dictionary.dict_id()}
    context = {"root": root, "assets": assets, "resources": resources, "dictionaries": by_id,
               "stream": (root / "romfs.bin").open("rb")}
    _CONTEXTS[key] = context
    return context


def _read(stream, row):
    stream.seek(row["offset"])
    value = stream.read(row["size"])
    if len(value) != row["size"]:
        raise ValueError("truncated indexed RomFS resource")
    return value


def convert_inventory_asset(romfs_dir, inventory_path, asset_id, output_path):
    require_asset_id(asset_id)
    context = _context(romfs_dir, inventory_path)
    asset = context["assets"].get(asset_id)
    if asset is None:
        raise ValueError("asset id must identify exactly one census record")
    expected = {
        "platform_tag_hex": "4f756e63", "dimension_name": "2d",
        "tile_mode_name": "block-linear", "depth": 1,
        "conversion_status": "parsed", "subresources_enumerated": True,
    }
    fmt, color_space = asset.get("format"), asset.get("color_space")
    if fmt not in CONVERTER_FORMATS or color_space not in ("linear", "srgb"):
        raise ValueError("asset format is not supported by the native converter")
    if BNTX_FORMATS.get(asset.get("format_code")) != (fmt, color_space):
        raise ValueError("inventory format code does not match the converter semantics")
    for key, value in expected.items():
        if asset.get(key) != value:
            raise ValueError(f"asset does not satisfy verified conversion profile: {key}")
    if not asset.get("subresource_ranges"):
        raise ValueError("asset subresource ranges are not fully reconciled")

    candidates = context["resources"].get(asset["source_resource_id"], [])
    if len(candidates) != 1:
        raise ValueError("inventory resource id did not resolve uniquely in RomFS index")

    compressed = _read(context["stream"], candidates[0])
    source_sha = hashlib.sha256(compressed).hexdigest()
    if source_sha != asset.get("source_sha256"):
        raise ValueError("RomFS resource hash does not match the inventory")
    frame = zstandard.get_frame_parameters(compressed)
    dictionary = context["dictionaries"].get(frame.dict_id)
    raw = zstandard.ZstdDecompressor(dict_data=dictionary).decompress(compressed, max_output_size=512 << 20)
    # Records from a later BNTX in the same container carry bntx_index (inventory scope "all"); older records
    # have none and mean the first blob, which is what the original census used.
    blobs = bntx_blob_offsets(raw)
    blob_index = asset.get("bntx_index", 0)
    if type(blob_index) is not int or blob_index < 0:
        raise ValueError("bntx_index must be a non-negative integer")
    if blob_index >= len(blobs):
        raise ValueError("indexed texture resource no longer contains the recorded BNTX blob")
    bntx_at = blobs[blob_index]
    bntx = raw[bntx_at:]
    records = parse_bntx(bntx)
    ordinal = asset["record_ordinal"]
    if type(ordinal) is not int or ordinal < 0:
        raise ValueError("record_ordinal must be a non-negative integer")
    if ordinal >= len(records):
        raise ValueError("texture ordinal is outside the parsed BNTX table")
    record = records[ordinal]
    if record["descriptor_hash"] != asset["descriptor_sha256"]:
        raise ValueError("BRTI descriptor hash does not match the inventory")
    if record["channel_mapping"] is None or record["channel_mapping"] != asset.get("channel_mapping"):
        raise ValueError("BRTI channel mapping is missing or does not match the inventory")
    if not record["conversion_semantics_proven"]:
        raise ValueError("BRTI conversion semantics are not proven")

    subresources = []
    block_width, block_height, bytes_per_block = CONVERTER_FORMATS[fmt]
    if record["subresource_ranges"] != asset["subresource_ranges"]:
        raise ValueError("BRTI subresource ranges differ from inventory")
    for sub in record["subresource_ranges"]:
        mip = record["mip_ranges"][sub["mip"]]
        start, length = sub["offset"], sub["byte_length"]
        tiled = bntx[start:start + length]
        if len(tiled) != length:
            raise ValueError("BRTI mip range is truncated")
        block_rows = (mip["height"] + block_height - 1) // block_height
        effective_block_height = record["raw_block_height_log2"]
        while effective_block_height and (8 << (effective_block_height - 1)) >= block_rows:
            effective_block_height -= 1
        subresources.append((mip["mip"], sub["layer"], mip["width"], mip["height"],
                             effective_block_height, tiled))
    artifact = make_asset_subresources(subresources, fmt, color_space == "srgb", record["channel_mapping"])
    meta, payload = parse_asset(artifact)
    expected_payload_size = sum(((max(1, meta["width"] >> mip) + block_width - 1) // block_width) *
                                ((max(1, meta["height"] >> mip) + block_height - 1) // block_height) * bytes_per_block
                                for mip in range(meta["mip_levels"])) * meta["layers"]
    if len(payload) != expected_payload_size or len(meta["subresources"]) != len(record["subresource_ranges"]):
        raise ValueError("native artifact failed exact mip-payload validation")

    output = require_generated_output(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(artifact)
    report = {
        "schema": "dk-native-asset-conversion-evidence", "version": 1,
        "producer": "native_asset_convert_bntx.py", "conversion_status": "converted",
        "asset_id": asset_id, "source_resource_id": asset["source_resource_id"],
        "source_sha256": source_sha, "descriptor_sha256": record["descriptor_hash"],
        "format_code": record["format_code"], "format": fmt,
        "color_space": color_space, "platform_tag_hex": record["platform_tag_hex"],
        "channel_mapping": record["channel_mapping"],
        "width": record["width"], "height": record["height"],
        "mip_levels": len(record["mip_ranges"]), "layers": record["layers"],
        "source_layout": "switch2-block-linear", "output_layout": meta["layout"],
        "output_size": len(artifact), "output_sha256": hashlib.sha256(artifact).hexdigest(),
        "source_mip_geometry_verified": True, "round_trip_payload_validation": "pass",
        "coverage_complete": False,
    }
    write_report(output.with_suffix(output.suffix + ".json"), report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("romfs", type=Path)
    parser.add_argument("inventory", type=Path)
    parser.add_argument("asset_id")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = convert_inventory_asset(args.romfs, args.inventory, args.asset_id, args.output)
    print("converted {} mips of {} {} texture; SHA-256 {}".format(
        report["mip_levels"], report["width"], report["format"], report["output_sha256"]))


if __name__ == "__main__":
    main()
