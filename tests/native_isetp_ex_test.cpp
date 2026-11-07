#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
static LoweredProgram lower(Instruction op) {
    Instruction end{}; end.words[0] = (7u << 12) | 0x94d; end.words[2] = 7u << 23;
    return LowerToMaxwell(std::array{op, end}, "extended-comparison");
}
int main() {
    Instruction op{{0x0500720c, 0x000000ff, 0x03f05100, 0x000fda00}};
    for (unsigned cmp : {2u, 5u}) {
        op.words[2] = (op.words[2] & ~(7u << 12)) | (cmp << 12);
        auto result = lower(op);
        CHECK(bool(result));
        if (!result) continue;
        const u64 inst = result.instructions[0];
        CHECK(((inst >> 49) & 7) == cmp);
        CHECK(((inst >> 45) & 3) == (cmp == 5));
        CHECK(((inst >> 39) & 15) == 0);
        for (unsigned equal = 0; equal < 2; ++equal) for (unsigned low = 0; low < 2; ++low) {
            const bool reference = equal ? low : cmp == 5;
            const bool comparison = cmp == 2 ? equal : !equal;
            const bool lowered = cmp == 2 ? comparison && low : comparison || low;
            CHECK(reference == lowered);
        }
    }
    op.words[2] = (op.words[2] & ~(7u << 12)) | (1u << 12); CHECK(!lower(op));
    op.words[2] = (op.words[2] & ~(7u << 12)) | (5u << 12);
    op.words[2] &= ~(7u << 20); CHECK(!lower(op));
    op.words[2] |= 7u << 20; op.words[2] &= ~(7u << 23); CHECK(!lower(op));
    std::printf("native ISETP.EX: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
