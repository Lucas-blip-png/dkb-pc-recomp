"""Summarise an evidence bundle written by tools/perf_run.ps1: frame times, compile stalls, GPU and process samples,
and one verdict per performance acceptance threshold (numbered 10-14).

    python tools/perf_evidence.py build/agent-perf/<stamp>-<visit> [--warm-after 60]

Writes evidence.txt and evidence.json into the bundle and prints evidence.txt. Standard library only.

Frame times: measure=flight runs read flight/*/frames.tsv, one row per present (interval_ms, and the render thread's
blocked time per kind). Dumps overlap at their edges; a frame number seen twice keeps its first row. Frames after the
last dump are not in any dump (no dump fires at exit), so the coverage line compares the row count with the game's
own frame count from logs/dkb.log (logs/totk.log for a TotK build).

measure=summary runs have no flight rows; their frame statistics come from the game's frametime lines in the log
(always on, one per 60 s and one at exit: mean, 1% low, counts over 25/50/100 ms, the five worst frames). Those are
per-period aggregates, so the stall and low-fps checks are coarser there.

"warm" means frames whose time since the first present is at least --warm-after seconds. It is an assumption: the
run does not detect when gameplay starts, so warm is a time window, not a scene.
"""
import argparse
import csv
import json
import math
import re
import statistics
import sys
from pathlib import Path

COMPILE_COLS = ('pipe_ms', 'xlate_ms')          # render-thread waits: pipeline builds and shader translation
COMPILE_STALL_MS = 1000.0                       # gate 12: a compile freeze of a second or more
STALL_MS = 50.0                                 # gate 11: no stall over 50 ms in warmed gameplay
LOW1_MIN_FPS = 55.0                             # gate 11: 1% low at least 55 fps
SUSTAINED_MIN_FPS = 59.5                        # gate 11: "sustained 60": the cap is 60, so allow a sliver of sampling error

FT_LINE = re.compile(r'frametime: (?P<tag>.+?): (?P<n>\d+) frames in (?P<s>[\d.]+) s, mean (?P<mean>[\d.]+) fps, '
                     r'1% low (?P<low>[\d.]+) fps, over 25 ms (?P<o25>\d+), 50 ms (?P<o50>\d+), 100 ms (?P<o100>\d+)')
WORST = re.compile(r'frametime:   worst frame (?P<f>\d+): (?P<ms>[\d.]+) ms; shader translation (?P<xl>[\d.]+), '
                   r'pipeline build (?P<pb>[\d.]+), texture upload (?P<tx>[\d.]+)')
ERR = re.compile(r'validation error|unimplemented|fatal|device lost|shader failure|VK_ERROR|unhandled exception', re.I)
SIZE = re.compile(r'(\d{3,5})\s*[x×]\s*(\d{3,5})')


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


SAMPLE_HEAD = ['elapsed_s', 'ac_line', 'gpu_temp_c', 'gpu_power_w', 'gpu_sm_mhz', 'gpu_sm_max_mhz', 'gpu_mem_mhz',
               'gpu_util_pct', 'gpu_mem_used_mib', 'gpu_throttle_mask']
# Process columns: working set, commit (private), peak, handles, threads. Older bundles were written with N1 formatting,
# which put thousands separators ("1,911.0") into these fields; the regex takes both forms.
SAMPLE_TAIL = re.compile(r'(\d+(?:,\d{3})*\.\d+|n/a),(\d+(?:,\d{3})*\.\d+|n/a),(\d+(?:,\d{3})*\.\d+|n/a),'
                         r'(\d+|n/a),(\d+|n/a)$')


def read_samples(path):
    """Rows of samples.csv as dicts. Works on both the old (N1, comma-split) and the new (F1) process columns."""
    if not path.exists():
        return []
    rows = []
    for ln in path.read_text(encoding='ascii', errors='replace').splitlines()[1:]:
        head = ln.split(',', 10)
        if len(head) < 10:
            continue
        m = SAMPLE_TAIL.search(ln)
        if not m:
            continue
        row = dict(zip(SAMPLE_HEAD, head[:10]))
        row.update({'proc_working_set_mib': m[1].replace(',', ''), 'proc_commit_mib': m[2].replace(',', ''),
                    'proc_peak_ws_mib': m[3].replace(',', ''), 'proc_handles': m[4], 'proc_threads': m[5]})
        rows.append(row)
    return rows


def read_flight(bundle):
    """frame -> row, the names of the dumps read, and the rows skipped as malformed (should be 0).
    A row keeps the first dump that holds its frame."""
    frames, dumps, skipped = {}, [], 0
    for p in sorted((bundle / 'flight').glob('*/frames.tsv')):
        rows = [ln.split('\t') for ln in p.read_text(encoding='utf-8', errors='replace').splitlines()
                if ln and not ln.startswith('#')]
        if len(rows) < 2:
            continue
        cols = rows[0]
        dumps.append(p.parent.name)
        for r in rows[1:]:
            try:
                if len(r) != len(cols):
                    raise ValueError
                d = dict(zip(cols, r))
                rec = {c: float(d[c]) for c in cols if c not in ('frame', 'thumb')}
                f = int(d['frame'])
            except (KeyError, ValueError):
                skipped += 1
                continue
            frames.setdefault(f, rec)
    return dict(sorted(frames.items())), dumps, skipped


def parse_log(path):
    out = {'periods': [], 'errors': [], 'error_examples': [], 'size_line': None, 'translator_line': None, 'log_lines': 0}
    if not path.exists():
        return out
    period = None
    for ln in path.read_text(encoding='utf-8', errors='replace').splitlines():
        out['log_lines'] += 1
        if m := FT_LINE.search(ln):
            period = {k: m[k] for k in ('tag',)}
            period.update({'n': int(m['n']), 's': float(m['s']), 'mean': float(m['mean']), 'low': float(m['low']),
                           'o25': int(m['o25']), 'o50': int(m['o50']), 'o100': int(m['o100']), 'worst': []})
            out['periods'].append(period)
        elif (m := WORST.search(ln)) and period is not None:
            period['worst'].append({'f': int(m['f']), 'ms': float(m['ms']), 'xlate_ms': float(m['xl']),
                                    'pipe_ms': float(m['pb']), 'tex_ms': float(m['tx'])})
        elif ERR.search(ln):
            out['errors'].append(ln)
        if out['size_line'] is None and re.search(r'window|client|surface', ln, re.I) and (m := SIZE.search(ln)):
            out['size_line'] = (ln.strip()[:200], f'{m[1]}x{m[2]}')
        if out['translator_line'] is None and 'translator' in ln:
            out['translator_line'] = ln.strip()[:200]
    out['error_examples'] = out['errors'][:3]
    out['error_count'] = len(out['errors'])
    del out['errors']
    return out


def fps_low(ms_desc, frac):
    k = max(1, math.ceil(len(ms_desc) * frac))
    return 1000.0 * k / sum(ms_desc[:k])


def frame_stats(recs):
    """recs: [(frame, row)] with interval_ms > 0. Returns None when empty."""
    if not recs:
        return None
    ms = [r['interval_ms'] for _, r in recs]
    asc = sorted(ms)
    desc = asc[::-1]

    def pct(q):
        return asc[min(len(asc) - 1, int(q * len(asc)))]
    worst_f, worst_r = max(recs, key=lambda fr: fr[1]['interval_ms'])
    total_s = sum(ms) / 1000.0
    return {
        'frames': len(ms), 'span_s': round(total_s, 2), 'mean_fps': round(len(ms) / total_s, 2),
        'mean_ms': round(statistics.fmean(ms), 2), 'p50_ms': round(pct(0.50), 2), 'p95_ms': round(pct(0.95), 2),
        'p99_ms': round(pct(0.99), 2), 'p999_ms': round(pct(0.999), 2),
        'low1_fps': round(fps_low(desc, 0.01), 2), 'low0_1_fps': round(fps_low(desc, 0.001), 2),
        'low1_by_p99_fps': round(1000.0 / pct(0.99), 2),
        'over25': sum(m > 25 for m in ms), 'over50': sum(m > 50 for m in ms), 'over100': sum(m > 100 for m in ms),
        'longest_ms': round(worst_r['interval_ms'], 1), 'longest_frame': worst_f, 'longest_t_s': round(worst_r['t_ms'] / 1000.0, 2),
    }


BLOCK_COLS = ('pace_ms', 'fence_ms', 'drain_ms', 'pipe_ms', 'xlate_ms', 'tex_ms', 'astc_ms')


def anatomy(recs):
    """Where the slowest 1% of frames spend their time: mean interval and each flight wait (ms), and the part of the
    interval that none of the waits cover (game and replay CPU time on the render thread, plus GPU-bound submits)."""
    if not recs:
        return None
    n = max(1, math.ceil(len(recs) * 0.01))
    slow = sorted(recs, key=lambda fr: -fr[1]['interval_ms'])[:n]
    mean = {c: statistics.fmean(r.get(c, 0.0) for _, r in slow) for c in BLOCK_COLS}
    iv = statistics.fmean(r['interval_ms'] for _, r in slow)
    return {'frames': n, 'interval_ms': round(iv, 1), 'waits_ms': {c: round(v, 1) for c, v in mean.items()},
            'unattributed_ms': round(iv - sum(mean.values()), 1)}


def frame_anatomy(frames, f):
    r = frames.get(f)
    if r is None:
        return None
    return {'frame': f, 't_s': round(r['t_ms'] / 1000.0, 2), 'interval_ms': round(r['interval_ms'], 1),
            'waits_ms': {c: round(r.get(c, 0.0), 1) for c in BLOCK_COLS},
            'unattributed_ms': round(r['interval_ms'] - sum(r.get(c, 0.0) for c in BLOCK_COLS), 1)}


def compile_summary(frames):
    ev, unexplained = [], []
    pipe = xl = tex = 0.0
    frames_with_compile = 0
    for f, r in frames.items():
        cw = sum(r.get(c, 0.0) for c in COMPILE_COLS)
        pipe += r.get('pipe_ms', 0.0)
        xl += r.get('xlate_ms', 0.0)
        tex += r.get('tex_ms', 0.0)
        frames_with_compile += cw > 0
        if cw >= COMPILE_STALL_MS:
            ev.append({'frame': f, 't_s': round(r['t_ms'] / 1000.0, 2), 'interval_ms': round(r['interval_ms'], 1),
                       'pipe_ms': round(r.get('pipe_ms', 0.0), 1), 'xlate_ms': round(r.get('xlate_ms', 0.0), 1)})
        elif r['interval_ms'] >= COMPILE_STALL_MS:
            unexplained.append({'frame': f, 't_s': round(r['t_ms'] / 1000.0, 2), 'interval_ms': round(r['interval_ms'], 1)})
    return {
        'pipe_wait_total_ms': round(pipe, 1), 'xlate_wait_total_ms': round(xl, 1), 'tex_wait_total_ms': round(tex, 1),
        'frames_with_compile_wait': frames_with_compile,
        'shaders_translated': int(sum(r.get('shaders_xlated', 0.0) for r in frames.values())),
        'events_1s': ev, 'long_frames_without_compile_wait_1s': unexplained,
    }


def gpu_summary(rows):
    def col(name):
        return [v for v in (num(r.get(name)) for r in rows) if v is not None]
    if not rows:
        return {'samples': 0}
    masks = sorted({r.get('gpu_throttle_mask', '') for r in rows} - {'', 'n/a', '0x0000000000000000'})
    temp, pw, sm, util, mem = col('gpu_temp_c'), col('gpu_power_w'), col('gpu_sm_mhz'), col('gpu_util_pct'), col('gpu_mem_used_mib')
    out = {'samples': len(rows), 'throttle_masks_seen': masks}
    if temp:
        out['temp_c_max'] = max(temp)
    if pw:
        out['power_w_avg'] = round(statistics.fmean(pw), 1)
        out['power_w_max'] = max(pw)
    if sm:
        out['sm_mhz_avg'] = round(statistics.fmean(sm), 0)
        out['sm_mhz_min'] = min(sm)
    if util:
        out['util_pct_avg'] = round(statistics.fmean(util), 1)
    if mem:
        out['mem_used_mib_max'] = max(mem)
    return out


def proc_summary(rows):
    def col(name):
        return [v for v in (num(r.get(name)) for r in rows) if v is not None]
    ws, commit, handles, threads = col('proc_working_set_mib'), col('proc_commit_mib'), col('proc_handles'), col('proc_threads')
    if not ws:
        return {'samples': 0}
    return {'samples': len(rows), 'working_set_mib_max': max(ws), 'commit_mib_first': commit[0] if commit else None,
            'commit_mib_last': commit[-1] if commit else None, 'commit_mib_max': max(commit) if commit else None,
            'handles_max': max(handles) if handles else None, 'threads_max': max(threads) if threads else None}


def verdict(gate, status, detail):
    return {'gate': gate, 'verdict': status, 'detail': detail}


DIAGNOSTIC_SWITCHES = ('FLIGHT', 'TRACE', 'PROFILE', 'PROFILE_SLOW', 'PROFILE_RENDER', 'PROFILE_CHAINS',
                       'VK_VALIDATION')
OFF_VALUES = ('', '0', 'off', 'false', 'no')


def ini_diagnostics(text):
    """The diagnostic switches a copied dkb.ini or totk.ini would turn on, as runtime/cfg.c reads it: "key = value"
    lines, a line starting with '#' or ';' is a comment and '[...]' only groups, the key is the variable name without
    TOTK_ or DKB_ (case-insensitive), the value is the rest of the line, trimmed, optionally in double quotes."""
    found = []
    for line in (text or '').splitlines():
        line = line.strip()
        if not line or line[0] in '#;[' or '=' not in line:
            continue
        key, value = (part.strip() for part in line.split('=', 1))
        key = key.upper()
        for prefix in ('TOTK_', 'DKB_'):
            if key.startswith(prefix):
                key = key[len(prefix):]
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        if key in DIAGNOSTIC_SWITCHES and value.strip().lower() not in OFF_VALUES:
            found.append(key)
    return found


def build(bundle, warm_after):
    run = json.loads((bundle / 'run.json').read_text(encoding='utf-8-sig'))
    settings = run.get('settings', {})
    measure = run.get('measure', 'flight')
    frames, dumps, skipped = read_flight(bundle) if measure == 'flight' else ({}, [], 0)
    # The game writes its log lines to stderr (runtime/main.c log_open puts the log beside the stderr file only when a
    # log file is opened); the stderr file is the complete record, so it is the fallback.
    log_path = next((p for p in (bundle / 'bin' / 'logs' / 'dkb.log', bundle / 'bin' / 'logs' / 'totk.log',
                                  bundle / 'logs' / 'dkb.log', bundle / 'logs' / 'totk.log', bundle / 'stderr.txt')
                     if p.exists()), bundle / 'stderr.txt')
    log = parse_log(log_path)
    res_log_source = str(log_path.relative_to(bundle))
    samples = read_samples(bundle / 'samples.csv')

    recs = [(f, r) for f, r in frames.items() if r['interval_ms'] > 0]
    warm_t = warm_after * 1000.0
    warm = [(f, r) for f, r in recs if r['t_ms'] >= warm_t]
    whole_s, warm_s = frame_stats(recs), frame_stats(warm)
    periods = log['periods']
    game_frames = sum(p['n'] for p in periods)
    worst_all = sorted((w for p in periods for w in p['worst']), key=lambda w: -w['ms'])

    res = {'bundle': str(bundle), 'visit': run.get('visit'), 'measure': measure, 'status': run.get('status'),
           'harness_note': run.get('harness_note'), 'stop': run.get('stop'), 'game_exit_code': run.get('game_exit_code'),
           'exe_sha256': run.get('exe', {}).get('sha256'), 'machine': run.get('machine'),
           'settings': settings, 'warm_after_s': warm_after, 'flight_dumps': dumps, 'flight_rows_skipped': skipped,
           'frames_flight': whole_s, 'frames_flight_warm': warm_s,
           'frametime_periods': [{k: v for k, v in p.items() if k != 'worst'} for p in periods],
           'frames_in_game_log': game_frames,
           'coverage_flight_rows_over_game_frames': (round(len(recs) / game_frames, 4) if game_frames and recs else None),
           'compile': compile_summary(dict(recs)) if recs else None,
           'worst_frames_log': worst_all[:10], 'gpu': gpu_summary(samples), 'process': proc_summary(samples),
           'log': {k: v for k, v in log.items() if k != 'periods'}, 'log_source': res_log_source,
           'anatomy_slowest_1pct_warmed': anatomy(warm), 'anatomy_slowest_1pct_whole': anatomy(recs),
           'anatomy_longest_frame': frame_anatomy(frames, whole_s['longest_frame']) if whole_s else None}

    # ----- verdicts
    v = []
    if run.get('status') != 'finished':
        v.append(verdict('run', 'NOT-MEASURED', f"run status is {run.get('status')!r} (stop: {run.get('stop')}); thresholds below use what exists"))
    v.append(verdict('10 first- and repeat-visit coverage checked separately', 'NOT-MEASURED',
                     f"visit={run.get('visit')}: this bundle is one visit; coverage of content is not computed by this tool"))
    want = settings.get('TOTK_WINDOW_SIZE', '')
    if log['size_line']:
        got = log['size_line'][1]
        v.append(verdict('11 fixed 1080p', 'PASS' if got == want == '1920x1080' else 'FAIL',
                         f"settings request {want}; log line reports {got}: {log['size_line'][0]!r}"))
    else:
        v.append(verdict('11 fixed 1080p', 'NOT-MEASURED', f"settings request {want}; the log has no window size line"))

    if measure == 'flight' and warm_s:
        v.append(verdict('11 sustained 60 fps (warmed)', 'PASS' if warm_s['mean_fps'] >= SUSTAINED_MIN_FPS else 'FAIL',
                         f"warmed mean {warm_s['mean_fps']} fps over {warm_s['frames']} frames ({warm_s['span_s']} s); threshold {SUSTAINED_MIN_FPS}"))
        v.append(verdict('11 1% low at least 55 fps (warmed)', 'PASS' if warm_s['low1_fps'] >= LOW1_MIN_FPS else 'FAIL',
                         f"warmed 1% low (mean of slowest 1%) {warm_s['low1_fps']} fps; by p99 {warm_s['low1_by_p99_fps']} fps; threshold {LOW1_MIN_FPS}"))
        v.append(verdict('11 no stall over 50 ms in warmed gameplay', 'PASS' if warm_s['over50'] == 0 else 'FAIL',
                         f"{warm_s['over50']} warmed frames over 50 ms; warmed longest {warm_s['longest_ms']} ms at frame "
                         f"{warm_s['longest_frame']} (t {warm_s['longest_t_s']} s); whole-run longest {whole_s['longest_ms']} ms"))
    elif measure == 'summary' and periods:
        # Each period is 60 s of play from the first present, so a period starts at the sum of the earlier ones.
        starts, t = [], 0.0
        for p in periods:
            starts.append(t)
            t += p['s']
        warm_periods = [p for p, s0 in zip(periods, starts) if s0 >= warm_after]
        if warm_periods:
            n = sum(p['n'] for p in warm_periods)
            s = sum(p['s'] for p in warm_periods)
            mean = n / s if s else 0
            low = min(p['low'] for p in warm_periods)
            o50 = sum(p['o50'] for p in warm_periods)
            v.append(verdict('11 sustained 60 fps (warmed)', 'PASS' if mean >= SUSTAINED_MIN_FPS else 'FAIL',
                             f"summary mode, warmed periods: mean {mean:.2f} fps over {n} frames; threshold {SUSTAINED_MIN_FPS}"))
            v.append(verdict('11 1% low at least 55 fps (warmed)', 'PASS' if low >= LOW1_MIN_FPS else 'FAIL',
                             f"summary mode: lowest warmed period 1% low {low} fps (per-period, bin-based); threshold {LOW1_MIN_FPS}"))
            v.append(verdict('11 no stall over 50 ms in warmed gameplay', 'PASS' if o50 == 0 else 'FAIL',
                             f"summary mode: {o50} frames over 50 ms in warmed periods (counts only; worst-five lists hold the rest)"))
        else:
            v.append(verdict('11 warmed thresholds', 'NOT-MEASURED', 'summary mode: no period starts after the warm-up'))
    else:
        for g in ('11 sustained 60 fps (warmed)', '11 1% low at least 55 fps (warmed)', '11 no stall over 50 ms in warmed gameplay'):
            v.append(verdict(g, 'NOT-MEASURED', 'no frame records in the bundle' + (' (summary mode gives no frames)' if measure == 'summary' else '')))
    v.append(verdict('11 loading reported separately', 'NOT-MEASURED', 'loading screens are not detected by this tool'))

    if res['compile'] is not None or measure == 'summary':
        if res['compile'] is not None:
            ev = res['compile']['events_1s']
            v.append(verdict('12 no multi-second compile freeze entering content', 'PASS' if not ev else 'FAIL',
                             f"{len(ev)} frames with pipeline+translation waits of 1 s or more; longest waits in worst_frames_log; "
                             f"run covers first presents to exit, the tool does not know when prepared content was entered"))
        else:
            long_w = [w for w in worst_all if w['pipe_ms'] + w['xlate_ms'] >= COMPILE_STALL_MS]
            v.append(verdict('12 no multi-second compile freeze entering content', 'FAIL' if long_w else 'PASS' if worst_all else 'NOT-MEASURED',
                             f"summary mode: {len(long_w)} of the {len(worst_all)} worst frames logged had compile waits of 1 s or more"))
    else:
        v.append(verdict('12 no multi-second compile freeze entering content', 'NOT-MEASURED', 'no frame records'))

    on = [k for k in ('TOTK_FLIGHT', 'TOTK_TRACE', 'TOTK_PROFILE', 'TOTK_PROFILE_SLOW', 'TOTK_PROFILE_RENDER',
                      'TOTK_PROFILE_CHAINS', 'TOTK_VK_VALIDATION') if settings.get(k) not in (None, '', '0')]
    # The run's environment is cleared of TOTK_/DKB_ variables, but the settings file copied next to the exe is still
    # read by the game: a switch it turns on counts unless the run's own settings override it.
    ini = run.get('settings_ini') or {}
    for key in ini_diagnostics(ini.get('text', '') if ini.get('present') else ''):
        if f'TOTK_{key}' not in settings:
            on.append(f'{key} (in the copied settings file)')
    v.append(verdict('14 controlled settings, detailed diagnostic logging off',
                     'FAIL' if on else 'PASS',
                     (f"diagnostics on: {', '.join(on)} (cfg.c files these under 'diagnostics'); an acceptance-style run "
                      f"uses -Measure summary and no profiler" if on else
                      'diagnostics off: TOTK_TRACE, TOTK_PROFILE, validation, telemetry and overlay unset; frametime summaries only')))
    res['verdicts'] = v
    return res


def render(res):
    L = []
    add = L.append
    add(f"EVIDENCE {res['bundle']}")
    add(f"HARNESS TEST, not acceptance evidence. visit={res['visit']} measure={res['measure']} status={res['status']} "
        f"stop={res['stop']} exit={res['game_exit_code']}")
    m = res.get('machine') or {}
    add(f"machine: {m.get('cpu')}; {m.get('ram_gb')} GB; gpu {m.get('gpu')}; power plan {m.get('power_plan')}; "
        f"line at start {m.get('power_line_at_start')}")
    add(f"exe sha256 {res['exe_sha256']}")
    add('settings: ' + ' '.join(f'{k}={v}' for k, v in res['settings'].items() if not k.endswith('DIR') and k != 'TOTK_FLIGHT_AT'))
    add(f"warmed = frames at least {res['warm_after_s']} s after the first present")
    for label, s in (('whole run', res['frames_flight']), (f"warmed (>= {res['warm_after_s']} s)", res['frames_flight_warm'])):
        if not s:
            add(f"FRAMES {label}: none")
            continue
        add(f"FRAMES {label}: {s['frames']} frames, {s['span_s']} s, mean {s['mean_fps']} fps ({s['mean_ms']} ms), "
            f"p50 {s['p50_ms']} p95 {s['p95_ms']} p99 {s['p99_ms']} p99.9 {s['p999_ms']} ms")
        add(f"    1% low {s['low1_fps']} fps (mean of slowest 1%), 0.1% low {s['low0_1_fps']} fps, "
            f"1% by p99 {s['low1_by_p99_fps']} fps")
        add(f"    frames over 25 ms {s['over25']}, over 50 ms {s['over50']}, over 100 ms {s['over100']}; "
            f"longest {s['longest_ms']} ms at frame {s['longest_frame']} (t {s['longest_t_s']} s)")
    if res['frames_flight'] is None:
        add('FRAMES: ' + ('summary mode: no per-frame rows, see the frametime lines below' if res['measure'] == 'summary'
                          else 'no flight rows were read (check flight/ and the TOTK_FLIGHT settings)'))
    add(f"COVERAGE flight rows / game frames: {res['coverage_flight_rows_over_game_frames']} "
        f"({len(res['flight_dumps'])} dumps, {res['flight_rows_skipped']} malformed rows skipped; game frames from its "
        f"frametime lines: {res['frames_in_game_log']})")
    for p in res['frametime_periods']:
        add(f"  frametime {p['tag']}: {p['n']} frames in {p['s']} s, mean {p['mean']} fps, 1% low {p['low']} fps, "
            f"over 25/50/100 ms {p['o25']}/{p['o50']}/{p['o100']}")
    for label, key in (('slowest 1% warmed', 'anatomy_slowest_1pct_warmed'), ('longest frame', 'anatomy_longest_frame')):
        a = res.get(key)
        if a:
            w = a['waits_ms'] if 'waits_ms' in a else None
            head = f"{a.get('frames', 'frame ' + str(a.get('frame')))}" if 'frames' in a else f"frame {a['frame']} at {a['t_s']} s"
            add(f"ANATOMY {label} ({head}): interval {a['interval_ms']} ms = " +
                ', '.join(f"{k[:-3]} {v}" for k, v in w.items()) + f", unattributed {a['unattributed_ms']} ms")
    c = res['compile']
    if c:
        add(f"COMPILE (render thread): pipeline wait {c['pipe_wait_total_ms']} ms, translation wait {c['xlate_wait_total_ms']} ms, "
            f"texture wait {c['tex_wait_total_ms']} ms; {c['frames_with_compile_wait']} frames waited; "
            f"{c['shaders_translated']} shaders translated")
        add(f"    events of 1 s or more: {len(c['events_1s'])}; long frames with no compile wait: {len(c['long_frames_without_compile_wait_1s'])}")
        for e in c['events_1s'][:10]:
            add(f"      frame {e['frame']} at {e['t_s']} s: {e['interval_ms']} ms (pipe {e['pipe_ms']}, xlate {e['xlate_ms']})")
    if res['worst_frames_log']:
        add('WORST FRAMES (game log): ' + '; '.join(f"f{w['f']} {w['ms']} ms (xlate {w['xlate_ms']}, pipe {w['pipe_ms']})"
                                                  for w in res['worst_frames_log'][:5]))
    g = res['gpu']
    add(f"GPU ({g.get('samples', 0)} samples every 5 s): temp max {g.get('temp_c_max')} C, power avg {g.get('power_w_avg')} "
        f"max {g.get('power_w_max')} W, SM clock avg {g.get('sm_mhz_avg')} min {g.get('sm_mhz_min')} MHz, "
        f"util avg {g.get('util_pct_avg')}%, VRAM max {g.get('mem_used_mib_max')} MiB, throttle masks {g.get('throttle_masks_seen')}")
    p = res['process']
    add(f"PROCESS: working set max {p.get('working_set_mib_max')} MiB; commit first {p.get('commit_mib_first')} "
        f"last {p.get('commit_mib_last')} max {p.get('commit_mib_max')} MiB; handles max {p.get('handles_max')}, threads max {p.get('threads_max')}")
    lg = res['log']
    add(f"LOG ({res['log_source']}): {lg.get('log_lines')} lines; error-like lines {lg.get('error_count')}" +
        (f"; first: {lg['error_examples']}" if lg.get('error_examples') else ''))
    add(f"    translator: {lg.get('translator_line')}")
    add(f"    window size line: {lg['size_line'][0] if lg.get('size_line') else None}")
    add('VERDICTS')
    for x in res['verdicts']:
        add(f"  {x['verdict']:<13} {x['gate']}: {x['detail']}")
    return '\n'.join(L) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('bundle', type=Path)
    ap.add_argument('--warm-after', type=float, default=60.0)
    a = ap.parse_args()
    bundle = a.bundle.resolve()
    if not (bundle / 'run.json').exists():
        sys.exit(f'no run.json in {bundle}')
    res = build(bundle, a.warm_after)
    text = render(res)
    (bundle / 'evidence.txt').write_text(text, encoding='utf-8')
    (bundle / 'evidence.json').write_text(json.dumps(res, indent=2, default=str), encoding='utf-8')
    sys.stdout.write(text)


if __name__ == '__main__':
    main()
