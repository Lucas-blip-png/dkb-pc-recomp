"""The vtable slots through which runtime/vk_dlss.c and runtime/vk_dlssg.cpp call NGX's parameter object, without NGX
or a GPU.

NVSDK_NGX_Parameter is a C++ class built by MSVC with virtual methods only, and this runtime has no NVIDIA header, so
each helper casts slot N of the object's vtable to the overload it means. MSVC lays an overload set out in reverse
declaration order, which puts Set(name, T) at slots 0 to 7 for void *, ID3D12Resource *, ID3D11Resource *, int,
unsigned, double, float and unsigned long long, and Get(name, T *) eight slots later. A wrong number compiles and calls
another overload at run time (an int read as a float, a pointer stored as an integer), so this reads every
`slot(p, N)` cast in both files and checks N against the type the cast passes.

It also checks each file's Shutdown1 type: the DLL's export takes an out pointer after the device, which NVIDIA's header
leaves out, and stores through it unconditionally; declared with the device alone, the call writes through whatever
the second argument register holds (a crash in NVSDK_NGX_D3D12_Shutdown1 writing address 0).

Usage: python tests/ngx_vtable_test.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILES = [ROOT / 'runtime' / 'vk_dlss.c', ROOT / 'runtime' / 'vk_dlssg.cpp']

SET_SLOT = {'void *': 0, 'ID3D12Resource *': 1, 'ID3D11Resource *': 2, 'int': 3, 'unsigned': 4, 'double': 5,
            'float': 6, 'unsigned long long': 7}
# ((R (*)(NgxParam *, const char *, T))slot(p, N)): the cast's last parameter type and the slot
SHUTDOWN = re.compile(r'typedef NgxResult \(\*PfnShutdown\)\(([^)]*)\);')
CAST = re.compile(r'\(\(\w+ \(\*\)\(NgxParam \*, const char \*, ([^)]+?)\)\)slot\(p, (\d+)\)\)')


def expected(t):
    """The slot MSVC gives the overload whose value parameter is t: a Set's type, or a Get's (that type and one more
    '*', eight slots later). Set(void *) and Set(ID3D12Resource *) are matched before their Gets' shape."""
    t = re.sub(r'\s*\*', ' *', ' '.join(t.split())).replace('* *', '**')
    if t in SET_SLOT:
        return SET_SLOT[t]
    if t.endswith('*'):
        inner = t[:-1].rstrip()
        if inner in SET_SLOT:
            return SET_SLOT[inner] + 8
    return None


def main():
    bad, seen = [], 0
    for f in FILES:
        text = f.read_text(encoding='utf-8')
        for m in CAST.finditer(text):
            seen += 1
            t, n = m.group(1), int(m.group(2))
            want = expected(t)
            if want != n:
                line = text.count('\n', 0, m.start()) + 1
                bad.append(f'{f.name}:{line}: slot {n} for {t!r}, MSVC layout gives {want}')
        shutdowns = SHUTDOWN.findall(text)
        if len(shutdowns) != 1 or len(shutdowns[0].split(',')) != 2 or not shutdowns[0].split(',')[1].strip().startswith('unsigned *'):
            bad.append(f'{f.name}: PfnShutdown is {shutdowns}, not (device, unsigned *left)')
    if seen < 6:
        bad.append(f'only {seen} slot casts found: the pattern no longer matches the helpers')
    for b in bad:
        print(b)
    print(f'{seen} slot casts checked, {len(bad)} wrong')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
