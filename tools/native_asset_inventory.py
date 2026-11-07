#!/usr/bin/env python3
"""Read-only census of BNTX texture records in compressed RomFS texture resources.

The current repository has a BNTX peer-swap helper, but not a complete image-layout
decoder. This reader reports identities and fields that can be bounded by that format
parser and labels subresource payload layout unresolved rather than guessing.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from native_asset_layout import bntx_blob_offsets, parse_bntx


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError("BNTX range outside resource")
    return data[offset:offset + size]


def bntx_records(data):
    """Return descriptors only after the layout reader bounds their mip ranges."""
    records = parse_bntx(data)
    for rec in records:
        rec["expected_subresource_count"] = rec["layers"] * rec["mip_levels"] * rec["depth"]
        rec["mip_pointer_present"] = True
    return records


def sarc_members(data):
    """Bounded SARC reader used to obtain the repository's packaged Zstandard dictionaries."""
    if data[:4] != b"SARC" or data[6:8] != b"\xff\xfe":
        raise ValueError("unsupported dictionary SARC")
    header, total, start = struct.unpack_from("<HxxII", data, 4)
    if total != len(data) or header < 20 or checked(data, header, 4) != b"SFAT":
        raise ValueError("invalid dictionary SARC")
    count = struct.unpack_from("<H", data, header + 6)[0]
    table = header + 12
    string_table = table + count * 16
    if checked(data, string_table, 4) != b"SFNT":
        raise ValueError("invalid dictionary SARC string table")
    names = string_table + 8
    result = []
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from("<4I", checked(data, table + 16 * i, 16))
        name_at = names + (attr & 0xffffff) * 4
        if attr >> 24 != 1 or not names <= name_at < start or hi < lo:
            raise ValueError("invalid dictionary SARC member")
        checked(data, start + lo, hi - lo)
        result.append(data[start + lo:start + hi])
    return result


def census(romfs_dir, scope="all"):
    """Census BNTX texture records in indexed Tex/ resources (scope "tex") or in every indexed .zs resource (scope "all").

    Records of the first blob in a container keep the asset_id they had before scope "all" existed. Records of later
    blobs use a bntx_index in their asset_id so they cannot collide.
    """
    import zstandard

    if scope not in ("tex", "all"):
        raise ValueError("scope must be tex or all")
    root = Path(romfs_dir)
    manifest = json.loads((root / "index.json").read_text(encoding="utf-8"))
    image = root / "romfs.bin"
    if manifest.get("status") != "pass" or image.stat().st_size != manifest.get("romfs_size"):
        raise ValueError("RomFS extraction manifest is absent or inconsistent")
    report = {"schema": "dk-native-asset-source-inventory", "version": 2,
              "producer": "native_asset_inventory.py", "game_build": "dkb100", "scope": scope,
              "romfs_sha256": manifest.get("romfs_sha256"), "source_resource_count": 0,
              "texture_container_count": 0, "texture_record_count": 0,
              "assets": [], "unresolved": [], "coverage_complete": False,
              # Resources whose zstd frame needs a dictionary this RomFS index does not provide. They hold no BRTI
              # records the census can read, so coverage of the game's texture set is incomplete by this count.
              "not_decodable_resources": 0, "not_decodable_examples": []}
    def read(stream, row):
        stream.seek(row["offset"])
        value = stream.read(row["size"])
        if len(value) != row["size"]:
            raise ValueError("truncated indexed RomFS resource")
        return value
    with image.open("rb") as f:
        dictionary_rows = [r for r in manifest["files"] if r["path"].lower().endswith("zsdic.pack.zs")]
        dictionaries = []
        if dictionary_rows:
            dictionary_pack = read(f, dictionary_rows[0])
            dictionary_sarc = zstandard.ZstdDecompressor().decompress(dictionary_pack, max_output_size=512 << 20)
            dictionaries = [zstandard.ZstdCompressionDict(member) for member in sarc_members(dictionary_sarc)]
    by_id = {d.dict_id(): d for d in dictionaries if d.dict_id()}
    if scope == "tex":
        candidates = [r for r in manifest["files"] if r["path"].startswith("Tex/") and r["path"].endswith(".zs")]
    else:
        candidates = [r for r in manifest["files"] if r["path"].endswith(".zs")]
    report["source_resource_count"] = len(candidates)
    with image.open("rb") as f:
        for row in candidates:
            compressed = read(f, row)
            src_hash = hashlib.sha256(compressed).hexdigest()
            asset_id = hashlib.sha256(row["path"].encode("utf-8")).hexdigest()
            try:
                frame = zstandard.get_frame_parameters(compressed)
                dictionary = by_id.get(frame.dict_id)
                raw = zstandard.ZstdDecompressor(dict_data=dictionary).decompress(compressed, max_output_size=512 << 20)
            except zstandard.ZstdError:
                report["not_decodable_resources"] += 1
                if len(report["not_decodable_examples"]) < 20:
                    report["not_decodable_examples"].append(row["path"])
                if scope == "tex" or row["path"].startswith("Tex/"):
                    report["unresolved"].append({"asset_id": asset_id, "source_sha256": src_hash,
                                                 "reason": "resource-not-decompressible-as-zstd"})
                continue
            offsets = bntx_blob_offsets(raw)
            if not offsets:
                continue
            for bntx_index, pos in enumerate(offsets):
                try:
                    records = bntx_records(raw[pos:])
                except (ValueError, struct.error) as error:
                    report["unresolved"].append({"asset_id": asset_id, "source_sha256": src_hash,
                                                 "reason": "embedded-bntx-header-or-texture-table-unsupported",
                                                 "parser_error": str(error)})
                    continue
                report["texture_container_count"] += 1
                report["texture_record_count"] += len(records)
                for rec in records:
                    if bntx_index == 0:
                        ident = hashlib.sha256((asset_id + ":" + str(rec["ordinal"]) + ":" + rec["name_hash"]).encode()).hexdigest()
                    else:
                        ident = hashlib.sha256((asset_id + ":blob" + str(bntx_index) + ":" + str(rec["ordinal"]) + ":" + rec["name_hash"]).encode()).hexdigest()
                    is_mapped = rec["format"] is not None
                    is_supported_shape = (rec["dimension_name"] == "2d" and rec["tile_mode_name"] == "block-linear" and
                                          rec["depth"] == 1)
                    conversion_ready = rec["conversion_semantics_proven"] and is_supported_shape
                    if (not rec["platform_semantics_verified"] or not is_mapped or
                        rec["tile_mode_name"] is None or rec["dimension_name"] is None):
                        status, reason = "unresolved", "unverified-platform-or-unknown-layout-enum"
                    elif not conversion_ready:
                        status, reason = "unsupported", "known-format-but-unverified-or-unsupported-layout"
                    else:
                        status, reason = "parsed", "source-ranges-resolved-conversion-not-attempted"
                    report["assets"].append({"asset_id": ident, "source_resource_id": asset_id,
                                             "source_sha256": src_hash, "container": "BNTX",
                                             "bntx_index": bntx_index, "bntx_offset": pos,
                                             "record_ordinal": rec["ordinal"],
                                             "source_path": row["path"],
                                             "descriptor_sha256": rec["descriptor_hash"],
                                             "platform": rec["platform_name"],
                                             "platform_tag_hex": rec["platform_tag_hex"],
                                             "format_code": rec["format_code"],
                                             "format": rec["format"], "color_space": rec["color_space"],
                                             "channel_mapping": rec["channel_mapping"],
                                             "surface_dim_raw": rec["surface_dim_raw"],
                                             "surface_dim_name": rec["surface_dim_name"],
                                             "dimension": rec["dimension"], "tile_mode": rec["tile_mode"],
                                             "dimension_name": rec["dimension_name"],
                                             "tile_mode_name": rec["tile_mode_name"],
                                             "block_height_gobs": rec["block_height_gobs"],
                                             "raw_block_height_log2": rec["raw_block_height_log2"],
                                             "block_height_log2": rec["block_height_log2"],
                                             "raw_alignment": rec["raw_alignment"],
                                             "alignment": rec["alignment"],
                                             "width": rec["width"], "height": rec["height"],
                                             "depth": rec["depth"], "layers": rec["layers"],
                                             "mip_levels": rec["mip_levels"],
                                             "expected_subresource_count": rec["expected_subresource_count"],
                                             "mip_ranges": rec["mip_ranges"],
                                             "subresource_ranges": rec["subresource_ranges"],
                                             "subresources_enumerated": rec["per_slice_ranges_proven"],
                                             "subresource_layout": "exact-mip-ranges" if rec["per_slice_ranges_proven"] else "mip-ranges-only-layer-depth-unsplit",
                                             "conversion_status": status, "reason": reason})
    statuses = [asset["conversion_status"] for asset in report["assets"]]
    report["layout_counts"] = {status: statuses.count(status) for status in ("parsed", "unsupported", "unresolved")}
    report["coverage_complete"] = False
    report["unresolved_categories"] = ["per-layer/depth byte-range boundaries for arrays or volumes",
                                       "unknown format codes and unsupported format/layout combinations",
                                       "Ounc tiled geometry that does not reconcile exactly"]
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("romfs", type=Path, help="verified extracted RomFS directory")
    p.add_argument("output", type=Path)
    p.add_argument("--scope", choices=("tex", "all"), default="all",
                   help="tex: Tex/ resources only (the original census); all: every indexed .zs (default)")
    a = p.parse_args()
    result = census(a.romfs, a.scope)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    counts = result["layout_counts"]
    print("native asset census: {} containers, {} records; {} parsed, {} unsupported, {} unresolved; coverage incomplete".format(
        result["texture_container_count"], result["texture_record_count"], counts["parsed"],
        counts["unsupported"], counts["unresolved"]))


if __name__ == "__main__":
    main()
