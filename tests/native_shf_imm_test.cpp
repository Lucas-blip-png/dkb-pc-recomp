#include <algorithm>
#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;

static unsigned checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)

// SHF.L.U32.HI Rd, RZ, Rb, imm; wrap is Ampere bit 75 (word 2 bit 11). Built field by field, not captured.
static Instruction shf(unsigned dst, unsigned shift_reg, unsigned immediate, bool wrap) {
    Instruction op{};
    op.words[0] = 0x419u | (7u << 12) | (dst << 16) | (255u << 24);
    op.words[1] = immediate;
    op.words[2] = 0x00010600u | shift_reg | (wrap ? 0x800u : 0u);
    return op;
}

static LoweredProgram lower(Instruction op) {
    Instruction end{};
    end.words[0] = (7u << 12) | 0x94d;
    end.words[2] = 7u << 23;
    const std::array code{op, end};
    return LowerToMaxwell(code, "shf-immediate");
}

static bool has(const LoweredProgram& program, std::uint64_t word) {
    return std::find(program.instructions.begin(), program.instructions.end(), word) != program.instructions.end();
}

int main() {
    // Maxwell SHL (0x5c48): Rd bits 0-7, Ra bits 8-15 (the immediate staged in R250), Rb bits 20-27,
    // wrap (shift masked with 31) in bit 39. Without .W a shift of 32 or more must give 0 (bit 39 clear).
    const std::uint64_t common = 0x5c48000000070000ULL | 5ULL | (250ULL << 8) | (7ULL << 20);
    const auto no_wrap = lower(shf(5, 7, 0x12345678u, false));
    CHECK(bool(no_wrap));
    CHECK(has(no_wrap, common));
    CHECK(!has(no_wrap, common | (1ULL << 39)));
    CHECK(!has(no_wrap, common | (1ULL << 50)));

    const auto wrapped = lower(shf(5, 7, 0x12345678u, true));
    CHECK(bool(wrapped));
    CHECK(has(wrapped, common | (1ULL << 39)));
    CHECK(!has(wrapped, common | (1ULL << 50)));

    auto unsupported = shf(5, 7, 1, false);
    unsupported.words[0] = (unsupported.words[0] & ~(255u << 24)) | (4u << 24);
    CHECK(!lower(unsupported));
    std::printf("native SHF immediate: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
