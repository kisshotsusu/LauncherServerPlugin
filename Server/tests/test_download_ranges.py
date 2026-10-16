"""HTTP byte-range regression tests, including real interrupted transfers."""
import tempfile
import threading
import unittest
from pathlib import Path
from http.server import ThreadingHTTPServer
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from unittest.mock import patch
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_server import UpdateHandler

class RangeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.file = Path(self.temp.name) / 'data.bin'
        self.data = bytes(range(256)) * 1024
        self.file.write_bytes(self.data)
        target = str(self.file)
        class Handler(UpdateHandler):
            def do_GET(self): self._send_file(target)
            def log_message(self, *args): pass
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = 'http://127.0.0.1:' + str(self.server.server_port)
    def tearDown(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join(); self.temp.cleanup()
    def get(self, **headers):
        return urlopen(Request(self.url, headers=headers), timeout=5)
    def test_full_and_validators(self):
        with self.get() as r:
            self.assertEqual(r.status, 200); self.assertEqual(r.read(), self.data)
            self.assertEqual(r.headers['Accept-Ranges'], 'bytes')
            self.assertTrue(r.headers['ETag']); self.assertTrue(r.headers['Last-Modified'])
    def test_open_closed_suffix_and_clamped_ranges(self):
        for header, start, end in [('bytes=65536-',65536,len(self.data)-1),('bytes=11-31',11,31),('bytes=-17',len(self.data)-17,len(self.data)-1),('bytes=262140-999999',262140,len(self.data)-1)]:
            with self.subTest(header=header), self.get(Range=header) as r:
                self.assertEqual(r.status,206)
                self.assertEqual(r.headers['Content-Range'],f'bytes {start}-{end}/{len(self.data)}')
                self.assertEqual(int(r.headers['Content-Length']),end-start+1)
                self.assertEqual(r.read(),self.data[start:end+1])
    def test_unsatisfiable_and_malformed(self):
        for value in ['bytes=999999-','bytes=10-9','bytes=-0','bytes=x-y','bytes=0-1,3-4','bytes=-']:
            with self.subTest(value=value), self.assertRaises(HTTPError) as caught: self.get(Range=value)
            self.assertEqual(caught.exception.code,416)
            self.assertEqual(caught.exception.headers['Content-Range'],f'bytes */{len(self.data)}')
    def test_if_range_changed_and_matching(self):
        with self.get() as r: etag=r.headers['ETag']; modified=r.headers['Last-Modified']; r.read()
        for validator in [etag, modified]:
            with self.get(Range='bytes=5-', **{'If-Range':validator}) as r:
                self.assertEqual(r.status,206); self.assertEqual(r.read(),self.data[5:])
        with self.get(Range='bytes=5-', **{'If-Range':'"outdated"'}) as r:
            self.assertEqual(r.status,200); self.assertEqual(r.read(),self.data)
    def test_resume_after_client_disconnect(self):
        with self.get() as r: prefix=r.read(4096)
        with self.get(Range=f'bytes={len(prefix)}-') as r:
            self.assertEqual(prefix+r.read(),self.data)
    def test_empty_file(self):
        self.file.write_bytes(b'')
        with self.get() as r: self.assertEqual(r.status,200); self.assertEqual(r.read(),b'')
        with self.assertRaises(HTTPError) as caught: self.get(Range='bytes=0-')
        self.assertEqual(caught.exception.code,416)

if __name__ == '__main__': unittest.main()
