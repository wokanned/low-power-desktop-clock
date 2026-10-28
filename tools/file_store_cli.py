"""Manage DeskClock W25Q128 files through the USART2 maintenance protocol.

Only live commands import pyserial. The frame codec and tests use the standard
library. Closing the link never proves whether an unanswered mutation committed.
"""

import argparse
import dataclasses
import json
import os
from pathlib import Path
import secrets
import struct
import sys
import time
import zlib


MAGIC = b"DCFS"
VERSION = 1
MAX_PAYLOAD = 256
MAX_PATH = 95
MAX_COMPONENT = 31
BLOCK_SIZE = 4096
HEADER = struct.Struct("<4sBBBBIIIIIiHH")
CRC = struct.Struct("<I")
FINAL = 1
BUSY = 2
OP_HELLO = 0
OP_STATUS = 1
OP_LIST = 2
OP_MKDIR = 3
OP_READ = 4
OP_REMOVE = 5
OP_RENAME = 6
OP_UPLOAD_BEGIN = 7
OP_UPLOAD_WRITE = 8
OP_UPLOAD_COMMIT = 9
OP_UPLOAD_CANCEL = 10
OP_FORMAT = 11
OP_STAT = 12
OP_SELFTEST = 13
OP_EXIT = 14
STATE_NAMES = ("unavailable", "initializing", "unformatted", "ready", "error")


class ProtocolError(Exception):
    """The peer did not satisfy the wire contract."""


class UncertainResultError(ProtocolError):
    """No final reply: the device may still have completed the request."""


class RemoteError(ProtocolError):
    def __init__(self, operation, result):
        self.operation = operation
        self.result = result
        super().__init__(f"operation {operation} failed: {result}")


@dataclasses.dataclass(frozen=True)
class Frame:
    operation: int
    session: int
    sequence: int
    offset: int = 0
    size: int = 0
    value_crc32: int = 0
    result: int = 0
    flags: int = 0
    file_type: int = 0
    payload: bytes = b""

    def encode(self):
        if len(self.payload) > MAX_PAYLOAD:
            raise ValueError("frame payload exceeds 256 bytes")
        if self.flags not in (0, FINAL, BUSY):
            raise ValueError("invalid frame flags")
        if not 0 <= self.operation <= OP_EXIT:
            raise ValueError("invalid operation")
        header = HEADER.pack(MAGIC, VERSION, self.operation, self.flags,
                             self.file_type, self.session, self.sequence,
                             self.offset, self.size, self.value_crc32,
                             self.result, len(self.payload), 0)
        body = header[4:] + self.payload
        return header + self.payload + CRC.pack(zlib.crc32(body))


class FrameParser:
    """Bounded streaming parser; recover at the next valid magic and CRC."""

    def __init__(self):
        self.buffer = bytearray()
        self.rejected = 0

    def feed(self, data):
        frames = []
        # Process bounded slices so a caller passing a large capture cannot
        # cause the parser to retain that capture as an unfinished frame.
        for start in range(0, len(data), MAX_PAYLOAD):
            self.buffer.extend(data[start:start + MAX_PAYLOAD])
            while True:
                magic_at = self.buffer.find(MAGIC)
                if magic_at < 0:
                    del self.buffer[:-3]
                    break
                if magic_at:
                    del self.buffer[:magic_at]
                if len(self.buffer) < HEADER.size:
                    break
                fields = HEADER.unpack_from(self.buffer)
                (_, version, operation, flags, file_type, session, sequence,
                 offset, size, value_crc32, result, length, reserved) = fields
                if (version != VERSION or operation > OP_EXIT or
                        flags not in (0, FINAL, BUSY) or reserved or
                        length > MAX_PAYLOAD):
                    self.rejected += 1
                    del self.buffer[0]
                    continue
                total = HEADER.size + length + CRC.size
                if len(self.buffer) < total:
                    # A damaged length field must not hide a complete later
                    # response forever. Only resync early when that candidate
                    # independently has a valid header and trailer CRC.
                    candidate = self.buffer.find(MAGIC, 1)
                    recovered = False
                    while candidate >= 0:
                        tail = bytes(self.buffer[candidate:])
                        if len(tail) >= HEADER.size:
                            candidate_fields = HEADER.unpack_from(tail)
                            candidate_length = candidate_fields[-2]
                            candidate_total = HEADER.size + candidate_length + CRC.size
                            if (candidate_fields[1] == VERSION and
                                    candidate_fields[2] <= OP_EXIT and
                                    candidate_fields[3] in (0, FINAL, BUSY) and
                                    candidate_fields[-1] == 0 and
                                    candidate_length <= MAX_PAYLOAD and
                                    len(tail) >= candidate_total and
                                    zlib.crc32(tail[4:candidate_total - CRC.size]) ==
                                    CRC.unpack_from(tail, candidate_total - CRC.size)[0]):
                                self.rejected += 1
                                del self.buffer[:candidate]
                                recovered = True
                                break
                        candidate = self.buffer.find(MAGIC, candidate + 1)
                    if recovered:
                        continue
                    break
                wanted = CRC.unpack_from(self.buffer, total - CRC.size)[0]
                actual = zlib.crc32(self.buffer[4:total - CRC.size])
                if wanted != actual:
                    self.rejected += 1
                    del self.buffer[0]
                    continue
                payload = bytes(self.buffer[HEADER.size:total - CRC.size])
                del self.buffer[:total]
                frames.append(Frame(operation, session, sequence, offset,
                                    size, value_crc32, result, flags,
                                    file_type, payload))
        return frames


def encode_path(path, *, root_allowed=True):
    try:
        raw = path.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("remote paths must use ASCII") from error
    if not raw or not raw.startswith(b"/") or len(raw) > MAX_PATH:
        raise ValueError("remote path must be absolute and at most 95 bytes")
    if any(byte < 0x20 or byte >= 0x7f or byte == ord("\\") for byte in raw):
        raise ValueError("remote path contains a control character")
    if path == "/":
        if root_allowed:
            return raw
        raise ValueError("this operation cannot target the root directory")
    parts = path[1:].split("/")
    if any(not part or part in (".", "..") or len(part) > MAX_COMPONENT
           for part in parts):
        raise ValueError("path components must contain 1..31 bytes; no . or ..")
    if ".upload.tmp" in parts:
        raise ValueError("the upload temporary path is reserved")
    return raw


class Client:
    """One stop-and-wait session. The exact packet is reused on retry.

    A BUSY response restarts the short reply wait but cannot extend the absolute
    operation deadline. After an ambiguous timeout this client refuses further
    requests: a new sequence could accidentally hide an unfinished mutation.
    """

    def __init__(self, port, *, session=None, reply_timeout=5.0,
                 retries=5, operation_timeout=300.0, clock=time.monotonic):
        if reply_timeout <= 0 or retries < 0 or operation_timeout <= 0:
            raise ValueError("invalid timeout/retry settings")
        self.port = port
        self.session = session if session is not None else secrets.randbelow(0xffffffff) + 1
        if not 1 <= self.session <= 0xffffffff:
            raise ValueError("session must be a nonzero uint32")
        self.reply_timeout = reply_timeout
        self.retries = retries
        self.operation_timeout = operation_timeout
        self.clock = clock
        self.sequence = 0
        self.parser = FrameParser()
        self.uncertain = False
        self.started = False

    def _write(self, packet):
        try:
            count = self.port.write(packet)
        except (OSError, KeyboardInterrupt):
            self.uncertain = True
            raise
        if count != len(packet):
            self.uncertain = True
            raise UncertainResultError("partial serial write; request outcome is unknown")

    def exchange(self, operation, *, payload=b"", offset=0, size=0,
                 value_crc32=0, check=True):
        if self.uncertain:
            raise UncertainResultError("previous request outcome is unknown; wait for device cleanup")
        if operation == OP_HELLO and self.sequence != 0:
            raise ProtocolError("HELLO is only valid at the start of a session")
        if operation != OP_HELLO and not self.started:
            raise ProtocolError("session has not completed HELLO")
        if self.sequence >= 0xffffffff:
            raise ProtocolError("sequence space exhausted; start a new session")
        request = Frame(operation, self.session, self.sequence, offset, size,
                        value_crc32, payload=payload)
        packet = request.encode()
        deadline = self.clock() + self.operation_timeout
        retry_at = self.clock() + self.reply_timeout
        attempts = 0
        self._write(packet)
        while True:
            now = self.clock()
            if now >= deadline:
                self.uncertain = True
                raise UncertainResultError("operation deadline expired; request may have committed")
            if now >= retry_at:
                if attempts >= self.retries:
                    self.uncertain = True
                    raise UncertainResultError("no final reply after retries; request may have committed")
                self._write(packet)
                attempts += 1
                retry_at = now + self.reply_timeout
            try:
                received = self.port.read(HEADER.size + MAX_PAYLOAD + CRC.size)
            except (OSError, KeyboardInterrupt):
                self.uncertain = True
                raise
            for response in self.parser.feed(received):
                if (response.session != self.session or
                        response.sequence != self.sequence or
                        response.operation != operation):
                    continue
                if response.flags == BUSY:
                    retry_at = self.clock() + self.reply_timeout
                    continue
                if response.flags != FINAL:
                    continue
                self.sequence += 1
                if operation == OP_HELLO and response.result == 0:
                    self.started = True
                if operation == OP_EXIT:
                    self.started = False
                if check and response.result != 0:
                    raise RemoteError(operation, response.result)
                return response

    def hello(self):
        return self.exchange(OP_HELLO)

    def status(self):
        response = self.exchange(OP_STATUS, check=False)
        state = response.offset
        return {
            "state": STATE_NAMES[state] if state < len(STATE_NAMES) else f"unknown({state})",
            "result": response.result,
            "used_blocks": response.value_crc32,
            "total_blocks": response.size,
            "block_size": BLOCK_SIZE,
            "upload_active": bool(response.file_type & 1),
            "busy": bool(response.file_type & 2),
        }

    def list(self, path):
        payload = encode_path(path)
        cursor = 0
        while True:
            response = self.exchange(OP_LIST, payload=payload, offset=cursor)
            if not response.payload:
                return
            if response.offset <= cursor:
                raise ProtocolError("directory cursor did not advance")
            try:
                name = response.payload.decode("ascii")
            except UnicodeDecodeError as error:
                raise ProtocolError("invalid directory name encoding") from error
            if not name or "/" in name or "\x00" in name or len(name) > MAX_COMPONENT:
                raise ProtocolError("invalid directory name")
            yield {"name": name, "size": response.size, "type": response.file_type}
            cursor = response.offset

    def upload(self, local, remote):
        path = encode_path(remote, root_allowed=False)
        local = Path(local)
        size = local.stat().st_size
        if size > 0xffffffff:
            raise ValueError("file is too large for the protocol")
        checksum = 0
        with local.open("rb") as source:
            for data in iter(lambda: source.read(65536), b""):
                checksum = zlib.crc32(data, checksum)
        self.exchange(OP_UPLOAD_BEGIN, payload=path, size=size, value_crc32=checksum)
        try:
            offset = 0
            with local.open("rb") as source:
                while True:
                    data = source.read(MAX_PAYLOAD)
                    if not data:
                        break
                    if offset + len(data) > size:
                        raise ProtocolError("source changed while uploading")
                    self.exchange(OP_UPLOAD_WRITE, payload=data, offset=offset)
                    offset += len(data)
            if offset != size:
                raise ProtocolError("source changed while uploading")
            response = self.exchange(OP_UPLOAD_COMMIT)
            if response.size != size or response.value_crc32 != checksum:
                raise ProtocolError("commit reply has the wrong length or CRC32")
        except BaseException:
            if not self.uncertain:
                try:
                    self.exchange(OP_UPLOAD_CANCEL)
                except (OSError, ProtocolError):
                    pass
            raise
        return {"size": size, "crc32": f"{checksum:08x}"}

    def download(self, remote, local, *, overwrite=False):
        path = encode_path(remote, root_allowed=False)
        local = Path(local)
        if local.exists() and not overwrite:
            raise FileExistsError(f"{local} exists; use --overwrite to replace it")
        response = self.exchange(OP_STAT, payload=path)
        if response.file_type != 1:
            raise ProtocolError("download source is not a regular file")
        size = response.size
        # Use exclusive creation to preserve previous interrupted downloads.
        temporary = local.with_name(local.name + ".part")
        offset = 0
        checksum = 0
        try:
            with temporary.open("xb") as output:
                while offset < size:
                    wanted = min(MAX_PAYLOAD, size - offset)
                    response = self.exchange(OP_READ, payload=path, offset=offset, size=wanted)
                    if (response.offset != offset or response.size != size or
                            len(response.payload) != wanted):
                        raise ProtocolError("download size or offset changed")
                    output.write(response.payload)
                    checksum = zlib.crc32(response.payload, checksum)
                    offset += len(response.payload)
                output.flush()
                os.fsync(output.fileno())
            if overwrite:
                os.replace(temporary, local)
            else:
                # Creating a hard link fails atomically if the destination
                # appeared while downloading, unlike an exists/replace pair.
                # Both names live in the same directory and filesystem.
                os.link(temporary, local)
                temporary.unlink()
        except BaseException:
            # Preserve .part as evidence/recoverable data. A later run will
            # require the user to rename or remove it, never silently truncate.
            raise
        return {"size": size, "crc32": f"{checksum:08x}"}

    def close(self):
        if self.started and not self.uncertain:
            self.exchange(OP_EXIT)


def _text_command(port, command, expected, *, timeout=5.0,
                  retry_interval=None, clock=time.monotonic):
    packet = command.encode("ascii") + b"\n"
    if port.write(packet) != len(packet):
        raise ProtocolError("partial console command write")
    deadline = clock() + timeout
    retry_at = clock() + retry_interval if retry_interval else deadline
    received = bytearray()
    while clock() < deadline:
        if retry_interval and clock() >= retry_at:
            if port.write(packet) != len(packet):
                raise ProtocolError("partial console command write")
            retry_at = clock() + retry_interval
        received.extend(port.read(256))
        if expected in received:
            return
        if len(received) > 1024:
            del received[:-1024]
    raise ProtocolError(f"console did not reply to {command!r}; wait for any old session to expire")


def enter_transfer(port):
    # A leading newline terminates any unfinished console input. The tool does
    # not toggle DTR/RTS or reset the device to establish a session.
    if port.write(b"\n") != 1:
        raise ProtocolError("partial console delimiter write")
    # EXIT retains a binary final reply briefly for ACK-loss replay. Repeating
    # the idempotent ON command allows immediate subsequent tool invocations
    # to cross that grace period without resetting the device.
    _text_command(port, "CONSOLE ON", b"OK CONSOLE ON\r\n",
                  timeout=10.0, retry_interval=1.0)
    _text_command(port, "FS TRANSFER", b"OK FS TRANSFER\r\n")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="USART2 serial port, e.g. COM5")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--operation-timeout", type=float, default=300.0,
                        help="absolute per-operation deadline in seconds (default: 300)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    listing = commands.add_parser("list")
    listing.add_argument("path", nargs="?", default="/")
    for name in ("mkdir", "remove", "stat"):
        child = commands.add_parser(name)
        child.add_argument("path")
    rename = commands.add_parser("rename")
    rename.add_argument("source")
    rename.add_argument("destination")
    for name in ("format", "selftest"):
        child = commands.add_parser(name)
        child.add_argument("--confirm", action="store_true", required=True,
                           help="explicitly authorize the destructive command")
    upload = commands.add_parser("upload")
    upload.add_argument("local", type=Path)
    upload.add_argument("remote")
    download = commands.add_parser("download")
    download.add_argument("remote")
    download.add_argument("local", type=Path)
    download.add_argument("--overwrite", action="store_true")
    args = parser.parse_args(argv)
    if args.baud <= 0 or args.operation_timeout <= 0:
        parser.error("baud and operation timeout must be positive")
    return args


def run_command(client, args):
    if args.command == "status":
        return client.status()
    if args.command == "list":
        return list(client.list(args.path))
    if args.command == "upload":
        return client.upload(args.local, args.remote)
    if args.command == "download":
        return client.download(args.remote, args.local, overwrite=args.overwrite)
    if args.command == "rename":
        payload = (encode_path(args.source, root_allowed=False) + b"\x00" +
                   encode_path(args.destination, root_allowed=False))
        client.exchange(OP_RENAME, payload=payload)
        return {"result": "ok"}
    if args.command in ("format", "selftest"):
        client.exchange(OP_FORMAT if args.command == "format" else OP_SELFTEST,
                        payload=b"CONFIRM")
        return {"result": "ok"}
    operations = {"mkdir": OP_MKDIR, "remove": OP_REMOVE, "stat": OP_STAT}
    response = client.exchange(operations[args.command],
                               payload=encode_path(args.path, root_allowed=args.command == "stat"))
    return {"result": "ok", "size": response.size, "type": response.file_type}


def main(argv=None):
    args = parse_args(argv)
    try:
        import serial
    except ImportError:
        print("Live serial commands require pyserial: python -m pip install pyserial", file=sys.stderr)
        return 2
    client = None
    try:
        # Suppress modem-control resets where supported; setting attributes
        # before open avoids the initial automatic asserted DTR/RTS states.
        port = serial.Serial(port=None, baudrate=args.baud, timeout=0.1, write_timeout=5)
        port.dtr = False
        port.rts = False
        port.port = args.port
        with port:
            enter_transfer(port)
            client = Client(port, operation_timeout=args.operation_timeout)
            client.hello()
            try:
                result = run_command(client, args)
            finally:
                if client.started and not client.uncertain:
                    try:
                        client.close()
                    except (OSError, ProtocolError) as error:
                        print(f"Session cleanup: {error}", file=sys.stderr)
            print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError, ProtocolError, serial.SerialException) as error:
        print(str(error), file=sys.stderr)
        if client is not None and client.uncertain:
            print("Do not blindly repeat a mutation. Wait for device cleanup, then reconnect and inspect the file.",
                  file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted; any unfinished operation may still be running on the device.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
