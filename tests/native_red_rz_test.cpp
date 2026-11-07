#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;

static unsigned checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)

static Instruction red_rz(unsigned address, unsigned operation, unsigned type) {
    Instruction op{};
    op.words[0] = (address << 24) | 0x0000798e;
    op.words[1] = 0x000000ff;
    op.words[2] = (type << 9) | (operation << 23) | 0x0000e100;
    op.words[3] = 0x000fe200;
    return op;
}

static LoweredProgram lower(Instruction op) {
    Instruction end{};
    end.words[0] = (7u << 12) | 0x94d;
    end.words[2] = 7u << 23;
    const std::array code{op, end};
    return LowerToMaxwell(code, "red-rz-zero-source");
}

int main() {
    for (const auto [address, operation, type] : std::array{
             std::array{0u, 2u, 0u}, std::array{2u, 1u, 0u}, std::array{4u, 2u, 0u},
             std::array{2u, 2u, 1u}}) {
        const auto result = lower(red_rz(address, operation, type));
        CHECK(bool(result));
        const std::uint64_t expected = 0xebf8000000000000ULL | (7ULL << 16) |
            255ULL | (std::uint64_t{address} << 8) | (std::uint64_t{type} << 20) |
            (std::uint64_t{operation} << 23) | (1ULL << 48);
        CHECK(std::find(result.instructions.begin(), result.instructions.end(), expected) != result.instructions.end());
    }
    for (unsigned reserved : {250u, 254u}) {
        auto op = red_rz(2, 2, 0);
        op.words[1] = reserved;
        CHECK(!lower(op));
    }
    auto unsupported = red_rz(2, 2, 0);
    unsupported.words[2] |= 8u << 23;
    CHECK(!lower(unsupported));
    std::printf("native RED RZ source: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}





