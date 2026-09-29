import json
import os
import struct
import tempfile
import unittest

import tracediff


def f64(x: float) -> str:
    return struct.pack(">d", x).hex()


def f32(x: float) -> str:
    return struct.pack(">f", x).hex()


HEADER = {"format": "mcporl-trace", "version": 1, "mc_version": "test", "source": "sim",
          "domains": ["movement"], "fields": {"pos.x": "f64", "yRot": "f32", "hurtTime": "i32"}}


def write_trace(tmp: str, name: str, ticks: list[dict], header: dict = HEADER) -> str:
    path = os.path.join(tmp, name)
    with open(path, "w") as f:
        f.write(json.dumps(header) + "\n")
        for t in ticks:
            f.write(json.dumps(t) + "\n")
    return path


def tick(t: int, x: float, yaw: float = 0.0, hurt: int = 0) -> dict:
    return {"t": t, "inputs": {}, "entities": {"p0": {"pos.x": f64(x), "yRot": f32(yaw), "hurtTime": hurt}}}


class TraceDiffTest(unittest.TestCase):
    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self.tmp = self._dir.name

    def tearDown(self):
        self._dir.cleanup()

    def run_diff(self, a, b):
        return tracediff.compare(tracediff.load(a), tracediff.load(b))

    def test_identical(self):
        ticks = [tick(i, i * 0.1, 45.0, i % 3) for i in range(50)]
        a = write_trace(self.tmp, "a", ticks)
        b = write_trace(self.tmp, "b", ticks)
        r = self.run_diff(a, b)
        self.assertTrue(r.ok)
        self.assertEqual(r.ticks_compared, 50)
        self.assertEqual(tracediff.main([a, b]), 0)

    def test_one_ulp_is_caught(self):
        x = 0.1
        x_next = struct.unpack(">d", struct.pack(">Q", struct.unpack(">Q", struct.pack(">d", x))[0] + 1))[0]
        a = write_trace(self.tmp, "a", [tick(0, 0.0), tick(1, x)])
        b = write_trace(self.tmp, "b", [tick(0, 0.0), tick(1, x_next)])
        r = self.run_diff(a, b)
        self.assertFalse(r.ok)
        self.assertEqual(len(r.mismatches), 1)
        m = r.mismatches[0]
        self.assertEqual((m.tick, m.field, m.ulps), (1, "pos.x", 1))
        self.assertEqual(tracediff.main([a, b]), 1)

    def test_signed_zero_differs(self):
        a = write_trace(self.tmp, "a", [tick(0, 0.0)])
        b = write_trace(self.tmp, "b", [tick(0, -0.0)])
        r = self.run_diff(a, b)
        self.assertEqual(len(r.mismatches), 1)
        self.assertEqual(r.mismatches[0].ulps, 0)
        self.assertIn("signed zero", r.mismatches[0].describe())

    def test_int_field(self):
        a = write_trace(self.tmp, "a", [tick(0, 1.0, hurt=10)])
        b = write_trace(self.tmp, "b", [tick(0, 1.0, hurt=9)])
        r = self.run_diff(a, b)
        self.assertEqual([m.field for m in r.mismatches], ["hurtTime"])

    def test_only_shared_fields_compared(self):
        header_b = dict(HEADER, fields={"pos.x": "f64"})
        a = write_trace(self.tmp, "a", [tick(0, 1.0, yaw=10.0)])
        b = write_trace(self.tmp, "b", [{"t": 0, "entities": {"p0": {"pos.x": f64(1.0)}}}], header_b)
        self.assertTrue(self.run_diff(a, b).ok)

    def test_type_conflict_is_structural(self):
        header_b = dict(HEADER, fields={"pos.x": "f32", "yRot": "f32", "hurtTime": "i32"})
        a = write_trace(self.tmp, "a", [tick(0, 1.0)])
        b = write_trace(self.tmp, "b", [tick(0, 1.0)], header_b)
        r = self.run_diff(a, b)
        self.assertFalse(r.ok)
        self.assertTrue(r.structural)

    def test_length_mismatch(self):
        a = write_trace(self.tmp, "a", [tick(0, 1.0), tick(1, 1.0)])
        b = write_trace(self.tmp, "b", [tick(0, 1.0)])
        self.assertFalse(self.run_diff(a, b).ok)

    def test_bad_header(self):
        a = write_trace(self.tmp, "a", [], {"format": "nope"})
        b = write_trace(self.tmp, "b", [tick(0, 1.0)])
        self.assertEqual(tracediff.main([a, b]), 2)

    def test_ulp_ordering_across_zero(self):
        neg = struct.unpack(">Q", struct.pack(">d", -5e-324))[0]
        pos = struct.unpack(">Q", struct.pack(">d", 5e-324))[0]
        self.assertEqual(tracediff.ulps("f64", neg, pos), 2)


if __name__ == "__main__":
    unittest.main()
