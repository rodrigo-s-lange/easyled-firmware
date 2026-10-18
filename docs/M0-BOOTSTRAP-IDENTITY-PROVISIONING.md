# M0 — bootstrap identity and secret provisioning

Status: implemented and validated against QEMU/fakes; the live hardware
provisioning step itself has not yet been run against a real board.
Date: 2026-10-18.

## What this closes

Before this change, `ApplicationConfig` supplied `mqtt_hardware_id` and
`mqtt_bootstrap_token` through an optional, gitignored
`main/network_credentials.hpp`. No such file exists in the repository, so
provisioned mode had no real `hardware_id`/secret source at all. This
replaces that gap with:

- `components/pixels_bootstrap_identity` — `hardware_id` derived from this
  device's own eFuse MAC, and a durable, versioned, per-device NVS record for
  the 32-byte bootstrap secret;
- `tools/provision_bootstrap_identity.py` — the bench/factory tool that
  writes that record, offline, without the operational firmware's
  involvement;
- `main.cpp` wiring so provisioned boot reads both from this store instead of
  from `ApplicationConfig`, which no longer carries either field at all.

The already-integrated MQTT session/credential runtime (#47-B,
`docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md`) is unchanged by this work; it simply
now receives a real `hardware_id` and bootstrap token instead of none.

## 1. `hardware_id`: origin and this unit's value

`hardware_id` is the ESP32 base eFuse MAC, rendered as exactly 12 lowercase
hexadecimal characters with no separators
(`pixels_bootstrap_identity::macToHardwareId()`,
`pixels_bootstrap_identity::currentHardwareId()` wrapping
`esp_efuse_mac_get_default()`). It is never compiled as a constant, never
derived from `hardware_binding_id` (the rebindable UUID), and never supplied
by `ApplicationConfig`.

The confirmed M0 physical unit's eFuse MAC is `14:2b:2f:dc:fa:5c`, so its
`hardware_id` is:

```text
142b2fdcfa5c
```

This value is used throughout this document's examples and the focused
validation target (`validation_bootid/`). It is not a secret; publishing it
here and to the backend is expected (`docs/M0-BACKEND-FIRMWARE-INPUTS.md`).

## 2. The bootstrap secret: NVS binary and HTTP text representations

The backend's closed contract (confirmed 2026-10-18):

- the operational, over-the-wire representation is unpadded base64url text,
  exactly 43 ASCII characters;
- the NVS-durable representation is the 32 raw decoded bytes, never the text;
- the `Authorization` header is `Bearer <43-character text>`;
- the backend computes SHA-256 over the 43 transmitted ASCII bytes, never
  over the 32 binary bytes;
- no padding (`=`), whitespace, newline or NUL byte is ever transmitted;
- the text must be the single canonical encoding of its own decoded
  bytes — decoding then re-encoding must reproduce it exactly, or the
  value is rejected, never silently normalized.

`pixels_bootstrap_identity::encodeSecretText()`/`decodeSecretText()`
implement exactly this contract and are proven against the closed public
vector below (Section 6); `tools/provision_bootstrap_identity.py` implements
the identical contract independently in Python (`--self-test` proves the two
have not drifted apart) so the record it writes is byte-for-byte what the
firmware expects — confirmed directly: the tool's generated image, parsed
with the official `nvs_tool.py`, decodes to the exact 57-byte record
`validation_bootid/`'s own cross-tool test embeds and loads successfully.

## 3. Durable record: namespace, key, schema, partition, size

| | |
|---|---|
| Partition | `easyled` (shared with the #47-B MQTT credential A/B store, different namespace) |
| Partition offset/size | `0x190000` / `0x20000` (128 KiB) — `partitions_network.csv` |
| NVS namespace | `easyled_boot` |
| NVS key | `identity` (one `blob` entry) |
| Schema version | `1` |
| Record size | 57 bytes, fixed |

Record layout (`#pragma pack(push, 1)`, matched exactly by the Python tool):

```text
offset  size  field
0       4     magic (0x42534944, little-endian)
4       1     schema (1)
5       3     reserved (zero)
8       13    hardware_id (12 lowercase hex chars + NUL)
21      32    secret (raw bytes)
53      4     crc32 (little-endian, over bytes 0..52)
```

`BootstrapIdentityStore::init(hardware_id)` loads and validates this record;
any empty/truncated/corrupt/wrong-schema/eFuse-mismatched record is reported
as `SecretState::Faulted`, never silently treated as `Empty`, and the device
never falls back to any other credential source. `provision()` (the
bench-tool-only write path) refuses to run from anything but `Empty` — the
record is written exactly once by this round's tooling; re-provisioning the
same physical unit is always onto a freshly erased, `Empty` store (Section 5).

## 4. Provisioning procedure (placeholder values only)

Prerequisite: the target's `easyled` partition must be known-fresh (factory
image, or just erased) before running this. The tool refuses to overwrite an
identity it can detect; see `--existing-dump` below for how to let it detect
one at all.

```powershell
# One-time: confirm the firmware's own self-test matches the tool's.
python tools/provision_bootstrap_identity.py --self-test

# Recommended: dump the target's existing `easyled` region first (non-destructive)
# so the tool can refuse a real overwrite instead of trusting an operator claim.
python -m esptool --port COMx read-flash 0x190000 0x20000 easyled-before.bin

python tools/provision_bootstrap_identity.py `
    --hardware-id 142b2fdcfa5c `
    --output identity-142b2fdcfa5c.bin `
    --existing-dump easyled-before.bin
# Prompts for the secret text with input hidden (no echo). --secret-env
# NAME or --secret TEXT are also accepted -- see --help for the tradeoffs
# of each (the latter is visible in shell history/process list).

# Printed only: hardware_id, destination path, schema, "OK" -- never the secret.

python -m esptool --port COMx write-flash 0x190000 identity-142b2fdcfa5c.bin
```

Without a usable `--existing-dump`, the tool requires an explicit
`--confirm-fresh-partition` flag instead — a deliberate acknowledgment that
the operator, not the tool, is vouching for the partition being blank.

The tool never contacts a network, never flashes hardware itself (printing
the exact `esptool` command instead), and is not reachable through any MQTT
command or other operational-firmware API — provisioning happens entirely
offline, over the physical bench/factory connection.

## 5. Boot sequence

In provisioned mode (`mqtt_provisioning_enabled == true`) only:

1. read `hardware_id` from eFuse (`currentHardwareId()`);
2. open `BootstrapIdentityStore` against that `hardware_id`;
3. if `state() != Valid` (no record, or any validation failure): log a
   diagnostic that names `hardware_id` but never the secret, and treat MQTT
   provisioning as `Incomplete` — provisioned networking stays off; durable
   pixel state and local rendering are unaffected (AGENTS.md);
4. if `Valid`: read the 43-character `secretText()` once, at boot, into a
   stable-lifetime buffer, and pass both it and `hardware_id` to
   `MqttCredentialCoordinator::Config` exactly where `ApplicationConfig`'s two
   removed fields used to feed it. Nothing downstream of that handoff
   changed — the already-validated #47-B session/credential runtime continues
   unmodified.

Laboratory mode (`mqtt_provisioning_enabled == false`) never touches this
store at all and is behaviorally identical to before this change.

## 6. Closed public test vector

Published as a test vector only — never an operational secret or firmware
default:

| | |
|---|---|
| 32 bytes | `00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f` |
| 43-char text | `AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8` |
| SHA-256 of the 43 ASCII bytes | `ea866a757e4c38babfa8127cbe9a409d3e1f93a00ff1488ff735fcf917afffd0` |
| Header | `Authorization: Bearer AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8` |

Independently re-verified against Python's own `hashlib`/`base64` during this
round (not just transcribed): the SHA-256 matches, the decode/re-encode round
trip reproduces the text exactly, and `tools/provision_bootstrap_identity.py
--self-test` checks the same three facts on every run.

## 7. Reflash / OTA / erase / power-loss matrix

| Event | Effect on the bootstrap secret |
|---|---|
| Reflash bootloader/partition-table/app binaries only | Preserved — none of those images cover the `easyled` partition region |
| Application OTA update | Preserved — OTA never touches the `easyled` NVS partition |
| `esptool erase-flash`, an explicit erase of the `easyled` partition, or writing an incompatible partition table | Lost — `BootstrapIdentityStore::init()` loads `Empty`; the unit needs physical re-provisioning |
| Re-provisioning the **same** physical unit after such an erase | Allowed, from a custodied copy of the same secret: `provision()` only ever refuses when `state() != Empty`, so a freshly erased (hence `Empty`) store accepts a new `provision()` call for that same `hardware_id` without any new backend claim being required |
| Attempting to provision a **different** identity (swap) onto a unit that still holds a valid record | Refused — out of scope this round; `provision()` requires `Empty`, and the CLI's `--existing-dump` check independently refuses before ever generating an image |
| Power loss during `provision()` itself | The store is left exactly as it was before the call (write + commit + verified readback, same durability model as the #47-B credential store); a record is never partially visible. Power loss **during image generation on the host** leaves no partition touched at all — nothing has been flashed yet |
| Backup (`--existing-dump`'s read-back image) reused to "restore" a secret onto a **different** physical unit | Not a supported flow and not what this mechanism is for: the firmware's own `init()` cross-checks the record's `hardware_id` against the live eFuse value and reports `Faulted` for any mismatch, so a cloned image never silently activates on the wrong unit |

## 8. Automated evidence

- `validation_bootid/` (new, focused target; frozen `validation/` untouched):
  **158/158 checks, `ALL_PASS`**, confirmed by `run_qemu.py` exit code 0.
  Covers: `macToHardwareId()` against synthetic MAC bytes including this
  unit's own; the full closed vector (bytes/text/SHA-256/header, including
  proof a NUL never enters the hash or header); `decodeSecretText()`
  rejecting wrong length, padding, whitespace, newline, invalid alphabet and
  non-canonical encoding; an empty store; provision/round-trip across a
  simulated reboot; refusal to overwrite; four-stage fault injection;
  corruption and truncation; eFuse incompatibility; and the cross-tool
  compatibility proof in Section 2.
- `tools/provision_bootstrap_identity.py --self-test`: passes, independent of
  any QEMU build.
- Product build: clean. `validation/` (frozen): **468/468**.
  `validation_mqtt_credentials/`: **298/298**. `validation_mqtt_session/`:
  **98/98**. All three unchanged from `docs/CONNECTIVITY-ARCHITECTURE.md`'s
  recorded gates — no regression.
- Not automated, and not possible without the physical unit: flashing this
  tool's output to the real M0 board, confirming `esp_efuse_mac_get_default()`
  really returns `14:2b:2f:dc:fa:5c` on that hardware, and a live
  provisioned-mode boot reaching a real backend with this secret. The board's
  serial port was not available during this round (see
  `docs/M0-BACKEND-FIRMWARE-INPUTS.md`).

## 9. Still pending from the platform

Unchanged from `docs/M0-BACKEND-FIRMWARE-INPUTS.md`: the real `trust_ref`
value(s) and CA mapping for the MQTT WSS environment, and the deployed
bootstrap base URL. Neither blocks the work in this document; both still
block a live end-to-end run. Backend-side storage of this secret's SHA-256
digest (`device_bootstrap_identities.bootstrap_secret_hash`) and the
manufacturing-row seeding for this unit's `hardware_id` remain backend/process
work, not firmware work.
