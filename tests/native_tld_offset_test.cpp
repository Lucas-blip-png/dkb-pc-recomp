// Ampere TLD.LZ with the address-offset flag (bit 76, AOFFI) on a 2D fetch. The lowering emits a Maxwell TLDS word, and
// the expected fields below come from the translator's own decoder, tools/vendor/ryujinx/src/Ryujinx.Graphics.Shader:
//  - Decoders/InstDecoders.cs InstTlds (lines 4928-4942): Dest bits 0..7, SrcA 8..15, Pred 16..18, SrcB 20..27,
//    Dest2 28..35, TidB 36..48, Nodep bit 49, WMask 50..52, Target 53..56.
//  - Decoders/InstDecoders.cs TldsTarget (lines 677-688): Texture2DLodZeroOffset = 0x4.
//  - Instructions/InstEmitTexture.cs (lines 530-533): target 4 reads its coordinates from Ra and Ra+1 (the Ra() helper
//    steps the register index), and (line 559) with the offset flag AddTextureOffset takes Rb as a packed offset: x in
//    bits 0..3, y in bits 4..7, each signed 4-bit (AddTextureOffset, InstEmitTexture.cs ~line 171).
// The Ampere side is a captured instruction of a compute stage: its offset register holds the constant 1 and the
// neighbouring offset registers hold 16 and 17, which is the 4-bit packing above. That packing is an inference from the
// register roles, not from an NVIDIA document.
#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
static LoweredProgram lower(Instruction op) {
    Instruction end{}; end.words[0] = (7u << 12) | 0x94d; end.words[2] = 7u << 23;
    return LowerToMaxwell(std::array{op, end}, "offset-fetch");
}
// The vendored decoder's view of a TLDS word (field positions as in InstTlds).
struct Tlds {
    u32 dest, srca, pred, srcb, dest2, tidb, wmask, target;
    bool nodep;
};
static Tlds decode_tlds(u64 w) {
    return Tlds{u32(w & 0xff), u32((w >> 8) & 0xff), u32((w >> 16) & 7), u32((w >> 20) & 0xff), u32((w >> 28) & 0xff),
                u32((w >> 36) & 0x1fff), u32((w >> 50) & 7), u32((w >> 53) & 0xf), ((w >> 49) & 1) != 0};
}
int main() {
    // A captured TLD.LZ.AOFFI R5, R2 (x), R3 (y), offset R8, 2D.
    Instruction aoffi{{0x02057b66, 0x30000408, 0x049e11ff, 0x000fc200}};
    const auto r = lower(aoffi);
    CHECK(bool(r));
    if (r && !r.instructions.empty()) {
        const u64 w = r.instructions[0];
        const Tlds t = decode_tlds(w);
        CHECK(t.target == 4);      // Texture2DLodZeroOffset
        CHECK(t.dest == 5);        // destination R5
        CHECK(t.srca == 2);        // coordinate pair R2 (x) and R3 (y) through the decoder's Ra() steps
        CHECK(t.srcb == 8);        // packed offset register R8
        CHECK(t.pred == 7);        // PT
        CHECK(t.dest2 == 255);     // no second destination (Ampere bits 64..71 = 0xff)
        CHECK(t.tidb == 4);        // descriptor/index field (Ampere bits 40..53)
        CHECK(t.wmask == 0);       // swizzle 0: R G (Ampere mask 1 in bits 72..75)
        CHECK(t.nodep);            // Ampere bit 90 (word 2 bit 26) = 1 in this word, copied to bit 49
    } else CHECK(false);
    // A non-offset 2D fetch keeps target 2 (the existing path), so the offset flag is what selects target 4.
    Instruction plain = aoffi;
    plain.words[2] &= ~(1u << 12);
    const auto p = lower(plain);
    CHECK(bool(p));
    if (p && !p.instructions.empty()) CHECK(((p.instructions[0] >> 53) & 15) == 2);
    else CHECK(false);
    // Offset with a multisample or array or volume fetch is not one of the captured forms: rejected.
    Instruction ms = aoffi; ms.words[2] |= 1u << 14;   // bit 78 multisample
    CHECK(!lower(ms));
    // 2D array (dimension 5) with the offset: no single TLDS target has both the layer and the offset, so it stays rejected.
    Instruction arr = aoffi; arr.words[1] = (arr.words[1] & ~(7u << 29)) | (5u << 29);
    CHECK(!lower(arr));
    // Odd coordinate pair: Ra+1 must be the second coordinate register of the pair.
    Instruction odd = aoffi; odd.words[0] = (odd.words[0] & ~(0xffu << 24)) | (3u << 24);
    CHECK(!lower(odd));
    std::printf("native offset fetch: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
