"""Where the slow frames spend their CPU: the TOTK_PROFILE_SLOW dumps of a perf_run profiled bundle, against the whole run.

    python tools/perf_slow_profile.py build/agent-perf/<bundle>/prof/profile.txt <link map of the same exe> [--slow-ms 100]

Inputs: profile.txt (the whole-run profile: base, module ranges, rip counts) and profile.txt.f<frame> dumps, one per
slow present (TOTK_PROFILE_SLOW=<ms>): `frame N interval_ms X` and `tstack <tid> <ms into the window> <rip>...` lines.
Each sample's leaf Rip (the first address of the chain) is mapped to the nearest map symbol in the exe (the map is the
link map of the same exe). Outside-exe leaves are grouped by module.

Output: for the dumps with interval >= --slow-ms, the leaf functions and the inclusive functions (any frame of the chain),
each with its share of the slow samples and its share of the whole-run samples (from the rip counts of profile.txt, which
keep only the top Rips). A large gap between the two shares marks what only the slow frames do.
Stdlib only, plus tools/prof_report.py for the map and the module lookup.
"""
import argparse
import bisect
import collections
import glob
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prof_report  # noqa: E402  (load_map, load_profile: the same parsers as the whole-run report)


def dump_frames(profile_path):
    """frame -> (interval_ms, render thread id, [(tid, t_ms, [rip...])])."""
    out = {}
    for p in sorted(glob.glob(str(profile_path) + '.f*')):
        m = re.search(r'\.f(\d+)$', p)
        if not m:
            continue
        frame, interval, render, samples = int(m[1]), None, None, []
        for ln in Path(p).read_text(encoding='utf-8', errors='replace').splitlines():
            if ln.startswith('frame '):
                mm = re.match(r'frame (\d+) interval_ms ([\d.]+)', ln)
                if mm:
                    frame, interval = int(mm[1]), float(mm[2])
            elif ln.startswith('main '):
                mm = re.search(r'render (\d+)', ln)
                render = int(mm[1]) if mm else None
            elif ln.startswith('tstack '):
                parts = ln.split()
                if len(parts) >= 4:
                    samples.append((int(parts[1]), float(parts[2]), [int(x, 16) for x in parts[3:]]))
        out[frame] = (interval, render, samples)
    return out


# Leaves in these modules are waits, not work: the sampled thread is blocked in the kernel.
BLOCKED_MODULES = ('ntdll.dll', 'win32u.dll', 'KERNELBASE.dll', 'KERNEL32.DLL')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('map')
    ap.add_argument('--slow-ms', type=float, default=100.0)
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    prof = prof_report.load_profile(a.profile)
    addrs, names = prof_report.load_map(a.map)
    base = prof['base']
    exe = next((m for m in prof['modules'] if m[0] == base), None)
    exe_end = base + exe[1] if exe else base

    def name_of(rip):
        if base <= rip < exe_end:
            i = bisect.bisect_right(addrs, rip - base) - 1
            return names[i] if i >= 0 else '?'
        m = next((m for m in prof['modules'] if m[0] <= rip < m[0] + m[1]), None)
        return f'{Path(m[2]).name}!outside' if m else 'unknown'

    dumps = dump_frames(a.profile)
    slow = {f: v for f, v in dumps.items() if v[0] is not None and v[0] >= a.slow_ms}
    print(f'profile {a.profile}: base {base:#x}, exe ' + (f'{exe[1]:#x} bytes' if exe else 'module not in the profile') + f', {len(dumps)} dumps, {len(slow)} with interval >= {a.slow_ms} ms')
    if not slow:
        print('no slow dumps: nothing to attribute')
        return
    leaf, incl, n_leaf, n_incl = collections.Counter(), collections.Counter(), 0, 0
    n_all = n_blocked = n_other_thread = 0
    frames_detail = []
    for f in sorted(slow):
        interval, render, samples = slow[f]
        frames_detail.append((f, interval, len(samples)))
        for tid, _t, chain in samples:
            if not chain:
                continue
            n_all += 1
            if render is not None and tid != render:
                n_other_thread += 1
                continue
            lm = name_of(chain[0])
            if any(lm.startswith(b.lower()) or lm.startswith(b) for b in BLOCKED_MODULES):
                n_blocked += 1
                continue
            n_leaf += 1
            leaf[lm] += 1
            seen = set()
            for rip in chain:
                nm = name_of(rip)
                if nm not in seen:
                    seen.add(nm)
                    incl[nm] += 1
            n_incl += 1
    print(f'render thread samples in the slow dumps: {n_leaf} running, {n_blocked} blocked in the kernel (waits); '
          f'{n_other_thread} from other threads not counted; {n_all} samples in all')
    whole = collections.Counter()
    total = sum(n for _, n in prof['rips']) or 1
    for rip, n in prof['rips']:
        whole[name_of(rip)] += n

    print('\nslow dumps (frame, interval ms, samples):')
    for f, iv, n in frames_detail[:40]:
        print(f'  f{f}  {iv:8.1f} ms  {n} samples')
    if len(frames_detail) > 40:
        print(f'  ... {len(frames_detail) - 40} more')
    print(f'\nleaf functions in the slow dumps ({n_leaf} samples); whole-run share from the top rips of profile.txt:')
    for nm, n in leaf.most_common(a.top):
        print(f'  {100 * n / n_leaf:6.2f}%  whole-run {100 * whole.get(nm, 0) / total:6.2f}%  {nm}')
    print(f'\ninclusive (function anywhere in the chain) in the slow dumps ({n_incl} samples):')
    for nm, n in incl.most_common(a.top):
        print(f'  {100 * n / n_incl:6.2f}%  {nm}')


if __name__ == '__main__':
    sys.exit(main())
