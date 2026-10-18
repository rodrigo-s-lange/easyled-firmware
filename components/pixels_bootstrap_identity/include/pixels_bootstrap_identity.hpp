#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "nvs.h"

// Durable, per-device origin for the #47-B MQTT bootstrap identity
// (hardware_id + bootstrap secret), per docs/M0-BOOTSTRAP-IDENTITY-PROVISIONING.md.
// Application/product layer only -- no esp32libfun, platform or pixel/effect
// dependency. hardware_id derivation from the eFuse MAC is pure and testable
// with synthetic bytes; the secret store is NVS-backed, versioned, and fails
// closed on anything empty, truncated, corrupted, schema-mismatched or
// eFuse-mismatched. Production boot reads this; it never writes it -- see
// the separate bench/factory provisioning tool.

namespace pixels_bootstrap_identity {

constexpr size_t kHardwareIdBytes = 12;  // lowercase hex characters, no separators
constexpr size_t kSecretBytes = 32;      // raw bootstrap-secret bytes
constexpr size_t kSecretTextBytes = 43;  // unpadded base64url characters

/// Formats a 6-byte MAC as exactly 12 lowercase hex characters, no
/// separators -- the EasyLED `hardware_id` canonical form
/// (`hardware_bindings.hardware_id`). Pure; never touches hardware, so a
/// test can exercise it with synthetic MAC bytes.
/// @param mac 6-byte MAC address, most significant byte first.
/// @param out Receives 12 lowercase hex characters plus NUL.
void macToHardwareId(const uint8_t mac[6], char out[kHardwareIdBytes + 1]);

/// Reads this device's own base eFuse MAC and formats it as the canonical
/// `hardware_id`. The only non-pure function in this header; production
/// code calls this once at boot and never derives `hardware_id` from
/// `hardware_binding_id` or any compiled-in constant.
/// @param out Receives 12 lowercase hex characters plus NUL on success.
/// @return ESP_OK, or the underlying eFuse read error.
esp_err_t currentHardwareId(char out[kHardwareIdBytes + 1]);

/// Encodes exactly 32 bytes as 43-character unpadded base64url text (RFC
/// 4648 \S5 alphabet, `-`/`_`, no `=`) -- the canonical HTTP
/// `Authorization: Bearer` representation of the bootstrap secret. The
/// trailing partial sextet's unused low bits are always zero, so this is
/// always the canonical encoding of its input. Pure.
/// @param bytes Exactly kSecretBytes input bytes.
/// @param out Receives 43 characters plus NUL.
void encodeSecretText(const uint8_t bytes[kSecretBytes], char out[kSecretTextBytes + 1]);

/// Decodes `text` back to 32 bytes, accepting only the single canonical
/// encoding of those bytes: exactly 43 bytes, every character in the
/// base64url alphabet, no padding/whitespace/NUL, and the trailing
/// character's two unused low bits exactly zero (otherwise re-encoding the
/// decoded bytes would not reproduce `text`, which this function treats as
/// rejection, not silent normalization). Pure.
/// @param text Candidate text, not required to be NUL-terminated.
/// @param size Exact byte count of `text`; anything but kSecretTextBytes is rejected.
/// @param out Receives the decoded 32 bytes only on success; left unchanged otherwise.
/// @return ESP_OK, or ESP_ERR_INVALID_SIZE/INVALID_ARG for any non-canonical input.
esp_err_t decodeSecretText(const char *text, size_t size, uint8_t out[kSecretBytes]);

/// Lifecycle state of the durable per-device bootstrap secret record.
enum class SecretState : uint8_t {
    Empty,   ///< No record exists yet (fresh partition, or after an explicit erase).
    Valid,   ///< A verified record exists and its hardware_id matches this device's eFuse value.
    Faulted, ///< A record exists but failed validation, integrity or the eFuse cross-check.
};

/// Test-only fault-injection point inside the next durable write, mirroring
/// `pixels_mqtt_credentials::StoreTestFault`. Production code never sets
/// this away from `None`.
enum class StoreTestFault : uint8_t {
    None,
    BeforeWrite,    ///< The write is never attempted (as if the caller crashed first).
    DuringWrite,    ///< `nvs_set_blob()` is replaced by a synthetic failure.
    DuringCommit,   ///< The write runs for real; `nvs_commit()` is replaced by a synthetic failure.
    DuringReadback, ///< Write and commit run for real; the post-commit readback is forced to mismatch.
};

/// NVS-backed durable store for exactly one per-device bootstrap secret,
/// bound to this device's own `hardware_id`. Uses the dedicated `easyled`
/// partition, its own namespace. Single control-task use only.
///
/// Durability model: one self-contained record (schema + hardware_id +
/// 32-byte secret + CRC32), written with a single `nvs_set_blob()` +
/// `nvs_commit()` + readback-verify; RAM exposes the secret only once that
/// write is confirmed. A failure before an SDK write leaves the state Empty;
/// once a write has been attempted, any failure changes the in-memory state
/// to Faulted until reboot/load determines whether flash contains Empty,
/// Valid or Faulted data. On load, an empty namespace
/// is `Empty`; a present-but-invalid (wrong size, bad magic/schema, CRC
/// mismatch) or eFuse-mismatched record is `Faulted`, never silently
/// reinterpreted as `Empty` -- the caller must not provision over a record
/// it cannot explain.
class BootstrapIdentityStore {
public:
    ~BootstrapIdentityStore();

    /// Opens the namespace and loads/validates any existing record against
    /// `hardware_id` (this device's own eFuse value, supplied by the
    /// caller -- see currentHardwareId() -- never read by this class
    /// itself, so a test can inject synthetic values).
    /// @param hardware_id This device's canonical hardware_id; exactly
    ///        kHardwareIdBytes lowercase hex characters.
    /// @return ESP_OK (state() reports Empty/Valid/Faulted; a bad record is
    ///         never itself an error return), or an NVS error.
    esp_err_t init(const char hardware_id[kHardwareIdBytes + 1]);
    /// Closes the namespace; a persisted record remains.
    void end();

    SecretState state() const;
    /// This device's own hardware_id, as supplied to init(); available
    /// regardless of state().
    const char *hardwareId() const;
    /// The 43-character canonical base64url text of the secret, stable for
    /// this object's lifetime.
    /// @param out Receives 43 characters plus NUL on success only.
    /// @return ESP_OK, or ESP_ERR_INVALID_STATE when state() != Valid.
    esp_err_t secretText(char out[kSecretTextBytes + 1]) const;

    /// Durably writes a fresh secret for this device's own hardware_id
    /// (from init()). Intended for the bench/factory provisioning tool
    /// alone; production boot never calls this. Refuses to run from
    /// anything but Empty -- re-provisioning the same identity after an
    /// explicit erase is provisioning onto a fresh Empty store, never an
    /// overwrite of a live record; swapping to a different identity is out
    /// of scope for this round and must fail the same way.
    /// @param secret Exactly kSecretBytes bytes.
    /// @return ESP_OK after a verified commit+readback,
    ///         ESP_ERR_INVALID_STATE when state() != Empty, or a storage
    ///         error. A failure after an SDK write was attempted leaves the
    ///         runtime state Faulted so a retry cannot silently overwrite an
    ///         uncertain durable result; reboot/load or explicit erase is
    ///         required to resolve it.
    esp_err_t provision(const uint8_t secret[kSecretBytes]);

    /// Test-only: see StoreTestFault. Cleared back to None automatically
    /// after the next write attempt. Production code never calls this.
    void setFaultInjectionForTest(StoreTestFault fault);

private:
    esp_err_t load();

    nvs_handle_t handle_ = 0;
    bool initialized_ = false;
    SecretState state_ = SecretState::Empty;
    char hardware_id_[kHardwareIdBytes + 1]{};
    uint8_t secret_[kSecretBytes]{};
    StoreTestFault fault_for_test_ = StoreTestFault::None;
};

} // namespace pixels_bootstrap_identity
