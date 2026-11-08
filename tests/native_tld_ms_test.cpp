#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
static LoweredProgram lower(Instruction op) {
    Instruction end{}; end.words[0] = (7u << 12) | 0x94d; end.words[2] = 7u << 23;
    return LowerToMaxwell(std::array{op, end}, "multisample-fetch");
}
int main() {
    Instruction op{{0x10127b66, 0x30800813, 0x049e4716, 0x001fc200}};
    const auto r = lower(op);
    CHECK(bool(r)); CHECK(r.required_texture_buffer == 2);
    if (r && !r.instructions.empty()) {
        const u64 x = r.instructions[0];
        CHECK(((x >> 53) & 15) == 6); // 2D multisample
        CHECK(((x >> 8) & 255) == 16); // x/y register pair
        CHECK(((x >> 20) & 255) == 19); // sample register
        CHECK((x & 255) == 18); CHECK(((x >> 28) & 255) == 22);
    } else CHECK(false);
    op.words[2] |= 1u << 12; CHECK(!lower(op)); // offset
    op.words[2] &= ~(1u << 12);
    // Plain (non-multisample) 2D fetch with a split second destination: bit 35 of the TLDS word is the top bit of the
    // second destination register (Dest2, bits 28..35), so it must be 0 here (second destination R22).
    auto plain = op; plain.words[2] &= ~(1u << 14);
    const auto plain_result = lower(plain);
    CHECK(bool(plain_result));
    if (plain_result && !plain_result.instructions.empty()) {
        const u64 x = plain_result.instructions[0];
        CHECK(((x >> 28) & 255) == 22); CHECK(((x >> 35) & 1) == 0); CHECK(((x >> 53) & 15) == 2);
    } else CHECK(false);
    // The address-offset flag (bit 76) on a 2D fetch lowers to TLDS target 4 with the offset in Rb (native_tld_offset_test).
    plain.words[2] |= 1u << 12; CHECK(lower(plain));
    if (lower(plain)) CHECK(((lower(plain).instructions[0] >> 53) & 15) == 4);
    op.words[2] &= ~(1u << 12); op.words[0] |= 1u << 24; CHECK(!lower(op)); // odd pair
    op.words[0] &= ~(1u << 24); op.words[1] = (op.words[1] & ~(7u << 29)) | (5u << 29);
    CHECK(!lower(op)); // array multisample not implemented
    std::printf("native multisample fetch: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
