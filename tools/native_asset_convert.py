#!/usr/bin/env python3
"""Convert one Switch 2 block-linear 2D texture level into a versioned native asset."""
import argparse
import json
import struct
from pathlib import Path

MAGIC = b"DKNATEX\0"
VERSION = 1
ASTC_BLOCKS = ((4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6),
               (8, 8), (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12))
FORMATS = {
    "rgba8": (1, 1, 4), "r16-uint": (1, 1, 2), "b10g11r11-ufloat": (1, 1, 4), "rgba16f": (1, 1, 8), "bc1": (4, 4, 8), "bc2": (4, 4, 16), "bc3": (4, 4, 16),
    "bc4": (4, 4, 8), "bc4-snorm": (4, 4, 8), "bc5": (4, 4, 16), "bc5-snorm": (4, 4, 16),
    "bc6h-sfloat": (4, 4, 16), "bc6h-ufloat": (4, 4, 16), "bc7": (4, 4, 16),
}
for _bw, _bh in ASTC_BLOCKS:
    FORMATS[f"astc-{_bw}x{_bh}"] = (_bw, _bh, 16)


def gob_offset(x, y):
    return (x % 64 // 32) * 256 + (y % 8 // 4) * 128 + (x % 32 // 16) * 64 + (y % 4 // 2) * 32 + (y % 2) * 16 + x % 16


def convert(src, width, height, fmt, block_height, to_linear=True):
    if fmt not in FORMATS or width <= 0 or height <= 0 or not 0 <= block_height <= 5:
        raise ValueError("unsupported format or invalid dimensions/block height")
    bw, bh, bpb = FORMATS[fmt]
    rows = (height + bh - 1) // bh
    row_bytes = ((width + bw - 1) // bw) * bpb
    slices = (row_bytes + 63) // 64
    gob_h = 1 << block_height
    block_rows = (rows + 8 * gob_h - 1) // (8 * gob_h)
    required = slices * block_rows * 512 * gob_h
    linear_size = row_bytes * rows
    expected_source_size = required if to_linear else linear_size
    if len(src) != expected_source_size:
        raise ValueError(f"source length {len(src)} != expected {expected_source_size}")
    out = bytearray(linear_size if to_linear else required)
    for y in range(rows):
        for x in range(row_bytes):
            offset = (y // (8 * gob_h)) * slices * 512 * gob_h + (x // 64) * 512 * gob_h + (y % (8 * gob_h) // 8) * 512 + gob_offset(x, y)
            if to_linear:
                out[y * row_bytes + x] = src[offset]
            else:
                out[offset] = src[y * row_bytes + x]
    return bytes(out)


def make_asset(src, width, height, fmt, srgb, block_height, mip_levels=1, layers=1):
    if mip_levels != 1 or layers != 1:
        raise ValueError("use make_asset_subresources for multiple mips or layers")
    pixels = convert(src, width, height, fmt, block_height)
    bw, bh, bpb = FORMATS[fmt]
    meta = {"schema": "dk-native-texture", "version": VERSION, "width": width, "height": height,
            "format": fmt, "color_space": "srgb" if srgb else "linear", "mip_levels": 1,
            "layers": 1, "block_width": bw, "block_height": bh, "bytes_per_block": bpb,
            "layout": "linear-block-rows", "source_layout": "switch2-block-linear", "source_block_height_gobs": block_height}
    header = json.dumps(meta, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return MAGIC + struct.pack("<II", VERSION, len(header)) + header + pixels


def make_asset_subresources(subresources, fmt, srgb, channel_mapping=None):
    """Build v2 from ordered (mip, layer, width, height, block_height_gobs, tiled_bytes) tuples."""
    if fmt not in FORMATS or not subresources:
        raise ValueError("format and at least one subresource are required")
    if channel_mapping is not None and (not isinstance(channel_mapping, (list, tuple)) or
            len(channel_mapping) != 4 or any(type(c) is not int or c < 0 or c > 5 for c in channel_mapping)):
        raise ValueError("invalid channel mapping")
    bw, bh, bpb = FORMATS[fmt]
    ordered = sorted(subresources, key=lambda s: (s[0], s[1]))
    keys = [(s[0], s[1]) for s in ordered]
    if len(set(keys)) != len(keys) or keys != [(m, l) for m in range(max(k[0] for k in keys)+1)
                                                    for l in range(max(k[1] for k in keys)+1)]:
        raise ValueError("subresources must form a complete zero-based mip/layer grid")
    base_w, base_h = ordered[0][2:4]
    payloads, entries = [], []
    offset = 0
    for mip, layer, width, height, block_height, tiled in ordered:
        if width != max(1, base_w >> mip) or height != max(1, base_h >> mip):
            raise ValueError("mip dimensions do not match base dimensions")
        data = convert(tiled, width, height, fmt, block_height)
        entries.append({"mip": mip, "layer": layer, "width": width, "height": height,
                        "source_block_height_gobs": block_height, "offset": offset, "byte_length": len(data)})
        payloads.append(data); offset += len(data)
    meta = {"schema": "dk-native-texture", "version": 2, "width": base_w, "height": base_h,
            "format": fmt, "color_space": "srgb" if srgb else "linear",
            "mip_levels": max(k[0] for k in keys)+1, "layers": max(k[1] for k in keys)+1,
            "block_width": bw, "block_height": bh, "bytes_per_block": bpb,
            "layout": "linear-block-rows-subresources", "source_layout": "switch2-block-linear",
            "subresources": entries}
    if channel_mapping is not None:
        meta["channel_mapping"] = list(channel_mapping)
    header = json.dumps(meta, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return MAGIC + struct.pack("<II", 2, len(header)) + header + b"".join(payloads)


def parse_asset(blob):
    """Validate the v1 header and exact payload geometry; return metadata and payload."""
    if len(blob) < 16 or blob[:8] != MAGIC:
        raise ValueError("invalid native texture magic or truncated header")
    version, header_len = struct.unpack_from("<II", blob, 8)
    if version not in (1, 2) or header_len > len(blob) - 16:
        raise ValueError("unsupported version or truncated metadata")
    try:
        meta = json.loads(blob[16:16 + header_len])
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise ValueError("invalid metadata JSON") from e
    fmt = meta.get("format")
    if meta.get("schema") != "dk-native-texture" or fmt not in FORMATS:
        raise ValueError("unsupported asset schema or format")
    mapping = meta.get("channel_mapping")
    if mapping is not None and (not isinstance(mapping, list) or len(mapping) != 4 or
                                any(type(c) is not int or c < 0 or c > 5 for c in mapping)):
        raise ValueError("invalid channel mapping")
    bw, bh, bpb = FORMATS[fmt]
    width, height = meta.get("width"), meta.get("height")
    if (not isinstance(width, int) or not isinstance(height, int) or width <= 0 or height <= 0 or
        meta.get("version") != version or meta.get("block_width") != bw or meta.get("block_height") != bh or
        meta.get("bytes_per_block") != bpb or meta.get("color_space") not in ("linear", "srgb") or
        not isinstance(meta.get("mip_levels"), int) or not isinstance(meta.get("layers"), int) or
        meta["mip_levels"] < 1 or meta["layers"] < 1):
        raise ValueError("inconsistent or unsupported texture metadata")
    payload = blob[16 + header_len:]
    if version == 1:
        if meta["mip_levels"] != 1 or meta["layers"] != 1 or meta.get("layout") != "linear-block-rows":
            raise ValueError("unsupported v1 texture metadata")
        expected = ((width + bw - 1) // bw) * ((height + bh - 1) // bh) * bpb
        if len(payload) != expected: raise ValueError(f"payload length {len(payload)} != expected {expected}")
    else:
        entries = meta.get("subresources")
        expected_keys = [(m, l) for m in range(meta["mip_levels"]) for l in range(meta["layers"])]
        if meta.get("layout") != "linear-block-rows-subresources" or not isinstance(entries, list) or len(entries) != len(expected_keys):
            raise ValueError("unsupported or incomplete v2 subresources")
        cursor = 0
        for entry, key in zip(entries, expected_keys):
            m, l = key; w, h = max(1, width >> m), max(1, height >> m)
            size = ((w + bw - 1)//bw) * ((h + bh - 1)//bh) * bpb
            if (entry != {"mip": m, "layer": l, "width": w, "height": h,
                          "source_block_height_gobs": entry.get("source_block_height_gobs"),
                          "offset": cursor, "byte_length": size} or
                not isinstance(entry.get("source_block_height_gobs"), int) or not 0 <= entry["source_block_height_gobs"] <= 5):
                raise ValueError("invalid v2 subresource geometry or ordering")
            cursor += size
        if len(payload) != cursor: raise ValueError(f"payload length {len(payload)} != expected {cursor}")
    return meta, payload


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path); p.add_argument("output", type=Path)
    p.add_argument("--width", type=int, required=True); p.add_argument("--height", type=int, required=True)
    p.add_argument("--format", choices=FORMATS, required=True); p.add_argument("--color-space", choices=("linear", "srgb"), required=True)
    p.add_argument("--block-height-gobs", type=int, required=True)
    a = p.parse_args()
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(make_asset(a.source.read_bytes(), a.width, a.height, a.format, a.color_space == "srgb", a.block_height_gobs))


if __name__ == "__main__":
    main()
