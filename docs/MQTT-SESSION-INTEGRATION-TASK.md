# MQTT session integration after #47-B

Status: ready for implementation  
Owner layer: EasyLED product firmware/application  
Product-code baseline: `cfc50d0a97657b82697cdd2eb313f278baf095f4`  
Date: 2026-10-17

## Outcome

Connect the already-tested `MqttSessionController` to the real
`pixels_network::NetworkSession` without requiring a deployed EasyLED backend.
Provisioned boot must bring up Wi-Fi and trusted time before requesting a
credential, then create/restart MQTT with the selected durable credential.
Laboratory mode must keep its current static-credential behavior.

This is application/product work. It does not belong in `esp32libfun`, the
pixel libraries, or `easyled-platform`.

## Pre-flight and isolation

Before editing:

1. Read `AGENTS.md`, `README.md`, the public headers for `pixels_network`,
   `pixels_mqtt_credentials`, and `pixels_control_runtime`, plus
   `docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md`.
2. Fetch and confirm that this task document is the only documentation commit
   after the product-code baseline above. Branch from the `main` commit that
   contains this document. Stop and report any additional divergence.
3. Inspect active worktrees on the Desktop and i5. Stop if another firmware
   worktree owns this same Wi-Fi/MQTT integration scope.
4. Create an isolated worktree and branch, suggested name:
   `feat/mqtt-session-provisioning-integration`.
5. Preserve parallel work. Do not edit `easyled-platform`.

Before this task document was committed, Desktop `main` and `origin/main` both
pointed to the product-code baseline above. The old #47-B and P1 worktrees were
clean historical worktrees; no `easyled-firmware` worktree was found under
`/home/rodrigo/worktrees` on i5.

## Current facts

- `NetworkSession::init(Config)` currently validates Wi-Fi and MQTT together,
  creates the ESP-MQTT client immediately, and only later does `start()` bring
  up Wi-Fi.
- Provisioned boot therefore currently initializes the durable credential
  store and deliberately returns with networking offline.
- `MqttCredentialCoordinator`, `MqttSessionController`, the A/B credential
  store, HTTPS bootstrap client, trust resolver, and recovery rules are already
  implemented and validated.
- `MqttTransportSession` deliberately has only `StillTrying`, `Connected`, and
  `Ambiguous`. Do not restore a guessed pre-authentication error category.
- A loaded `CurrentConfirmed` currently produces `Idle`, so the existing
  controller does not yet reconnect that credential after reboot. Likewise,
  after a confirmed session is lost, the current orchestration has no bounded
  path back to bootstrap if the broker never accepts that credential again.
  Real integration must close both lifecycle gaps rather than only make the
  first bootstrap connect.
- ESP-MQTT `MQTT_EVENT_CONNECTED` is the accepted CONNACK evidence. A granted
  QoS1 SUBACK is a separate, stronger product-readiness condition for command
  subscription; it must not delay durable credential promotion.
- The platform still owes real URLs, trust references/CAs, bootstrap-token
  delivery, rate limits, and a deployed environment. The architecture and
  deterministic validation in this task do not depend on those values.

## Required design

### 1. Separate Wi-Fi ownership from MQTT identity

Make the smallest cohesive change to `NetworkSession` that supports:

- initializing and starting Wi-Fi with SSID/password while no MQTT identity
  exists;
- configuring/starting/restarting MQTT later, without disconnecting Wi-Fi;
- ending MQTT alone while preserving Wi-Fi and SNTP connectivity;
- retaining the existing combined `init(Config)` path as a compatibility
  wrapper for laboratory mode and existing callers/tests;
- retaining `stop()`/`end()` combined semantics for full application shutdown.

Names are implementation choices, but the public API should expose two clear
configurations, equivalent to `WifiConfig` and `MqttConfig`. Every new public
method needs the repository's `///`, `@param`, and `@return` documentation.

Wi-Fi-only initialization must allocate/prepare the bounded incoming queue but
must not create an ESP-MQTT client. MQTT configuration must continue to require
`wss://`, exact non-wildcard command topics, and active certificate/hostname
verification. Never add an insecure or fail-open path.

### 2. Add the real transport adapter

Implement a thin application-layer adapter from `NetworkSession` to
`pixels_mqtt::MqttTransportSession`. Keep the credential component independent
of `pixels_network`; the adapter belongs in product/application orchestration
(for example a small dedicated component or `pixels_control_runtime`).

The adapter must:

- end/recreate only MQTT on `restart(identity)`; Wi-Fi remains connected;
- copy every bounded identity string whose source may change. It must never
  leave ESP-MQTT borrowing `StoredCredential` buffers: successful promotion
  changes/clears pending storage after CONNACK;
- make the CA lifetime explicit. A stable firmware trust table may be borrowed
  if its lifetime is guaranteed and documented; otherwise copy it safely;
- carry stable copies of the credential's state/event/telemetry/cmd topics for
  the product runtime. Provisioned publications and subscriptions must use the
  issued topics; `ApplicationConfig`'s literal laboratory topics are not a
  fallback in provisioned mode;
- report `Connected` exactly once for the current attempt after a new real
  `MQTT_EVENT_CONNECTED`/CONNACK;
- report `Ambiguous` for an immediate MQTT setup/start failure, disconnect or
  transport error before CONNACK, or a monotonic attempt timeout;
- ignore/reject stale events from an earlier client/attempt so they cannot
  promote the new credential;
- sample `NetworkSession`; it must not become a second owner that also calls the
  session's main `poll()` when the application loop already does so.

Expose only the minimum additional monotonic counters/epoch in
`NetworkSession::Status` needed to attribute connection, disconnect, and error
events to the current attempt. Do not expose credentials.

### 3. Define MQTT restart cleanup

An identity restart must begin a clean security/session boundary:

- clear incomplete and queued inbound commands from the previous MQTT session;
- invalidate every tracked outgoing publication so prior PUBACK state becomes
  `Unknown`, even if no disconnect callback is delivered during deliberate
  client destruction;
- reset subscription state and subscribe identifiers;
- prevent a late callback from the destroyed client from marking the new
  session connected or subscribed;
- preserve Wi-Fi, IP, trusted wall clock, durable pixel state, and rendering.

Document whether ESP-MQTT's own old outbox is discarded by destruction. The
application-level state/capability retry logic remains authoritative and must
reuse the same durable state sequence for a publication whose delivery became
unknown.

### 4. Wire provisioned boot in `main.cpp`

Preserve the present fail-closed profile classification:

- incomplete provisioned profile: network remains off;
- laboratory profile: the existing static combined session path remains
  behaviorally compatible;
- complete provisioned profile: never fall back to laboratory MQTT credentials.

For a complete provisioned profile, the order is:

1. restore local pixels/durable state as today;
2. initialize credential store/coordinator and Wi-Fi-only session;
3. start Wi-Fi;
4. call `NetworkSession::poll()` once per control-loop iteration;
5. after IP, start SNTP as today;
6. only after the same plausible-clock gate used for MQTT/TLS, call
   `MqttSessionController::tick()`;
7. allow bootstrap HTTPS to persist pending before the adapter starts MQTT;
8. after CONNACK is durably reported, continue waiting for the normal QoS1
   command SUBACK before receiving commands or publishing telemetry;
9. keep rendering and recovered configuration alive through every network
   failure.

Do not mark `AttemptInFlight` while Wi-Fi is down or the wall clock is not yet
usable for TLS. A controller/store error is logged without secrets and retried
with existing durable/backoff semantics; it must not switch modes.

There must be one owner for `NetworkSession::poll()`. The controller adapter
only observes the attempt-specific session outcome.

### 5. Complete cold-boot and post-connection recovery

Extend the controller orchestration by the minimum amount necessary to make a
durably confirmed credential useful across reboot and recoverable later:

- on cold boot with only `CurrentConfirmed`, make one durably marked attempt
  with `current` and start MQTT; do not bootstrap merely because the device
  rebooted;
- after a confirmed connection, continue monitoring the same session without
  repeatedly re-reporting later automatic reconnect CONNACKs as a new
  credential promotion;
- tolerate ordinary short network loss through `NetworkSession`'s normal
  reconnect behavior;
- after a bounded monotonic reconnect/CONNACK deadline, convert the loss into
  the existing conservative ambiguous/current retry path;
- after the existing bounded full-round limit, enter `BootstrapRecovery` and
  request a fresh pending credential while preserving the last current value
  until a replacement actually confirms;
- never tight-loop MQTT restarts or HTTPS issuance.

Use the existing A/B transitions and recovery meaning. A crash after a durable
`beginAttempt(false)` must continue to load as `AmbiguousActivation`. Do not
invent an `expires_at` wall-clock authority: the #47-B contract says it is
advisory and the broker decides acceptance.

### 6. Route provisioned topics with the selected credential

The issued credential contains `topic_state`, `topic_event`,
`topic_telemetry`, and `topic_cmd`. In provisioned mode:

- `topic_cmd` is the exact QoS1 subscription;
- state and command lifecycle/capability events publish to the issued state and
  event topics respectively, preserving the existing retain/QoS policy;
- retain `topic_telemetry` in the stable runtime snapshot even if this firmware
  does not publish a separate telemetry stream yet;
- rotate the complete topic set together with the MQTT identity;
- expose no interval in which a command from one identity can cause an event
  to be published on another identity's topic.

Laboratory mode continues using the existing `ApplicationConfig` topics.
Prefer a small injected topic provider or bounded runtime snapshot over mutable
borrowed pointers. Do not rebuild the command executor simply to change topics.

## Acceptance checks

Use deterministic fakes/synthetic MQTT events. No real broker, backend, token,
or CA is required for this task.

At minimum prove:

1. existing laboratory initialization and command/PUBACK behavior still pass;
2. Wi-Fi-only init/start is valid without an MQTT credential/client;
3. provisioned orchestration cannot bootstrap before Wi-Fi plus trusted time;
4. a credential is durably pending before the first MQTT restart;
5. MQTT restart leaves Wi-Fi up;
6. the adapter owns credential strings: mutating/clearing the source after
   `restart()` does not change the live MQTT configuration;
7. a current-attempt CONNACK produces `Connected` even before SUBACK;
8. disconnect/error/timeout before CONNACK produces `Ambiguous`;
9. a stale event from an earlier attempt cannot confirm the new attempt;
10. deliberate restart clears inbound fragments/messages and makes tracked
    publication states `Unknown`;
11. rotation selects pending first and current only as courtesy, using the
    existing #47-B state machine unchanged;
12. a durable reporting failure is retried without restarting or repolling the
    transport, preserving the existing final correction;
13. incomplete provisioning remains fail-closed with no static-MQTT fallback;
14. `CurrentConfirmed` reconnects after reboot without requesting a new
    credential first;
15. brief post-CONNACK network loss can reconnect with current, while a bounded
    persistent failure reaches the existing current retry/BootstrapRecovery
    path without a tight loop;
16. provisioned subscribe/state/event topics come from the selected credential
    and rotate together; laboratory topics remain unchanged;
17. a Composer/render step succeeds while network/bootstrap/MQTT is offline.

Do not add checks to the frozen `validation/` runner: it has only 772 bytes of
static DRAM headroom and must remain 468/468. Put new checks in a focused target
with comfortable headroom; extending `validation_mqtt_credentials/` is allowed
only if its responsibility remains clear, otherwise create a small
`validation_mqtt_session/` target.

## Gates and evidence

Before handoff:

- build the product target;
- run frozen `validation/` and retain exactly `468/468 ALL_PASS`;
- run `validation_mqtt_credentials/` and retain exactly `298/298 ALL_PASS`;
- run the new/focused QEMU checks and report exact counts;
- run `git diff --check`;
- report flash/DRAM/IRAM sizes and remaining headroom for every changed target;
- verify boot banners identify the tested commit (no unexplained `-dirty`);
- update `README.md` and the #47-B lifecycle documentation to describe the new
  boot order, the CONNACK/SUBACK distinction, and remaining live-environment
  blockers.

Commit cohesive checkpoints locally. Do not push, merge, flash hardware,
deploy, access real services, or add secrets. Report every file/commit, test
result, measured limit, discovered bug, and concrete blocker.

## Out of scope

- platform/backend changes;
- real trust-reference values, credentials, or endpoints;
- live broker/HTTPS integration;
- OTA, partition changes, rebind flow, preset download, effect work;
- changes to `esp32libfun` or pixel/effect/composer dependencies;
- changing the reviewed #47-B durability/ambiguity rules beyond the minimum
  cold-boot and live-session orchestration needed above.
