#include <algorithm>
#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)

static LoweredProgram lower(Instruction op) {
    Instruction end{};
    end.words[0] = (7u << 12) | 0x94d;
    end.words[2] = 7u << 23;
    return LowerToMaxwell(std::array{op, end}, "local-last-use");
}

int main() {
    // Captured LDL.LU R22, [0x4].
    Instruction op{{0xff167983, 0x00000400, 0x00300800, 0x000f2200}};
    const auto last = lower(op);
    op.words[2] = 0x00100800;
    const auto normal = lower(op);
    CHECK(bool(last));
    CHECK(bool(normal));
    CHECK(last.instructions == normal.instructions);
    const u64 expected = 0xef40000000000000ULL | (7ULL << 16) | 22ULL |
                         (255ULL << 8) | (4ULL << 20) | (4ULL << 48);
    CHECK(std::find(last.instructions.begin(), last.instructions.end(), expected) != last.instructions.end());
    op.words[2] = 0x00500800;
    CHECK(!lower(op));
    Instruction store{{0xff007387, 0x00000416, 0x00100800, 0x000f2200}};
    CHECK(bool(lower(store)));
    store.words[2] = 0x00300800;
    CHECK(!lower(store));
    // 128-bit LDL (memory type 6, four registers, 16-byte aligned offset). R240-R243 is fine; R244-R247 would
    // write the lowering scratch registers R246 and R247 and must be rejected.
    auto wide = [](unsigned dst) { return Instruction{{0xff007983u | (dst << 16), 0x00001000u, 0x00100c00u, 0x000f2200u}}; };
    CHECK(bool(lower(wide(240))));
    CHECK(!lower(wide(244)));
    CHECK(!lower(wide(246)));
    // The same bound applies to a wide store from R244.
    auto wide_store = [](unsigned src) { return Instruction{{0xff007387u, 0x00001000u | src, 0x00100c00u, 0x000f2200u}}; };
    CHECK(bool(lower(wide_store(240))));
    CHECK(!lower(wide_store(244)));
    std::printf("native LDL.LU: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
