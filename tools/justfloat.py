"""ADXL345 schema 1: fixed 16 little-endian float32 channels and +Inf tail."""

import math
import struct
from collections import Counter

SCHEMA_VERSION = 1
MODULUS = 1 << 24
NOMINAL_PERIOD_MS = 10
SCALE = 0.0039
TAIL = b"\x00\x00\x80\x7f"
FIELDS = (
    "timestamp_ms", "sequence", "raw_x", "raw_y", "raw_z",
    "ax_g", "ay_g", "az_g", "filtered_x_g", "filtered_y_g", "filtered_z_g",
    "temperature_c", "sample_error_count", "queue_drop_count", "uart_error_count",
    "status_flags",
)
UNITS = ("ms", "count", "LSB", "LSB", "LSB", "g", "g", "g", "g", "g", "g",
         "degC", "count", "count", "count", "bitmask")
CHANNEL_COUNT = len(FIELDS)
PAYLOAD_SIZE = CHANNEL_COUNT * 4
FRAME_SIZE = PAYLOAD_SIZE + len(TAIL)
INITIALIZED, VALID, TEMP_INVALID, NOT_READY, IO_ERROR, CONFIG_ERROR, STARTUP, OVERRUN, LATE, SATURATED = (
    1 << i for i in range(10)
)
INTEGER_INDICES = (0, 1, 12, 13, 14, 15)


def validate(values):
    """Reject wrong shapes/types, impossible state and mismatched physical scaling."""
    if len(values) != CHANNEL_COUNT:
        return False
    for i in INTEGER_INDICES:
        value = values[i]
        if not math.isfinite(value) or value != int(value) or not 0 <= value < MODULUS:
            return False
    flags = int(values[15])
    if flags >= (1 << 10) or not flags & TEMP_INVALID or not math.isnan(values[11]):
        return False
    if flags & VALID:
        if not flags & INITIALIZED or flags & (STARTUP | NOT_READY | IO_ERROR | CONFIG_ERROR):
            return False
        if not all(math.isfinite(v) for v in values[2:11]):
            return False
        for raw, accel in zip(values[2:5], values[5:8]):
            if raw != int(raw) or not -32768 <= raw <= 32767:
                return False
            if not math.isclose(raw * SCALE, accel, abs_tol=1e-5, rel_tol=1e-5):
                return False
    elif not all(math.isnan(v) for v in values[2:11]):
        return False
    return True


class JustFloatParser:
    """Bounded buffering; recover at a delimiter using the preceding 64 bytes.

    JustFloat has no CRC or schema identifier on the wire. Semantic validation
    helps with synchronization but cannot prove that every bit is uncorrupted.
    """

    def __init__(self):
        self.buffer = bytearray()
        self.discarded_bytes = 0
        self.bad_frames = 0
        self.frames = 0

    def feed(self, data):
        self.buffer.extend(data)
        output = []
        while True:
            end = self.buffer.find(TAIL)
            if end < 0:
                # Retain a full payload and a possible split delimiter prefix.
                keep = PAYLOAD_SIZE + len(TAIL) - 1
                if len(self.buffer) > keep:
                    drop = len(self.buffer) - keep
                    self.discarded_bytes += drop
                    del self.buffer[:drop]
                return output
            segment = bytes(self.buffer[:end])
            del self.buffer[:end + len(TAIL)]
            if len(segment) >= PAYLOAD_SIZE:
                values = struct.unpack("<16f", segment[-PAYLOAD_SIZE:])
                if validate(values):
                    self.discarded_bytes += len(segment) - PAYLOAD_SIZE
                    self.frames += 1
                    output.append(values)
                    continue
            self.bad_frames += 1
            self.discarded_bytes += len(segment) + len(TAIL)

    def finish(self):
        if self.buffer:
            self.bad_frames += 1
            self.discarded_bytes += len(self.buffer)
            self.buffer.clear()


class QualityTracker:
    def __init__(self):
        self.previous = None
        self.count = 0
        self.valid_count = 0
        self.missing_frames = 0
        self.resets = 0
        self.elapsed_ms = 0
        self.intervals_count = 0
        self.interval_sum = 0.0
        self.interval_square_sum = 0.0
        self.interval_min = None
        self.interval_max = None
        self.issues = Counter()
        self.last_counters = [0, 0, 0]
        self.counter_increases = [0, 0, 0]

    def observe(self, values):
        timestamp, sequence = (int(v) for v in values[:2])
        counters = [int(v) for v in values[12:15]]
        flags = int(values[15])
        issues = []
        interval = None
        self.count += 1
        if flags & VALID:
            self.valid_count += 1
        else:
            issues.append("invalid_sample")
        for flag, label in ((OVERRUN, "sensor_overrun"), (LATE, "late"), (SATURATED, "saturated")):
            if flags & flag:
                issues.append(label)
        if self.previous is not None:
            old_timestamp, old_sequence, old_counters = self.previous
            delta_t = (timestamp - old_timestamp) % MODULUS
            delta_s = (sequence - old_sequence) % MODULUS
            # Reset requires evidence of startup plus an earlier clock/sequence.
            reset = (bool(flags & STARTUP or not flags & INITIALIZED)
                     and timestamp < 10000 and sequence < 1000
                     and timestamp < old_timestamp and sequence <= old_sequence
                     and all(a <= b for a, b in zip(counters, old_counters)))
            if reset:
                self.resets += 1
                issues.append("device_reset")
            else:
                if delta_s == 0:
                    issues.append("duplicate_sequence")
                elif delta_s >= MODULUS // 2:
                    issues.append("sequence_backwards")
                else:
                    self.missing_frames += delta_s - 1
                    if delta_s > 1:
                        issues.append("sequence_gap")
                if delta_t == 0 or delta_t >= MODULUS // 2:
                    issues.append("timestamp_backwards")
                else:
                    interval = delta_t
                    self.elapsed_ms += delta_t
                    if 0 < delta_s < MODULUS // 2:
                        per_sample = delta_t / delta_s
                        self.intervals_count += 1
                        self.interval_sum += per_sample
                        self.interval_square_sum += per_sample * per_sample
                        self.interval_min = per_sample if self.interval_min is None else min(self.interval_min, per_sample)
                        self.interval_max = per_sample if self.interval_max is None else max(self.interval_max, per_sample)
                        if abs(per_sample - NOMINAL_PERIOD_MS) > 2:
                            issues.append("period_out_of_range")
                for i, (current, old) in enumerate(zip(counters, old_counters)):
                    if current < old:
                        issues.append("counter_backwards")
                    else:
                        self.counter_increases[i] += current - old
                # A gap may include host corruption; do not invent its cause.
                if 1 < delta_s < MODULUS // 2 and counters[1:] == old_counters[1:]:
                    issues.append("unexplained_gap")
        self.previous = timestamp, sequence, counters
        self.last_counters = counters
        self.issues.update(issues)
        return interval, issues

    def report(self, parser):
        mean = self.interval_sum / self.intervals_count if self.intervals_count else None
        std = math.sqrt(max(0, self.interval_square_sum / self.intervals_count - mean * mean)) if mean else None
        failures = {"timestamp_backwards", "sequence_backwards", "counter_backwards", "duplicate_sequence"}
        if failures.intersection(self.issues) or not self.count or not self.valid_count:
            result = "fail"
        elif self.issues or parser.bad_frames or parser.discarded_bytes or any(self.last_counters):
            result = "warn"
        else:
            result = "pass"
        return {
            "quality": result, "frames": self.count, "valid_samples": self.valid_count,
            "missing_frames": self.missing_frames, "device_resets": self.resets,
            "observed_device_span_ms": self.elapsed_ms,
            "sample_period_mean_ms": mean, "sample_period_std_ms": std,
            "sample_period_min_ms": self.interval_min, "sample_period_max_ms": self.interval_max,
            "sample_rate_hz": 1000 / mean if mean else None,
            "counter_names": list(FIELDS[12:15]), "last_counters": self.last_counters,
            "counter_increases_during_capture": self.counter_increases,
            "issues": dict(self.issues), "bad_frames": parser.bad_frames,
            "discarded_bytes": parser.discarded_bytes,
        }
