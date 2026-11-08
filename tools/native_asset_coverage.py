#!/usr/bin/env python3
"""Report offline texture conversion coverage without inferring conversion from file presence."""
import argparse
import json
from pathlib import Path

from native_asset_paths import write_report

# Runtime format mappings inspected in runtime/vk_formats.c. Converter support is narrower.
RUNTIME_FORMATS = ["rgba8", "r16-uint", "b10g11r11-ufloat", "r8", "rg8", "rgba16f", "bc1", "bc2", "bc3", "bc4", "bc5", "bc6h", "bc7", "astc-4x4", "astc-5x4", "astc-5x5", "astc-6x5", "astc-6x6", "astc-8x5", "astc-8x6", "astc-8x8", "astc-10x5", "astc-10x6", "astc-10x8", "astc-10x10", "astc-12x10", "astc-12x12", "depth-stencil"]
CONVERTER_FORMATS = ["rgba8", "r16-uint", "b10g11r11-ufloat", "rgba16f", "bc1", "bc2", "bc3", "bc4", "bc4-snorm", "bc5", "bc5-snorm",
                     "bc6h-sfloat", "bc6h-ufloat", "bc7", "astc-4x4", "astc-5x4", "astc-5x5",
                     "astc-6x5", "astc-6x6", "astc-8x5", "astc-8x6", "astc-8x8", "astc-10x5",
                     "astc-10x6", "astc-10x8", "astc-10x10", "astc-12x10", "astc-12x12"]
SUPPORTED_LAYOUTS = ["switch2-block-linear-2d", "single-subresource", "complete-mip-layer-grid"]
UNSUPPORTED_LAYOUTS = ["linear-source", "3d", "multisample", "swizzle-channel-remap", "sparse-or-partial-mip-layer-grid"]


def inventory(records):
    """records explicitly describe discovered identities and verified conversions."""
    rows = []
    seen = set()
    for r in records:
        identity = r.get("asset_id")
        if not isinstance(identity, str) or not identity or identity in seen:
            raise ValueError("each observed asset needs a unique non-empty asset_id")
        seen.add(identity)
        fmt, layout = r.get("format"), r.get("layout")
        converted = r.get("converted") is True and isinstance(r.get("artifact_sha256"), str) and bool(r["artifact_sha256"])
        rows.append({"asset_id": identity, "format": fmt, "layout": layout,
                     "status": "converted" if converted and fmt in CONVERTER_FORMATS and layout in SUPPORTED_LAYOUTS else
                               "unsupported" if fmt not in CONVERTER_FORMATS or layout not in SUPPORTED_LAYOUTS else "not-converted",
                     "reason": None if converted and fmt in CONVERTER_FORMATS and layout in SUPPORTED_LAYOUTS else
                              "format-or-layout-unsupported" if fmt not in CONVERTER_FORMATS or layout not in SUPPORTED_LAYOUTS else
                              "no-verified-conversion-artifact"})
    return {"schema": "dk-native-asset-coverage", "version": 1, "producer": "native_asset_coverage.py",
            "runtime_formats": RUNTIME_FORMATS, "converter_formats": CONVERTER_FORMATS,
            "supported_layouts": SUPPORTED_LAYOUTS, "unsupported_layouts": UNSUPPORTED_LAYOUTS,
            "observed_assets": rows, "absent_assets": "unknown-not-converted",
            "coverage_complete": False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("records", type=Path, help="JSON array of explicitly observed asset records")
    p.add_argument("output", type=Path)
    a = p.parse_args()
    result = inventory(json.loads(a.records.read_text(encoding="utf-8")))
    write_report(a.output, result)


if __name__ == "__main__": main()
