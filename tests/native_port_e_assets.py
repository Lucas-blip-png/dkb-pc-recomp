import importlib.util
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))

spec = importlib.util.spec_from_file_location("native_asset_convert", Path(__file__).parents[1] / "tools" / "native_asset_convert.py")
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
coverage_spec = importlib.util.spec_from_file_location("native_asset_coverage", Path(__file__).parents[1] / "tools" / "native_asset_coverage.py")
coverage = importlib.util.module_from_spec(coverage_spec); coverage_spec.loader.exec_module(coverage)


class AssetConversion(unittest.TestCase):
    def test_bntx_inventory_reader_bounds_and_subresource_metadata(self):
        import struct
        inv_spec = importlib.util.spec_from_file_location("native_asset_inventory", Path(__file__).parents[1] / "tools" / "native_asset_inventory.py")
        inv = importlib.util.module_from_spec(inv_spec); inv_spec.loader.exec_module(inv)
        blob = bytearray(0x800)
        blob[:4] = b"BNTX"
        blob[0x20:0x24] = b"NX  "
        struct.pack_into("<IQ", blob, 0x24, 1, 0x100)
        struct.pack_into("<Q", blob, 0x100, 0x180)
        blob[0x180:0x184] = b"BRTI"
        blob[0x180 + 0x11] = 2
        struct.pack_into("<HHH", blob, 0x180 + 0x12, 1, 0, 2)
        struct.pack_into("<I", blob, 0x180 + 0x1c, 0x0B01)
        struct.pack_into("<5I", blob, 0x180 + 0x24, 8, 4, 1, 1, 0)
        struct.pack_into("<I", blob, 0x180 + 0x50, 0x400)
        struct.pack_into("<I", blob, 0x180 + 0x54, 0x200)
        blob[0x180 + 0x58:0x180 + 0x5c] = bytes([4, 3, 2, 1])
        struct.pack_into("<Q", blob, 0x180 + 0x60, 0x240)
        struct.pack_into("<Q", blob, 0x180 + 0x70, 0x280)
        struct.pack_into("<H", blob, 0x240, 4); blob[0x242:0x246] = b"test"
        struct.pack_into("<QQ", blob, 0x280, 0x310, 0x510)
        blob[0x300:0x304] = b"BRTD"
        struct.pack_into("<I", blob, 0x308, 0x400)
        struct.pack_into("<I", blob, 0x1C, len(blob)); struct.pack_into("<Q", blob, 0x30, 0x300)
        records = inv.bntx_records(bytes(blob))
        self.assertEqual((records[0]["width"], records[0]["height"], records[0]["layers"], records[0]["mip_levels"]), (8, 4, 1, 2))
        self.assertEqual(records[0]["mip_ranges"], [
            {"mip": 0, "offset": 0x310, "byte_length": 0x200, "minimum_geometry_bytes": 128, "expected_block_linear_bytes": 0x200, "block_linear_geometry_matches": True, "width": 8, "height": 4, "depth": 1, "layers": 1},
            {"mip": 1, "offset": 0x510, "byte_length": 0x200, "minimum_geometry_bytes": 32, "expected_block_linear_bytes": 0x200, "block_linear_geometry_matches": True, "width": 4, "height": 2, "depth": 1, "layers": 1}])
        self.assertEqual((records[0]["format_code"], records[0]["format"], records[0]["color_space"], records[0]["platform_tag_hex"]), (0x0B01, "rgba8", "linear", "4e582020"))
        self.assertEqual(records[0]["channel_mapping"], [4, 3, 2, 1])
        with self.assertRaises(ValueError): inv.bntx_records(bytes(blob[:0x220]))

        # Two layers, each containing the two 512-byte mips in layer-major order.
        array = bytearray(blob) + bytearray(0x400)
        struct.pack_into('<I', array, 0x1C, len(array))
        struct.pack_into('<I', array, 0x1B0, 2)  # layers
        struct.pack_into('<I', array, 0x1D0, 0x800)  # image size
        struct.pack_into('<I', array, 0x308, 0x800)  # BRTD payload size
        record = inv.bntx_records(bytes(array))[0]
        self.assertTrue(record['conversion_semantics_proven'])
        self.assertEqual([(s['mip'], s['layer'], s['offset'], s['byte_length'])
                          for s in record['subresource_ranges']],
                         [(0, 0, 0x310, 0x200), (0, 1, 0x710, 0x200),
                          (1, 0, 0x510, 0x200), (1, 1, 0x910, 0x200)])
        struct.pack_into('<Q', array, 0x288, 0x610)
        self.assertFalse(inv.bntx_records(bytes(array))[0]['conversion_semantics_proven'])

    def test_bntx_layout_rejects_bad_ranges_and_unknown_semantics(self):
        import struct
        layout_spec = importlib.util.spec_from_file_location("native_asset_layout", Path(__file__).parents[1] / "tools" / "native_asset_layout.py")
        layout = importlib.util.module_from_spec(layout_spec); layout_spec.loader.exec_module(layout)
        blob = bytearray(0x500)
        blob[:4] = b"BNTX"; blob[0x20:0x24] = b"NX  "
        struct.pack_into("<IQ", blob, 0x24, 1, 0x100); struct.pack_into("<Q", blob, 0x100, 0x180)
        blob[0x180:0x184] = b"BRTI"; blob[0x191] = 2
        struct.pack_into("<HHH", blob, 0x192, 1, 0, 1)
        struct.pack_into("<I", blob, 0x19C, 0xDEAD)
        struct.pack_into("<5I", blob, 0x1A4, 16, 8, 1, 1, 0)
        struct.pack_into("<I", blob, 0x1D0, 0x20)
        struct.pack_into("<I", blob, 0x1D4, 0x200)
        struct.pack_into("<Q", blob, 0x1E0, 0x240); struct.pack_into("<Q", blob, 0x1F0, 0x280)
        struct.pack_into("<H", blob, 0x240, 1); blob[0x242] = 0
        struct.pack_into("<Q", blob, 0x280, 0x310)
        blob[0x300:0x304] = b"BRTD"; struct.pack_into("<I", blob, 0x308, 0x100)
        struct.pack_into("<I", blob, 0x1C, len(blob)); struct.pack_into("<Q", blob, 0x30, 0x300)
        record = layout.parse_bntx(bytes(blob))[0]
        self.assertIsNone(record["format"])
        self.assertIsNone(record["color_space"])
        self.assertFalse(record["conversion_semantics_proven"])
        self.assertEqual(record["tile_mode_name"], "block-linear")
        self.assertEqual(layout.FORMATS[0x0A03], ("r16-uint", "linear"))
        self.assertEqual(layout.FORMATS[0x0F05], ("b10g11r11-ufloat", "linear"))
        self.assertEqual(layout.FORMATS[0x1505], ("rgba16f", "linear"))
        self.assertEqual(layout.BLOCK_GEOMETRY["r16-uint"], (1, 1, 2))
        self.assertEqual(layout.FORMATS[0x1C06], ("bc3", "srgb"))
        self.assertEqual(layout.FORMATS[0x1B01], ("bc2", "linear"))
        self.assertEqual(layout.FORMATS[0x3406], ("astc-8x8", "srgb"))
        self.assertEqual(layout.block_linear_size(512, 256, "astc-8x8", 3, 512), 32768)
        blob[0x20:0x24] = b"Ounc"
        struct.pack_into("<H", blob, 0x192, 0)
        unverified = layout.parse_bntx(bytes(blob))[0]
        self.assertEqual(unverified["platform_name"], "ounc")
        self.assertEqual(unverified["platform_tag_hex"], "4f756e63")
        self.assertEqual(unverified["format_code"], 0xDEAD)
        self.assertTrue(unverified["platform_semantics_verified"])
        self.assertIsNone(unverified["format"])
        self.assertEqual(unverified["tile_mode_name"], "block-linear")
        self.assertEqual(unverified["block_height_gobs"], 1)
        struct.pack_into("<Q", blob, 0x280, 0x4F0)
        with self.assertRaises(ValueError): layout.parse_bntx(bytes(blob))

    def test_lossless_gob_round_trip_formats_and_edges(self):
        rng = random.Random(0xD0B)
        for i, (fmt, (bw, bh, bpb)) in enumerate(mod.FORMATS.items()):
            width, height, th = 65 + 3 * i, 33 + 2 * i, i % 4
            row_bytes = ((width + bw - 1) // bw) * bpb
            rows = (height + bh - 1) // bh
            gob_height = 1 << th
            size = ((row_bytes + 63) // 64) * ((rows + 8 * gob_height - 1) // (8 * gob_height)) * 512 * gob_height
            tiled = bytes(rng.randrange(256) for _ in range(size))
            linear = mod.convert(tiled, width, height, fmt, th)
            repacked = mod.convert(linear, width, height, fmt, th, False)
            self.assertEqual(mod.convert(repacked, width, height, fmt, th), linear, fmt)

    def test_r16_uint_preserves_integer_texels_without_normalization(self):
        import struct
        words = (0x0000, 0x0001, 0x7FFF, 0xFFFF)
        linear = struct.pack("<4H", *words)
        tiled = mod.convert(linear, 2, 2, "r16-uint", 0, False)
        self.assertEqual(mod.convert(tiled, 2, 2, "r16-uint", 0), linear)
        artifact = mod.make_asset_subresources([(0, 0, 2, 2, 0, tiled)], "r16-uint", False)
        meta, payload = mod.parse_asset(artifact)
        self.assertEqual((meta["format"], meta["color_space"], meta["bytes_per_block"]), ("r16-uint", "linear", 2))
        self.assertEqual(payload, linear)

    def test_container_metadata_and_rejection(self):
        import json, struct
        payload = bytes(512)
        asset = mod.make_asset(payload, 8, 8, "rgba8", True, 0)
        n = struct.unpack_from("<I", asset, 12)[0]
        meta = json.loads(asset[16:16+n])
        parsed, decoded = mod.parse_asset(asset)
        self.assertEqual((meta["version"], meta["color_space"], meta["mip_levels"], meta["layers"]), (1, "srgb", 1, 1))
        self.assertEqual(parsed, meta); self.assertEqual(len(decoded), 8 * 8 * 4)
        with self.assertRaises(ValueError): mod.parse_asset(asset[:-1])
        with self.assertRaises(ValueError): mod.make_asset(payload, 8, 8, "rgba8", False, 0, mip_levels=2)
        with self.assertRaises(ValueError): mod.convert(b"", 8, 8, "astc", 0)

    def test_v2_mip_array_subresources_are_lossless_and_strict(self):
        import struct
        rng = random.Random(19)
        subs, expected = [], []
        for mip, w, h in ((0, 9, 5), (1, 4, 2), (2, 2, 1)):
            for layer in range(2):
                bw, bh, bpb = mod.FORMATS["bc3"]
                row = ((w + bw - 1)//bw)*bpb; rows = (h + bh - 1)//bh
                th = mip % 2; gh = 1 << th
                size = ((row+63)//64)*((rows+8*gh-1)//(8*gh))*512*gh
                tiled = bytes(rng.randrange(256) for _ in range(size))
                subs.append((mip, layer, w, h, th, tiled))
                expected.append(mod.convert(tiled, w, h, "bc3", th))
        asset = mod.make_asset_subresources(subs, "bc3", True)
        meta, payload = mod.parse_asset(asset)
        self.assertEqual((struct.unpack_from("<I", asset, 8)[0], meta["mip_levels"], meta["layers"]), (2, 3, 2))
        self.assertEqual(payload, b"".join(expected))
        with self.assertRaises(ValueError): mod.make_asset_subresources(subs[:-1], "bc3", True)
        mapped = mod.make_asset_subresources(subs, "bc3", True, [4, 3, 2, 1])
        self.assertEqual(mod.parse_asset(mapped)[0]["channel_mapping"], [4, 3, 2, 1])
        with self.assertRaises(ValueError): mod.make_asset_subresources(subs, "bc3", True, [2, 3, 4, 6])

    def test_layered_array_formats_round_trip_per_layer_and_mip(self):
        # Covers the formats of the census's previously unsupported array records (BC4, BC5,
        # BC5-snorm, BC7 with 2 to 4 layers) plus the uncompressed 16-bit float and packed-float
        # formats at six layers, the layer count of the 0xf05 records. Each layer's tiled bytes
        # must convert to exactly the bytes of that layer, in the layer-major order the converter writes.
        import struct
        rng = random.Random(29)
        cases = [("bc4", 4), ("bc5", 4), ("bc5-snorm", 3), ("bc7", 2), ("rgba16f", 6), ("b10g11r11-ufloat", 6)]
        for fmt, layers in cases:
            with self.subTest(fmt=fmt, layers=layers):
                bw, bh, bpb = mod.FORMATS[fmt]
                subs, expected = [], []
                for mip, w, h in ((0, 9, 5), (1, 4, 2), (2, 2, 1)):
                    for layer in range(layers):
                        row = ((w + bw - 1) // bw) * bpb
                        rows = (h + bh - 1) // bh
                        th = mip % 2
                        gh = 1 << th
                        size = ((row + 63) // 64) * ((rows + 8 * gh - 1) // (8 * gh)) * 512 * gh
                        tiled = bytes(rng.randrange(256) for _ in range(size))
                        subs.append((mip, layer, w, h, th, tiled))
                        expected.append(mod.convert(tiled, w, h, fmt, th))
                asset = mod.make_asset_subresources(subs, fmt, False)
                meta, payload = mod.parse_asset(asset)
                self.assertEqual((meta["mip_levels"], meta["layers"]), (3, layers))
                self.assertEqual(payload, b"".join(expected))
                with self.assertRaises(ValueError):
                    mod.make_asset_subresources(subs[:-1], fmt, False)

    def test_reference_decoders_match_hand_built_blocks(self):
        import struct
        ref_spec = importlib.util.spec_from_file_location("native_asset_reference", Path(__file__).parents[1] / "tools" / "native_asset_reference.py")
        ref = importlib.util.module_from_spec(ref_spec); ref_spec.loader.exec_module(ref)
        close = lambda a, b: all(abs(x - y) < 1e-6 for x, y in zip(a, b))
        # rgba16f: 0x3C00 is 1.0, 0xC000 is -2.0
        px = ref.decode("rgba16f", struct.pack("<4H", 0x3C00, 0xC000, 0x0000, 0x3C00), 1, 1)[0]
        self.assertTrue(close(px, (1.0, -2.0, 0.0, 1.0)))
        # b10g11r11: 1.0 in each channel (exponent 15, mantissa 0) packs to 0x3C0 | 0x3C0<<11 | 0x1E0<<22
        word = 0x3C0 | (0x3C0 << 11) | (0x1E0 << 22)
        self.assertTrue(close(ref.decode("b10g11r11-ufloat", struct.pack("<I", word), 1, 1)[0], (1.0, 1.0, 1.0, 1.0)))
        # rgba8 passes channels through
        self.assertTrue(close(ref.decode("rgba8", bytes([255, 0, 128, 255]), 1, 1)[0], (1.0, 0.0, 128 / 255.0, 1.0)))
        # r16-uint returns raw integers
        self.assertEqual(ref.decode("r16-uint", struct.pack("<H", 4321), 1, 1)[0][0], 4321.0)
        # BC1 4-colour mode (c0 > c1): colour 0 white, colour 1 black, index 1 everywhere -> black
        block = struct.pack("<HHI", 0xFFFF, 0x0000, 0x55555555)
        self.assertTrue(all(close(p, (0.0, 0.0, 0.0, 1.0)) for p in ref.decode("bc1", block, 4, 4)))
        # BC1 3-colour mode (c0 <= c1): index 3 is transparent black
        block = struct.pack("<HHI", 0x0000, 0xFFFF, 0xFFFFFFFF)
        self.assertTrue(all(close(p, (0.0, 0.0, 0.0, 0.0)) for p in ref.decode("bc1", block, 4, 4)))
        # BC4 unorm, 8-value mode (a0 > a1): index 0 -> alpha0, index 1 -> alpha1, index 7 -> (a0 + 6*a1)/7 = 1/7
        bits = sum(((0 if i < 8 else 1) << (3 * i)) for i in range(16)) & ((1 << 48) - 1)
        row = ref.decode("bc4", struct.pack("<BB", 255, 0) + bits.to_bytes(6, "little"), 4, 4)
        self.assertAlmostEqual(row[0][0], 1.0)
        self.assertAlmostEqual(row[8][0], 0.0)
        bits = sum((7 << (3 * i)) for i in range(16)) & ((1 << 48) - 1)
        self.assertAlmostEqual(ref.decode("bc4", struct.pack("<BB", 255, 0) + bits.to_bytes(6, "little"), 4, 4)[0][0], 1.0 / 7.0)
        # BC4 unorm, 6-value mode (a0 <= a1): index 6 -> 0.0, index 7 -> 1.0
        bits = sum((6 << (3 * i)) for i in range(16)) & ((1 << 48) - 1)
        vals = ref.decode("bc4", struct.pack("<BB", 0, 255) + bits.to_bytes(6, "little"), 4, 4)
        self.assertAlmostEqual(vals[0][0], 0.0)
        # BC5 carries red and green from two BC4 blocks
        red = struct.pack("<BB", 255, 255) + bytes(6)
        green = struct.pack("<BB", 0, 0) + bytes(6)
        vals = ref.decode("bc5", red + green, 4, 4)
        self.assertTrue(close(vals[5], (1.0, 0.0, 0.0, 1.0)))
        # Each formats outside the covered set must fail loudly, never decode to a guess
        with self.assertRaises(KeyError): ref.decode("bc7", bytes(16), 4, 4)
        with self.assertRaises(KeyError): ref.decode("bc6h-sfloat", bytes(16), 4, 4)

    def test_surface_dim_byte_names_and_cube_claim_is_gated(self):
        # Byte 0x5c of BRTI is a SurfaceDim value. Values 1, 3 and 5 are confirmed on real RomFS records
        # (2D, cube on the six-layer records, 2D arrays); 3D is not confirmed, so no record may claim a cube
        # while SURFACE_DIM_3D_CONFIRMED is false.
        spec = importlib.util.spec_from_file_location("native_asset_layout", Path(__file__).parents[1] / "tools" / "native_asset_layout.py")
        lay = importlib.util.module_from_spec(spec); spec.loader.exec_module(lay)
        self.assertEqual(lay.SURFACE_DIM_NAMES[1], "2d")
        self.assertEqual(lay.SURFACE_DIM_NAMES[3], "cube")
        self.assertEqual(lay.SURFACE_DIM_NAMES[5], "2d-array")
        self.assertEqual(lay.SURFACE_DIM_OFFSET, 0x5c)
        self.assertEqual(lay.surface_dim_fields(3)["surface_dim_name"], "cube")
        self.assertTrue(lay.surface_dim_fields(3)["surface_dim_validated"])
        self.assertFalse(lay.surface_dim_fields(2)["surface_dim_validated"])
        # The gate: with 3D unconfirmed the cube field stays unknown, never False or True.
        self.assertIsNone(lay.surface_dim_fields(3, three_d_confirmed=False)["is_cube"])
        self.assertIsNone(lay.surface_dim_fields(1, three_d_confirmed=False)["is_cube"])
        # Once 3D is confirmed, cube-valued records are cubes and 2D arrays are not.
        self.assertTrue(lay.surface_dim_fields(3, three_d_confirmed=True)["is_cube"])
        self.assertTrue(lay.surface_dim_fields(8, three_d_confirmed=True)["is_cube"])
        self.assertFalse(lay.surface_dim_fields(5, three_d_confirmed=True)["is_cube"])
        self.assertIsNone(lay.SURFACE_DIM_NAMES.get(42))

    def test_coverage_is_explicit_and_absent_never_converted(self):
        result = coverage.inventory([
            {"asset_id": "seen-supported", "format": "bc3", "layout": "complete-mip-layer-grid", "converted": True, "artifact_sha256": "abc"},
            {"asset_id": "r16-uint", "format": "r16-uint", "layout": "switch2-block-linear-2d", "converted": True, "artifact_sha256": "fed"},
            {"asset_id": "seen-unsupported", "format": "astc-unknown", "layout": "switch2-block-linear-2d", "converted": True, "artifact_sha256": "def"},
            {"asset_id": "seen-unconverted", "format": "rgba8", "layout": "switch2-block-linear-2d", "converted": True},
        ])
        self.assertEqual([r["status"] for r in result["observed_assets"]], ["converted", "converted", "unsupported", "not-converted"])
        self.assertEqual(result["absent_assets"], "unknown-not-converted")
        self.assertFalse(result["coverage_complete"])

    def test_reconciliation_requires_complete_source_inventory(self):
        import hashlib, json, subprocess, tempfile
        root = Path(__file__).resolve().parents[1]
        (root / 'build').mkdir(exist_ok=True)   # a clean checkout has no build folder
        with tempfile.TemporaryDirectory(dir=root / 'build') as directory:
            out = Path(directory)
            inventory = {"coverage_complete": False, "assets": [{"asset_id": "sample",
                "conversion_status": "parsed", "format": "rgba16f", "dimension_name": "2d",
                "layers": 1, "depth": 1, "tile_mode_name": "block-linear", "subresources_enumerated": True,
                "channel_mapping": [2, 3, 4, 5]}]}
            raw = json.dumps(inventory).encode()
            (out / 'inventory.json').write_bytes(raw)
            batch = {"producer": "tools/native_asset_convert_all.py", "inventory_sha256": hashlib.sha256(raw).hexdigest(),
                     "eligible_conversion_complete": True, "verified_artifacts_in_run": 1, "inventory_records": 1}
            (out / 'batch.json').write_text(json.dumps(batch), encoding='utf-8')
            (out / 'sample.dkntex').write_bytes(b'fixture')
            (out / 'sample.dkntex.json').write_text(json.dumps({"asset_id": "sample", "conversion_status": "converted",
                "round_trip_payload_validation": "pass", "output_sha256": "fixture", "channel_mapping": [2, 3, 4, 5]}), encoding='utf-8')
            subprocess.run([sys.executable, str(root / 'tools/native_asset_coverage_reconcile.py'),
                            str(out / 'inventory.json'), str(out / 'batch.json'), str(out), str(out / 'result.json')],
                           check=True, capture_output=True)
            result = json.loads((out / 'result.json').read_text(encoding='utf-8'))
            self.assertEqual(result['converted_supported_records'], 1)
            self.assertFalse(result['coverage_complete'])

    def test_cached_artifact_requires_source_channel_mapping(self):
        import hashlib, json, tempfile
        from native_asset_convert_all import valid_existing, upgrade_channel_metadata
        root = Path(__file__).resolve().parents[1]
        tiled = bytes(512)
        (root / 'build').mkdir(exist_ok=True)   # a clean checkout has no build folder
        with tempfile.TemporaryDirectory(dir=root / 'build') as directory:
            path = Path(directory) / 'sample.dkntex'
            row = {'asset_id': 'sample', 'source_sha256': 'source', 'descriptor_sha256': 'descriptor',
                   'channel_mapping': [2, 2, 2, 1], 'format': 'rgba8', 'color_space': 'linear',
                   'width': 1, 'height': 1, 'mip_levels': 1, 'layers': 1}
            def save(mapping):
                blob = mod.make_asset_subresources([(0, 0, 1, 1, 0, tiled)], 'rgba8', False, mapping)
                path.write_bytes(blob)
                report = {**row, 'output_sha256': hashlib.sha256(blob).hexdigest(),
                          'round_trip_payload_validation': 'pass', 'conversion_status': 'converted'}
                path.with_suffix('.dkntex.json').write_text(json.dumps(report), encoding='utf-8')
            save(None)
            self.assertFalse(valid_existing(path, row))
            old_payload = mod.parse_asset(path.read_bytes())[1]
            self.assertFalse(upgrade_channel_metadata(path, {**row, 'descriptor_sha256': 'wrong'}))
            self.assertTrue(upgrade_channel_metadata(path, row))
            self.assertEqual(mod.parse_asset(path.read_bytes())[1], old_payload)
            save(row['channel_mapping'])
            self.assertTrue(valid_existing(path, row))
            self.assertFalse(valid_existing(path, {**row, 'descriptor_sha256': 'different'}))

    @staticmethod
    def _synthetic_bntx(fill=0, brtd_in_pixels=False):
        """A 0x800-byte BNTX with one 8x4 rgba8 texture of two mips, built field by field (no game data)."""
        import struct
        blob = bytearray(0x800)
        blob[:4] = b"BNTX"
        blob[0x20:0x24] = b"NX  "
        struct.pack_into("<IQ", blob, 0x24, 1, 0x100)
        struct.pack_into("<Q", blob, 0x100, 0x180)
        blob[0x180:0x184] = b"BRTI"
        blob[0x180 + 0x11] = 2
        struct.pack_into("<HHH", blob, 0x180 + 0x12, 1, 0, 2)
        struct.pack_into("<I", blob, 0x180 + 0x1c, 0x0B01)
        struct.pack_into("<5I", blob, 0x180 + 0x24, 8, 4, 1, 1, 0)
        struct.pack_into("<I", blob, 0x180 + 0x50, 0x400)
        struct.pack_into("<I", blob, 0x180 + 0x54, 0x200)
        blob[0x180 + 0x58:0x180 + 0x5c] = bytes([4, 3, 2, 1])
        struct.pack_into("<Q", blob, 0x180 + 0x60, 0x240)
        struct.pack_into("<Q", blob, 0x180 + 0x70, 0x280)
        struct.pack_into("<H", blob, 0x240, 4); blob[0x242:0x246] = b"test"
        struct.pack_into("<QQ", blob, 0x280, 0x310, 0x510)
        blob[0x300:0x304] = b"BRTD"
        struct.pack_into("<I", blob, 0x308, 0x400)
        for i in range(0x310, 0x710):
            blob[i] = fill
        if brtd_in_pixels:
            blob[0x400:0x404] = b"BRTD"   # pixel bytes that look like a data-block magic
        struct.pack_into("<I", blob, 0x1C, len(blob)); struct.pack_into("<Q", blob, 0x30, 0x300)
        return bytes(blob)

    def test_every_bntx_in_a_container_is_found_and_brtd_in_pixels_is_harmless(self):
        inv_spec = importlib.util.spec_from_file_location("native_asset_inventory", Path(__file__).parents[1] / "tools" / "native_asset_inventory.py")
        inv = importlib.util.module_from_spec(inv_spec); inv_spec.loader.exec_module(inv)
        layout_spec = importlib.util.spec_from_file_location("native_asset_layout", Path(__file__).parents[1] / "tools" / "native_asset_layout.py")
        layout = importlib.util.module_from_spec(layout_spec); layout_spec.loader.exec_module(layout)
        first, second = self._synthetic_bntx(1), self._synthetic_bntx(2, brtd_in_pixels=True)
        pad = bytes(128)
        raw = pad + first + bytes(0x40) + second + bytes(0x20)
        offsets = layout.bntx_blob_offsets(raw)
        self.assertEqual(offsets, [128, 128 + len(first) + 0x40])
        # Each blob parses on its own and the first does not see the second's BRTD magic or pixels.
        for offset in offsets:
            self.assertEqual(len(layout.parse_bntx(raw[offset:])), 1)
        # A texture whose pixel data contains "BRTD" is still a record.
        self.assertEqual(len(inv.bntx_records(second)), 1)
        # A file size past the end of the resource, or inside the header, is rejected.
        import struct
        short = bytearray(first); struct.pack_into("<I", short, 0x1C, len(short) + 1)
        with self.assertRaises(ValueError): layout.parse_bntx(bytes(short))
        tiny = bytearray(first); struct.pack_into("<I", tiny, 0x1C, 0x10)
        with self.assertRaises(ValueError): layout.parse_bntx(bytes(tiny))
        # The data block must be where the header says it is.
        moved = bytearray(first); struct.pack_into("<Q", moved, 0x30, 0x320)
        with self.assertRaises(ValueError): layout.parse_bntx(bytes(moved))

    def test_generated_output_guard_and_asset_ids(self):
        import os, tempfile
        import native_asset_paths as paths
        with tempfile.TemporaryDirectory() as td:
            repo = Path(td) / "repo"
            (repo / "build").mkdir(parents=True); (repo / "generated").mkdir(); (repo / "tools").mkdir()
            self.assertEqual(paths.require_generated_output(repo / "build" / "a" / "x.json", repo), (repo / "build" / "a" / "x.json").resolve())
            self.assertEqual(paths.require_generated_output(repo / "generated" / "x.json", repo), (repo / "generated" / "x.json").resolve())
            for bad in (repo / "tools" / "x.json", repo / "x.json", repo / "build" / ".." / "tools" / "x.json",
                        Path(td) / "outside.json", repo / "build_extra" / "x.json"):
                with self.assertRaises(ValueError): paths.require_generated_output(bad, repo)
            outside = Path(td) / "outside"; outside.mkdir()
            try:
                os.symlink(outside, repo / "build" / "link", target_is_directory=True)
            except (OSError, NotImplementedError):
                link_made = False
            else:
                link_made = True
            if link_made:   # a symlink inside build/ must not lead out of it
                with self.assertRaises(ValueError): paths.require_generated_output(repo / "build" / "link" / "x.json", repo)
            report = paths.write_report(repo / "build" / "r.json", {"a": 1}, repo)
            self.assertIn(paths.GENERATED_MARKER, report.read_text(encoding="utf-8"))
            with self.assertRaises(ValueError): paths.write_report(repo / "tools" / "r.json", {"a": 1}, repo)
        good = "0123456789abcdef" * 4
        self.assertEqual(paths.require_asset_id(good), good)
        for bad in ("../" + good[3:], good.upper(), good[:-1], good + "0", "", None, 5, good[:-1] + "/"):
            with self.assertRaises(ValueError): paths.require_asset_id(bad)

    def test_generated_marker_is_in_the_artifact_and_dkntex_is_blocked(self):
        import native_asset_paths as paths
        blob = mod.make_asset(bytes(1024), 8, 4, "rgba8", False, 1)
        meta, _ = mod.parse_asset(blob)
        self.assertEqual(meta["generated_marker"], paths.GENERATED_MARKER)
        cc_spec = importlib.util.spec_from_file_location("check_content", Path(__file__).parents[1] / "tools" / "check_content.py")
        cc = importlib.util.module_from_spec(cc_spec); cc_spec.loader.exec_module(cc)
        self.assertIn(".dkntex", cc.BLOCKED_SUFFIXES)
        self.assertEqual(cc.GENERATED_MARKER, paths.GENERATED_MARKER)

    def test_parse_asset_bounds_the_subresource_counts(self):
        import json, struct, time
        base = {"schema": "dk-native-texture", "version": 2, "format": "rgba8", "width": 8, "height": 4,
                "block_width": 1, "block_height": 1, "bytes_per_block": 4, "color_space": "linear",
                "layout": "linear-block-rows-subresources", "subresources": []}

        def blob_for(meta):
            header = json.dumps(meta).encode()
            return mod.MAGIC + struct.pack("<II", 2, len(header)) + header

        started = time.monotonic()
        for mips, layers in ((10_000_000, 1), (1, 10_000_000), (33, 1), (1, 65537)):
            with self.assertRaises(ValueError):
                mod.parse_asset(blob_for({**base, "mip_levels": mips, "layers": layers}))
        self.assertLess(time.monotonic() - started, 2.0)   # nothing of that size is built
        # Deeply nested metadata is a ValueError, not a RecursionError.
        header = b"[" * 100000 + b"]" * 100000
        with self.assertRaises(ValueError):
            mod.parse_asset(mod.MAGIC + struct.pack("<II", 2, len(header)) + header)

    def test_bc3_colour_blocks_are_always_four_colour(self):
        import struct
        import native_asset_reference as ref
        # c0 = pure blue < c1 = pure red: BC1 would use the 3-colour palette; the colour half of BC3 never does.
        colour = struct.pack("<HHI", 0x001F, 0xF800, 0xAAAAAAAA)   # every texel uses palette index 2
        alpha = bytes([255, 255]) + bytes(6)
        pixels = ref.decode_bc3(alpha + colour, 4, 4)
        r, g, b, a = pixels[0]
        self.assertAlmostEqual(r, 1 / 3, places=2); self.assertAlmostEqual(g, 0.0, places=2)
        self.assertAlmostEqual(b, 2 / 3, places=2); self.assertAlmostEqual(a, 1.0, places=2)
        # The same bytes read as BC1 (c0 <= c1) take the 3-colour palette: the midpoint.
        r1, _g1, b1, _a1 = ref.decode_bc1(colour, 4, 4)[0]
        self.assertAlmostEqual(r1, 0.5, places=2); self.assertAlmostEqual(b1, 0.5, places=2)
        self.assertIn("bc2", ref.NOT_COVERED)


if __name__ == "__main__": unittest.main()

