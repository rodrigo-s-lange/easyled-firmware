#!/usr/bin/env python3
"""Bench/factory provisioning tool for the M0 bootstrap identity record.

Generates a standalone NVS partition image (via the official ESP-IDF
nvs_partition_gen.py) holding exactly one versioned record -- this unit's
hardware_id plus its 32-byte bootstrap secret -- in the layout
components/pixels_bootstrap_identity/pixels_bootstrap_identity.cpp reads at
boot. See docs/M0-BOOTSTRAP-IDENTITY-PROVISIONING.md for the full procedure,
the record layout, and the reflash/erase/power-loss matrix.

This tool never contacts a network, never flashes hardware itself, and never
prints the secret. It is not the operational firmware's command surface --
provisioning a secret this way, instead of through any MQTT command or
runtime API, is deliberate (AGENTS.md: never commit/log credentials; this
task: never use the operational firmware to set the secret).
"""
import argparse
import base64
import getpass
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HARDWARE_ID_BYTES = 12
SECRET_BYTES = 32
SECRET_TEXT_BYTES = 43
RECORD_MAGIC = 0x42534944  # "BSID"-ish; must match pixels_bootstrap_identity.cpp exactly
SCHEMA = 1
NAMESPACE = "easyled_boot"
RECORD_KEY = "identity"
EASYLED_PARTITION_OFFSET = 0x190000  # partitions_network.csv
EASYLED_PARTITION_SIZE = 0x20000  # partitions_network.csv

HARDWARE_ID_RE = re.compile(r"^[0-9a-f]{12}$")
BASE64URL_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")

# The same closed public vector the firmware's own validation target proves
# against -- run with --self-test to confirm this script's encode/decode/CRC
# logic has not drifted from the C++ implementation it must stay in sync with.
_SELF_TEST_BYTES = bytes(range(32))
_SELF_TEST_TEXT = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"
_SELF_TEST_SHA256 = "ea866a757e4c38babfa8127cbe9a409d3e1f93a00ff1488ff735fcf917afffd0"


class ProvisioningError(Exception):
    pass


def decode_secret_text(text: str) -> bytes:
    """Accepts only the single canonical base64url encoding of 32 bytes:
    exactly 43 characters, no padding/whitespace, and re-encoding the
    decoded bytes must reproduce `text` exactly -- the same canonicality
    contract as pixels_bootstrap_identity::decodeSecretText()."""
    if not isinstance(text, str) or len(text) != SECRET_TEXT_BYTES:
        raise ProvisioningError(f"secret text must be exactly {SECRET_TEXT_BYTES} characters")
    if not BASE64URL_RE.match(text):
        raise ProvisioningError("secret text must be unpadded base64url (A-Z a-z 0-9 - _), no whitespace/newline")
    try:
        decoded = base64.urlsafe_b64decode(text + "=")
    except Exception as exc:  # noqa: BLE001 - surfaced as a clean provisioning error
        raise ProvisioningError(f"secret text is not valid base64url: {exc}") from exc
    if len(decoded) != SECRET_BYTES:
        raise ProvisioningError("decoded secret is not exactly 32 bytes")
    reencoded = base64.urlsafe_b64encode(decoded).decode("ascii").rstrip("=")
    if reencoded != text:
        raise ProvisioningError("secret text is not the canonical encoding of its own decoded bytes"
                                " (non-canonical trailing bits) -- rejected, never silently normalized")
    return decoded


def encode_secret_text(secret: bytes) -> str:
    if len(secret) != SECRET_BYTES:
        raise ProvisioningError("secret must be exactly 32 bytes")
    return base64.urlsafe_b64encode(secret).decode("ascii").rstrip("=")


def build_record(hardware_id: str, secret: bytes) -> bytes:
    """Exactly mirrors pixels_bootstrap_identity.cpp's WireRecord: magic(4) +
    schema(1) + reserved(3) + hardware_id(13, NUL-padded) + secret(32) +
    crc32(4) over everything before it, all little-endian (ESP32 native)."""
    if not HARDWARE_ID_RE.match(hardware_id):
        raise ProvisioningError("hardware_id must be exactly 12 lowercase hex characters")
    if len(secret) != SECRET_BYTES:
        raise ProvisioningError("secret must be exactly 32 bytes")
    hardware_id_field = hardware_id.encode("ascii") + b"\x00"
    assert len(hardware_id_field) == HARDWARE_ID_BYTES + 1
    body = struct.pack("<I", RECORD_MAGIC) + bytes([SCHEMA]) + bytes(3) + hardware_id_field + secret
    assert len(body) == 4 + 1 + 3 + (HARDWARE_ID_BYTES + 1) + SECRET_BYTES  # == 53
    crc = zlib.crc32(body) & 0xFFFFFFFF
    record = body + struct.pack("<I", crc)
    assert len(record) == 57
    return record


def self_test() -> None:
    decoded = decode_secret_text(_SELF_TEST_TEXT)
    if decoded != _SELF_TEST_BYTES:
        raise ProvisioningError("self-test: decode mismatch against the closed public vector")
    if encode_secret_text(_SELF_TEST_BYTES) != _SELF_TEST_TEXT:
        raise ProvisioningError("self-test: encode mismatch against the closed public vector")
    import hashlib
    digest = hashlib.sha256(_SELF_TEST_TEXT.encode("ascii")).hexdigest()
    if digest != _SELF_TEST_SHA256:
        raise ProvisioningError("self-test: SHA-256-over-text mismatch against the closed public vector")
    if dump_has_written_data([]):
        raise ProvisioningError("self-test: an empty NVS dump was classified as containing data")
    synthetic_other_namespace = [{
        "namespace": "pixels_state", "key": "active", "state": "Written", "is_empty": False,
    }]
    if not dump_has_written_data(synthetic_other_namespace):
        raise ProvisioningError("self-test: existing data in another namespace was not detected")
    synthetic_identity = [{
        "namespace": NAMESPACE, "key": RECORD_KEY, "state": "Written", "is_empty": False,
    }]
    if not dump_has_written_data(synthetic_identity):
        raise ProvisioningError("self-test: an existing bootstrap identity was not detected")
    print("self-test OK: encode/decode/SHA-256 and shared-partition guards match the closed contract")


def dump_has_written_data(parsed: object) -> bool:
    """Return True for any written value in any namespace.

    The generated image is a complete replacement for the shared `easyled`
    partition. It is therefore safe only when the source partition contains
    no application data at all, not merely when easyled_boot/identity is
    absent.
    """
    if not isinstance(parsed, list):
        raise ProvisioningError(f"unexpected nvs_tool.py JSON shape (not a list): {type(parsed).__name__}")
    return any(isinstance(entry, dict) and entry.get("state") == "Written" and
               not entry.get("is_empty", False) for entry in parsed)


def existing_partition_has_data(dump_path: Path) -> bool:
    """Inspect a full `easyled` dump and reject any written application data."""
    if not dump_path.is_file():
        raise ProvisioningError(f"existing partition dump not found: {dump_path}")
    if dump_path.stat().st_size != EASYLED_PARTITION_SIZE:
        raise ProvisioningError(
            f"existing partition dump must be exactly 0x{EASYLED_PARTITION_SIZE:x} bytes"
        )
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise ProvisioningError("IDF_PATH is not set; cannot locate nvs_tool.py to check the existing dump")
    nvs_tool = Path(idf_path) / "components" / "nvs_flash" / "nvs_partition_tool" / "nvs_tool.py"
    if not nvs_tool.is_file():
        raise ProvisioningError(f"nvs_tool.py not found at {nvs_tool}")
    result = subprocess.run([sys.executable, str(nvs_tool), "-d", "minimal", "-f", "json", str(dump_path)],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise ProvisioningError(f"nvs_tool.py failed reading {dump_path}: {result.stderr.strip()}")
    try:
        parsed = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise ProvisioningError(f"could not parse nvs_tool.py output: {exc}") from exc
    # "minimal" returns written namespace/key values. Any one of them means
    # flashing a standalone replacement image would destroy existing data.
    return dump_has_written_data(parsed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hardware-id", help="This unit's eFuse hardware_id, 12 lowercase hex characters")
    parser.add_argument("--output", type=Path, help="Destination NVS image path (e.g. identity-142b2fdcfa5c.bin)")
    parser.add_argument("--secret-env", metavar="VARNAME",
                        help="Read the secret text from this environment variable instead of prompting."
                             " The variable itself is never logged by this tool; clear your shell history/"
                             " unset it afterward if your shell persists environment history.")
    parser.add_argument("--existing-dump", type=Path,
                        help="Path to a partition dump of this device's live `easyled` region (e.g. via"
                             " `esptool.py read_flash 0x190000 0x20000 dump.bin`), checked with the official"
                             " nvs_tool.py. The dump must contain no written application data because the"
                             " generated image replaces the entire shared partition.")
    parser.add_argument("--confirm-fresh-partition", action="store_true",
                        help="Required when --existing-dump is omitted: an explicit acknowledgment that you"
                             " have independently confirmed the target `easyled` partition has never held a"
                             " bootstrap identity record (e.g. a factory-fresh or just-erased unit). This tool"
                             " cannot verify that claim without --existing-dump.")
    parser.add_argument("--self-test", action="store_true",
                        help="Run only the closed-vector self-test and exit; no hardware_id/secret/output needed.")
    args = parser.parse_args()

    if args.self_test:
        try:
            self_test()
        except ProvisioningError as exc:
            print(f"self-test FAILED: {exc}", file=sys.stderr)
            return 1
        return 0

    if not args.hardware_id or not args.output:
        parser.error("--hardware-id and --output are required (or pass --self-test alone)")
    if not HARDWARE_ID_RE.match(args.hardware_id):
        print("error: --hardware-id must be exactly 12 lowercase hex characters", file=sys.stderr)
        return 1
    if args.output.exists():
        print(f"error: {args.output} already exists; choose a new path, this tool never overwrites"
              " a previously generated image", file=sys.stderr)
        return 1
    if args.existing_dump is not None:
        try:
            partition_has_data = existing_partition_has_data(args.existing_dump)
        except ProvisioningError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        if partition_has_data:
            print("error: the dumped shared `easyled` partition contains written application data;"
                  " refusing to generate a replacement image that would erase it. This factory tool"
                  " accepts only a wholly blank/erased partition.",
                  file=sys.stderr)
            return 1
    elif not args.confirm_fresh_partition:
        print("error: pass --existing-dump <dump.bin> to verify no identity is already provisioned, or"
              " --confirm-fresh-partition if you have independently confirmed the target partition is"
              " factory-fresh or was just erased", file=sys.stderr)
        return 1

    if args.secret_env:
        secret_text = os.environ.get(args.secret_env)
        if secret_text is None:
            print(f"error: environment variable {args.secret_env} is not set", file=sys.stderr)
            return 1
    else:
        secret_text = getpass.getpass("Bootstrap secret (43-character base64url, input hidden): ")

    try:
        secret_bytes = decode_secret_text(secret_text)
    except ProvisioningError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        secret_text = "\0" * len(secret_text) if secret_text else secret_text  # best-effort only; see docs

    record = build_record(args.hardware_id, secret_bytes)
    secret_bytes = bytes(len(secret_bytes))  # best-effort local clear; see docs on Python's limits here

    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_dir_path = Path(tmp_dir)
        record_path = tmp_dir_path / "identity.bin"
        record_path.write_bytes(record)
        csv_path = tmp_dir_path / "identity.csv"
        csv_path.write_text(
            "key,type,encoding,value\n"
            f"{NAMESPACE},namespace,,\n"
            f"{RECORD_KEY},file,binary,{record_path.resolve()}\n",
            encoding="ascii",
        )
        idf_path = os.environ.get("IDF_PATH")
        if not idf_path:
            print("error: IDF_PATH is not set; activate the ESP-IDF environment first", file=sys.stderr)
            return 1
        generator = Path(idf_path) / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
        result = subprocess.run(
            [sys.executable, str(generator), "generate", str(csv_path), str(args.output),
             hex(EASYLED_PARTITION_SIZE)],
            capture_output=True, text=True, check=False,
        )
        # Best-effort: overwrite the temp record file before TemporaryDirectory
        # cleanup removes it, since it briefly held the raw secret bytes.
        try:
            record_path.write_bytes(bytes(len(record)))
        except OSError:
            pass
        if result.returncode != 0:
            print("error: nvs_partition_gen.py failed:", file=sys.stderr)
            print(result.stderr, file=sys.stderr)
            return 1

    print(f"hardware_id: {args.hardware_id}")
    print(f"destination: {args.output}")
    print(f"schema: {SCHEMA}")
    print(f"namespace/key: {NAMESPACE}/{RECORD_KEY}")
    print("result: OK")
    print(f"\nFlash this image to the `easyled` partition only on a unit you have confirmed is fresh/erased there:")
    print(f"  python -m esptool --chip esp32 write-flash 0x{EASYLED_PARTITION_OFFSET:x} {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
