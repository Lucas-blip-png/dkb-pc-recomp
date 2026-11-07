// Narrow SULD dimension regression: lower the retained 1D mode and reject unimplemented dimensions.
#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
static void bits(Instruction& op, unsigned at, unsigned count, u32 value) {
    for (unsigned b=0;b<count;b++) { unsigned word=(at+b)/32, bit=(at+b)%32;
        op.words[word]=(op.words[word]&~(1u<<bit))|(((value>>b)&1u)<<bit); }
}
static LoweredProgram lower(unsigned dim) {
    Instruction suld{}, exit{};
    bits(suld,0,12,0xb97); bits(suld,12,4,7); bits(suld,16,8,12); bits(suld,24,8,32);
    bits(suld,40,14,16); bits(suld,54,5,3); bits(suld,61,3,dim); bits(suld,72,4,5);
    suld.words[2]=0x001e0000u|(5u<<8);
    bits(exit,0,12,0x94d); bits(exit,12,4,7); bits(exit,87,4,7);
    const std::array code{suld,exit}; return LowerToMaxwell(code,"suld-dimension");
}
int main() {
    for (unsigned dim : {0u,3u,4u}) {
        auto p=lower(dim); CHECK(bool(p)); if (!p) continue;
        CHECK(p.required_texture_buffer==3);
        unsigned found=0;
        for (auto op:p.instructions) if ((op>>56)==0xeb) {
            ++found; CHECK(((op>>33)&7u)==dim); CHECK(((op>>8)&255u)==32u);
            CHECK(((op>>20)&15u)==5u); CHECK(((op>>36)&0x1fffu)==16u);
        }
        CHECK(found==1);
    }
    for (unsigned dim : {1u,2u,5u,6u,7u}) CHECK(!lower(dim));
    std::printf("native SULD dimensions: %u checks, %u failures\n",checks,failures);
    return failures!=0;
}
