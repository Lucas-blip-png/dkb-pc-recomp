#include <array>
#include <cstdio>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;

static unsigned checks;
static unsigned failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)

static Instruction atomic_add() {
    Instruction op{};
    op.words = {0x030c0593, 0x00000c0c, 0x001ee000, 0x002ea200};
    return op;
}

static LoweredProgram lower(Instruction op) {
    Instruction end{};
    end.words[0] = (7u << 12) | 0x94d;
    end.words[2] = 7u << 23;
    const std::array code{op, end};
    return LowerToMaxwell(code, "suatom-1d-add");
}

int main() {
    auto result = lower(atomic_add());
    CHECK(bool(result));
    CHECK(result.required_texture_buffer == 0);
    const std::uint64_t barrier = 0xef98000000000100ULL;
    const std::uint64_t atomic = 0xea0000c000c0030cULL;
    const auto has = [&](std::uint64_t instruction) {
        return std::find(result.instructions.begin(), result.instructions.end(), instruction) != result.instructions.end();
    };
    CHECK(has(barrier));
    CHECK(has(atomic));
    CHECK(std::count(result.instructions.begin(), result.instructions.end(), barrier) == 2);
    auto unsupported = atomic_add();
    unsupported.words[2] ^= 0x1000;
    CHECK(!lower(unsupported));
    unsupported = atomic_add();
    unsupported.words[2] = 0x001ee000;
    unsupported.words[1] |= 1u << 29;
    CHECK(!lower(unsupported));
    std::printf("native SUATOM 1D add: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
