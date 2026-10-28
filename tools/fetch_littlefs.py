"""Fetch the pinned, unmodified upstream sources; never run downloaded code."""
import hashlib
import json
from pathlib import Path
from urllib.request import urlopen

TAG = "v2.9.3"
FILES = ("lfs.c", "lfs.h", "lfs_util.c", "lfs_util.h", "LICENSE.md")
BASE = f"https://raw.githubusercontent.com/littlefs-project/littlefs/{TAG}/"


def main():
    destination = Path(__file__).resolve().parents[1] / "middleware" / "littlefs"
    downloaded = {}
    for name in FILES:
        with urlopen(BASE + name, timeout=30) as response:
            downloaded[name] = response.read(2 * 1024 * 1024)
        if not downloaded[name] or b"<html" in downloaded[name][:200].lower():
            raise ValueError(f"Invalid source response: {name}")
    if b"0x00020009" not in downloaded["lfs.h"]:
        raise ValueError("Unexpected littlefs API version")
    destination.mkdir(parents=True, exist_ok=True)
    for name, content in downloaded.items():
        (destination / name).write_bytes(content)
    manifest = {"repository": "https://github.com/littlefs-project/littlefs",
                "tag": TAG,
                "sha256": {name: hashlib.sha256(data).hexdigest()
                           for name, data in downloaded.items()}}
    (destination / "upstream.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Fetched {TAG}: {len(downloaded)} unmodified files")


if __name__ == "__main__":
    main()
