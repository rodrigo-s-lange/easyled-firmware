"""Run the MQTT session-integration validation target in QEMU, never opening a serial port."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, help="Path to qemu-system-xtensa")
    parser.add_argument("--build", type=Path,
                        default=Path(__file__).resolve().parents[4] / "build" / "network-validation")
    parser.add_argument("--timeout", type=int, default=45,
                        help="Maximum QEMU runtime in seconds (default: 45)")
    args = parser.parse_args()
    build = args.build.resolve()
    manifest = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    flash = build / "qemu-flash.bin"
    merge = [sys.executable, "-m", "esptool", "--chip", "esp32", "merge-bin",
             "--output", str(flash), "--pad-to-size", "4MB"]
    for offset, file in manifest["flash_files"].items():
        merge.extend([offset, str(build / file)])
    subprocess.run(merge, check=True)
    command = [args.qemu, "-nographic", "-machine", "esp32", "-nic", "none",
               "-drive", f"file={flash.as_posix()},if=mtd,format=raw", "-no-reboot"]
    try:
        result = subprocess.run(command, capture_output=True, timeout=args.timeout)
        output = result.stdout + result.stderr
        if result.returncode != 0:
            print(f"QEMU exited with code {result.returncode}; check runtime dependencies.", file=sys.stderr)
    except subprocess.TimeoutExpired as error:
        # subprocess.run kills and waits for QEMU on timeout; no background VM remains.
        output = (error.stdout or b"") + (error.stderr or b"")
    transcript = output.decode("utf-8", errors="replace")
    (build / "qemu-validation.log").write_text(transcript, encoding="utf-8")
    print(transcript[-2000:])
    success = re.search(r"MQTT_SESSION_VALIDATION checks=\d+ failures=0 RESULT=ALL_PASS",
                        transcript)
    fault = re.search(
        r"Guru Meditation|assert failed|FAILED check|RESULT=FAIL|task_wdt: Task watchdog",
        transcript,
    )
    return 0 if success and not fault else 1


if __name__ == "__main__":
    raise SystemExit(main())
