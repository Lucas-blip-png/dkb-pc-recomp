#include <algorithm>
#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;

static unsigned checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)

static Instruction captured_funnel() {
    Instruction op{};
    op.words = {0x04067899, 0x00000010, 0x08010605, 0x000fe200};
    return op;
}

static LoweredProgram lower(Instruction op) {
    Instruction end{};
    end.words[0] = (7u << 12) | 0x94d;
    end.words[2] = 7u << 23;
    const std::array code{op, end};
    return LowerToMaxwell(code, "uniform-ushf-left-high");
}

int main() {
    auto result = lower(captured_funnel());
    CHECK(bool(result));
    // Destination UR6 maps to R222 (UR0 is R216), the low word UR4 to R220 and the high word UR5 to R221, so swapping
    // the two source words changes the expected word.
    const std::uint64_t expected = 0x36f8000000000000ULL | (7ULL << 16) | std::uint64_t{222} |
        (std::uint64_t{220} << 8) | (std::uint64_t{16} << 20) | (std::uint64_t{221} << 39);
    CHECK(std::find(result.instructions.begin(), result.instructions.end(), expected) != result.instructions.end());

    auto unsupported = captured_funnel();
    unsupported.words[1] = 33;
    CHECK(!lower(unsupported));
    unsupported = captured_funnel();
    unsupported.words[0] = (unsupported.words[0] & ~(15u << 12)) | (6u << 12);
    CHECK(!lower(unsupported));
    unsupported = captured_funnel();
    unsupported.words[2] |= 0x800;
    CHECK(!lower(unsupported));
    unsupported = captured_funnel();
    unsupported.words[2] = 0x08010600u | 30u;
    CHECK(!lower(unsupported));
    std::printf("native uniform USHF left-high: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}

