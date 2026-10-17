import math
import os
import struct
import unittest
from pathlib import Path

from justfloat import (INITIALIZED, VALID, TEMP_INVALID, STARTUP, IO_ERROR,
                       MODULUS, TAIL, FIELDS, SCALE, JustFloatParser, QualityTracker)


def sample(sequence=0, timestamp=100, flags=INITIALIZED | VALID | TEMP_INVALID,
           counters=(0, 0, 0)):
    data = [float(timestamp), float(sequence), -256.0, 0.0, 256.0,
            -256 * SCALE, 0.0, 256 * SCALE, -256 * SCALE, 0.0, 256 * SCALE,
            math.nan, *map(float, counters), float(flags)]
    if not flags & VALID:
        data[2:11] = [math.nan] * 9
    return tuple(data)


def encode(values):
    return struct.pack("<16f", *values) + TAIL


class ProtocolTests(unittest.TestCase):
    def test_all_chunk_boundaries_and_nan(self):
        wire = encode(sample())
        for cut in range(len(wire) + 1):
            parser = JustFloatParser()
            frames = parser.feed(wire[:cut]) + parser.feed(wire[cut:])
            self.assertEqual(len(frames), 1)
            self.assertEqual(frames[0][2:5], (-256, 0, 256))
            self.assertTrue(math.isnan(frames[0][11]))
            self.assertEqual(parser.bad_frames, 0)

    def test_bytewise_and_concatenated(self):
        wire = encode(sample()) + encode(sample(1, 110))
        parser = JustFloatParser()
        frames = []
        for byte in wire:
            frames.extend(parser.feed(bytes([byte])))
        self.assertEqual(len(frames), 2)
        self.assertEqual(len(JustFloatParser().feed(wire)), 2)

    def test_noise_partial_frame_and_missing_delimiter(self):
        wire = b"noise" + encode(sample())[:25] + TAIL + encode(sample(1, 110))
        parser = JustFloatParser()
        self.assertEqual(parser.feed(wire)[0][1], 1)
        self.assertEqual(parser.bad_frames, 1)
        parser = JustFloatParser()
        self.assertEqual(parser.feed(encode(sample())[:-4] + encode(sample(1, 110)))[0][1], 1)
        self.assertEqual(parser.discarded_bytes, 64)

    def test_payload_byte_loss_recovers_next_frame(self):
        damaged = encode(sample())
        damaged = damaged[:9] + damaged[10:]
        parser = JustFloatParser()
        frames = parser.feed(damaged + encode(sample(1, 110)))
        self.assertEqual([f[1] for f in frames], [1])
        self.assertEqual(parser.bad_frames, 1)

    def test_invalid_status_and_wrong_channel_count(self):
        parser = JustFloatParser()
        wrong = struct.pack("<3f", 1, 2, 3) + TAIL
        impossible = list(sample())
        impossible[5] = 42
        frames = parser.feed(wrong + encode(impossible) + encode(sample()))
        self.assertEqual(len(frames), 1)
        self.assertEqual(parser.bad_frames, 2)

    def test_invalid_sample_is_retained(self):
        flags = TEMP_INVALID | IO_ERROR
        parser = JustFloatParser()
        frames = parser.feed(encode(sample(flags=flags)))
        self.assertEqual(len(frames), 1)
        tracker = QualityTracker()
        tracker.observe(frames[0])
        self.assertEqual(tracker.report(parser)["quality"], "fail")

    def test_bounded_noise_and_unfinished_tail(self):
        parser = JustFloatParser()
        parser.feed(b"x" * 1000000)
        self.assertLessEqual(len(parser.buffer), 67)
        parser.finish()
        self.assertEqual(parser.discarded_bytes, 1000000)
        self.assertEqual(parser.bad_frames, 1)

    def test_counter_gaps_and_period_statistics(self):
        tracker = QualityTracker()
        tracker.observe(sample(0, 100))
        interval, issues = tracker.observe(sample(3, 130, counters=(1, 2, 0)))
        self.assertEqual(interval, 30)
        self.assertIn("sequence_gap", issues)
        self.assertNotIn("unexplained_gap", issues)
        self.assertEqual(tracker.missing_frames, 2)
        report = tracker.report(JustFloatParser())
        self.assertEqual(report["sample_period_mean_ms"], 10)
        self.assertEqual(report["counter_increases_during_capture"], [1, 2, 0])
        self.assertEqual(report["quality"], "warn")

    def test_backwards_duplicate_and_unexplained_gap(self):
        tracker = QualityTracker()
        tracker.observe(sample(1, 100))
        self.assertIn("unexplained_gap", tracker.observe(sample(3, 120))[1])
        self.assertIn("duplicate_sequence", tracker.observe(sample(3, 130))[1])
        self.assertIn("timestamp_backwards", tracker.observe(sample(4, 110))[1])
        self.assertEqual(tracker.report(JustFloatParser())["quality"], "fail")

    def test_wire_clock_and_sequence_wrap(self):
        tracker = QualityTracker()
        tracker.observe(sample(MODULUS - 1, MODULUS - 5))
        interval, issues = tracker.observe(sample(0, 5))
        self.assertEqual(interval, 10)
        self.assertEqual(issues, [])

    def test_ready_driven_intervals(self):
        tracker = QualityTracker()
        tracker.observe(sample(0, 100))
        for sequence, timestamp in enumerate((111, 121, 132, 142), start=1):
            interval, issues = tracker.observe(sample(sequence, timestamp))
            self.assertIn(interval, (10, 11))
            self.assertEqual(issues, [])
        self.assertEqual(tracker.missing_frames, 0)
        self.assertEqual(tracker.report(JustFloatParser())["quality"], "pass")

    def test_reset_requires_startup_evidence(self):
        tracker = QualityTracker()
        tracker.observe(sample(100, 2000, counters=(2, 3, 1)))
        flags = INITIALIZED | TEMP_INVALID | STARTUP
        self.assertIn("device_reset", tracker.observe(sample(0, 100, flags, (0, 0, 0)))[1])
        self.assertEqual(tracker.resets, 1)

    def test_jitter_counter_regression_and_empty_capture(self):
        tracker = QualityTracker()
        self.assertEqual(tracker.report(JustFloatParser())["quality"], "fail")
        tracker.observe(sample(0, 100, counters=(1, 1, 1)))
        issues = tracker.observe(sample(1, 125, counters=(0, 1, 1)))[1]
        self.assertIn("period_out_of_range", issues)
        self.assertIn("counter_backwards", issues)

    @unittest.skipUnless(os.environ.get("ADXL345_C_FIXTURE"), "Run the host test runner for a C-generated fixture")
    def test_actual_c_encoder(self):
        parser = JustFloatParser()
        frames = parser.feed(Path(os.environ["ADXL345_C_FIXTURE"]).read_bytes())
        self.assertEqual(len(frames), 2)
        self.assertEqual(frames[0][:5], (100, 0, -256, 0, 256))
        self.assertEqual(int(frames[0][15]), INITIALIZED | VALID | TEMP_INVALID)
        self.assertTrue(math.isnan(frames[1][2]))
        self.assertTrue(math.isnan(frames[1][11]))
        self.assertEqual(parser.bad_frames, 0)


if __name__ == "__main__":
    unittest.main()
