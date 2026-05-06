import unittest
from pathlib import Path
import io


class PathlibTest(unittest.TestCase):
    PATH = "/tmp/pystro_test_pathlib.txt"

    def setUp(self):
        try:
            Path(self.PATH).unlink(missing_ok=True)
        except: pass

    def tearDown(self):
        try:
            Path(self.PATH).unlink(missing_ok=True)
        except: pass

    def test_properties(self):
        p = Path("/tmp/foo/bar.txt")
        self.assertEqual(p.name, "bar.txt")
        self.assertEqual(p.parent, Path("/tmp/foo"))
        self.assertEqual(p.suffix, ".txt")
        self.assertEqual(p.stem, "bar")

    def test_parts(self):
        self.assertEqual(Path("/usr/local/bin").parts, ("/", "usr", "local", "bin"))
        self.assertEqual(Path("a/b").parts, ("a", "b"))

    def test_with_suffix(self):
        self.assertEqual(Path("/tmp/foo.txt").with_suffix(".md"), Path("/tmp/foo.md"))

    def test_join(self):
        p = Path("/tmp") / "subdir" / "file.txt"
        self.assertEqual(str(p), "/tmp/subdir/file.txt")

    def test_write_read_text(self):
        p = Path(self.PATH)
        p.write_text("hello")
        self.assertEqual(p.read_text(), "hello")

    def test_unlink_missing_ok(self):
        Path(self.PATH).unlink(missing_ok=True)


class BytesIOTest(unittest.TestCase):
    def test_seek_read(self):
        b = io.BytesIO()
        b.write(b"hello world")
        b.seek(6)
        self.assertEqual(b.read(), b"world")

    def test_initial(self):
        b = io.BytesIO(b"abc")
        self.assertEqual(b.read(), b"abc")

    def test_tell(self):
        b = io.BytesIO(b"abcdef")
        b.seek(3)
        self.assertEqual(b.tell(), 3)
        b.read(2)
        self.assertEqual(b.tell(), 5)


class StringIOTest(unittest.TestCase):
    def test_read_seek(self):
        s = io.StringIO("hello")
        self.assertEqual(s.read(), "hello")
        s.seek(0)
        self.assertEqual(s.read(), "hello")

    def test_write_get(self):
        s = io.StringIO()
        s.write("a"); s.write("bc")
        self.assertEqual(s.getvalue(), "abc")


unittest.main(globals())
