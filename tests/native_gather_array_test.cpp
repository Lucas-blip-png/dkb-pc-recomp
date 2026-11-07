#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
static LoweredProgram lower(Instruction op) {
    Instruction end{}; end.words[0] = (7u << 12) | 0x94d; end.words[2] = 7u << 23;
    return LowerToMaxwell(std::array{op, end}, "array-gather");
}
int main() {
    // TLD4.SCR.R R20,R34,R10,R36, array 2D, all four components.
    Instruction op{{0x0a227b63, 0xb0800824, 0x041e0f14, 0x100f4200}};
    const auto r = lower(op);
    CHECK(bool(r));
    CHECK(r.required_texture_buffer == 2);
    if (r && r.instructions.size() >= 8) {
        for (unsigned i = 0; i < 3; ++i) {
            CHECK((r.instructions[i] & 255) == 250 + i);
            CHECK(((r.instructions[i] >> 20) & 255) == (i < 2 ? 10 + i : 36));
        }
        CHECK(((r.instructions[3] >> 28) & 7) == 3);
        CHECK(((r.instructions[3] >> 31) & 15) == 15);
        for (unsigned i = 0; i < 4; ++i) CHECK((r.instructions[4 + i] & 255) == (i < 2 ? 34 + i : 20 + i - 2));
    } else CHECK(false);
    op.words[2] |= 1u << 12; CHECK(!lower(op)); // offset form
    op.words[2] &= ~(1u << 12); op.words[0] |= 1u << 24; CHECK(!lower(op)); // odd coordinate pair
    std::printf("native array gather: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
