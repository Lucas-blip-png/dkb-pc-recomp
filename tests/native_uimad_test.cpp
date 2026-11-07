#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;

static unsigned checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)

static unsigned mapped(unsigned reg) {
    return reg < 22 ? 216 + reg : reg < 28 ? 218 + reg : 255;
}

static Instruction uimad(unsigned dst, unsigned left, unsigned right, unsigned immediate, bool uniform) {
    Instruction op{};
    const unsigned dst_reg = uniform ? dst : mapped(dst);
    const unsigned left_reg = uniform ? left : mapped(left);
    const unsigned right_reg = uniform ? right : mapped(right);
    op.words[0] = (left_reg << 24) | (dst_reg << 16) | (7u << 12) | (uniform ? 0x4a4u : 0x424u);
    op.words[1] = immediate;
    op.words[2] = (uniform ? 0x0f8e0200u : 0x078e0200u) | right_reg;
    return op;
}

static LoweredProgram lower(Instruction op) {
    Instruction end{};
    end.words[0] = (7u << 12) | 0x94d;
    end.words[2] = 7u << 23;
    const std::array code{op, end};
    return LowerToMaxwell(code, "uimad-immediate");
}

int main() {
    for (unsigned dst : {0u, 8u, 21u, 22u, 27u})
        for (unsigned left : {0u, 4u, 21u, 22u, 27u, 63u})
            for (unsigned right : {0u, 9u, 21u, 22u, 27u, 63u})
                for (unsigned imm : {0u, 1u, 0xa4u, 0xffffffffu}) {
                    auto actual = lower(uimad(dst, left, right, imm, true));
                    auto expected = lower(uimad(dst, left, right, imm, false));
                    CHECK(bool(actual));
                    CHECK(bool(expected));
                    CHECK(actual.instructions == expected.instructions);
                }
    CHECK(!lower(uimad(28, 4, 9, 1, true)));
    CHECK(!lower(uimad(8, 28, 9, 1, true)));
    CHECK(!lower(uimad(8, 4, 28, 1, true)));
    auto unsupported = uimad(8, 4, 9, 1, true);
    unsupported.words[2] |= 0x400u;
    CHECK(!lower(unsupported));
    auto op = uimad(8, 4, 9, 1, true);
    Instruction other{}, end{};
    other.words[0] = (216u << 16) | (7u << 12) | 0x301;
    end.words[0] = (7u << 12) | 0x94d;
    end.words[2] = 7u << 23;
    const std::array code{other, op, end};
    CHECK(!LowerToMaxwell(code, "uimad-reserved-register"));
    std::printf("native UIMAD immediate: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
