// spirv_lq_fold.cpp: the light-quad fold, a workaround for an NVIDIA fault in a few Bananza vertex stages (gfx_spirv.c).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The fold of vertex module w (n words). The stages it is for compute a selector r = (a computed index) & 3 that picks
// one of four constant-buffer words, and an `& 3` whose result feeds an integer compare, an OpSelect or an OpSwitch
// (the "test class") is replaced by 0, so r is 0 for every invocation.
//   construct < 0: the whole fold. The module must be a vertex stage with an Input at Location 9 and exactly two
//                  test-class `& 3` results; both become 0.
//   construct >= 0: only the test-class `& 3` results that reach the branch condition of selection construct number
//                  `construct` (OpSelectionMerge instructions counted in module order from 0) become 0. No Location 9
//                  input is required; at least one such result must exist.
// Returns 1 and a malloc'd copy in *out (*out_n words, *nulled results replaced) when the pattern is there, 0 when it is
// not (nothing is allocated), -1 when the module cannot be read. With out == NULL it only checks the pattern.
int spirv_lq_fold(const uint32_t *w, uint64_t n, int construct, uint32_t **out, uint64_t *out_n, unsigned *nulled);

#ifdef __cplusplus
}
#endif
