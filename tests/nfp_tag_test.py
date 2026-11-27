"""The amiibo tag reader of runtime/ov_nfp.c on a pretended tag: no tag dump is in the repository.

The actual parse_flipper, tag_is_figure and model_fill run on an NTAG215 image built here: the Flipper Zero text form
gives back the bytes it was written from, a file with too few pages is refused, only a figure's tag passes the test, and
ModelInfo has the series in byte 3, the model number as a native u16 and the figure's type in byte 6 (the order
nn::nfp gives, not the tag's).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from nvn2_compute_c0_test import function

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG_BYTES 540
#define TAG_MODEL 0x54
@HEXVAL@
@FLIPPER@
@FIGURE@
@MODEL@

static unsigned failures;
#define CHECK(c, what) do { if (!(c)) failures++, fprintf(stderr, "FAIL %s\n", what); } while (0)

int main(void) {
    // A pretended figure's tag: an ID, the NTAG215 capability container, and a model with every byte distinct
    uint8_t t[TAG_BYTES];
    for (int i = 0; i < TAG_BYTES; i++) t[i] = (uint8_t)(i * 7 + 3);
    static const uint8_t cc[4] = {0xf1, 0x10, 0xff, 0xee};
    memcpy(t + 0x0c, cc, 4);
    static const uint8_t model[8] = {0x12, 0x34, 0x56, 0x01, 0xab, 0xcd, 0x1a, 0x02};   // id id variant type num num series 02
    memcpy(t + TAG_MODEL, model, 8);
    CHECK(tag_is_figure(t), "a figure's tag is not taken for one");

    uint8_t u[TAG_BYTES];
    memcpy(u, t, sizeof u), u[0x0c] ^= 1;
    CHECK(!tag_is_figure(u), "a tag that is no NTAG215 passes");
    memcpy(u, t, sizeof u), u[TAG_MODEL + 7] = 0;
    CHECK(!tag_is_figure(u), "a model that does not end in 02 passes");

    // The Flipper Zero form, with the header lines such a file has and a line that is not a page
    static char text[16384];
    int n = snprintf(text, sizeof text, "Filetype: Flipper NFC device\nVersion: 4\nPages total: 135\nPages read: 135\n");
    for (int p = 0; p < 135; p++)
        n += snprintf(text + n, sizeof text - (size_t)n, "Page %d: %02X %02X %02X %02X\n", p, t[p * 4], t[p * 4 + 1],
                      t[p * 4 + 2], t[p * 4 + 3]);
    memset(u, 0, sizeof u);
    CHECK(parse_flipper(text, (uint64_t)n, u), "135 pages are refused");
    CHECK(!memcmp(u, t, sizeof u), "the pages read differ from the tag");
    // 129 pages: too few for a tag
    const char *cut = strstr(text, "Page 129:");
    memset(u, 0, sizeof u);
    CHECK(!parse_flipper(text, (uint64_t)(cut - text), u), "129 pages pass");
    // lower-case digits and no newline after the last page
    for (char *q = text; *q; q++) *q = (char)(*q >= 'A' && *q <= 'F' ? tolower(*q) : *q);
    memset(u, 0, sizeof u);
    CHECK(parse_flipper(text, (uint64_t)n - 1, u) && !memcmp(u, t, sizeof u), "lower-case pages differ");

    uint8_t m[0x40];
    memset(m, 0xee, sizeof m);
    model_fill(m, t);
    static const uint8_t want[8] = {0x12, 0x34, 0x56, 0x1a, 0xcd, 0xab, 0x01, 0};
    CHECK(!memcmp(m, want, 8), "ModelInfo is not {character[3], series, number (native u16), type}");
    int rest = 0;
    for (int i = 8; i < 0x40; i++) rest |= m[i];
    CHECK(!rest, "ModelInfo's reserved bytes are not zero");

    if (!failures) printf("nfp tag: the Flipper form, the figure test and ModelInfo hold\n");
    return failures != 0;
}
"""


def source(nfp):
    return (FIXTURE.replace('@HEXVAL@', function(nfp, 'static int hexval('))
            .replace('@FLIPPER@', function(nfp, 'static int parse_flipper('))
            .replace('@FIGURE@', function(nfp, 'static int tag_is_figure('))
            .replace('@MODEL@', function(nfp, 'static void model_fill(')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('clang') or shutil.which('gcc'))
    args = parser.parse_args()
    if not args.cc:
        parser.error('pass --cc with a C compiler')
    nfp = (ROOT / 'runtime/ov_nfp.c').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory() as temp:
        src = Path(temp) / 'fixture.c'
        src.write_text(source(nfp), encoding='utf-8')
        for opt in ('-O0', '-O2'):
            exe = Path(temp) / ('fixture' + opt + ('.exe' if os.name == 'nt' else ''))
            compiled = subprocess.run([args.cc, '-std=gnu11', opt, '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)],
                                      capture_output=True, text=True, timeout=60)
            if compiled.returncode:
                print(compiled.stdout + compiled.stderr)
                return compiled.returncode
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
            print(opt, run.stdout + run.stderr, end='')
            if run.returncode:
                return run.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
