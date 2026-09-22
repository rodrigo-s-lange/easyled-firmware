# Pixel firmware network session

Application-layer Wi-Fi + MQTT over secure WebSocket (WSS), prepared independently
of the pixel effects work. This is a buildable connection example and reusable
application component, **not the completed EasyLED device integration**. It does
not render pixels or execute received commands. No EasyLED files are dependencies.

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

**Do not flash this standalone example onto the current preset bench board.** Its
standard large-app partition table differs from the application's preset layout.
Integration must retain the application's partitions and backups and recheck
combined image size before any flash. No flash or serial action was performed for
this delivery.

## Focused local validation

`validation/` runs the actual fragment assembler in an ESP32 QEMU target with no
network adapter or serial-port access. It checks fragmented/empty/binary messages,
capacity limits, invalid offsets, interrupted messages and replacement by a new
message. It does **not** validate radio, TLS, broker permissions or reconnection.

```powershell
cd validation
idf.py -B ../../../../build/network-validation -D IDF_TARGET=esp32 build
python run_qemu.py --qemu "PATH_TO/qemu-system-xtensa.exe"
```

The script merges only a local emulated flash file, captures the result in
the root `build/network-validation/qemu-validation.log`, and terminates QEMU after
15 seconds. This shorter build path avoids Windows compiler path limits. The expected
marker is `NETWORK_VALIDATION checks=25 failures=0 RESULT=ALL_PASS`.

On Windows, this QEMU distribution also needs `libiconv-2.dll` on `PATH`. In the
local environment, prepend `C:\Program Files\Git\mingw64\bin` to the current
shell's `PATH` before invoking the script (no machine-wide configuration change).

Build verified on 2026-09-22 with ESP-IDF 6.0.2: standalone network image 966160 B,
static DRAM 42271 B (23.39%), IRAM 87891 B (67.06%). These figures exclude runtime
Wi-Fi/TLS heap use and do not represent the combined pixel application.
The validation target also built and ran on Espressif QEMU 9.2.2: all 25 checks
passed, with no assertion or panic. No physical board or broker was accessed.

## Next integration step

Connect this component to the agreed device identity/topic configuration and
command dispatcher in the product firmware. That step includes command IDs and
expiry, application acknowledgements, HTTPS payload retrieval, state publication
and provisioning. Validate Wi-Fi loss, broker loss/restart, subscription rejection
and queue pressure against a development broker, then measure heap/stack and
render continuity with the combined firmware. These remain pending; a successful
standalone build does not establish product readiness.

Official references: [ESP-IDF 6.0.2 MQTT component migration](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/protocols/mqtt.html),
[ESP-MQTT 1.1.0](https://components.espressif.com/components/espressif/mqtt/versions/1.1.0/readme).
