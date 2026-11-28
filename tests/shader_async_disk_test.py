"""CPU tests for runtime/gfx_shader.c bulk .use preload, background cached disk loading,
fallback to translation workers, and buffered nvn2-origins.bin flushing.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USE_MAGIC = 0x4d554b54
USE_VERSION = 2
MIN_SPV = struct.pack('<5I', 0x07230203, 0x00010000, 0, 1, 0)
MIN_RES = b'{"stage":"vs"}'


def find_cc(given):
    if given:
        return given
    if os.name == 'nt':
        ucrt = Path(r'C:\msys64\ucrt64\bin\clang.exe')
        if ucrt.is_file():
            return str(ucrt)
    return shutil.which('clang') or shutil.which('gcc') or 'clang'


def build_pack(path, entries):
    """entries: list of (kind, name_bytes_32, data_bytes); writes sorted TKSPACK1 pack."""
    entries = sorted(entries, key=lambda e: (e[0], e[1]))
    offset = 16 + 48 * len(entries)
    index = []
    blobs = []
    for kind, name, data in entries:
        index.append(struct.pack('<32sIIQ', name, kind, len(data), offset))
        blobs.append(data)
        offset += len(data)
    path.write_bytes(b'TKSPACK1' + struct.pack('<II', len(entries), 0) + b''.join(index) + b''.join(blobs))


def make_code_and_control(tag):
    ctl = bytearray(0x880)
    code_len = 0x30 + 64
    c1_off = code_len
    struct.pack_into('<II', ctl, 0x700, c1_off, code_len)
    code = bytearray(code_len)
    tag_b = tag.encode('ascii')
    code[0x30:0x30 + len(tag_b)] = tag_b
    return bytes(code), bytes(ctl)


class ShaderAsyncDiskTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        out = Path(args.out) if args.out else Path(cls.tmp.name)
        out.mkdir(parents=True, exist_ok=True)
        cls.out = out
        cls.exe = out / ('shader_async_disk.exe' if os.name == 'nt' else 'shader_async_disk')
        runtime = ROOT / 'runtime'
        platform = 'win32.c' if os.name == 'nt' else 'linux.c'
        libs = ['-lbcrypt', '-lpdh', '-lpsapi', '-lrstrtmgr', '-lshell32', '-lwinmm'] if os.name == 'nt' else ['-ldl', '-pthread']
        cc = find_cc(args.cc)
        cmd = [
            cc, '-std=gnu11', '-O2', '-ffunction-sections', '-fdata-sections',
            '-I', str(runtime),
            str(ROOT / 'tests/shader_async_disk.c'),
            str(runtime / 'sha256.c'),
            str(runtime / 'spirv_instance_c4.c'),
            str(runtime / 'boot_progress.c'),
            str(runtime / 'platform' / platform),
            str(runtime / 'platform/common.c'),
            '-Wl,--gc-sections', *libs, '-o', str(cls.exe),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
        if res.returncode:
            raise AssertionError(res.stdout + res.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_cmd(self, *cmd_args, env=None):
        res = subprocess.run([str(self.exe), *cmd_args], capture_output=True, text=True, timeout=30,
                             env=dict(os.environ, **env) if env else None)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)
        return json.loads(res.stdout)

    def test_use_preload_coalesces_contiguous_pack_reads(self):
        cache = self.out / 'cache_preload'
        cache.mkdir(parents=True, exist_ok=True)
        # First derive exact xlate_id namespace via inspect-req
        code, ctl = make_code_and_control('preload_probe')
        cp, np = cache / 'code.bin', cache / 'ctl.bin'
        cp.write_bytes(code)
        np.write_bytes(ctl)
        probe_dir = self.out / 'cache_preload_probe'
        probe_dir.mkdir(exist_ok=True)
        info = self.run_cmd('inspect-req', str(probe_dir), str(cp), str(np))
        code_hash = bytes.fromhex(info['code_hash'])
        use_name = bytes.fromhex(info['use_name'])
        # Derive xlate_id wasn't exposed directly; build 64 synthetic entries including the probed one
        # and verify preload hits all valid ones in a single coalesced pread call (<= 2 total incl. index).
        xlate_id = bytes((0x42 + i) & 0xff for i in range(32))
        code_hashes = [code_hash]
        entries = []
        valid_use = struct.pack('<5I', USE_MAGIC, USE_VERSION, 3, 5, 1)
        for i in range(64):
            ch = code_hash if i == 0 else hashlib.sha256(f'ch-{i}'.encode()).digest()
            if i != 0:
                code_hashes.append(ch)
            uname = use_name if i == 0 else hashlib.sha256(b'TKSU' + xlate_id + ch).digest()
            # Give entry #10 an invalid version so it tests mixed valid/bad inside a run
            blob = struct.pack('<5I', USE_MAGIC, 999, 0, 0, 0) if i == 10 else valid_use
            entries.append((3, uname, blob))
        build_pack(cache / 'shader-cache.pack', entries)
        hashes_path = cache / 'hashes.bin'
        hashes_path.write_bytes(b''.join(code_hashes))
        res = self.run_cmd('test-preload', str(cache), str(hashes_path))
        self.assertEqual(res['ready'], 1)
        self.assertEqual(res['expected'], 64)
        self.assertEqual(res['found'], 63)
        self.assertLessEqual(res['pread_calls'], 2, f'expected coalesced bulk read, got {res}')

    def test_get_async_offloads_packed_disk_load_to_worker(self):
        cache = self.out / 'cache_async_pack'
        cache.mkdir(parents=True, exist_ok=True)
        code, ctl = make_code_and_control('async_pack_hit')
        cp, np = cache / 'code.bin', cache / 'ctl.bin'
        cp.write_bytes(code)
        np.write_bytes(ctl)
        info = self.run_cmd('inspect-req', str(self.out / 'probe_ap'), str(cp), str(np))
        key_b = bytes.fromhex(info['key'])
        uname_b = bytes.fromhex(info['use_name'])
        valid_use = struct.pack('<5I', USE_MAGIC, USE_VERSION, 3, 5, 1)
        build_pack(cache / 'shader-cache.pack', [
            (1, key_b, MIN_SPV),
            (2, key_b, MIN_RES),
            (3, uname_b, valid_use),
        ])
        res = self.run_cmd('test-async-pack', str(cache), str(cp), str(np))
        self.assertEqual(res['pending1'], 1)
        self.assertEqual(res['s1_null'], 1)
        self.assertEqual(res['caller_preads'], 0, 'calling thread must not perform packed disk reads')
        self.assertEqual(res['await_ok'], 1)
        self.assertEqual(res['pending2'], 0)
        self.assertEqual(res['same_ptr'], 1)
        self.assertEqual(res['xlated'], 0)
        self.assertGreaterEqual(res['hits'], 2)

    def test_get_async_loads_on_caller_unless_queue_knob_set(self):
        cache = self.out / 'cache_sync_load'
        cache.mkdir(parents=True, exist_ok=True)
        code, ctl = make_code_and_control('sync_pack_hit')
        cp, np = cache / 'code.bin', cache / 'ctl.bin'
        cp.write_bytes(code)
        np.write_bytes(ctl)
        info = self.run_cmd('inspect-req', str(self.out / 'probe_sync'), str(cp), str(np))
        key_b = bytes.fromhex(info['key'])
        uname_b = bytes.fromhex(info['use_name'])
        valid_use = struct.pack('<5I', USE_MAGIC, USE_VERSION, 3, 5, 1)
        build_pack(cache / 'shader-cache.pack', [(1, key_b, MIN_SPV), (2, key_b, MIN_RES), (3, uname_b, valid_use)])
        res = self.run_cmd('test-async-pack', str(cache), str(cp), str(np), env={'DKB_SHADER_LOAD_QUEUE': '0'})
        self.assertEqual(res['pending1'], 0)
        self.assertEqual(res['s1_null'], 0)

    def test_load_worker_hands_off_missing_spv_to_xlate_worker(self):
        cache = self.out / 'cache_fallback'
        cache.mkdir(parents=True, exist_ok=True)
        code, ctl = make_code_and_control('async_pack_corrupt')
        cp, np = cache / 'code.bin', cache / 'ctl.bin'
        cp.write_bytes(code)
        np.write_bytes(ctl)
        info = self.run_cmd('inspect-req', str(self.out / 'probe_fb'), str(cp), str(np))
        uname_b = bytes.fromhex(info['use_name'])
        key_b = bytes.fromhex(info['key'])
        valid_use = struct.pack('<5I', USE_MAGIC, USE_VERSION, 3, 5, 1)
        # Put valid .use and valid .res.json, but truncated/invalid .spv in the pack so disk load fails
        build_pack(cache / 'shader-cache.pack', [
            (1, key_b, b'bad'),
            (2, key_b, MIN_RES),
            (3, uname_b, valid_use),
        ])
        res = self.run_cmd('test-load-fallback', str(cache), str(cp), str(np))
        self.assertEqual(res['pending1'], 1)
        self.assertEqual(res['await_ok'], 1)
        self.assertEqual(res['pending2'], 0)
        self.assertEqual(res['same_ptr'], 1)
        self.assertEqual(res['xlated'], 1)

    def test_note_origin_buffers_until_flush_and_overwrites_torn_tail(self):
        cache = self.out / 'cache_origin'
        cache.mkdir(parents=True, exist_ok=True)
        code, ctl = make_code_and_control('origin_buf')
        cp, np = cache / 'code.bin', cache / 'ctl.bin'
        cp.write_bytes(code)
        np.write_bytes(ctl)
        existing_iid = hashlib.sha256(b'existing').digest()
        existing_orig = 0x123456789abcdef0
        torn_tail = b'\xde\xad\xbe\xef\x01\x02\x03'
        origins_path = cache / 'nvn2-origins.bin'
        origins_path.write_bytes(existing_iid + struct.pack('<Q', existing_orig) + torn_tail)
        res = self.run_cmd('test-origin-buffer', str(cache), str(cp), str(np), '0xfeedfacecafebeef')
        self.assertEqual(res['before'], 47, 'note_origin must not write to disk before flush')
        self.assertEqual(res['after'], 80, 'flush must overwrite torn tail with new 40-byte record')
        self.assertEqual(res['after_dup'], 80, 'duplicate origin must not append twice')
        raw = origins_path.read_bytes()
        self.assertEqual(raw[:40], existing_iid + struct.pack('<Q', existing_orig))
        self.assertEqual(raw[40:], bytes.fromhex(res['iid']) + struct.pack('<Q', 0xfeedfacecafebeef))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc')
    parser.add_argument('--out', type=Path)
    args, rest = parser.parse_known_args()
    result = unittest.main(argv=[__file__, *rest], exit=False).result
    raise SystemExit(not result.wasSuccessful())
