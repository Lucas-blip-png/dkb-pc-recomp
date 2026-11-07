#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
static LoweredProgram lower(Instruction op) {
    Instruction end{}; end.words[0] = (7u << 12) | 0x94d; end.words[2] = 7u << 23;
    return LowerToMaxwell(std::array{op, end}, "strong-store");
}
int main() {
    Instruction op{{0xff000986, 0x00003007, 0x0c10e906, 0x000fee00}};
    const auto r = lower(op);
    CHECK(bool(r));
    if (r && r.instructions.size() >= 3) {
        CHECK(r.instructions[0] == 0xef98000000000100ULL);
        CHECK(r.instructions[2] == 0xef98000000000100ULL);
        CHECK((r.instructions[1] & 255) == 7);
        CHECK(((r.instructions[1] >> 8) & 255) == 222); // UR6
        CHECK(((r.instructions[1] >> 20) & 0xffffff) == 0x30);
    } else CHECK(false);
    op.words[2] = (op.words[2] & ~0xe00u) | (5u << 9); CHECK(!lower(op));
    op.words[2] = 0x0c10e906; op.words[0] &= 0x00ffffffu; CHECK(!lower(op));
    // Captured 128-bit store with a 32-bit R4 offset and 64-bit UR4 base.
    Instruction wide{{0x04007986, 0, 0x0810ed04, 0x000fe200}};
    const auto w = lower(wide);
    CHECK(bool(w));
    if (w && w.instructions.size() >= 5) {
        CHECK(w.instructions[2] == 0xef98000000070100ULL);
        CHECK(w.instructions[4] == 0xef98000000070100ULL);
        CHECK(((w.instructions[3] >> 8) & 255) == 250);
        CHECK(((w.instructions[3] >> 48) & 7) == 6);
    } else CHECK(false);
    std::printf("native strong store: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
