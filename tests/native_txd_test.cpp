#include <algorithm>
#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;

static unsigned checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)

// TXD 2D (dimension 1): destination Rd, coordinates R8:R9, gradients R10:R13, channel mask and the "high"
// destination (third and fourth channels). Built field by field, not captured.
static Instruction txd(unsigned dst, unsigned high, unsigned mask) {
    Instruction op{};
    op.words[0] = 0xb6cu | (7u << 12) | (dst << 16) | (8u << 24);
    op.words[1] = 10u | (2u << 22) | (1u << 29); // derivatives R10, binding 2, dimension 1 (2D)
    op.words[2] = 0x001e0000u | (mask << 8) | high;
    return op;
}

static LoweredProgram lower(Instruction op) {
    Instruction end{};
    end.words[0] = (7u << 12) | 0x94d;
    end.words[2] = 7u << 23;
    const std::array code{op, end};
    return LowerToMaxwell(code, "txd");
}

int main() {
    CHECK(bool(lower(txd(4, 6, 15))));   // four channels, consecutive pairs R4:R5, R6:R7
    CHECK(bool(lower(txd(4, 12, 15))));  // four channels, second pair elsewhere
    CHECK(bool(lower(txd(4, 255, 15)))); // four channels, third and fourth discarded (high == RZ): lowered before
    CHECK(bool(lower(txd(4, 12, 7))));   // three channels, third channel to R12
    CHECK(!lower(txd(4, 255, 7)));       // three channels need a real register for the third
    CHECK(!lower(txd(4, 5, 15)));        // odd pair for the third and fourth channels
    CHECK(!lower(txd(5, 6, 15)));        // odd destination pair
    CHECK(!lower(txd(4, 6, 3)));         // unsupported channel mask
    std::printf("native TXD destinations: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
