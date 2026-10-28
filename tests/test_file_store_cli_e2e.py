"""Exercise Python CLI against the real C protocol, file engine and littlefs.

The executable uses a bounded in-memory NOR model, never a physical serial port.
"""

from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import file_store_cli as fs


class PipePort:
    def __init__(self, executable):
        self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE)
        self.buffer = bytearray()
        self.now = 0.0
        self.drop_next_final = False
        self.corrupt_next_request = False
        self.mutation_deliveries = 0

    def clock(self):
        return self.now

    def _read_exact(self, length):
        result = bytearray()
        while len(result) < length:
            chunk = self.process.stdout.read(length - len(result))
            if not chunk:
                raise RuntimeError(f"C peer exited: {self.process.poll()}")
            result.extend(chunk)
        return bytes(result)

    def write(self, packet):
        packet = bytes(packet)
        delivered = packet
        if self.corrupt_next_request:
            self.corrupt_next_request = False
            delivered = packet[:-1] + bytes([packet[-1] ^ 1])
        self.process.stdin.write(struct.pack("<I", len(delivered)) + delivered)
        self.process.stdin.flush()
        length = struct.unpack("<I", self._read_exact(4))[0]
        response = self._read_exact(length)
        if self.drop_next_final:
            frames = fs.FrameParser().feed(response)
            if any(frame.flags == fs.FINAL for frame in frames):
                self.drop_next_final = False
                response = b"".join(frame.encode() for frame in frames if frame.flags != fs.FINAL)
        self.buffer.extend(response)
        return len(packet)

    def read(self, size):
        self.now += 0.1
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def close(self):
        self.process.stdin.close()
        code = self.process.wait(timeout=10)
        self.process.stdout.close()
        if code:
            raise RuntimeError(f"C peer failed: {code}")


class IntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = os.environ.get("CC", "gcc")
        if not shutil.which(cls.compiler):
            raise RuntimeError(f"C compiler unavailable: {cls.compiler}")
        cls.output = ROOT / "build" / "file-cli-e2e"
        cls.output.mkdir(parents=True, exist_ok=True)
        cls.executable = cls.output / ("file_store_cli_peer.exe" if os.name == "nt" else "file_store_cli_peer")
        sources = ["tests/file_store_cli_peer.c", "app/file_transfer.c",
                   "middleware/file_store.c", "middleware/littlefs/lfs.c",
                   "middleware/littlefs/lfs_util.c"]
        command = [cls.compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O2",
                   "-DLFS_NO_MALLOC", "-DLFS_NO_DEBUG", "-DLFS_NO_WARN", "-DLFS_NO_ERROR", "-DLFS_NAME_MAX=31"]
        command.extend("-I" + str(ROOT / folder) for folder in ("device", "middleware", "app"))
        command.extend(str(ROOT / path) for path in sources)
        command.extend(["-o", str(cls.executable)])
        subprocess.run(command, check=True, cwd=ROOT)

    def setUp(self):
        self.port = PipePort(self.executable)
        self.client = fs.Client(self.port, session=0x32849576, clock=self.port.clock,
                                reply_timeout=0.3, retries=2)
        self.client.hello()

    def tearDown(self):
        self.port.close()

    def format(self):
        self.client.exchange(fs.OP_FORMAT, payload=b"CONFIRM")

    def test_unformatted_does_not_implicitly_format(self):
        self.assertEqual(self.client.status()["state"], "unformatted")
        with self.assertRaises(fs.RemoteError) as failed:
            self.client.exchange(fs.OP_MKDIR, payload=b"/missing")
        self.assertEqual(failed.exception.result, -1003)
        self.assertEqual(self.client.status()["state"], "unformatted")
        with self.assertRaises(fs.RemoteError) as failed:
            self.client.exchange(fs.OP_FORMAT)
        self.assertEqual(failed.exception.result, -1001)  # malformed wire request
        self.assertEqual(self.client.status()["state"], "unformatted")
        self.format()
        snapshot = self.client.status()
        self.assertEqual(snapshot["state"], "ready")
        self.assertEqual(snapshot["total_blocks"], 3070)
        self.assertGreater(snapshot["used_blocks"], 0)

    def test_binary_upload_replace_list_download_rename_remove(self):
        self.format()
        self.client.exchange(fs.OP_MKDIR, payload=b"/images")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.bin"
            downloaded = Path(directory) / "downloaded.bin"
            first = b"old data\x00\xff"
            source.write_bytes(first)
            self.client.upload(source, "/images/picture.bin")
            # Includes frame magic, every byte value, exact and partial blocks.
            second = bytes(range(256)) * 34 + b"DCFS\x00\r\n\xffend"
            source.write_bytes(second)
            self.client.upload(source, "/images/picture.bin")
            entries = list(self.client.list("/images"))
            self.assertEqual(entries, [{"name": "picture.bin", "size": len(second), "type": 1}])
            response = self.client.download("/images/picture.bin", downloaded)
            self.assertEqual(downloaded.read_bytes(), second)
            self.assertEqual(response["crc32"], f"{zlib.crc32(second):08x}")
            self.client.exchange(fs.OP_RENAME, payload=b"/images/picture.bin\x00/images/new.bin")
            self.client.exchange(fs.OP_REMOVE, payload=b"/images/new.bin")
            self.assertEqual(list(self.client.list("/images")), [])

    def test_commit_crc_failure_preserves_old_file(self):
        self.format()
        with tempfile.TemporaryDirectory() as directory:
            old = Path(directory) / "old.bin"
            old.write_bytes(b"keep old")
            self.client.upload(old, "/target")
            newer = bytes(range(256))
            self.client.exchange(fs.OP_UPLOAD_BEGIN, payload=b"/target", size=len(newer),
                                 value_crc32=zlib.crc32(newer) ^ 1)
            self.client.exchange(fs.OP_UPLOAD_WRITE, payload=newer)
            with self.assertRaises(fs.RemoteError) as failed:
                self.client.exchange(fs.OP_UPLOAD_COMMIT)
            self.assertEqual(failed.exception.result, -1004)
            self.client.download("/target", Path(directory) / "kept.bin")
            self.assertEqual((Path(directory) / "kept.bin").read_bytes(), b"keep old")
            self.assertFalse(self.client.status()["upload_active"])

    def test_lost_final_reply_replays_without_duplicate_mutation(self):
        self.format()
        self.port.drop_next_final = True
        # A second mkdir execution would return EXISTS. Replayed success proves
        # the firmware returned its cached final instead of running mkdir again.
        self.client.exchange(fs.OP_MKDIR, payload=b"/deduplicated")
        self.assertEqual(list(self.client.list("/")),
                         [{"name": "deduplicated", "size": 0, "type": 2}])

    def test_corrupt_request_crc_is_ignored_then_retry_succeeds(self):
        self.format()
        self.port.corrupt_next_request = True
        self.client.exchange(fs.OP_MKDIR, payload=b"/crc_checked")
        self.assertEqual(list(self.client.list("/")),
                         [{"name": "crc_checked", "size": 0, "type": 2}])


if __name__ == "__main__":
    unittest.main()
