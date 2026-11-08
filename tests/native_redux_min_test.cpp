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
    // Inactive lanes must not change the unsigned minimum: the per-lane mask step is LOP.OR with B inverted
    // (bits 41 = OR, 40 = negate B), giving 0xffffffff for an inactive lane, never the AND that yields 0.
    const std::uint64_t mask_or=0x5c40000000070000ULL | (1ULL<<41) | (1ULL<<40) | 251ULL | (251ULL<<8) | (252ULL<<20) | (7ULL<<48);
    const std::uint64_t mask_and=0x5c40000000070000ULL | 251ULL | (251ULL<<8) | (252ULL<<20) | (7ULL<<48);
    CHECK(std::count(result.instructions.begin(),result.instructions.end(),mask_or)==32);
    CHECK(std::count(result.instructions.begin(),result.instructions.end(),mask_and)==0);
    auto max=redux_min(); max.words[0]=0x040773c4; max.words[2]=0x00014000; max.words[3]=0x000e3000;
    auto max_result=lower(max); CHECK(bool(max_result));
    CHECK(std::count(max_result.instructions.begin(),max_result.instructions.end(),mask_and)==32);
    CHECK(std::count(max_result.instructions.begin(),max_result.instructions.end(),mask_or)==0);
    auto sum=redux_min(); sum.words[2]=0x0000c000; auto sum_result=lower(sum); CHECK(bool(sum_result));
    CHECK(std::count(sum_result.instructions.begin(),sum_result.instructions.end(),mask_and)==32);
    auto unsupported=redux_min(); unsupported.words[2]=0x00018000; CHECK(!lower(unsupported));
    unsupported=redux_min(); unsupported.words[0]=(unsupported.words[0]&~(15u<<12))|(6u<<12); CHECK(!lower(unsupported));
    std::printf("native REDUX unsigned min: %u checks, %u failures\n",checks,failures); return failures!=0;
}
