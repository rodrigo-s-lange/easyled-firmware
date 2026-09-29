# Pixel firmware network session

Application-layer Wi-Fi + MQTT over secure WebSocket (WSS), compiled together
with the pixel transport, effects, Composer and two-output adapter. This is a
buildable integration boundary, **not the completed EasyLED device firmware**.
It does not yet download or activate remote configuration. No EasyLED repository
files are dependencies.

`components/pixels_network` owns the session and bounded message queue. It reuses
the existing `esp32libfun_wifi_sta` HAL for initialization and connection state;
the application supplies retry policy using the ESP-IDF Wi-Fi API. No framework
core module was added or changed. ESP-IDF **6.0.x** is required; ESP-MQTT is pinned
to **1.1.0** through its component manifest.

## Behavior

- Wi-Fi retries with delays increasing from 1 to 30 seconds, after allowing ten
  seconds for the initial association. MQTT transport retries every three seconds.
- WSS validates the server certificate and hostname using the ESP-IDF certificate
  bundle, or an explicitly supplied PEM CA. MQTT starts only after the application
  has supplied a plausible clock; the example starts SNTP after obtaining an IP.
- Every MQTT connection subscribes again to one exact command topic. The session
  becomes ready after the broker grants QoS1. Rejected/downgraded subscriptions
  request another connection attempt.
- Incoming fragments are assembled before delivery. Payloads are limited to
  1024 bytes, with a queue for two complete messages. Malformed/oversized messages,
  retained deliveries and other topics are rejected. A full queue drops the new
  message and increments `queue_overflows`; this is not durable command delivery.
- Outgoing messages use QoS1 enqueue with a 4096-byte SDK outbox limit. A negative
  return means the publication was not accepted. A message ID confirms enqueue,
  not broker acknowledgement or successful command execution.
- MQTT callbacks never execute product actions. The application receives complete
  bytes, including their length, and owns parsing, validation and deduplication.
  MQTT QoS1 may deliver duplicates; application acknowledgements and retry policy
  are still required. Retain rejection is not an age/expiry check for commands.

Lifecycle, `poll()`, `publish()` and `receive()` belong to one application task,
separate from rendering. Keep the session and configuration strings alive through
`end()`, and call `poll()` at least every 250 ms. This example owns the shared Wi-Fi
connection exclusively; do not run another Wi-Fi lifecycle owner alongside it.
Stopping the session clears queued incoming messages, not the SDK's pending QoS1
outbox. Use `end()` before changing device identity or credentials.

## Provisional firmware contract boundary

`components/pixels_firmware_contract` implements the offline portion of
`FW-CONTRACT-ALIGN-2026-09-22` without assuming a broker, credentials or storage
layout:

- bounded parsing of the closed command envelope for `apply_config`,
  `activate_preset`, `reboot` and `ota`; unknown JSON fields are accepted, while
  consumed duplicate fields and invalid types/ranges are rejected;
- exact-byte SHA-256 verification using the ESP-IDF PSA crypto API;
- compact serialization of `command_received`, `command_accepted`,
  `command_rejected`, `command_applied` and `command_failed`;
- `TerminalJournal`, an application-owned durable interface that permits a
  duplicate `command_id` to replay its original terminal event without executing
  the command again;
- `DurableStaging`, an application-owned transaction interface that separates
  append, checksum/commit, frame-boundary activation and rollback. Its contract
  permits `command_applied` only after recoverable commit and successful atomic
  activation.

The component deliberately does not select NVS keys, journal capacity, cache
eviction or a partition layout.

## Offline command executor

`components/pixels_payload_adapter` turns exact, checksum-verified
`apply_config`/`activate_preset` payload bytes into a validated pixel
candidate. It reuses `esp_pixels_fx::describe()` as the one effect catalog and
`esp_pixels_composer::AtScene`/`SegmentSpec` as the one scene representation --
never a second metadata or scene system. Supported subset: 1D segments only,
`ledmap:null`/absent (a preset may omit `ledmap` entirely; a config payload
must carry it), an empty `inputs`, one or two `digital_out` WS2812B outputs with
`reversed:false`, output-capable non-reserved ESP32 GPIOs (unique across slots),
strict `#RRGGBB` segment colors, `replace`/`add`/`alpha`
blend names, and segments addressed by `segment_index` (0..31, mapped to
Composer id `segment_index + 1`). Config segments must fit inside the sum of
the configured output pixel counts. A structurally valid but not-yet-implemented
field (`is_2d:true`, a non-null `ledmap`, a non-empty `inputs`, a physical
output `reversed:true`, a present segment `mirror`/`reversed`/`custom`/
`color_source` field regardless of value, an unregistered/dimension-mismatched
`effect_id`, or a preset payload's own non-empty `outputs`) is rejected as
`SchemaError::Unsupported`, never silently ignored. Unknown JSON members
anywhere are accepted for forward compatibility. Limits: 8192 bytes per
payload, one or two outputs, 32 segments, 2048 pixels per output.

`components/pixels_command_executor` is the synchronous, application-layer
orchestrator: one `execute()` call turns one command message into a terminal
outcome, independent of MQTT/HTTP/clock/NVS-policy specifics through four
narrow injected interfaces (`PayloadSource`, `EventSink`, `TimestampSource`,
`ActiveTargetProbe`). Step order: bounded `command_id` extraction; a duplicate
replays its exact stored terminal event and stops (no re-parse, re-resolve or
re-activation); full envelope parse (`command_received`); resolve bytes,
checksum-verify, and schema-validate -- all before touching durable/live state.
For `apply_config`, the envelope checksum is authoritative and cannot be
replaced by resolver metadata; for `activate_preset`, the resolver supplies the
digest because the provisional envelope has none. `reboot`/`ota` stop here as
`command_rejected`/`not_implemented`, parsed but never executed. An
`activate_preset` already matching the live target emits `command_accepted`
and a successful terminal result without staging or restarting the animation.
Other accepted pixel commands continue through `staging.begin()` -> `append()`
-> `verifyAndCommit()` -> `activateAtFrameBoundary()`, then durably store and
publish `command_applied`. A
checksum/schema/unsupported failure before staging is `command_rejected` and
does not roll back; a resolver transport/cache failure is `command_failed`.
A failure at staging, activation, or storing the terminal record after a
successful activation is `command_failed`,
followed by `rollback()`; a rollback error is exposed as `rollback_failed`
rather than hidden. "Applied but not saved" is forbidden.
Reason codes beyond `pixels_firmware::parseErrorCode()`/
`pixels_payload::schemaErrorCode()`: `checksum_mismatch`, `resolve_failed`,
`staging_failed`, `activation_failed`, `terminal_store_failed`,
`rollback_failed`, `not_implemented`. `Executor` owns its fixed-capacity
scratch (command, payload buffer, candidate, event buffers) as class members,
never task-stack
locals or function-local statics, so it is safe to place on a small FreeRTOS
task and never aliases across a second instance.

Not implemented by this delivery: real HTTPS retrieval, MQTT topic policy,
capability publication, NVS-backed `TerminalJournal`/`DurableStaging`, OTA
execution, and reconfiguring a `PixelFrameSink`'s physical outputs at runtime
(`Composer::build()` runs once per boot in this cut; a config payload's own
`outputs[]` are validated but not yet re-applied to a live sink).

## Build and local profile

From an ESP-IDF 6.0.2 shell in this directory:

```powershell
idf.py -D IDF_TARGET=esp32 build
```

Without `main/network_credentials.hpp`, startup reports that networking is
disabled and returns. An optional, gitignored profile has this shape:

```cpp
#pragma once
#include "network_session.hpp"
static pixels_network::NetworkSession::Config networkConfig()
{
    return {
        .ssid = "YOUR_WIFI",
        .wifi_password = "YOUR_WIFI_PASSWORD",
        .broker_uri = "wss://broker.example.invalid/mqtt",
        .client_id = "YOUR_DEVICE_ID",
        .username = "YOUR_DEVICE_USERNAME",
        .password = "YOUR_DEVICE_PASSWORD",
        .command_topic = "YOUR_EXACT_COMMAND_TOPIC",
    };
}
```

These are placeholders, not an EasyLED provisioning contract. Supply credentials
only for an authorized development device. The example does not print payloads or
credentials. It does not erase NVS automatically on initialization failure.

The example's large-app partition table differs from the preset application's
layout. Back up the complete flash before a bench run and restore that image
afterward; flashing only the previous application binary is insufficient to
restore its partition table and NVS data.

For USB links that fail during a single 4 MiB read, `validation/backup_flash.py`
reads 256 KiB chunks, retries each failed chunk at a lower baud rate, validates
every chunk length and assembles one SHA-256-labelled image. It refuses to
overwrite an existing output:

```powershell
python validation/backup_flash.py --port COM6 --output build/flash-before.bin
```

## Focused local validation

`validation/` runs the fragment assembler, firmware-contract component, the
payload adapter, the command executor and a real Composer render in an ESP32
QEMU target with no network adapter. The seven fixture strings the tests use
(two payloads, two command envelopes, three events) live as inspectable files
under `validation/fixtures/*.provisional.json`, embedded into the binary via
`EMBED_TXTFILES` rather than duplicated as string literals. It checks the two
exact provisional fixture hashes, command parsing, event serialization,
terminal replay, the payload adapter's happy paths and a representative
type/range/duplicate/unsupported failure table, and the executor's full
scenario table (happy-path apply_config, duplicate replay, preset cache-miss/
already-active no-op, checksum-mismatch rejection, staging/activation-failure
rollback, terminal-store-failure rollback, terminal-publish-failure survival,
reboot/ota rejection, and 20 repeated commands with no leak) through a real
`Composer`/`FakeSink` pair, proving a fixture's own bytes reach rendered
pixels. It does **not** validate radio, TLS, broker permissions, storage
durability or HTTPS.

```powershell
cd validation
idf.py -B ../../../../build/network-contract-validation -D IDF_TARGET=esp32 build
python run_qemu.py --qemu "PATH_TO/qemu-system-xtensa.exe"
```

The script merges only a local emulated flash file, captures the result in
`build/network-contract-validation/qemu-validation.log`, and terminates QEMU after
15 seconds. The expected marker is
`NETWORK_CONTRACT_VALIDATION checks=153 failures=0 RESULT=ALL_PASS`.

On Windows, this QEMU distribution also needs `libiconv-2.dll` on `PATH`. In the
local environment, prepend `C:\Program Files\Git\mingw64\bin` to the current
shell's `PATH` before invoking the script (no machine-wide configuration change).

The validation target uses the ESP-IDF 4096 B main-task stack. Large candidates,
fixture strings, journal records and fake storage remain in owning/static
storage; QEMU runs the repeated-command loop without stack overflow or panic.

Build verified with ESP-IDF 6.0.2: combined image 971880 B, static DRAM 43207 B
(23.91%) and IRAM 87891 B (67.06%). The 1.5 MiB app partition retains 564000 B
(37%) after the build. These figures exclude runtime Wi-Fi/TLS heap use.
The focused image is 261725 B and ran on Espressif QEMU 9.2.2: all 153 checks
passed, with no assertion or panic. Its own static DRAM is 148284 B (82.04%,
dominated by `.bss` -- the test harness's fixed-capacity fakes, including a
second `Composer`/`FakeSink` pair and several `ComposerBackedDurableStaging`/
event-recording instances, are deliberately `static`, not stack-resident) and
IRAM 44899 B (34.26%). No broker was accessed.

## Next integration step

Wire `pixels_command_executor` into the combined application's own MQTT
callback/control task, backed by concrete (not `static`-test-fake) `PayloadSource`
(HTTPS), `EventSink` (MQTT publish), `TimestampSource` (SNTP-derived) and
`ActiveTargetProbe` implementations, plus NVS-backed `TerminalJournal` and
`DurableStaging`. Reconfiguring a `PixelFrameSink`'s physical outputs at
runtime from a config payload's own `outputs[]` is still open. HTTPS
retrieval, state publication, provisioning and broker-loss tests remain
dependent on the EasyLED delivery flows. Runtime heap, stack and render
continuity must be measured with the combined firmware and an authorized
development broker before product release.

Official references: [ESP-IDF 6.0.2 MQTT component migration](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/protocols/mqtt.html),
[ESP-MQTT 1.1.0](https://components.espressif.com/components/espressif/mqtt/versions/1.1.0/readme),
[cJSON 1.7.19](https://components.espressif.com/components/espressif/cjson/versions/1.7.19/readme).
