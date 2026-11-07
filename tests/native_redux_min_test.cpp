#include <array>
#include <cstdio>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;
static unsigned checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)
static Instruction redux_min() { Instruction op{}; op.words={0x040673c4,0,0x00010000,0x002e6e00}; return op; }
static LoweredProgram lower(Instruction op) { Instruction end{}; end.words[0]=(7u<<12)|0x94d; end.words[2]=7u<<23; const std::array code{op,end}; return LowerToMaxwell(code,"redux-min-unsigned"); }
int main() {
    auto result=lower(redux_min()); CHECK(bool(result));
    const std::uint64_t minop=0x5c20000000070000ULL | 222ULL | (222ULL<<8) | (251ULL<<20) | (7ULL<<39);
    CHECK(std::find(result.instructions.begin(),result.instructions.end(),minop)!=result.instructions.end());
    auto max=redux_min(); max.words[0]=0x040773c4; max.words[2]=0x00014000; max.words[3]=0x000e3000; CHECK(bool(lower(max)));
    auto unsupported=redux_min(); unsupported.words[2]=0x00018000; CHECK(!lower(unsupported));
    unsupported=redux_min(); unsupported.words[0]=(unsupported.words[0]&~(15u<<12))|(6u<<12); CHECK(!lower(unsupported));
    std::printf("native REDUX unsigned min: %u checks, %u failures\n",checks,failures); return failures!=0;
}
