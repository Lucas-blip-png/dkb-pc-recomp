#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
static LoweredProgram lower(Instruction op) {
    Instruction end{}; end.words[0] = (7u << 12) | 0x94d; end.words[2] = 7u << 23;
    return LowerToMaxwell(std::array{op, end}, "uniform-carry");
}
int main() {
    Instruction uniform{{0x05050c10, 4, 0x08ffe4ff, 0x000fca00}};
    auto a = lower(uniform);
    CHECK(bool(a));
    Instruction ordinary = uniform;
    ordinary.words[0] = (ordinary.words[0] & ~0xfffu) | 0x210;
    ordinary.words[1] = 220; // UR4 maps to R220
    auto b = lower(ordinary);
    CHECK(bool(b)); CHECK(a.instructions == b.instructions);
    auto invalid = uniform; invalid.words[1] = 28; CHECK(!lower(invalid));
    invalid = uniform; invalid.words[1] |= 0x100; CHECK(!lower(invalid));
    invalid = uniform; invalid.words[2] &= ~(7u << 17); CHECK(!lower(invalid)); // carry-out with carry-in
    std::printf("native uniform carry: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
