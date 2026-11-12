"""Vertex modules through the real light-quad fold (runtime/spirv_lq_fold.cpp), each output checked with spirv-val. No
Vulkan, GPU or game runs.

The modules are synthetic, with the shape the fold looks for: an Input at Location 9 and `x & 3` results of every class.
Two are of the test class (one feeds an OpIEqual whose OpSelect reaches the output, one an OpINotEqual that decides the
module's only selection construct), one only reaches the output through a conversion, and one is the rounding fix-up
(x >> 31) & 3 of a signed divide, which also feeds a compare. The test checks that the whole fold replaces exactly the
two test-class results by a null constant and the module still validates; that the construct mode replaces only the one
that decides construct 0; that a result stored to a function-local variable and loaded back in the same block is
followed to its compare; and that near misses are refused: no Location 9 input (whole fold only), a third test-class
result, a fragment stage, and a construct no test-class result reaches. It does not execute the shaders.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
OP_COPY_OBJECT, OP_BITWISE_AND, OP_CONSTANT_NULL = 83, 199, 46


def instr(op, *words):
    return [(len(words) + 1) << 16 | op, *words]


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack(f'<{len(b) // 4}I', b))


def module(stage=0, location=9, third_test=False, via_local=False):
    """Returns (words, ids): ids names the `& 3` results A1 (test, select), A2 (test, branch), A3 (output only), R (round)."""
    ids = iter(range(1, 1000))
    (MAIN, IN, OUT, VOID, FN, INT, F32, BOOL, VEC4, PTR_I, PTR_O, C0, C1, C2, C3, C31, F0, F1, L0, LT, LM, V, X0, X, X1,
     A1, A2, A3, S, R, CR, CA1, F1S, CA2, F3, FR, SUM, SUM2, A4, CA4, F4S, SUM3, PTR_F, LOCAL, A1L) = (next(ids) for _ in range(45))
    w = []
    w += instr(17, 1)                                            # OpCapability Shader
    w += instr(14, 0, 1)                                         # OpMemoryModel Logical GLSL450
    w += instr(15, stage, MAIN, *string('main'), IN, OUT)        # OpEntryPoint
    if stage == 4:
        w += instr(16, MAIN, 7)                                  # OpExecutionMode OriginUpperLeft
    w += instr(71, IN, 30, location)                             # Location of the instance stream
    w += instr(71, OUT, 30, 0)
    w += instr(19, VOID) + instr(33, FN, VOID)
    w += instr(21, INT, 32, 1) + instr(22, F32, 32) + instr(20, BOOL) + instr(23, VEC4, F32, 4)
    w += instr(32, PTR_I, 1, VEC4) + instr(32, PTR_O, 3, F32) + instr(32, PTR_F, 7, INT)
    for cid, value in ((C0, 0), (C1, 1), (C2, 2), (C3, 3), (C31, 31)):
        w += instr(43, INT, cid, value)
    w += instr(43, F32, F0, f32(0.0)) + instr(43, F32, F1, f32(1.0))
    w += instr(59, PTR_I, IN, 1) + instr(59, PTR_O, OUT, 3)
    w += instr(54, VOID, MAIN, 0, FN) + instr(248, L0)
    w += instr(59, PTR_F, LOCAL, 7)                              # a function-local variable
    w += instr(61, VEC4, V, IN)                                  # OpLoad
    w += instr(81, F32, X0, V, 0)                                # OpCompositeExtract .x
    w += instr(110, INT, X, X0)                                  # OpConvertFToS
    w += instr(128, INT, X1, X, C1)                              # OpIAdd
    w += instr(OP_BITWISE_AND, INT, A1, X, C3)
    w += instr(OP_BITWISE_AND, INT, A2, X1, C3)
    w += instr(OP_BITWISE_AND, INT, A3, X, C3)
    w += instr(195, INT, S, X, C31)                              # OpShiftRightArithmetic
    w += instr(OP_BITWISE_AND, INT, R, S, C3)
    w += instr(170, BOOL, CR, R, C0)                             # OpIEqual: the round class is used by a compare too
    if via_local:   # A1 reaches its compare through the local, as the translator often routes it
        w += instr(62, LOCAL, A1) + instr(61, INT, A1L, LOCAL)
    w += instr(170, BOOL, CA1, A1L if via_local else A1, C1)     # OpIEqual
    w += instr(169, F32, F1S, CA1, F1, F0)                       # OpSelect
    w += instr(171, BOOL, CA2, A2, C2)                           # OpINotEqual
    w += instr(111, F32, F3, A3)                                 # OpConvertSToF
    w += instr(169, F32, FR, CR, F1, F0)
    w += instr(129, F32, SUM, F1S, F3) + instr(129, F32, SUM2, SUM, FR)   # OpFAdd
    last = SUM2
    if third_test:
        w += instr(OP_BITWISE_AND, INT, A4, X1, C3)
        w += instr(170, BOOL, CA4, A4, C0)
        w += instr(169, F32, F4S, CA4, F1, F0)
        w += instr(129, F32, SUM3, SUM2, F4S)
        last = SUM3
    w += instr(247, LM, 0)                                       # OpSelectionMerge: construct 0
    w += instr(250, CA2, LT, LM)                                 # OpBranchConditional
    w += instr(248, LT) + instr(62, OUT, last) + instr(249, LM)  # OpStore, OpBranch
    w += instr(248, LM) + instr(253) + instr(56)                 # OpReturn, OpFunctionEnd
    header = [0x07230203, 0x00010000, 0, 1000, 0]
    return header + w, {'A1': A1, 'A2': A2, 'A3': A3, 'R': R, 'A4': A4}


def ops(words):
    i = 5
    while i < len(words):
        n = words[i] >> 16
        yield words[i] & 0xffff, words[i + 1:i + n]
        i += n


class LightQuadFold(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        args.out.mkdir(parents=True, exist_ok=True)
        harness = args.out / 'harness.c'
        harness.write_text(r'''#include <stdio.h>
#include <stdlib.h>
#include "spirv_lq_fold.h"
// harness <construct> in.spv out.spv: exit 3 when the fold refuses the module, 4 when the check and the fold disagree
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    static uint32_t w[1 << 20];
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 2;
    size_t n = fread(w, 4, 1 << 20, f);
    fclose(f);
    int construct = atoi(argv[1]);
    uint64_t out_n = 0;
    uint32_t *o = NULL;
    unsigned nulled = 0, checked = 0;
    int check = spirv_lq_fold(w, n, construct, NULL, NULL, &checked);
    int r = spirv_lq_fold(w, n, construct, &o, &out_n, &nulled);
    if (check != r || checked != nulled) return 4;
    if (r != 1) return 3;
    f = fopen(argv[3], "wb");
    fwrite(o, 4, out_n, f);
    fclose(f);
    free(o);
    printf("%u\n", nulled);
    return 0;
}
''', encoding='utf-8')
        cls.exe = args.out / 'harness.exe'
        build = subprocess.run([str(args.cc), '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'), str(harness),
                                str(ROOT / 'runtime/spirv_lq_fold.cpp'), '-lstdc++', '-o', str(cls.exe)],
                               capture_output=True, text=True, timeout=120)
        assert build.returncode == 0, build.stdout + build.stderr

    def validate(self, name, path):
        check = subprocess.run([str(args.spirv_val), '--target-env', 'vulkan1.3', str(path)], capture_output=True,
                               text=True, timeout=30)
        self.assertEqual(check.returncode, 0, name + ': ' + check.stdout + check.stderr)

    def fold(self, name, words, construct=-1):
        """The folded module, or None when the fold refuses it."""
        src = args.out / f'{name}.in.spv'
        dst = args.out / f'{name}.out.spv'
        src.write_bytes(struct.pack(f'<{len(words)}I', *words))
        self.validate(name + ' before the fold', src)
        run = subprocess.run([str(self.exe), str(construct), str(src), str(dst)], capture_output=True, text=True, timeout=30)
        if run.returncode == 3:
            return None
        self.assertEqual(run.returncode, 0, f'{name}: exit {run.returncode} {run.stderr}')
        self.validate(name + ' after the fold', dst)
        data = dst.read_bytes()
        return list(struct.unpack(f'<{len(data) // 4}I', data))

    def folded_ids(self, words):
        nulls = {a[1] for op, a in ops(words) if op == OP_CONSTANT_NULL}
        return {a[1] for op, a in ops(words) if op == OP_COPY_OBJECT and a[2] in nulls}

    def test_the_whole_fold_nulls_the_two_test_class_results(self):
        words, ids = module()
        out = self.fold('whole', words)
        self.assertIsNotNone(out)
        self.assertEqual(self.folded_ids(out), {ids['A1'], ids['A2']})
        ands = {a[1] for op, a in ops(out) if op == OP_BITWISE_AND}
        self.assertEqual(ands, {ids['A3'], ids['R']}, 'the output-only and the rounding `& 3` stay')

    def test_a_result_routed_through_a_local_is_followed(self):
        words, ids = module(via_local=True)
        out = self.fold('via_local', words)
        self.assertIsNotNone(out)
        self.assertEqual(self.folded_ids(out), {ids['A1'], ids['A2']})

    def test_the_construct_fold_nulls_only_what_decides_the_construct(self):
        words, ids = module(location=8)   # no Location 9 input: the construct mode does not need one
        out = self.fold('construct0', words, construct=0)
        self.assertIsNotNone(out)
        self.assertEqual(self.folded_ids(out), {ids['A2']})

    def test_near_misses_are_refused(self):
        cases = {
            'no_location9': (module(location=8)[0], -1),
            'third_test_result': (module(third_test=True)[0], -1),
            'fragment_stage': (module(stage=4)[0], -1),
            'construct_not_reached': (module()[0], 1),
        }
        for name, (words, construct) in cases.items():
            with self.subTest(name):
                self.assertIsNone(self.fold(name, words, construct))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--spirv-val', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args, rest = parser.parse_known_args()
    raise SystemExit(not unittest.main(argv=[__file__] + rest, exit=False).result.wasSuccessful())
