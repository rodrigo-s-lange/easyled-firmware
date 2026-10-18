# M0 — firmware inputs requested by the backend

Date: 2026-10-18  
Firmware reference when prepared: `easyled-firmware/main` at `b8b6571`  
Status: sufficient for synthetic provisioning work; two environment values
remain pending before a live integration.

## 1. Physical `hardware_id`

The canonical identifier is the ESP32 base eFuse MAC, rendered as exactly 12
lowercase hexadecimal characters with no separators. This follows the EasyLED
control contract (`hardware_bindings.hardware_id`) and is distinct from both
`device_id` and the rebindable UUID `hardware_binding_id`.

Firmware should derive it at boot with `esp_efuse_mac_get_default()` and format
the six bytes as lowercase hex. It must not be compiled as a board-specific
constant or derived from `hardware_binding_id`.

The value of the current laboratory board is still pending. On 2026-10-18 the
previous COM6 port was absent and the stale COM16 entry could not be opened, so
no MAC value was guessed. Reading it with `esptool read-mac` is non-destructive
once the board is connected and the serial port is available.

## 2. Bootstrap secret provisioning and persistence

### Current implementation state

There is no production bootstrap-secret store yet. `ApplicationConfig` exposes
`mqtt_bootstrap_token`, but its only concrete source is an optional
`main/network_credentials.hpp` excluded from Git. No such file exists in the
repository, and provisioned mode therefore has no real secret today.

The `easyled` NVS partition is durable but is not encrypted in the current
build. Secure boot and flash encryption are also disabled. The current image
must not be described as field-secure secret storage.

### Recommended implementation direction

- Generate one independent high-entropy bootstrap secret per physical unit.
  Recommended representation: 32 random bytes encoded as unpadded base64url
  (43 characters).
- Backend stores only the raw 32-byte SHA-256 digest, matching
  `device_bootstrap_identities.bootstrap_secret_hash`.
- Never reuse or derive the claim secret, HTTPS device token, or MQTT password
  from this bootstrap secret.
- For the controlled M0 unit, provision the plaintext secret through a
  per-device NVS image/tool over the physical manufacturing/bench connection,
  into a dedicated namespace in the existing `easyled` NVS partition. The
  firmware reads it into bounded RAM at boot and never logs it.
- Do not compile the real secret into source, commit it, expose it through
  diagnostics, or return it from an API.
- Before field deployment, enable encrypted secret-at-rest storage as part of
  the manufacturing security work: NVS encryption backed by device keys, with
  flash encryption and secure boot policy closed and tested. Those eFuse-level
  operations are deliberately separate because they can be irreversible.

The exact NVS namespace/key and provisioning CLI are not implemented yet. This
does not block the current synthetic MQTT-session integration, but it is a
required firmware task before the live M0 unit is provisioned.

## 3. TLS trust anchors

There are two distinct TLS paths.

### Bootstrap and configuration HTTPS

Current firmware uses the ESP-IDF full certificate bundle and hostname
verification when no explicit PEM is supplied. The bootstrap client currently
sets no private CA (`ca_pem=nullptr`) and therefore uses that bundle. The live
EasyLED HTTPS endpoint should present a chain rooted in that public bundle.

### MQTT over WSS

The #47-B response carries `tls.trust_ref`; firmware resolves that opaque value
against a firmware-shipped allowlist before starting MQTT. Unknown values fail
closed. The bootstrap response never supplies CA bytes.

The real environment mapping remains a platform input:

- if WSS uses a publicly trusted certificate, define a stable `trust_ref` that
  explicitly selects the ESP-IDF public bundle;
- if it uses a private CA, provide the stable `trust_ref` name and its CA PEM
  for embedding in the firmware profile.

No insecure mode or hostname-verification bypass is acceptable. The backend
must still provide the environment's final `trust_ref` decision before live
integration; synthetic tests use a synthetic resolver/CA.

## 4. Laboratory physical profile

Confirmed physical bench:

- active output: logical slot 0 on GPIO16;
- 372 WS2812B-compatible pixels, GRB order;
- canvas: 31 columns × 12 rows;
- serpentine wiring by columns;
- pixel 0: lower-left corner;
- column 0 travels upward, column 1 downward, alternating thereafter;
- pixel 371: upper-right corner;
- GPIO17/slot 1 is physically disconnected and must not appear in the M0
  configuration payload, although the compiled board profile still supports
  that second logical slot with safe boot count zero.

Using coordinates `x=0..30` from left to right and `y=0..11` from bottom to
top, the physical index is:

```text
even x: index = 12*x + y
odd  x: index = 12*x + (11-y)
```

The remote payload owns the ledmap table. Firmware validates and applies it; it
does not infer serpentine orientation from the board profile.

## 5. What the backend can do now

Backend and firmware synthetic work can proceed without the board MAC or real
CA. Before the coordinated live M0 window, the teams must close:

1. read the board's actual eFuse MAC and seed the matching manufacturing row;
2. generate/provision the unique bootstrap secret and store only its SHA-256
   digest in the backend;
3. publish the environment bootstrap base URL;
4. publish the final MQTT WSS `trust_ref` and public-bundle/private-CA mapping;
5. provision the firmware profile without placing any secret in Git.

M0 proves one-device technical integration. Rebind/revocation and the complete
customer-facing MVP remain separate gates.
