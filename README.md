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
  request another connection attempt; a SUBACK for a superseded subscribe
  attempt (from before a disconnect/reconnect) is ignored.
- Incoming fragments are assembled before delivery. Payloads are limited to
  1024 bytes, with a queue for two complete messages. Malformed/oversized messages,
  retained deliveries and other topics are rejected. A full queue drops the new
  message and increments `queue_overflows`; this is not durable command delivery.
- Outgoing messages use QoS1 enqueue with a 4096-byte SDK outbox limit. A negative
  return means the publication was not accepted. A nonnegative message ID confirms
  enqueue only, never broker acknowledgement or successful command execution --
  call `publishStatus(msg_id)` for the broker's QoS1 PUBACK. It tracks the 8 most
  recently enqueued message IDs; an older unacknowledged one can be evicted by
  newer publishes, and any disconnect observed after an ID was enqueued also
  surfaces it as `Unknown` from then on, even if the SDK's own outbox still holds
  and later redelivers it. `Unknown` and `Pending` both mean "not yet confirmed
  delivered"; the caller decides whether and how to retry. This session makes no
  retry decision itself and consumes no second durable sequence number for a
  retried revision. The product enables ESP-MQTT's incremental packet IDs so the
  bounded tracker never has to disambiguate two live QoS1 publications that were
  assigned the same random ID -- see "Integrated control runtime" below.
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

`NetworkSession` also exposes a split API: `initWifi()`/`start()` bring up Wi-Fi
and the bounded incoming queue alone, with no MQTT client at all; `configureMqtt()`
adds a first MQTT identity later, at any point before or after `start()`;
`restartMqtt()` ends whatever identity is live (safe if none) and applies a new
one without ever disconnecting Wi-Fi, touching SNTP/trusted time, or affecting
durable pixel state or rendering; `endMqtt()` ends the MQTT identity alone. The
combined `init(Config)` remains a compatibility wrapper equivalent to
`initWifi()` + `configureMqtt()` with the same all-or-nothing validation, used by
laboratory mode and the existing `NetworkSession` test seam below unchanged. A
`restartMqtt()`/`endMqtt()` boundary invalidates every previously tracked publish
to `PublishAck::Unknown` (via `Status::connection_epoch`, exposed for exactly this
purpose) and clears queued/incomplete inbound commands from the old identity, even
though ESP-MQTT delivers no `DISCONNECTED` callback for a deliberate client
destruction; a callback already queued from the destroyed client cannot mark the
new identity connected or subscribed (`event()`'s own `event.client` guard, kept
independent of the vendored SDK's actual synchronous-teardown behavior). Destroying
a client this way also discards its own QoS1 outbox -- nothing from the old
identity's outbox survives or is redelivered under the new one.

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
`command_rejected`/`unsupported_command` -- the same reason code
`pixels_firmware::parseErrorCode()` returns for an unrecognized envelope
type -- parsed but never executed. An
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
`rollback_failed`. `Executor` owns its fixed-capacity
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
the exact state topic, and again after each successful command. A state
revision counts as delivered only once `NetworkSession::publishStatus()`
reports the broker's PUBACK for that exact enqueue -- not merely a nonnegative
`esp_mqtt_client_enqueue()` return. Enqueue rejection, a disconnect before the
PUBACK arrives, or no PUBACK within 5 seconds all leave the revision pending
and eligible for retry on the next loop iteration, re-enqueued under a new
message ID without allocating another `state_sequence`; a late PUBACK for a
superseded enqueue cannot be mistaken for the current one, because tracking is
also invalidated by any disconnect observed since that enqueue. The state
contains durable boot/state counters, active config/preset identity and the last
applied command. `capability_reported` uses the event topic, QoS1 without retain,
once after subscription and every 60 minutes, with the same PUBACK-gated
retry. At the event level it carries `device_id`, `hardware_binding_id`
(the same UUID as the current boot's `StateReport`), `generation` (the same
value as that same boot's state, never a second identity scheme),
`fw_version`, `capabilities` and `reported_at`. Inside `capabilities`, its
measured heap/PSRAM and output topology accompany supported effect IDs 0..21,
ledmap/2D/mirror/custom support, the 100-slot disposable local preset cache,
and `max_payload_bytes` -- the exact command-payload byte limit this firmware
build enforces (`pixels_payload::kMaxPayloadBytes`), not yet a safe
`max_ledmap_cells` figure, which still needs a heap-under-TLS measurement and
remains a separate task.

This PUBACK gate proves only that *this device's* broker TCP/TLS session
accepted the publication at the protocol level. It is not evidence that
EasyLED's backend ever persisted or reconciled that state/capability revision,
that a command's `command_applied`/`command_failed` event was durably
ingested, or that the backend's own view of device state converged after a
broker-side redelivery or an overlapping revision from the same device.
That backend-side reconciliation is tracked separately as EasyLED issues
#58/#59 and is unimplemented here; this firmware only tracks broker delivery
of its own most recent publications.

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

## Provisioned MQTT session integration

Provisioned boot (`mqtt_provisioning_enabled` with a complete profile) separates
Wi-Fi from MQTT identity instead of using `NetworkSession::init(Config)`'s
combined, literal-credential path:

1. restore durable pixel state (unchanged, before any networking);
2. initialize the durable credential store/coordinator, then
   `NetworkSession::initWifi()` + `start()` -- Wi-Fi comes up with no MQTT
   identity at all;
3. the single control-loop iteration calls `NetworkSession::poll()` (its one
   and only owner) every tick, starts SNTP after IP, and -- only once Wi-Fi is
   connected and the wall clock passes the same plausible-clock gate
   `NetworkSession::poll()` itself uses before ever starting MQTT
   (`NetworkSession::kMinPlausibleUnixTime`) -- calls
   `MqttSessionController::tick()`;
4. `components/pixels_mqtt_session_adapter` (`NetworkSessionMqttTransport`)
   bridges the controller to the real session: it copies every bounded
   identity string from `MqttSessionIdentity` into its own buffers before
   calling `NetworkSession::restartMqtt()`, so the live MQTT configuration
   never borrows a `StoredCredential`/pending buffer that the credential store
   can mutate or clear right after; the firmware-shipped trust-table CA
   pointer is the one exception deliberately borrowed, since
   `TableTrustResolver` entries are static and outlive every session.

CONNACK and SUBACK remain two separate signals, same as always: the adapter
reports `Connected` -- durably promoting the credential via the existing #47-B
store -- on the first real `MQTT_EVENT_CONNECTED` for the current attempt, before
any SUBACK; the product's own command/telemetry gate still waits for the normal
QoS1 `cmd` SUBACK separately (`NetworkSession::status().subscribed`), exactly as
laboratory mode always has. The adapter reports `Ambiguous` for an immediate
setup/start failure, a disconnect/transport error before that CONNACK, or a
bounded connect timeout; stale evidence from a superseded attempt cannot
confirm a new one, since every `restart()` recaptures a fresh baseline from
`NetworkSession::status()` (connection/error counters, `connection_epoch`) and
every outcome is judged only against that attempt's own baseline.

Two lifecycle gaps a plain "one attempt at a time" controller would leave open
are closed in `MqttSessionController` itself (`pixels_mqtt_credentials`), not
in the frozen #47-B state machine it drives: a cold boot that loads only
`CurrentConfirmed` (no `pending`) makes one durably marked reconnect attempt
with `current` instead of waiting forever for a rotation that was never
requested; and once `ready()` is true, `tick()` keeps sampling the adapter
instead of going idle, so ordinary brief loss is absorbed silently (through
`NetworkSession`/ESP-MQTT's own reconnect, never re-reported as a new
promotion), while a bounded post-confirmation reconnect deadline with no fresh
CONNACK still durably reports `Ambiguous` and falls back into the existing
ambiguous/current-retry path and, after the existing round limit, into
`BootstrapRecovery` -- never a tight loop, and never losing the last confirmed
`current` until a replacement actually confirms. A rotation (a new `pending`
persisted while already `ready()`) always preempts monitoring on the very next
`tick()`, restarting with the new identity immediately rather than waiting for
the old one to fail first.

In provisioned mode, `topic_cmd`/`topic_state`/`topic_event`/`topic_telemetry`
come from the selected credential, never from `ApplicationConfig`'s literal
laboratory topics. `pixels_mqtt_session::ProvisionedTopics` is a small,
stable-address snapshot that `MqttEventSink`/`MqttTelemetrySink` read a pointer
into once, at construction; `rotateProvisionedTopics()` overwrites its four
fields in one synchronous call, from the single control task, exactly when
`ready()` newly becomes true -- the whole topic set rotates together with the
identity, with no interval in which a command from one identity could trigger
an event on another's topic, and without ever reconstructing the sinks or the
command executor. Laboratory mode is unaffected: it keeps using
`ApplicationConfig::event_topic`/`state_topic` exactly as before, and this
snapshot/rotation path never runs for it.

Still blocked on the platform, unchanged from the #47-B round: real
`trust_ref`/CA values per environment, the deployed bootstrap base URL, the
real bootstrap-token delivery mechanism, and per-environment rate limits. Still
open as firmware/product work, now that the Wi-Fi-before-identity ordering
itself is resolved: a real broker/backend end-to-end run (this integration's
own QEMU coverage uses only deterministic fakes/synthetic MQTT events -- no
real broker, TLS peer, or backend was ever contacted), and heap/stack headroom
measurement for the provisioned path under real TLS load.

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
serialization (including `capability_reported`'s `hardware_binding_id`/
`generation`/`max_payload_bytes` boundaries: absent/invalid binding,
zero generation, and a zero/correct payload limit), terminal replay, the
payload adapter's happy paths and a representative
type/range/duplicate/unsupported failure table, and the executor's full
scenario table (happy-path apply_config, duplicate replay, preset cache-miss/
already-active no-op, checksum-mismatch rejection, staging/activation-failure
rollback, terminal-store-failure rollback, terminal-publish-failure survival,
reboot and ota rejection as `unsupported_command`, and 20 repeated commands
with no leak) through a real
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

`NetworkSession`'s connect/subscribe/publish-ack/reconnect state machine is
exercised the same way, through its public `event()` test seam: a session is
`init()`-ed (never `start()`-ed, so no real Wi-Fi task runs) and hand-built
`esp_mqtt_event_t` structs are fed directly to `event()`, with
`setSubscribeOverrideForTest()` substituting a deterministic ID generator for
the one SDK call (`esp_mqtt_client_subscribe()`) that requires an actually
-established broker connection to succeed. This covers, in one scripted
lifecycle: rejected and downgraded SUBACK (not ready), a stale SUBACK from a
superseded attempt, granted SUBACK (ready), bounded publish-ack tracking
(a 9th publish evicts the oldest of 8 tracked IDs), command-queue overflow
(a 3rd complete message is dropped and counted, the first two still FIFO-
correct), enqueue-to-PUBACK for both retained state and non-retained
capability publications (including an idempotent duplicate in-epoch PUBACK),
disconnect before a PUBACK arrives, a late PUBACK after that disconnect still
reporting `Unknown` rather than a false `Acknowledged`, enqueue rejection
while disconnected, reconnect with a fresh subscribe ID that rejects a stale
one from before the disconnect, and retrying the same state/capability
revision after reconnect without a new sequence number. A render frame is
produced successfully while the session sits disconnected, proving the
render/control path has no dependency on MQTT connectivity. None of this
opens a socket, a TLS context or a real FreeRTOS MQTT task; it does not
exercise ESP-MQTT's own TCP/TLS reconnection timing, backoff or outbox
persistence across a real connection loss.

```powershell
cd validation
idf.py -B build -D IDF_TARGET=esp32 build
python run_qemu.py --qemu "PATH_TO/qemu-system-xtensa.exe" --build build
```

The script merges only a local emulated flash file, captures the result in
`validation/build/qemu-validation.log`, and terminates QEMU
after 45 seconds by default. Pass `--timeout 90` on slower hosts. The expected
marker is `NETWORK_CONTRACT_VALIDATION checks=468 failures=0 RESULT=ALL_PASS`.

On Windows, this QEMU distribution also needs `libiconv-2.dll` on `PATH`. In the
local environment, prepend `C:\Program Files\Git\mingw64\bin` to the current
shell's `PATH` before invoking the script (no machine-wide configuration change).

The validation target uses an ESP-IDF 8192 B main-task stack. Large candidates,
fixture strings, journal records and fake storage remain in owning/static
storage; QEMU runs the repeated-command loop without stack overflow or panic.

Build verified with ESP-IDF 6.0.2: combined image 1132052 B, static DRAM 83567 B
(46.24%) and IRAM 90555 B (69.09%). The 1.5 MiB app partition retains the
margin reported by the build's own partition check. These figures include live
storage, executor, fixture resolver, the HTTPS payload source, the bounded
MQTT publish-ack tracking table, Composer activation objects, the #47-B
credential store/coordinator and the provisioned-mode session adapter/topic
snapshot, but exclude runtime Wi-Fi/TLS heap use.
The focused image is 741321 B and ran on Espressif QEMU 9.2.2: all 468 checks
passed, with no assertion, watchdog or panic. Its static DRAM is 179964 B
(99.57%, **772 B free**, dominated by `.bss` test fixtures and real-NVS
transaction buffers) and IRAM is 48047 B (36.66%). DRAM headroom is this thin
already and frozen deliberately: do not add checks here -- see the two
dedicated MQTT targets below. The combined image also booted through
real NVS initialization with the five-entry network partition table. No broker
or live HTTPS endpoint was accessed.

### MQTT credential lifecycle and session integration targets

Two further QEMU targets validate the #47-B durable credential lifecycle and
its real-session integration, kept out of the frozen target above precisely
because of its thin DRAM margin:

- `validation_mqtt_credentials/` -- the durable store/coordinator/session
  controller against deterministic fakes only (no `NetworkSession`), per
  `docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md`. Build and run the same way:
  ```powershell
  cd validation_mqtt_credentials
  idf.py -B build -D IDF_TARGET=esp32 build
  python run_qemu.py --qemu "PATH_TO/qemu-system-xtensa.exe" --build build --timeout 240
  ```
  Expected marker: `MQTT_CREDENTIALS_VALIDATION checks=298 failures=0 RESULT=ALL_PASS`.
  This target's own NVS A/B commits are real and comparatively slow under QEMU's
  flash emulation; budget several minutes, not the frozen target's ~90 seconds.
  Focused image 270697 B; static DRAM 90300 B (49.96%, 90436 B free); IRAM
  46171 B (35.23%, 84901 B free).
- `validation_mqtt_session/` -- the real integration this document's
  "Provisioned MQTT session integration" section describes:
  `NetworkSession`'s Wi-Fi/MQTT split driven through its `event()` test seam,
  the real `pixels_mqtt_session_adapter` against that real session (connect
  timeout, pre-CONNACK disconnect/error, stale-attempt rejection, identity
  ownership), `ProvisionedTopics` rotation, and the full
  controller-plus-adapter-plus-session chain for cold-boot reconnect, brief
  loss, and the bounded-deadline round-counted path to `BootstrapRecovery`.
  Same build/run shape:
  ```powershell
  cd validation_mqtt_session
  idf.py -B build -D IDF_TARGET=esp32 build
  python run_qemu.py --qemu "PATH_TO/qemu-system-xtensa.exe" --build build --timeout 90
  ```
  Expected marker: `MQTT_SESSION_VALIDATION checks=96 failures=0 RESULT=ALL_PASS`.
  Focused image 677828 B; static DRAM 53695 B (29.71%, 127041 B free); IRAM
  63827 B (48.7%, 67245 B free) -- comfortable headroom for future checks.
  No real broker, backend, trust reference, or secret is used by either target.

## Next integration step

Provision a real device credential through platform flow #47, then exercise
provisioned-mode `apply_config` against the deployed backend's own URL once
that environment and its definitive MQTT topics/credentials exist (backend
work tracked separately; issues #47 and #56-#61 remain open). Replace the
laboratory board profile with the first product board's reviewed
slot/GPIO/current limits. Replace the provisional capability object with its
canonical schema when EasyLED closes it, then exercise command/event/state
retain, acknowledgement and reconnect behavior with an authorized broker --
this firmware's PUBACK tracking (see "Integrated control runtime") has only
been proven against synthesized events, never a real broker's TCP/TLS
reconnect timing, backoff or outbox persistence across an actual connection
loss. EasyLED backend-side reconciliation of delivered state/capability
revisions and `command_applied`/`command_failed` events remains open as
issues #58/#59; this firmware's PUBACK is evidence of broker delivery only,
never of backend persistence. Provisioning, heap/stack measurements under TLS
load and broker-loss/render-continuity tests against a real broker remain
product work.

The Wi-Fi-before-MQTT-identity ordering that previously blocked wiring the
#47-B session controller to a real `NetworkSession` (see
`docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md`) is resolved -- see "Provisioned MQTT
session integration" above. What remains before provisioned mode can run
against a deployed environment is unchanged from that document's own list:
real `trust_ref`/CA values, the deployed bootstrap base URL, the real
bootstrap-token delivery mechanism, per-environment rate limits, and a real
broker/backend end-to-end run (this integration's own QEMU coverage is
deterministic fakes/synthetic MQTT events only).

Official references: [ESP-IDF 6.0.2 MQTT component migration](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/protocols/mqtt.html),
[ESP-MQTT 1.1.0](https://components.espressif.com/components/espressif/mqtt/versions/1.1.0/readme),
[cJSON 1.7.19](https://components.espressif.com/components/espressif/cjson/versions/1.7.19/readme).
