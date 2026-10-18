# M0 — firmware inputs requested by the backend

Date: 2026-10-18 (hardware_id/provisioning section updated 2026-10-18, same day)  
Firmware reference when prepared: `easyled-firmware/main` at `b8b6571`;
hardware_id derivation and the bootstrap-secret store landed afterward, on
`feat/m0-bootstrap-identity-provisioning`  
Status: the firmware-side mechanism described below is implemented and
validated (QEMU + a Python/C++ cross-tool check); two environment values
(deployed bootstrap URL, real `trust_ref`/CA) remain pending before a live
integration, plus the backend-side steps in Section 5.

## 1. Physical `hardware_id`

The canonical identifier is the ESP32 base eFuse MAC, rendered as exactly 12
lowercase hexadecimal characters with no separators. This follows the EasyLED
control contract (`hardware_bindings.hardware_id`) and is distinct from both
`device_id` and the rebindable UUID `hardware_binding_id`.

Firmware derives it at boot with `esp_efuse_mac_get_default()`
(`pixels_bootstrap_identity::currentHardwareId()`,
`components/pixels_bootstrap_identity`) and formats the six bytes as
lowercase hex. It is never compiled as a board-specific constant and never
derived from `hardware_binding_id`.

The confirmed M0 physical unit's eFuse MAC is `14:2b:2f:dc:fa:5c`, so its
`hardware_id` is:

```text
142b2fdcfa5c
```

(Read non-destructively with `esptool read-mac` once the board's serial port
is available; see `docs/M0-BOOTSTRAP-IDENTITY-PROVISIONING.md` for the full
provisioning procedure that consumes this value.)

## 2. Bootstrap secret provisioning and persistence

### Current implementation state

Implemented: `components/pixels_bootstrap_identity` (a durable, versioned,
per-device NVS record validated against the eFuse `hardware_id`, fail-closed
on anything empty/corrupt/schema-mismatched/eFuse-mismatched) and
`tools/provision_bootstrap_identity.py` (the offline bench/factory tool that
writes it, never through any MQTT command or other operational-firmware
API). `ApplicationConfig` no longer carries `mqtt_hardware_id` or
`mqtt_bootstrap_token` at all -- both now come exclusively from this store and
the eFuse read. Full record layout, provisioning procedure and the
reflash/erase/power-loss matrix: `docs/M0-BOOTSTRAP-IDENTITY-PROVISIONING.md`.

The `easyled` NVS partition is durable but is not encrypted in the current
build. Secure boot and flash encryption are also disabled. The current image
must not be described as field-secure secret storage -- confidentiality at
rest depends on physical protection of the board until encrypted secret-at-rest
storage (NVS encryption backed by device keys, with flash encryption and
secure boot policy closed and tested) is taken up as separate, deliberately
irreversible manufacturing security work, not part of this round.

### Representation (implemented exactly as recommended)

- One independent high-entropy bootstrap secret per physical unit: 32 random
  bytes, operational representation unpadded base64url, exactly 43 ASCII
  characters, `Authorization: Bearer <43 chars>`.
- Backend should store only the raw 32-byte SHA-256 digest **of those 43 ASCII
  bytes** (not of the 32 binary bytes), matching
  `device_bootstrap_identities.bootstrap_secret_hash`. Closed public test
  vector (never an operational value):
  `AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8` →
  `ea866a757e4c38babfa8127cbe9a409d3e1f93a00ff1488ff735fcf917afffd0`.
- Never reuse or derive the claim secret, HTTPS device token, or MQTT password
  from this bootstrap secret.
- The plaintext secret is provisioned through the per-device NVS image/tool
  above, over the physical manufacturing/bench connection, into its own
  namespace (`easyled_boot`) in the existing `easyled` NVS partition. The
  firmware reads it into bounded RAM at boot and never logs it.
- The real secret is never compiled into source, committed, exposed through
  diagnostics, or returned from an API -- audited by code inspection and
  enforced structurally (the tool never prints it; the firmware's own log
  lines name `hardware_id`, never the secret).

This firmware-side mechanism does not need a live backend to validate (QEMU +
the tool's own `--self-test`, both passing); it is a required precondition
for provisioning the live M0 unit, now satisfied on the firmware side.

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

Backend and firmware synthetic work can proceed without the real CA; the
board's eFuse MAC is now confirmed (Section 1). Before the coordinated live
M0 window, the teams must close:

1. ~~read the board's actual eFuse MAC~~ -- done, `142b2fdcfa5c` (Section 1);
   seed the matching manufacturing row with it;
2. generate the unique bootstrap secret and store only its SHA-256 digest (of
   the 43-character text, Section 2) in the backend; provision the plaintext
   onto the physical unit with `tools/provision_bootstrap_identity.py`
   (firmware-side mechanism implemented, Section 2 /
   `docs/M0-BOOTSTRAP-IDENTITY-PROVISIONING.md` -- generating/registering the
   actual secret value is still a backend/process step, not a firmware one);
3. publish the environment bootstrap base URL;
4. publish the final MQTT WSS `trust_ref` and public-bundle/private-CA mapping;
5. provision the firmware profile without placing any secret in Git (the
   profile no longer needs `mqtt_hardware_id`/`mqtt_bootstrap_token` at all --
   both now come from the eFuse read and the NVS store instead).

M0 proves one-device technical integration. Rebind/revocation and the complete
customer-facing MVP remain separate gates.
