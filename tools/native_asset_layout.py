#!/usr/bin/env python3
"""Conservative BNTX/BRTI descriptor and mip-range reader.

The format constants follow the BNTX structure documented by the Switch
Brew/3DSkit BNTX references. This module does not implement Tegra swizzling.
"""
import hashlib
import struct


ASTC_BLOCKS = ((4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6),
               (8, 8), (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12))


def _build_formats():
    formats = {
        # Switch Toolbox BNTX SurfaceFormat enum at 4b5f4dc5b71615baee702be9ab5627fba6ebffbf
        # resolves numeric 0x0A03 to R16_UINT; keep the integer sampling semantics.
        0x0B01: ("rgba8", "linear"), 0x0B06: ("rgba8", "srgb"),
        0x0A03: ("r16-uint", "linear"),
        # Switch Toolbox BNTX.GetFormat maps its R11_G11_B10_UINT enum to
        # R11G11B10_FLOAT (packed unsigned floats), rather than integer channels.
        0x0F05: ("b10g11r11-ufloat", "linear"),
        # nn::gfx channel 0x15 is R16G16B16A16 and type 0x05 is float.
        0x1505: ("rgba16f", "linear"),
        0x1A01: ("bc1", "linear"), 0x1A06: ("bc1", "srgb"),
        0x1B01: ("bc2", "linear"), 0x1B06: ("bc2", "srgb"),
        0x1C01: ("bc3", "linear"), 0x1C06: ("bc3", "srgb"),
        0x1D01: ("bc4", "linear"), 0x1D02: ("bc4-snorm", "linear"),
        0x1E01: ("bc5", "linear"), 0x1E02: ("bc5-snorm", "linear"),
        0x1F05: ("bc6h-sfloat", "linear"), 0x1F0A: ("bc6h-ufloat", "linear"),
        0x2001: ("bc7", "linear"), 0x2006: ("bc7", "srgb"),
    }
    for i, (width, height) in enumerate(ASTC_BLOCKS):
        name = f"astc-{width}x{height}"
        formats[0x2D01 + i * 0x100] = (name, "linear")
        formats[0x2D06 + i * 0x100] = (name, "srgb")
    return formats


FORMATS = _build_formats()
TILE_MODES = {0: "pitch-linear", 1: "block-linear"}
DIMENSIONS = {1: "1d", 2: "2d", 3: "3d"}
# BRTI byte 0x5c holds a SurfaceDim value (Syroot.NintenTools.NSW.Bntx GFX.SurfaceDim, read by reflection:
# Dim1D=0, Dim2D=1, Dim3D=2, DimCube=3, Dim1DArray=4, Dim2DArray=5, Dim2DMsaa=6, Dim2DMsaaArray=7,
# DimCubeArray=8). Byte 0x11 is the image dimension (1/2/3) and is a different field.
SURFACE_DIM_OFFSET = 0x5c
SURFACE_DIM_NAMES = {0: "1d", 1: "2d", 2: "3d", 3: "cube", 4: "1d-array", 5: "2d-array",
                     6: "2d-msaa", 7: "2d-msaa-array", 8: "cube-array"}
# Values confirmed against real records in the verified RomFS (see notes/native-port/handoffs/h5c-assets.md):
# 1 = 36 single-layer 2D records, 5 = 2D arrays with 2, 3 and 4 layers, 3 = the seven six-layer 0x0f05 records.
SURFACE_DIM_VALIDATED = {1, 3, 5}
# Value 2 (3D) is not confirmed. A cube claim is made only when the 3D value is also confirmed on a real record.
SURFACE_DIM_3D_CONFIRMED = False


def surface_dim_fields(value, three_d_confirmed=None):
    """Record fields for one raw BRTI byte 0x5c. is_cube is None (unknown) unless the 3D value is confirmed."""
    confirmed = SURFACE_DIM_3D_CONFIRMED if three_d_confirmed is None else three_d_confirmed
    return {"surface_dim_raw": value,
            "surface_dim_name": SURFACE_DIM_NAMES.get(value),
            "surface_dim_validated": value in SURFACE_DIM_VALIDATED,
            "is_cube": (value in (3, 8)) if confirmed else None}
BLOCK_GEOMETRY = {"rgba8": (1, 1, 4), "r16-uint": (1, 1, 2), "b10g11r11-ufloat": (1, 1, 4), "rgba16f": (1, 1, 8), "bc1": (4, 4, 8), "bc2": (4, 4, 16),
                   "bc3": (4, 4, 16), "bc4": (4, 4, 8), "bc4-snorm": (4, 4, 8),
                   "bc5": (4, 4, 16), "bc5-snorm": (4, 4, 16),
                   "bc6h-sfloat": (4, 4, 16), "bc6h-ufloat": (4, 4, 16), "bc7": (4, 4, 16)}
for _width, _height in ASTC_BLOCKS:
    BLOCK_GEOMETRY[f"astc-{_width}x{_height}"] = (_width, _height, 16)


def block_linear_size(width, height, fmt, block_height_log2, alignment):
    block_width, block_height, bytes_per_block = BLOCK_GEOMETRY[fmt]
    row_bytes = ((width + block_width - 1) // block_width) * bytes_per_block
    rows = (height + block_height - 1) // block_height
    while block_height_log2 and (8 << (block_height_log2 - 1)) >= rows:
        block_height_log2 -= 1
    gob_height = 1 << block_height_log2
    size = ((row_bytes + 63) // 64) * ((rows + 8 * gob_height - 1) // (8 * gob_height)) * 512 * gob_height
    return (size + alignment - 1) // alignment * alignment


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError("BNTX range outside resource")
    return data[offset:offset + size]


def _section_payload(data, magic):
    found = []
    start = 0
    while True:
        offset = data.find(magic, start)
        if offset < 0:
            break
        found.append(offset)
        start = offset + 1
    if len(found) != 1:
        raise ValueError("BNTX data section is absent or ambiguous")
    section = found[0]
    if section + 0x10 > len(data):
        raise ValueError("truncated BNTX data section")
    section_size = struct.unpack_from("<I", data, section + 8)[0]
    if section_size < 1:
        raise ValueError("invalid BNTX data section size")
    checked(data, section + 0x10, section_size)
    return section + 0x10, section + 0x10 + section_size


def parse_bntx(data):
    """Parse BRTI records and exact aggregate mip payload ranges.

    For known 2D array formats, reconcile the first layer's mip offsets and
    layer stride against the layer-major layout used by nvn_tex.c.
    """
    if len(data) < 0x30 or data[:4] != b"BNTX":
        raise ValueError("unsupported BNTX header")
    platform_tag = data[0x20:0x24]
    # Switch Toolbox's BNTX loader identifies the exact mixed-case "Ounc" tag as
    # Orin (BNTX.cs, BNTX.IsOrin/GetImageData). For this Bananza census, format
    # codes are accepted only when known BNTX block geometry reconciles every
    # singleton mip range. Ounc tile mode 0 is treated as block-linear under the
    # same exact-size check; the Switch 2 GOB byte order is recorded in docs/devlog.md.
    platform_name = {b"NX  ": "nx", b"OUNC": "ounc", b"Ounc": "ounc"}.get(platform_tag, "unverified")
    platform_semantics_verified = platform_tag in (b"NX  ", b"OUNC", b"Ounc")
    count, table = struct.unpack_from("<IQ", data, 0x24)
    if count > 1_000_000:
        raise ValueError("implausible BNTX texture count")
    checked(data, table, count * 8)
    payload_start, payload_end = _section_payload(data, b"BRTD")
    records = []
    for ordinal in range(count):
        info = struct.unpack_from("<Q", data, table + ordinal * 8)[0]
        checked(data, info, 0x78)
        if data[info:info + 4] != b"BRTI":
            raise ValueError("texture table entry is not BRTI")
        dimension = data[info + 0x11]
        tile_mode = struct.unpack_from("<H", data, info + 0x12)[0]
        mip_count = struct.unpack_from("<H", data, info + 0x16)[0]
        format_code = struct.unpack_from("<I", data, info + 0x1c)[0]
        width, height, depth, layers, block_height = struct.unpack_from("<5I", data, info + 0x24)
        image_size = struct.unpack_from("<I", data, info + 0x50)[0]
        alignment = struct.unpack_from("<I", data, info + 0x54)[0]
        channels = list(data[info + 0x58:info + 0x5c])
        surface_dim = data[info + SURFACE_DIM_OFFSET]
        name_ptr = struct.unpack_from("<Q", data, info + 0x60)[0]
        name_len = struct.unpack_from("<H", checked(data, name_ptr, 2), 0)[0]
        name = checked(data, name_ptr + 2, name_len)
        mip_ptr = struct.unpack_from("<Q", data, info + 0x70)[0]
        checked(data, mip_ptr, mip_count * 8)
        if not (1 <= width <= 65536 and 1 <= height <= 65536 and
                1 <= depth <= 65536 and 1 <= layers <= 65536 and
                1 <= mip_count <= 32 and (not platform_semantics_verified or block_height <= 5) and
                mip_count <= max(width, height, depth).bit_length()):
            raise ValueError("unsupported BRTI geometry")
        semantic = FORMATS.get(format_code) if platform_semantics_verified else None
        tile_mode_name = ("block-linear" if platform_tag == b"Ounc" and tile_mode == 0 else
                          None if platform_tag == b"Ounc" else TILE_MODES.get(tile_mode))
        if semantic and (alignment < 1 or alignment > (1 << 20) or alignment & (alignment - 1)):
            raise ValueError("unsupported BRTI image alignment")
        offsets = [struct.unpack_from("<Q", data, mip_ptr + mip * 8)[0]
                   for mip in range(mip_count)]
        if offsets != sorted(set(offsets)):
            raise ValueError("mip pointers are not strictly increasing")
        end = offsets[0] + image_size
        checked(data, offsets[0], image_size)
        if offsets[0] < payload_start or end > payload_end or offsets[-1] >= end:
            raise ValueError("BRTI image range is outside BRTD payload")
        ranges = []
        for mip, offset in enumerate(offsets):
            stop = offsets[mip + 1] if mip + 1 < mip_count else end
            if stop <= offset:
                raise ValueError("empty or overlapping BRTI mip range")
            mip_width, mip_height, mip_depth = (max(1, width >> mip),
                                                  max(1, height >> mip), max(1, depth >> mip))
            minimum = None
            if semantic:
                block_width, block_height_px, bytes_per_block = BLOCK_GEOMETRY[semantic[0]]
                minimum = ((mip_width + block_width - 1) // block_width *
                           ((mip_height + block_height_px - 1) // block_height_px) *
                           mip_depth * layers * bytes_per_block)
                if stop - offset < minimum and layers == 1 and mip_depth == 1:
                    raise ValueError(f"mip {mip} format {format_code:#06x} range {stop - offset} < minimum {minimum}")
            block_linear = ((platform_tag == b"Ounc" and tile_mode == 0) or
                            (platform_tag != b"Ounc" and tile_mode == 1))
            tiled_size = (block_linear_size(mip_width, mip_height, semantic[0], block_height, alignment)
                          if semantic and block_linear and mip_depth == 1 and layers == 1 else None)
            ranges.append({"mip": mip, "offset": offset, "byte_length": stop - offset,
                           "minimum_geometry_bytes": minimum if semantic else None,
                           "expected_block_linear_bytes": tiled_size,
                           "block_linear_geometry_matches": tiled_size == stop - offset if tiled_size is not None else None,
                           "width": mip_width, "height": mip_height,
                           "depth": mip_depth, "layers": layers})
        slices = []
        if semantic and block_linear and dimension == 2 and depth == 1:
            sizes = [block_linear_size(r["width"], r["height"], semantic[0], block_height, alignment)
                     for r in ranges]
            bh = block_height
            rows = (height + BLOCK_GEOMETRY[semantic[0]][1] - 1) // BLOCK_GEOMETRY[semantic[0]][1]
            while bh and (8 << (bh - 1)) >= rows:
                bh -= 1
            layer_alignment = max(alignment, 512 << bh) if layers > 1 else alignment
            stride = (sum(sizes) + layer_alignment - 1) // layer_alignment * layer_alignment
            expected_offsets = [offsets[0] + sum(sizes[:m]) for m in range(mip_count)]
            if offsets == expected_offsets and image_size == (stride * layers if layers > 1 else sum(sizes)):
                slices = [{"mip": mip, "layer": layer, "offset": offsets[mip] + layer * stride,
                           "byte_length": sizes[mip]} for mip in range(mip_count) for layer in range(layers)]
        records.append({"ordinal": ordinal, "name_hash": hashlib.sha256(name).hexdigest(),
                        "channel_mapping": channels if all(c <= 5 for c in channels) else None,
                        "descriptor_hash": hashlib.sha256(data[info:info + 0x78]).hexdigest(),
                        "platform_name": platform_name,
                        "platform_tag_hex": platform_tag.hex(),
                        "platform_semantics_verified": platform_semantics_verified,
                        "format_code": format_code,
                        "format": semantic[0] if semantic else None,
                        "color_space": semantic[1] if semantic else None,
                        "dimension": dimension, "tile_mode": tile_mode,
                        "dimension_name": DIMENSIONS.get(dimension) if platform_semantics_verified else None,
                        "tile_mode_name": tile_mode_name if platform_semantics_verified else None,
                        "raw_block_height_log2": block_height,
                        "block_height_log2": block_height if platform_semantics_verified else None,
                        "block_height_gobs": 1 << block_height if platform_semantics_verified else None,
                        "raw_alignment": alignment,
                        "alignment": alignment if platform_semantics_verified else None,
                        "width": width,
                        "height": height, "depth": depth, "layers": layers,
                        **surface_dim_fields(surface_dim),
                        "mip_levels": mip_count, "image_size": image_size,
                        "mip_ranges": ranges,
                        "subresource_ranges": slices,
                        "per_slice_ranges_proven": bool(slices),
                        "conversion_semantics_proven": semantic is not None and block_linear and
                            semantic[0] in BLOCK_GEOMETRY and dimension == 2 and
                            depth == 1 and bool(slices) and all(c <= 5 for c in channels)})
    return records


def bntx_blob_offsets(raw):
    """Offsets of every BNTX blob in a decompressed resource that parses under parse_bntx, in file order.

    Searching only the first "BNTX" missed blobs after the first in SARC-style containers. A candidate that does
    not parse is skipped, so an embedded false match cannot become a record.
    """
    offsets, pos = [], raw.find(b"BNTX")
    while pos >= 0:
        try:
            parse_bntx(raw[pos:])
            offsets.append(pos)
        except (ValueError, struct.error, OverflowError):
            pass
        pos = raw.find(b"BNTX", pos + 4)
    return offsets
