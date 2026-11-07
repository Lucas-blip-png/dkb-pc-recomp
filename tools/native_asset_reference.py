"""Independent CPU reference decoders for converted texture payloads.

Each decoder takes the bytes of one mip level of one layer in the converter's output layout (block rows, row-major, no
padding) and returns a list of RGBA float tuples in [0, 1] (signed formats map to [-1, 1]). They exist to check GPU
sampling results. They do not depend on the converter, the probe or the runtime's own decoders.

Covered: rgba8, r16-uint (returned as raw integers, not normalised), b10g11r11-ufloat, rgba16f,
bc1, bc3, bc4, bc4-snorm, bc5, bc5-snorm. Not covered (no independent decoder here yet): bc6h, bc7, and every ASTC
block size. Those formats are reported as not verified by the sampling probe.
"""
import math
import struct

NOT_COVERED = ("bc6h-sfloat", "bc6h-ufloat", "bc7")


def _half(bits):
    return struct.unpack("<e", struct.pack("<H", bits))[0]


def _unorm(v, bits):
    return v / float((1 << bits) - 1)


def decode_rgba8(data, width, height):
    out = []
    for i in range(width * height):
        r, g, b, a = data[4 * i:4 * i + 4]
        out.append((r / 255.0, g / 255.0, b / 255.0, a / 255.0))
    return out


def decode_r16_uint(data, width, height):
    """Integer texels are returned unscaled: the value is the raw 16-bit unsigned integer (0..65535)."""
    return [(float(struct.unpack_from("<H", data, 2 * i)[0]), 0.0, 0.0, 1.0) for i in range(width * height)]


def _float_bits(bits, exp_bits, man_bits):
    """Unsigned small float (no sign bit), bias 15, with subnormals; inf and NaN map to inf and nan."""
    exp = bits >> man_bits
    man = bits & ((1 << man_bits) - 1)
    if exp == 0:
        return (man / float(1 << man_bits)) * 2.0 ** (1 - 15)
    if exp == (1 << exp_bits) - 1:
        return math.inf if man == 0 else math.nan
    return (1.0 + man / float(1 << man_bits)) * 2.0 ** (exp - 15)


def decode_b10g11r11_ufloat(data, width, height):
    out = []
    for i in range(width * height):
        word = struct.unpack_from("<I", data, 4 * i)[0]
        r = _float_bits(word & 0x7FF, 5, 6)           # bits 0..10
        g = _float_bits((word >> 11) & 0x7FF, 5, 6)   # bits 11..21
        b = _float_bits((word >> 22) & 0x3FF, 5, 5)   # bits 22..31
        out.append((r, g, b, 1.0))
    return out


def decode_rgba16f(data, width, height):
    out = []
    for i in range(width * height):
        vals = struct.unpack_from("<4H", data, 8 * i)
        out.append(tuple(_half(v) for v in vals))
    return out


def _rgb565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return ((r * 255 + 15) // 31 / 255.0, (g * 255 + 31) // 63 / 255.0, (b * 255 + 15) // 31 / 255.0)


def _bc1_colors(block):
    c0, c1 = struct.unpack_from("<HH", block, 0)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    if c0 > c1:
        p2 = tuple((2 * a + b) / 3.0 for a, b in zip(p0, p1))
        p3 = tuple((a + 2 * b) / 3.0 for a, b in zip(p0, p1))
        return [p0, p1, p2, p3], False
    p2 = tuple((a + b) / 2.0 for a, b in zip(p0, p1))
    return [p0, p1, p2, (0.0, 0.0, 0.0)], True  # index 3 is transparent black in the 3-colour mode


def _bc1_block(block):
    """Returns 16 RGBA tuples, row-major inside the 4x4 block."""
    colors, three = _bc1_colors(block)
    indices = struct.unpack_from("<I", block, 4)[0]
    px = []
    for i in range(16):
        idx = (indices >> (2 * i)) & 3
        r, g, b = colors[idx]
        a = 0.0 if (three and idx == 3) else 1.0
        px.append((r, g, b, a))
    return px


def _bc4_values(block, signed):
    a0, a1 = struct.unpack_from("<BB", block, 0)
    if signed:
        a0 = struct.unpack("<b", bytes([a0]))[0]
        a1 = struct.unpack("<b", bytes([a1]))[0]
        v0, v1 = max(-1.0, a0 / 127.0), max(-1.0, a1 / 127.0)  # -128 clamps to -1.0
        if a0 > a1:
            vals = [v0, v1] + [((7 - i) * v0 + i * v1) / 7.0 for i in range(1, 7)]
        else:
            vals = [v0, v1] + [((5 - i) * v0 + i * v1) / 5.0 for i in range(1, 5)] + [-1.0, 1.0]
    else:
        v0, v1 = a0 / 255.0, a1 / 255.0
        if a0 > a1:
            vals = [v0, v1] + [((7 - i) * v0 + i * v1) / 7.0 for i in range(1, 7)]
        else:
            vals = [v0, v1] + [((5 - i) * v0 + i * v1) / 5.0 for i in range(1, 5)] + [0.0, 1.0]
    bits = int.from_bytes(block[2:8], "little")
    return [vals[(bits >> (3 * i)) & 7] for i in range(16)]


def _block_iter(data, width, height, block_bytes, fn):
    bw, bh = (width + 3) // 4, (height + 3) // 4
    out = [None] * (width * height)
    for by in range(bh):
        for bx in range(bw):
            off = (by * bw + bx) * block_bytes
            pix = fn(data[off:off + block_bytes])
            for y in range(4):
                for x in range(4):
                    px, py = bx * 4 + x, by * 4 + y
                    if px < width and py < height:
                        out[py * width + px] = pix[y * 4 + x]
    return out


def decode_bc1(data, width, height):
    return _block_iter(data, width, height, 8, _bc1_block)


def decode_bc3(data, width, height):
    def block(b):
        alpha = _bc4_values(b[0:8], signed=False)
        color = _bc1_block(b[8:16])
        return [(r, g, bb, alpha[i]) for i, (r, g, bb, _a) in enumerate(color)]
    return _block_iter(data, width, height, 16, block)


def decode_bc4(data, width, height, signed=False):
    def block(b):
        v = _bc4_values(b, signed)
        return [(x, 0.0, 0.0, 1.0) for x in v]
    return _block_iter(data, width, height, 8, block)


def decode_bc5(data, width, height, signed=False):
    def block(b):
        r = _bc4_values(b[0:8], signed)
        g = _bc4_values(b[8:16], signed)
        return [(r[i], g[i], 0.0, 1.0) for i in range(16)]
    return _block_iter(data, width, height, 16, block)


DECODERS = {
    "rgba8": decode_rgba8,
    "r16-uint": decode_r16_uint,
    "b10g11r11-ufloat": decode_b10g11r11_ufloat,
    "rgba16f": decode_rgba16f,
    "bc1": decode_bc1,
    "bc3": decode_bc3,
    "bc4": lambda d, w, h: decode_bc4(d, w, h, signed=False),
    "bc4-snorm": lambda d, w, h: decode_bc4(d, w, h, signed=True),
    "bc5": lambda d, w, h: decode_bc5(d, w, h, signed=False),
    "bc5-snorm": lambda d, w, h: decode_bc5(d, w, h, signed=True),
}


def decode(fmt, data, width, height):
    """Decodes one mip level of one layer. Raises KeyError for formats with no independent decoder here."""
    return DECODERS[fmt](data, width, height)
