"""installer/dlss.py: NVIDIA DLSS's download, against a local HTTP server with stand-in files (no network, no NVIDIA file).

The pinned table (LICENSE, FILES) is replaced by stand-ins. Checked: the license text comes back only with its pinned
hash; fetch() puts the DLL and the license in place and installed() then holds; a DLL whose bytes differ is refused and
leaves no file and no .part behind; the real table is one NVIDIA/DLSS release with a hash on every file, DLSS's DLL and
DLSS Frame Generation's.
"""
import hashlib
import http.server
import shutil
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from installer import dlss  # noqa: E402

BLOBS = {'LICENSE.txt': b'Stand-in license.\n' * 40, 'lib/nvngx_dlss.dll': b'MZ dlss' + bytes(range(256)) * 900}


def entry(path, name):
    data = BLOBS[path]
    return (path, name, len(data), hashlib.sha256(data).hexdigest())


class Handler(http.server.BaseHTTPRequestHandler):
    served = {}

    def do_GET(self):
        data = Handler.served.get(self.path.split('/', 2)[-1])
        if data is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class DlssTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.repo = f'http://127.0.0.1:{cls.server.server_port}/tag/'

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        Handler.served = dict(BLOBS)
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        self.pins = mock.patch.multiple(dlss, LICENSE=entry('LICENSE.txt', 'license.txt'),
                                        FILES=(entry('lib/nvngx_dlss.dll', 'nvngx_dlss.dll'),))
        self.pins.start()
        self.addCleanup(self.pins.stop)

    def test_license_and_fetch(self):
        self.assertEqual(dlss.license_text(self.repo), BLOBS['LICENSE.txt'].decode())
        self.assertFalse(dlss.installed(self.dir))
        seen = []
        dlss.fetch(self.dir, seen.append, repo=self.repo)
        self.assertTrue(dlss.installed(self.dir))
        self.assertEqual(seen[-1], dlss.total_bytes())
        Handler.served['LICENSE.txt'] = b'changed'
        with self.assertRaises(dlss.FetchError):
            dlss.license_text(self.repo)

    def test_wrong_bytes_refused(self):
        Handler.served['lib/nvngx_dlss.dll'] = BLOBS['lib/nvngx_dlss.dll'][:-1] + b'!'
        with self.assertRaises(dlss.FetchError) as caught:
            dlss.fetch(self.dir, repo=self.repo)
        self.assertEqual(caught.exception.args[0], 'nvngx_dlss.dll')
        self.assertFalse((self.dir / 'nvngx_dlss.dll').exists())
        self.assertFalse((self.dir / 'nvngx_dlss.dll.part').exists())

    def test_pinned_release(self):
        self.pins.stop()
        self.assertTrue(dlss.REPO.startswith('https://raw.githubusercontent.com/NVIDIA/DLSS/'))
        self.assertIn(dlss.TAG, dlss.REPO)
        self.assertEqual([name for _, name, _, _ in dlss.FILES], ['nvngx_dlss.dll', 'nvngx_dlssg.dll'])
        self.assertEqual({path.rsplit('/', 1)[0] for path, _, _, _ in dlss.FILES}, {'lib/Windows_x86_64/rel'})
        for _, _, size, sha in (dlss.LICENSE, *dlss.FILES):
            self.assertGreater(size, 0)
            self.assertEqual(len(sha), 64)
        self.pins.start()


if __name__ == '__main__':
    unittest.main()
