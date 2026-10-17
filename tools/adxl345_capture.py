"""Capture or replay ADXL345 JustFloat telemetry, write CSV and quality metadata."""

import argparse
import csv
import json
import math
import sys
import time
from collections import deque
from datetime import datetime, timezone
from pathlib import Path

from justfloat import (FIELDS, UNITS, SCHEMA_VERSION, NOMINAL_PERIOD_MS,
                       JustFloatParser, QualityTracker)


class LivePlot:
    def __init__(self):
        import matplotlib.pyplot as plt
        self.plt = plt
        plt.ion()
        self.figure, self.axes = plt.subplots(figsize=(10, 5))
        self.lines = [self.axes.plot([], [], label=axis)[0] for axis in ("X", "Y", "Z")]
        self.axes.set(xlabel="Host elapsed time (s)", ylabel="Acceleration (g)", title="ADXL345")
        self.axes.legend()
        self.axes.grid(True, alpha=0.3)
        self.history = deque(maxlen=1000)
        self.next_draw = 0.0

    def add(self, elapsed, values):
        self.history.append((elapsed, *values[5:8]))

    def draw(self):
        now = time.monotonic()
        if now < self.next_draw:
            return True
        if not self.plt.fignum_exists(self.figure.number):
            return False
        self.next_draw = now + 0.1
        if self.history:
            points = list(self.history)
            for i, line in enumerate(self.lines):
                line.set_data([p[0] for p in points], [p[i + 1] for p in points])
            self.axes.set_xlim(max(0, points[-1][0] - 10), max(10, points[-1][0]))
            finite = [v for p in points for v in p[1:] if math.isfinite(v)]
            if finite:
                self.axes.set_ylim(min(-0.1, min(finite) - 0.1), max(0.1, max(finite) + 0.1))
            self.figure.canvas.draw_idle()
        self.plt.pause(0.001)
        return True

    def close(self):
        self.plt.close(self.figure)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="Serial port, e.g. COM5")
    source.add_argument("--replay", type=Path, help="Replay a captured .bin file")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=60, help="Seconds; 0 means until stopped")
    parser.add_argument("--csv", type=Path, default=Path("adxl345_capture.csv"))
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--schema-version", type=int, default=SCHEMA_VERSION)
    args = parser.parse_args()
    if args.schema_version != SCHEMA_VERSION:
        parser.error(f"Only schema_version={SCHEMA_VERSION} is supported")
    if args.duration < 0 or args.baud <= 0:
        parser.error("duration must be >=0 and baud must be >0")
    if args.csv.suffix.lower() != ".csv":
        parser.error("--csv must have a .csv extension")
    outputs = [args.csv, args.csv.with_suffix(".bin"), args.csv.with_suffix(".json")]
    if any(path.exists() for path in outputs):
        parser.error("Output already exists; choose a new --csv path to preserve previous captures")
    if args.replay and args.replay.resolve() == args.csv.with_suffix(".bin").resolve():
        parser.error("Replay input must differ from the output .bin path")
    return args


def main():
    args = parse_args()
    source = None
    plot = None
    parser = JustFloatParser()
    tracker = QualityTracker()
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    metadata = {
        "schema_version": SCHEMA_VERSION,
        "signals": [dict(name=name, type="float32", unit=unit) for name, unit in zip(FIELDS, UNITS)],
        "nominal_period_ms": NOMINAL_PERIOD_MS, "baud": args.baud,
        "port": args.port, "replay": str(args.replay) if args.replay else None,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "firmware_identity": "Record the flashed image/version separately; JustFloat carries no identity",
    }
    error = None
    interrupted = False
    try:
        if args.replay:
            source = args.replay.open("rb")
        else:
            import serial
            source = serial.Serial(args.port, args.baud, timeout=0.05)
        if not args.no_plot:
            plot = LivePlot()
        started = time.monotonic()
        with args.csv.open("w", newline="", encoding="utf-8") as csv_file, args.csv.with_suffix(".bin").open("wb") as raw:
            writer = csv.writer(csv_file)
            writer.writerow(["host_elapsed_s", *FIELDS, "interval_ms", "quality_issues"])
            while not args.duration or time.monotonic() - started < args.duration:
                data = source.read(1024 if args.replay else max(1, min(source.in_waiting, 4096)))
                if not data and args.replay:
                    break
                raw.write(data)
                elapsed = time.monotonic() - started
                for values in parser.feed(data):
                    interval, issues = tracker.observe(values)
                    row = [int(v) if i in (0, 1, 12, 13, 14, 15) else v for i, v in enumerate(values)]
                    writer.writerow([elapsed, *row, interval, "|".join(issues)])
                    if plot:
                        plot.add(elapsed, values)
                if plot and not plot.draw():
                    break
    except KeyboardInterrupt:
        interrupted = True
    except (OSError, ImportError, RuntimeError, ValueError) as exc:
        error = str(exc)
    finally:
        if source is not None:
            source.close()
        if plot is not None:
            plot.close()
        parser.finish()
        metadata["duration_s"] = time.monotonic() - started
        metadata["stopped_by_user"] = interrupted
        metadata["error"] = error
        metadata["analysis"] = tracker.report(parser)
        if error:
            metadata["analysis"]["quality"] = "fail"
        args.csv.with_suffix(".json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(json.dumps(metadata["analysis"], indent=2))
    if error:
        print(error, file=sys.stderr)
    return 1 if metadata["analysis"]["quality"] == "fail" else 0


if __name__ == "__main__":
    raise SystemExit(main())
