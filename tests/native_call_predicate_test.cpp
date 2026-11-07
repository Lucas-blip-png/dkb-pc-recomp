#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
int main() {
    std::array<Instruction, 5> code{};
    code[0].words = {0x00007802, 32, 0xf00, 0}; // link R0
    code[1].words = {0x7944, 16, 0x03c00000, 0}; // call index 3
    code[2].words = {0x794d, 0, 0x03800000, 0}; // exit
    code[3].words = {0x1c00720b, 0x1b, 0x03f5d000, 0x000fda00}; // captured FSETP, no GPR write
    code[4].words = {0x7950, 0xffffffb0, 0x03c3ffff, 0}; // return through R0
    const auto ok = LowerToMaxwell(code, "call-predicate");
    CHECK(bool(ok));
    if (!ok) std::fprintf(stderr, "unexpected: %.*s at %u\n", int(ok.error.size()), ok.error.data(), ok.error_offset);
    code[3].words = {0x0000a947, 0, 0x03800000, 0}; // @!P2 BRA to next instruction
    CHECK(bool(LowerToMaxwell(code, "call-branch")));
    code[3].words = {0x01007202, 0, 0xf00, 0}; // MOV R0, R1 destroys link
    const auto bad = LowerToMaxwell(code, "call-write");
    CHECK(!bad); CHECK(bad.error == "native callee overwrites link register");
    std::printf("native call predicate: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
