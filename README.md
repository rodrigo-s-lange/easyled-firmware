# EasyLED firmware

Product firmware for EasyLED devices, with Wi-Fi + MQTT over secure WebSocket
(WSS), HTTPS payload delivery, durable command execution and local pixel
rendering. This repository owns the application and product lifecycle; hardware
access and pixel behavior remain versioned dependencies. The firmware restores
and activates durable pixel configuration and
executes the two versioned starter-pack resources end to end. It also publishes
retained device state and boot/hourly capability telemetry. At boot,
`apply_config` selects either the explicit laboratory mode backed by the
embedded fixture or provisioned mode backed exclusively by authenticated
HTTPS; it never falls back from HTTPS after a request failure.
`activate_preset` remains an embedded fixture until the platform publishes its
second download contract package. The live backend pipeline (the platform side
issuing commands and credentials) is still pending.

## Repository boundaries

- `easyled-firmware` owns product boot, connectivity, commands, durable state,
  partition layout, board profiles, recovery and OTA integration.
- `esp32libfun` owns the ESP-IDF HAL/framework components.
- `esp_pixels` owns the physical pixel transport library.
- `esp_pixels_fx` owns effect implementations and their stable IDs.
- `esp_pixels_composer` owns scenes, segments, mapping and composition.

The four library repositories are pinned as Git submodules under
`dependencies/`. Updating a dependency is an explicit firmware change followed
by validation; cloning a newer library branch cannot silently change a released
firmware build.

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
- compact serialization of the provisional retained device-state fixture and
  the PR #66 `capability_reported` event envelope;
- `TerminalJournal`, an application-owned durable interface that permits a
  duplicate `command_id` to replay its original terminal event without executing
  the command again;
- `DurableStaging`, an application-owned transaction interface that separates
  append, checksum/commit, frame-boundary activation and rollback. Its contract
  permits `command_applied` only after recoverable commit and successful atomic
  activation.

The contract component deliberately does not select NVS keys, journal capacity,
cache eviction or a partition layout. The application-level storage component
below supplies that policy without moving it into the framework core.

## Offline command executor

`components/pixels_payload_adapter` turns exact, checksum-verified
`apply_config`/`activate_preset` payload bytes into a validated pixel
candidate. It reuses `esp_pixels_fx::describe()` as the one effect catalog and
`esp_pixels_composer::AtScene`/`SegmentSpec` as the one scene representation --
never a second metadata or scene system. Config payloads accept 1D segments and
version-1 ledmaps with 2D `{x,y,w,h}` regions, including effects whose work
buffers are allocated by Composer. Presets remain 1D until activation receives
the active canvas context. The adapter accepts an empty `inputs`, one or two
logical `digital_out` WS2812B slots with `reversed:false`; GPIO binding belongs
to the application board profile and is never trusted from remote JSON. It accepts
strict `#RRGGBB` segment colors, `replace`/`add`/`alpha`
blend names, and segments addressed by `segment_index` (0..31, mapped to
Composer id `segment_index + 1`). A ledmap validates version, dimensions, map
length, holes, physical-index range and uniqueness. Config segments must fit the
flat output span or mapped canvas. A structurally valid but not-yet-implemented
field (a non-empty `inputs`, a physical output `reversed:true`, a present segment `mirror`/`reversed`/`custom`/
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

## Durable command storage

`components/pixels_durable_storage` provides the concrete NVS policy for the
executor. It uses the dedicated `easyled` partition from
`partitions_network.csv`, never erases or formats it automatically, and is
intended for a single application control task.

- `NvsTerminalJournal` retains eight immutable terminal results. A duplicate
  `command_id` returns the original bytes; a conflicting rewrite is rejected.
  Once full, a successfully committed and read-back record replaces the oldest.
  Corrupt journal data fails initialization to prevent accidental re-execution.
- `NvsDurableStaging` accepts at most 8192 exact payload bytes and uses a pending
  record plus A/B active records. Every record has schema, bounds, CRC32 and NVS
  commit/readback checks; payload activation additionally requires exact SHA-256.
- The last successful `apply_config` is retained separately as the base topology
  and canvas. Because a preset is scene-only, boot and rollback restore this base
  configuration before applying an active preset. A preset without a recoverable
  base configuration fails closed.
- On boot, the newest valid active record is reapplied at a render-frame boundary.
  A pending candidate is reconciled against its command's terminal journal
  entry: durable success keeps it; a missing result restores the prior A/B slot.
  Normal success calls `finalize()` after journaling `command_applied`; failure
  leaves the same recovery metadata for the next boot.
- A corrupt active set fails initialization instead of silently starting blank.
  `init()` returns `ESP_ERR_NOT_FOUND` on a legitimately empty partition while
  leaving the instance ready for its first transaction.
- `NvsTelemetryState` durably increments the boot counter, reserves each state
  sequence before publication, and remembers the last applied command UUID.
  `ActiveSnapshot` exposes the recovered command type, config version and preset
  identity without reparsing storage in the application loop. Preset activation
  preserves the active config version.

## Integrated control runtime

`components/pixels_control_runtime` supplies the concrete application adapters
and `main.cpp` now owns their lifecycle. Boot first builds a safe black topology,
opens the terminal journal, and reconstructs the newest durable output topology,
ledmap and scene before networking starts. With a local
profile, the same application task that calls `NetworkSession::poll()` dequeues
complete MQTT commands and calls `Executor::execute()`; no product action runs
inside an MQTT callback. Events are published non-retained with QoS1 through the
configured exact event topic and timestamps come from the SNTP-derived UTC clock.
After subscription, the runtime publishes one retained QoS1 state document on
the exact state topic. It publishes again after each successful command and
retries enqueue failures without consuming another sequence number. The state
contains durable boot/state counters, active config/preset identity and the last
applied command. `capability_reported` uses the event topic, QoS1 without retain,
once after subscription and every 60 minutes. Its measured heap/PSRAM and output
topology accompany supported effect IDs 0..21, ledmap/2D/mirror/custom support and
the 100-slot disposable local preset cache.

`TransactionalPixelActivationTarget` reparses persisted bytes on every
activation, resolves logical output slots through an immutable local board
profile, installs the ledmap before the scene, and reports success only after
`Composer::reclaim()` proves frame-boundary adoption. Remote JSON cannot choose
GPIOs. The current laboratory profile binds slot 0 to GPIO16 and slot 1 to
GPIO17, permits up to 2048 pixels per slot, and clears 372 GRB pixels on GPIO16
when no durable configuration exists. A failed rebuild or scene adoption restores
the prior confirmed topology, canvas and scene before returning the error. A
preset inherits the last confirmed base configuration; reboot restores that base
before the scene-only preset.

`EmbeddedPayloadSource` resolves only the starter pack's exact config URL and
preset UUID to the versioned fixture bytes embedded from `validation/fixtures`.
It is an explicit development bridge for offline/broker integration testing,
not an HTTPS substitute or a production compatibility claim. A different URL or
preset UUID fails as `resolve_failed` without changing live state.

`HttpsPayloadSource` is the prepared production config source. It receives its
HTTP client, bearer token, optional CA PEM, timeout and payload-size limit by
injection; the URL remains the command's `config_url`. The ESP-IDF client accepts
only HTTPS, verifies the peer certificate and hostname through the supplied CA
or the full certificate bundle, disables redirects and authorization retries,
and never logs the token or response body. It accepts only a complete HTTP 200
`application/json` body within the configured limit (8192 B maximum) and verifies
the command's SHA-256 before returning bytes to the executor. When present, the
canonical `X-Config-Checksum` response header must be a 64-character SHA-256 and
the exact body must match it through the same constant-time digest comparison.
Malformed, duplicate or divergent checksum headers fail closed. Consequently
the executor cannot stage or activate a partial, oversized or checksum-invalid
download. HTTP 401, 403 (including `missing_capability_manifest`), 404 and 500
all remain resolution failures and preserve the active durable configuration.
Preset resolution intentionally remains with `EmbeddedPayloadSource` until
EasyLED publishes the second #60 contract package.

The backend fixture from EasyLED PR #72 is embedded verbatim as
`validation/fixtures/sample-config-payload.canonical.json`: 537 bytes, no final
newline, SHA-256
`112ef822a45000a449e87b97170d5448cdfc2afbc11c33728e39a1095b4eca6f`.
Validation downloads and verifies those exact bytes using the definitive header,
then activates its logical slot through the local GPIO16 binding, materializes
the 4x4 ledmap and adopts its 2D segment. Validation also injects a post-rebuild
failure and proves restoration of the previous topology, canvas and scene. Object
parsing is key-order independent, including `outputs[].config`, and validation
exercises a reordered object.

The capability envelope/cadence follows PR #66, but the inner capability object
is explicitly provisional schema version 1 because EasyLED has not yet closed
that canonical schema. `main.cpp` now routes `apply_config` through
`HttpsPayloadSource` whenever a device bearer token is configured (see
"Two apply_config modes" below); capability-manifest ingestion from a live
backend is still not implemented. Also not implemented: final MQTT
topic/provisioning policy, OTA execution, and live broker-loss tests.

### Two apply_config modes

`main.cpp` selects between them once at boot, from the local profile alone --
never from a request outcome:

- **Laboratory mode** (no `https_bearer_token` in the profile): `apply_config`
  resolves from the same embedded fixture as `activate_preset`, for
  offline/broker-integration testing. This is the default with no profile at
  all, and remains available on a provisioned build that simply omits the
  token.
- **Provisioned mode** (`https_bearer_token` set): `apply_config` resolves
  over authenticated HTTPS exclusively -- see `HttpsPayloadSource` above. Any
  transport, status, size or checksum failure returns to the executor as-is;
  there is no fallback to the embedded fixture. `activate_preset` still
  resolves from the embedded fixture in this mode too, until EasyLED
  publishes the second #60 contract package.

A profile that sets a bearer token is read as an explicit claim of
provisioned mode. If that token is empty or over
`HttpsPayloadSource::kMaxBearerTokenBytes`, or a supplied CA is an empty
string, or an explicitly set timeout/max-payload-bytes is out of
`HttpsPayloadSource`'s supported bounds, startup logs the rejection and refuses
to start networking -- it never silently drops back to
laboratory mode for a device that claimed to be provisioned. Rendering and
durable-state restoration are unaffected; only networking is refused.

## Checkout, build and local profile

Clone with the pinned dependencies:

```powershell
git clone --recurse-submodules https://github.com/rodrigo-s-lange/easyled-firmware.git
cd easyled-firmware
```

For an existing checkout:

```powershell
git submodule update --init --recursive
```

From an ESP-IDF 6.0.2 shell in this directory:

```powershell
idf.py -B build -D IDF_TARGET=esp32 build
```

The build uses this target's `sdkconfig.defaults`, including
`partitions_network.csv`. The dependency paths are CMake cache variables, so a
developer can test a local library checkout without changing the committed pins,
for example `-D ESP_PIXELS_FX_DIR=C:/work/esp_pixels_fx`.

Without `main/network_credentials.hpp`, startup still initializes pixels and
restores durable local state, then leaves networking disabled. An optional,
gitignored profile has this shape (omit `.https_bearer_token` entirely for
laboratory mode; see "Two apply_config modes" above):

```cpp
#pragma once
#include "network_session.hpp"
static ApplicationConfig applicationConfig()
{
    return {
        .network = {
            .ssid = "YOUR_WIFI",
            .wifi_password = "YOUR_WIFI_PASSWORD",
            .broker_uri = "wss://broker.example.invalid/mqtt",
            .client_id = "YOUR_MQTT_CLIENT_ID",
            .username = "YOUR_DEVICE_USERNAME",
            .password = "YOUR_DEVICE_PASSWORD",
            .command_topic = "YOUR_EXACT_COMMAND_TOPIC",
        },
        .device_id = "YOUR_CANONICAL_DEVICE_UUID",
        .hardware_binding_id = "YOUR_HARDWARE_BINDING_UUID",
        .generation = 1,
        .firmware_version = "0.1.0-dev",
        .event_topic = "YOUR_EXACT_EVENT_TOPIC",
        .state_topic = "YOUR_EXACT_STATE_TOPIC",
        .https_bearer_token = "YOUR_DEVICE_HTTPS_BEARER_TOKEN", // provisioned mode; omit for laboratory mode
        // Omit server_ca_pem to use the ESP-IDF certificate bundle (the
        // default for a platform-issued certificate); set it only to pin a
        // private/self-signed CA.
        // .https_timeout_ms and .https_max_payload_bytes default to
        // HttpsPayloadSource::Config's own values (10 s / the component's
        // max payload) when left at zero; set either only to override it.
    };
}
```

These are placeholders, not an EasyLED provisioning contract. Supply credentials
only for an authorized development device. The example does not print payloads or
credentials. The firmware does not erase NVS automatically on initialization
failure.

`apply_config`'s `command.resource_url` (from the parsed command itself, never a
compiled-in endpoint) is fetched over HTTPS with this bearer token and TLS
verification always enabled; a download, status, content-type or checksum
failure is returned to the executor as-is -- the active durable configuration
is preserved exactly like any other resolve failure, never silently replaced by
a cached or embedded payload. An incomplete provisioned profile is rejected
before networking ever starts; see "Two apply_config modes" above.

The current product target uses a 4 MiB partition table with a 1.5 MiB factory application,
128 KiB dedicated `easyled` NVS and 64 KiB coredump partition. It differs from
the preset application's layout. Back up the complete flash before a bench run
and restore that image afterward; flashing only the previous application binary
is insufficient to restore its partition table and NVS data.

This target sets the task-watchdog timeout to 10 seconds in `sdkconfig.defaults`.
The 128 KiB NVS partition scan takes more than the ESP-IDF default five seconds
under the supported QEMU build; the same boot completes without disabling the
watchdog. No generated `sdkconfig` is edited.

For USB links that fail during a single 4 MiB read, `validation/backup_flash.py`
reads 256 KiB chunks, retries each failed chunk at a lower baud rate, validates
every chunk length and assembles one SHA-256-labelled image. It refuses to
overwrite an existing output:

```powershell
python validation/backup_flash.py --port COM6 --output build/flash-before.bin
```

## Focused local validation

`validation/` runs the fragment assembler, firmware-contract component, the
payload adapter, command executor, real NVS storage and the control-runtime
adapters in an ESP32 QEMU target with no network adapter. The nine fixture strings
the tests use (two payloads, two command envelopes, four events and one retained
state) live as inspectable files
under `validation/fixtures/`, embedded into the binary via
`EMBED_TXTFILES` rather than duplicated as string literals. It checks the two
exact provisional fixture hashes, command parsing, event/state/capability
serialization, terminal replay, the payload adapter's happy paths and a representative
type/range/duplicate/unsupported failure table, and the executor's full
scenario table (happy-path apply_config, duplicate replay, preset cache-miss/
already-active no-op, checksum-mismatch rejection, staging/activation-failure
rollback, terminal-store-failure rollback, terminal-publish-failure survival,
reboot/ota rejection, and 20 repeated commands with no leak) through a real
`Composer`/`FakeSink` pair, proving a fixture's own bytes reach rendered pixels.
It also executes the concrete eight-entry journal, immutable duplicates and
oldest eviction; A/B recovery and rollback across simulated reboot; an actual
8192-byte transaction; preset identity recovery; and corrupt-active rejection.
The telemetry store is rebooted in place to prove durable boot-counter,
state-sequence and last-applied-command behavior, while active snapshots prove
that config version survives preset activation.
It also covers exact embedded-resource lookup, timestamp format, logical-slot
rejection, transactional runtime reconstruction, canonical 4x4 activation,
scene-only preset inheritance, and restoration after an injected activation
failure. Durable-storage checks prove rebooting an active preset restores its
base configuration before the preset scene.
An injected fake HTTP client verifies exact config-byte delivery, URL/token/CA/
timeout/limit forwarding, non-HTTPS rejection, transport failure, HTTP status,
content type, oversize and checksum failures. It does **not** open a network
connection or validate a live TLS peer, broker permissions or physical flash
failure.

```powershell
cd validation
idf.py -B build -D IDF_TARGET=esp32 build
python run_qemu.py --qemu "PATH_TO/qemu-system-xtensa.exe" --build build
```

The script merges only a local emulated flash file, captures the result in
`validation/build/qemu-validation.log`, and terminates QEMU
after 45 seconds by default. Pass `--timeout 60` on slower hosts. The expected
marker is `NETWORK_CONTRACT_VALIDATION checks=321 failures=0 RESULT=ALL_PASS`.

On Windows, this QEMU distribution also needs `libiconv-2.dll` on `PATH`. In the
local environment, prepend `C:\Program Files\Git\mingw64\bin` to the current
shell's `PATH` before invoking the script (no machine-wide configuration change).

The validation target uses an ESP-IDF 8192 B main-task stack. Large candidates,
fixture strings, journal records and fake storage remain in owning/static
storage; QEMU runs the repeated-command loop without stack overflow or panic.

Build verified with ESP-IDF 6.0.2: combined binary 1109252 B, static DRAM 73391 B
(40.61%) and IRAM 90555 B (69.09%). The 1.5 MiB app partition retains the
margin reported by the build's own partition check. These figures include live
storage, executor, fixture resolver, the HTTPS payload source and Composer
activation objects but exclude runtime Wi-Fi/TLS heap use.
The focused binary is 701793 B and ran on Espressif QEMU 9.2.2: all 343 checks
passed, with no assertion, watchdog or panic. Its static DRAM is 176268 B
(97.53%, 4468 B free, dominated by `.bss` test fixtures and real-NVS transaction
buffers) and IRAM is 47715 B (36.40%). The combined image also booted through
real NVS initialization with the five-entry network partition table. No broker
or live HTTPS endpoint was accessed.

## Next integration step

Provision a real device credential through platform flow #47, then exercise
provisioned-mode `apply_config` against the deployed backend's own URL once
that environment and its definitive MQTT topics/credentials exist (backend
work tracked separately; issues #47 and #56-#61 remain open). Replace the
laboratory board profile with the first product board's reviewed
slot/GPIO/current limits. Replace the provisional capability object with its
canonical schema when EasyLED closes it, then exercise command/event/state
retain, acknowledgement and reconnect behavior with an authorized broker.
Provisioning, heap/stack measurements under TLS load and
broker-loss/render-continuity tests remain product work.

Official references: [ESP-IDF 6.0.2 MQTT component migration](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/protocols/mqtt.html),
[ESP-MQTT 1.1.0](https://components.espressif.com/components/espressif/mqtt/versions/1.1.0/readme),
[cJSON 1.7.19](https://components.espressif.com/components/espressif/cjson/versions/1.7.19/readme).
