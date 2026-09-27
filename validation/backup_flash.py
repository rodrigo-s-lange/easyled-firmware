#!/usr/bin/env python3
"""Read an ESP32 flash in verified chunks and assemble one backup image."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--flash-size", type=lambda value: int(value, 0), default=0x400000)
    parser.add_argument("--chunk-size", type=lambda value: int(value, 0), default=0x40000)
    args = parser.parse_args()

    if args.flash_size <= 0 or args.chunk_size <= 0 or args.flash_size % args.chunk_size:
        parser.error("flash size must be a positive multiple of chunk size")
    if args.output.exists():
        parser.error(f"refusing to overwrite {args.output}")

    chunks = args.output.with_suffix(args.output.suffix + ".chunks")
    chunks.mkdir(parents=True, exist_ok=False)
    selected: list[Path] = []
    for index, offset in enumerate(range(0, args.flash_size, args.chunk_size)):
        valid = None
        for attempt, baud in enumerate((460800, 115200)):
            candidate = chunks / f"chunk-{index:02d}-attempt-{attempt}.bin"
            command = [
                sys.executable, "-m", "esptool", "--chip", "esp32",
                "--port", args.port, "--baud", str(baud), "read-flash",
                str(offset), str(args.chunk_size), str(candidate),
            ]
            result = subprocess.run(command, capture_output=True, text=True)
            (chunks / f"chunk-{index:02d}-attempt-{attempt}.log").write_text(
                result.stdout + result.stderr, encoding="utf-8"
            )
            if result.returncode == 0 and candidate.exists() and candidate.stat().st_size == args.chunk_size:
                valid = candidate
                break
        if valid is None:
            raise RuntimeError(f"failed to read chunk {index} at offset 0x{offset:x}")
        selected.append(valid)
        print(f"backup chunk {index + 1}/{args.flash_size // args.chunk_size} ok", flush=True)

    digest = hashlib.sha256()
    with args.output.open("xb") as destination:
        for chunk in selected:
            data = chunk.read_bytes()
            destination.write(data)
            digest.update(data)
    if args.output.stat().st_size != args.flash_size:
        raise RuntimeError("assembled backup has the wrong size")
    print(f"sha256={digest.hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
