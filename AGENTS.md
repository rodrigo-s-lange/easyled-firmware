# AGENTS.md

Instructions for humans and AI agents working in `easyled-firmware`.

## Read first

1. `README.md`
2. `components/pixels_control_runtime/include/pixels_control_runtime.hpp`
3. `components/pixels_command_executor/include/pixels_command_executor.hpp`
4. The task-specific component header and README section before editing it

## Repository role

This repository is the EasyLED product firmware. It owns application startup,
network orchestration, command execution, durable state, board profiles,
partition layout, recovery and OTA integration.

It consumes pinned revisions of:

- `dependencies/esp32libfun` for the ESP-IDF HAL/framework;
- `dependencies/esp_pixels` for physical pixel transport;
- `dependencies/esp_pixels_fx` for effects;
- `dependencies/esp_pixels_composer` for scene composition.

Do not copy framework or effect implementation into this repository. Make a
cohesive change in its owning repository, validate it there, then update the
submodule pin here and run the firmware validation.

## Rules

- ESP-IDF 6.0 is the baseline. Do not enable RTTI.
- Do not edit generated `sdkconfig`; use `sdkconfig.defaults` or a target-specific
  defaults file.
- Never commit credentials, bearer tokens, private keys or device certificates.
- TLS certificate and hostname verification must remain enabled.
- Do not log credentials or complete remote payloads.
- Preserve the active durable configuration on download, checksum, parse,
  persistence or activation failure.
- Emit `command_applied` only after durable persistence and confirmed activation.
- Preserve terminal command deduplication across reboot.
- Keep rendering and local recovery independent from network availability.
- Treat provisional fixtures as tests, not as a production compatibility claim.
- Update `README.md` when product behavior, dependency pins or operator steps
  change.

## Validation

Build the product target and run the focused QEMU validation before checkpointing
behavioral changes. Hardware, broker and backend tests must be reported
separately; a QEMU pass is not evidence of live service integration.
