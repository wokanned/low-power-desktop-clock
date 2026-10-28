"""Host checks for the file maintenance wire contract; no serial port is opened."""

import contextlib
import dataclasses
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import file_store_cli as fs


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class FakePort:
    def __init__(self, clock, handler):
        self.clock = clock
        self.handler = handler
        self.writes = []
        self.pending = []
        self.read_error = None

    def enqueue(self, frame, delay=0):
        self.pending.append((self.clock.now + delay, frame.encode()))
        self.pending.sort(key=lambda item: item[0])

    def write(self, data):
        self.writes.append(bytes(data))
        parsed = fs.FrameParser().feed(data)
        if len(parsed) != 1:
            raise AssertionError("fake peer received a malformed request")
        self.handler(self, parsed[0])
        return len(data)

    def read(self, size):
        if self.read_error:
            raise self.read_error
        self.clock.now += 0.1
        if self.pending and self.pending[0][0] <= self.clock.now:
            due, data = self.pending.pop(0)
            if len(data) > size:
                self.pending.insert(0, (due, data[size:]))
            return data[:size]
        return b""


def final(request, **kwargs):
    fields = {"flags": fs.FINAL, "payload": b""}
    fields.update(kwargs)
    return dataclasses.replace(request, **fields)


def connected(handler, **kwargs):
    clock = FakeClock()

    def peer(port, request):
        if request.operation == fs.OP_HELLO:
            port.enqueue(final(request))
        else:
            handler(port, request)

    port = FakePort(clock, peer)
    client = fs.Client(port, session=0x12345678, clock=clock, **kwargs)
    client.hello()
    return client, port, clock


class CodecTests(unittest.TestCase):
    def test_header_and_crc_contract(self):
        frame = fs.Frame(8, 0x12345678, 2, offset=256, payload=b"123456789")
        wire = frame.encode()
        expected = struct.pack("<4sBBBBIIIIIiHH", b"DCFS", 1, 8, 0, 0,
                               0x12345678, 2, 256, 0, 0, 0, 9, 0)
        self.assertEqual(fs.HEADER.size, 36)
        self.assertEqual(wire[:36], expected)
        self.assertEqual(wire[36:-4], b"123456789")
        self.assertEqual(wire[-4:], struct.pack("<I", zlib.crc32(expected[4:] + b"123456789")))
        self.assertEqual(zlib.crc32(b"123456789"), 0xcbf43926)

    def test_all_frame_split_points(self):
        frame = fs.Frame(4, 3, 9, flags=fs.FINAL, payload=bytes(range(256)))
        wire = frame.encode()
        for cut in range(len(wire) + 1):
            parser = fs.FrameParser()
            self.assertEqual(parser.feed(wire[:cut]) + parser.feed(wire[cut:]), [frame])

    def test_bytewise_noise_and_concatenation(self):
        first = fs.Frame(4, 3, 1, flags=fs.FINAL, payload=b"DCFS\x00\xff")
        second = fs.Frame(1, 3, 2, flags=fs.BUSY, result=-1002)
        parser = fs.FrameParser()
        found = []
        for byte in b"noise" + first.encode() + second.encode():
            found.extend(parser.feed(bytes([byte])))
        self.assertEqual(found, [first, second])

    def test_crc_failure_recovers(self):
        frame = fs.Frame(1, 2, 3, flags=fs.FINAL)
        damaged = bytearray(frame.encode())
        damaged[-1] ^= 0x20
        parser = fs.FrameParser()
        self.assertEqual(parser.feed(damaged + frame.encode()), [frame])
        self.assertEqual(parser.rejected, 1)

    def test_damaged_length_does_not_hide_short_response(self):
        frame = fs.Frame(1, 2, 3, flags=fs.FINAL)
        damaged = bytearray(frame.encode())
        struct.pack_into("<H", damaged, 32, 256)
        parser = fs.FrameParser()
        self.assertEqual(parser.feed(damaged + frame.encode()), [frame])

    def test_invalid_headers_and_bounded_noise(self):
        frame = fs.Frame(1, 2, 3, flags=fs.FINAL)
        for index, value in ((4, 2), (5, 15), (6, 3), (34, 1)):
            damaged = bytearray(frame.encode())
            damaged[index] = value
            parser = fs.FrameParser()
            self.assertEqual(parser.feed(damaged + frame.encode()), [frame])
        parser = fs.FrameParser()
        self.assertEqual(parser.feed(b"x" * 100000), [])
        self.assertLessEqual(len(parser.buffer), 3)

    def test_payload_limit(self):
        self.assertEqual(len(fs.Frame(8, 1, 1, payload=b"x" * 256).encode()), 296)
        with self.assertRaises(ValueError):
            fs.Frame(8, 1, 1, payload=b"x" * 257).encode()


class PathTests(unittest.TestCase):
    def test_valid_ascii_paths_and_spaces(self):
        self.assertEqual(fs.encode_path("/"), b"/")
        self.assertEqual(fs.encode_path("/dir/my image.bin"), b"/dir/my image.bin")
        fs.encode_path("/" + "a" * 31 + "/" + "b" * 31 + "/" + "c" * 30)

    def test_invalid_paths(self):
        for path in ("", "relative", "/a/", "/a//b", "/a/./b", "/a/../b",
                     "/a\\b", "/a\x00b", "/a\nb", "/中文", "/" + "x" * 32,
                     "/.upload.tmp", "/a/.upload.tmp", "/.upload.tmp/x",
                     "/" + "a" * 31 + "/" + "b" * 31 + "/" + "c" * 31):
            with self.subTest(path=path), self.assertRaises(ValueError):
                fs.encode_path(path)
        with self.assertRaises(ValueError):
            fs.encode_path("/", root_allowed=False)


class SessionTests(unittest.TestCase):
    def test_console_on_retries_across_binary_exit_grace_period(self):
        clock = FakeClock()

        class ConsolePort:
            writes = []

            def write(self, packet):
                self.writes.append(packet)
                return len(packet)

            def read(self, _):
                clock.now += 0.1
                if clock.now >= 5.0 and len(self.writes) >= 6:
                    return b"OK CONSOLE ON\r\n"
                return b""

        port = ConsolePort()
        fs._text_command(port, "CONSOLE ON", b"OK CONSOLE ON\r\n",
                         timeout=10, retry_interval=1, clock=clock)
        self.assertTrue(all(packet == b"CONSOLE ON\n" for packet in port.writes))
        self.assertGreaterEqual(len(port.writes), 6)

    def test_lost_reply_retries_identical_packet_and_sequence(self):
        seen = []

        def peer(port, request):
            seen.append(request)
            if len(seen) == 2:
                port.enqueue(final(request))

        client, port, _ = connected(peer, reply_timeout=0.3)
        client.exchange(fs.OP_REMOVE, payload=b"/old")
        self.assertEqual(len(seen), 2)
        self.assertEqual(seen[0], seen[1])
        self.assertEqual(port.writes[-1], port.writes[-2])
        self.assertEqual(client.sequence, 2)

    def test_busy_prevents_short_retries_but_final_completes(self):
        def peer(port, request):
            for delay in (0.2, 0.4, 0.6, 0.8):
                port.enqueue(dataclasses.replace(request, flags=fs.BUSY, result=-1002), delay)
            port.enqueue(final(request), 1.0)

        client, port, _ = connected(peer, reply_timeout=0.5)
        client.exchange(fs.OP_FORMAT, payload=b"CONFIRM")
        self.assertEqual(len(port.writes), 2)  # HELLO and exactly one mutation

    def test_busy_cannot_extend_absolute_deadline(self):
        def peer(port, request):
            for index in range(1, 30):
                port.enqueue(dataclasses.replace(request, flags=fs.BUSY, result=-1002), index * 0.1)

        client, _, _ = connected(peer, reply_timeout=0.5, operation_timeout=1.0)
        with self.assertRaises(fs.UncertainResultError):
            client.exchange(fs.OP_UPLOAD_COMMIT)
        self.assertTrue(client.uncertain)
        with self.assertRaises(fs.UncertainResultError):
            client.exchange(fs.OP_UPLOAD_CANCEL)

    def test_wrong_session_sequence_operation_and_request_frames_ignored(self):
        def peer(port, request):
            port.enqueue(dataclasses.replace(final(request), session=99))
            port.enqueue(dataclasses.replace(final(request), sequence=99))
            port.enqueue(dataclasses.replace(final(request), operation=13))
            port.enqueue(dataclasses.replace(request, result=-999))
            port.enqueue(final(request))

        client, _, _ = connected(peer)
        self.assertEqual(client.exchange(fs.OP_MKDIR, payload=b"/ok").result, 0)

    def test_remote_failure_consumes_sequence(self):
        def peer(port, request):
            port.enqueue(final(request, result=-2 if request.sequence == 1 else 0))

        client, _, _ = connected(peer)
        with self.assertRaises(fs.RemoteError):
            client.exchange(fs.OP_REMOVE, payload=b"/missing")
        self.assertEqual(client.sequence, 2)
        client.exchange(fs.OP_MKDIR, payload=b"/new")
        self.assertEqual(client.sequence, 3)

    def test_retry_exhaustion_blocks_further_requests(self):
        client, port, _ = connected(lambda *_: None, reply_timeout=0.2, retries=2)
        with self.assertRaises(fs.UncertainResultError):
            client.exchange(fs.OP_REMOVE, payload=b"/unknown")
        self.assertEqual(len(port.writes), 4)  # HELLO + initial + two retries
        with self.assertRaises(fs.UncertainResultError):
            client.exchange(fs.OP_REMOVE, payload=b"/other")

    def test_read_failure_retains_ambiguous_outcome(self):
        def peer(port, _):
            port.read_error = OSError("disconnected")

        client, _, _ = connected(peer)
        with self.assertRaises(OSError):
            client.exchange(fs.OP_UPLOAD_COMMIT)
        self.assertTrue(client.uncertain)

    def test_partial_write_retains_ambiguous_outcome(self):
        client, port, _ = connected(lambda *_: None)
        port.write = lambda packet: len(packet) - 1
        with self.assertRaises(fs.UncertainResultError):
            client.exchange(fs.OP_MKDIR, payload=b"/partial")
        self.assertTrue(client.uncertain)

    def test_status_reports_unformatted_or_error_without_throwing(self):
        client, _, _ = connected(lambda port, request: port.enqueue(
            final(request, result=-84, offset=4, size=3070, value_crc32=2, file_type=3)))
        snapshot = client.status()
        self.assertEqual(snapshot["state"], "error")
        self.assertEqual(snapshot["result"], -84)
        self.assertTrue(snapshot["upload_active"])
        self.assertTrue(snapshot["busy"])


class FileTests(unittest.TestCase):
    def test_upload_binary_blocks_and_commit(self):
        contents = bytes(range(256)) * 2 + b"\x00end\xff"
        received = bytearray()

        def peer(port, request):
            if request.operation == fs.OP_UPLOAD_BEGIN:
                self.assertEqual(request.payload, b"/images/test.bin")
                self.assertEqual(request.size, len(contents))
                self.assertEqual(request.value_crc32, zlib.crc32(contents))
            if request.operation == fs.OP_UPLOAD_WRITE:
                self.assertEqual(request.offset, len(received))
                received.extend(request.payload)
            if request.operation == fs.OP_UPLOAD_COMMIT:
                port.enqueue(final(request, size=len(received), value_crc32=zlib.crc32(received)))
            else:
                port.enqueue(final(request))

        client, _, _ = connected(peer)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.bin"
            source.write_bytes(contents)
            result = client.upload(source, "/images/test.bin")
        self.assertEqual(bytes(received), contents)
        self.assertEqual(result["size"], len(contents))

    def test_failed_upload_cancels_temp(self):
        operations = []

        def peer(port, request):
            operations.append(request.operation)
            port.enqueue(final(request, result=-28 if request.operation == fs.OP_UPLOAD_WRITE else 0))

        client, _, _ = connected(peer)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.bin"
            source.write_bytes(b"payload")
            with self.assertRaises(fs.RemoteError):
                client.upload(source, "/new")
        self.assertEqual(operations, [fs.OP_UPLOAD_BEGIN, fs.OP_UPLOAD_WRITE, fs.OP_UPLOAD_CANCEL])

    def test_ambiguous_commit_does_not_send_new_cancel(self):
        operations = []

        def peer(port, request):
            operations.append(request.operation)
            if request.operation != fs.OP_UPLOAD_COMMIT:
                port.enqueue(final(request))

        client, _, _ = connected(peer, reply_timeout=0.2, retries=0)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "empty.bin"
            source.write_bytes(b"")
            with self.assertRaises(fs.UncertainResultError):
                client.upload(source, "/new")
        self.assertEqual(operations, [fs.OP_UPLOAD_BEGIN, fs.OP_UPLOAD_COMMIT])

    def test_download_binary_without_truncating_existing_file(self):
        contents = bytes(range(256)) + b"\x00\xff"

        def peer(port, request):
            if request.operation == fs.OP_STAT:
                port.enqueue(final(request, size=len(contents), file_type=1))
            else:
                port.enqueue(final(request, size=len(contents),
                                   payload=contents[request.offset:request.offset + request.size]))

        client, port, _ = connected(peer)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "saved.bin"
            result = client.download("/data", destination)
            self.assertEqual(destination.read_bytes(), contents)
            self.assertFalse(destination.with_name("saved.bin.part").exists())
            self.assertEqual(result["crc32"], f"{zlib.crc32(contents):08x}")
            calls = len(port.writes)
            with self.assertRaises(FileExistsError):
                client.download("/data", destination)
            self.assertEqual(len(port.writes), calls)
            self.assertEqual(destination.read_bytes(), contents)

    def test_bad_download_preserves_existing_destination_and_partial(self):
        def peer(port, request):
            if request.operation == fs.OP_STAT:
                port.enqueue(final(request, size=2, file_type=1))
            else:
                port.enqueue(final(request, size=9, payload=b"xx"))

        client, _, _ = connected(peer)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "saved.bin"
            destination.write_bytes(b"old")
            with self.assertRaises(fs.ProtocolError):
                client.download("/data", destination, overwrite=True)
            self.assertEqual(destination.read_bytes(), b"old")
            self.assertTrue(destination.with_name("saved.bin.part").exists())

    def test_existing_partial_is_not_truncated(self):
        client, _, _ = connected(lambda port, request: port.enqueue(
            final(request, size=0, file_type=1)))
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "saved.bin"
            partial = destination.with_name("saved.bin.part")
            partial.write_bytes(b"interrupted")
            with self.assertRaises(FileExistsError):
                client.download("/data", destination)
            self.assertEqual(partial.read_bytes(), b"interrupted")

    def test_directory_listing_cursor(self):
        def peer(port, request):
            if request.offset == 0:
                port.enqueue(final(request, offset=3, size=10, file_type=1, payload=b"a.bin"))
            else:
                self.assertEqual(request.offset, 3)
                port.enqueue(final(request))

        client, _, _ = connected(peer)
        self.assertEqual(list(client.list("/")), [{"name": "a.bin", "size": 10, "type": 1}])

    def test_destructive_commands_require_explicit_confirmation(self):
        for command in ("format", "selftest"):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                fs.parse_args(["--port", "COM5", command])
            args = fs.parse_args(["--port", "COM5", command, "--confirm"])
            self.assertTrue(args.confirm)


if __name__ == "__main__":
    unittest.main()
