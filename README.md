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
eviction or a partition layout. It also does not parse the downloaded config or
preset payload into a Composer scene yet. Those are the next application-layer
steps after the EasyLED schema fields and canonical byte rules are closed.

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

`validation/` runs the fragment assembler, firmware-contract component and a real
50-pixel Composer render in an ESP32 QEMU target with no network adapter. It checks
the two exact provisional fixture hashes, command parsing, event serialization,
terminal replay, pixel-stack linking and all previous message assembly cases. It
does **not** validate radio, TLS, broker permissions, storage durability or HTTPS.

```powershell
cd validation
idf.py -B ../../../../build/network-contract-validation -D IDF_TARGET=esp32 build
python run_qemu.py --qemu "PATH_TO/qemu-system-xtensa.exe"
```

The script merges only a local emulated flash file, captures the result in
`build/network-contract-validation/qemu-validation.log`, and terminates QEMU after
15 seconds. The expected marker is
`NETWORK_CONTRACT_VALIDATION checks=52 failures=0 RESULT=ALL_PASS`.

On Windows, this QEMU distribution also needs `libiconv-2.dll` on `PATH`. In the
local environment, prepend `C:\Program Files\Git\mingw64\bin` to the current
shell's `PATH` before invoking the script (no machine-wide configuration change).

Build verified with ESP-IDF 6.0.2: combined image 971968 B, static DRAM 43207 B
(23.91%) and IRAM 87891 B (67.06%). The 1.5 MiB app partition retains 564032 B
(37%) after the build. These figures exclude runtime Wi-Fi/TLS heap use.
The focused image is 198080 B and ran on Espressif QEMU 9.2.2: all 52 checks
passed, with no assertion or panic. No broker was accessed.

## Next integration step

Implement the bounded config/preset payload adapter into a durable candidate,
then provide concrete NVS-backed `TerminalJournal` and `DurableStaging`
implementations in the product application. HTTPS retrieval, state publication,
provisioning and broker-loss tests remain dependent on the EasyLED delivery flows.
Runtime heap, stack and render continuity must be measured with the combined
firmware and an authorized development broker before product release.

Official references: [ESP-IDF 6.0.2 MQTT component migration](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/protocols/mqtt.html),
[ESP-MQTT 1.1.0](https://components.espressif.com/components/espressif/mqtt/versions/1.1.0/readme),
[cJSON 1.7.19](https://components.espressif.com/components/espressif/cjson/versions/1.7.19/readme).
