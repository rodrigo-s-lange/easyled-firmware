#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "nvs.h"

// Application-owned foundation for the #47-B device MQTT credential
// lifecycle: parses the device-bootstrap response, resolves its trust_ref to
// a CA, persists the current/pending credential pair durably across reboot,
// requests new ones over HTTPS, and decides -- deterministically, testable
// against fakes -- what to do next. Independent of MQTT/HTTP transport
// specifics through injected interfaces; NetworkSession itself is touched
// only by the smallest seam documented in network_session.hpp. See
// docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md for the full contract and state
// tables this implements, and for what still depends on platform values
// (trust_ref table, bootstrap endpoint, bootstrap token delivery) not yet
// provided.

namespace pixels_mqtt {

constexpr size_t kUuidBytes = 36;
constexpr size_t kMaxPasswordBytes = 128;
constexpr size_t kMaxBrokerUriBytes = 191;
constexpr size_t kMaxTopicBytes = 191;
constexpr size_t kMaxTrustRefBytes = 63;
constexpr size_t kMaxExpiresAtBytes = 35; // "YYYY-MM-DDTHH:MM:SS.ffffffZ" is 27
constexpr size_t kMaxResponseBytes = 2048; // generous over the ~1.3 KiB fixture

enum class CredentialKind : uint8_t { InitialProvisioning, Rotation };

enum class ParseError : uint8_t {
    None,
    InvalidSize,
    /// Invalid JSON, a wrong member type, a consumed duplicate field, a
    /// missing required field, or a value outside what this firmware
    /// accepts (empty/over-length password, non-wss broker_uri, userinfo in
    /// the URI, verify_server != true, empty/over-bound/wildcard topic,
    /// status other than exactly "pending", unrecognized kind/activation).
    Malformed,
    /// device_id or hardware_binding_id did not match what the caller
    /// (this device, from its own durable identity) expected.
    IdentityMismatch,
};

/// One fully validated #47-B device-bootstrap response. Bounded, owned
/// fields only -- never pointers into the input buffer. Does not resolve
/// `trust_ref` to a CA; that is TrustResolver's job, injected separately, so
/// this parser never depends on the trust table.
struct DeviceCredentialResponse {
    char device_id[kUuidBytes + 1]{};
    char hardware_binding_id[kUuidBytes + 1]{};
    uint32_t generation = 0;
    char credential_id[kUuidBytes + 1]{};
    CredentialKind kind = CredentialKind::InitialProvisioning;
    char expires_at[kMaxExpiresAtBytes + 1]{}; // advisory metadata only; see contract
    char username[kUuidBytes + 1]{};           // == client_id == device_id
    char client_id[kUuidBytes + 1]{};
    char password[kMaxPasswordBytes + 1]{};
    char broker_uri[kMaxBrokerUriBytes + 1]{}; // wss:// only, no userinfo
    char trust_ref[kMaxTrustRefBytes + 1]{};
    char topic_state[kMaxTopicBytes + 1]{};
    char topic_event[kMaxTopicBytes + 1]{};
    char topic_telemetry[kMaxTopicBytes + 1]{};
    char topic_cmd[kMaxTopicBytes + 1]{};
};

/// Parses and validates one complete #47-B device-bootstrap response body.
/// Rejects anything this firmware cannot safely act on: non-UUID identities;
/// an identity not matching the caller's own device_id/hardware_binding_id;
/// `generation == 0`; a `credential.status` other than exactly `"pending"`;
/// an unrecognized `credential.kind`; an `activation` other than exactly
/// `"first_successful_mqtt_authentication"`; `username`/`client_id` not both
/// identical to `device_id`; an empty or over-128-byte `password`; a
/// `broker_uri` that is not exactly `wss://` with no userinfo;
/// `verify_server != true`; or an empty, over-bound, or `+`/`#`-bearing
/// topic. A consumed field that is duplicated, missing, or the wrong JSON
/// type is rejected the same way. Unknown members are accepted (forward
/// compatibility). Never allocates from the heap.
/// @param json UTF-8 response body bytes; no trailing NUL required.
/// @param size Exact byte count.
/// @param expected_device_id This device's own canonical UUID.
/// @param expected_hardware_binding_id This device's own current binding UUID.
/// @param out Receives the validated response on success only.
/// @param error Receives a stable rejection category; `None` on success.
/// @return ESP_OK, or ESP_ERR_INVALID_ARG/SIZE.
esp_err_t parseDeviceCredentialResponse(const char *json, size_t size,
                                        const char *expected_device_id,
                                        const char *expected_hardware_binding_id,
                                        DeviceCredentialResponse &out, ParseError &error);

// --- Trust resolver ----------------------------------------------------------

/// Resolves a `trust_ref` identifier to the CA material this firmware must
/// already hold for it -- the CA is never delivered by the bootstrap
/// response itself. An unrecognized `trust_ref` fails closed: there is no
/// fallback CA and no insecure mode, ever.
class TrustResolver {
public:
    virtual ~TrustResolver() = default;
    /// @param trust_ref Opaque identifier from a credential response.
    /// @param ca_pem Receives a stable pointer to PEM CA text on success only.
    /// @return ESP_OK, or ESP_ERR_NOT_FOUND for an unrecognized trust_ref.
    virtual esp_err_t resolve(const char *trust_ref, const char **ca_pem) const = 0;
};

/// Fixed, firmware-shipped `trust_ref` -> CA PEM table. The real
/// per-environment values are not fixed yet (platform/firmware agreement
/// pending -- see docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md); an empty or
/// incomplete table simply makes the missing `resolve()` calls fail closed,
/// which is the correct behavior until that agreement lands, never a reason
/// to add a fallback CA.
class TableTrustResolver final : public TrustResolver {
public:
    struct Entry {
        const char *trust_ref;
        const char *ca_pem;
    };
    /// @param entries Table that must outlive this resolver.
    /// @param count Number of entries.
    TableTrustResolver(const Entry *entries, size_t count);
    esp_err_t resolve(const char *trust_ref, const char **ca_pem) const override;

private:
    const Entry *entries_;
    size_t count_;
};

// --- HTTPS bootstrap client ---------------------------------------------------

/// One bounded `POST .../device-bootstrap/{hardware_id}/mqtt-credential`
/// request. The route takes no body; only the URL and Authorization header
/// carry information.
struct BootstrapRequest {
    const char *base_url = nullptr;        // e.g. "https://api.example.invalid"
    const char *hardware_id = nullptr;     // canonical UUID, appended to the path
    const char *bootstrap_token = nullptr; // Bearer token; never logged
    const char *ca_pem = nullptr;          // null selects the ESP-IDF certificate bundle
    uint32_t timeout_ms = 10000;
    char *destination = nullptr; // receives the exact response body
    size_t capacity = 0;
};

/// Metadata available only once a response is complete (never partial).
struct BootstrapResult {
    int status_code = 0;
    size_t size = 0;
    bool has_retry_after = false;
    uint32_t retry_after_s = 0;
};

/// Injectable HTTPS boundary for the bootstrap route. Implementations must
/// never log the Authorization header or the response body.
class MqttBootstrapClient {
public:
    virtual ~MqttBootstrapClient() = default;
    /// @param request URL, credential, trust source, timeout and destination.
    /// @param result Receives status, byte count and Retry-After when present.
    /// @return ESP_OK after a complete HTTP exchange, or a transport/size error.
    virtual esp_err_t requestCredential(const BootstrapRequest &request, BootstrapResult &result) = 0;
};

/// ESP-IDF HTTPS implementation: `POST` with an empty body, mandatory TLS
/// verification, no redirects, bounded response collection. Owns no retry or
/// backoff of its own -- the coordinator (section 5) owns that. Single
/// control-task use only.
class EspMqttBootstrapClient final : public MqttBootstrapClient {
public:
    esp_err_t requestCredential(const BootstrapRequest &request, BootstrapResult &result) override;

private:
    static constexpr size_t kAuthorizationBytes = 320;
    static constexpr size_t kUrlBytes = 320;
    char authorization_[kAuthorizationBytes]{};
    char url_[kUrlBytes]{};
};

// --- Durable credential store -------------------------------------------------

/// Lifecycle state of this device's MQTT credential pair, persisted across
/// reboot. See docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md for the full transition
/// table.
enum class CredentialState : uint8_t {
    Empty,              ///< No usable credential at all; must bootstrap.
    CurrentConfirmed,    ///< `current` has authenticated at least once.
    PendingUnused,        ///< `pending` persisted, never yet handed to transport.
    AttemptInFlight,      ///< `pending` (or `current`, as a courtesy) durably
                          ///< marked as handed to transport, outcome not yet known.
    AmbiguousActivation,  ///< An attempt was interrupted with no proof either way.
    BootstrapRecovery,    ///< Neither credential authenticated after bounded retries.
    ProvisioningFault,    ///< The bootstrap route itself rejected this device.
};

/// One durable credential record. Never logged or printed in full -- the
/// password field in particular must never reach `ESP_LOG*` or any status
/// API, including this component's own.
struct StoredCredential {
    bool valid = false;
    char credential_id[kUuidBytes + 1]{};
    char hardware_binding_id[kUuidBytes + 1]{};
    uint32_t generation = 0;
    char username[kUuidBytes + 1]{};
    char client_id[kUuidBytes + 1]{};
    char password[kMaxPasswordBytes + 1]{};
    char broker_uri[kMaxBrokerUriBytes + 1]{};
    char trust_ref[kMaxTrustRefBytes + 1]{};
    char topic_state[kMaxTopicBytes + 1]{};
    char topic_event[kMaxTopicBytes + 1]{};
    char topic_telemetry[kMaxTopicBytes + 1]{};
    char topic_cmd[kMaxTopicBytes + 1]{};
    char expires_at[kMaxExpiresAtBytes + 1]{}; // metadata only, see contract
};

/// NVS-backed durable store for exactly one `current` and one `pending`
/// credential, plus the state machine and attempt bookkeeping coordinating
/// their use. Uses the dedicated `easyled` partition, its own namespace,
/// CRC32 + readback on every commit -- the same discipline as
/// `pixels_durable_storage`. Single control-task use only.
class MqttCredentialStore {
public:
    ~MqttCredentialStore();

    /// Opens the namespace. A reboot found mid-attempt (`AttemptInFlight`)
    /// is reconciled here to `AmbiguousActivation` -- the only safe reading
    /// of "we do not know whether the broker saw it before power was lost".
    /// No partition erase or format migration is attempted.
    /// @return ESP_OK, or ESP_ERR_INVALID_RESPONSE on detected corruption --
    ///         this never fabricates a credential to recover from it.
    esp_err_t init();
    /// Closes the namespace; persisted records remain.
    void end();

    CredentialState state() const { return state_; }
    bool hasCurrent() const { return current_.valid; }
    bool hasPending() const { return pending_.valid; }
    const StoredCredential &current() const { return current_; }
    const StoredCredential &pending() const { return pending_; }
    /// True once the in-flight attempt was against `pending`; false means
    /// the courtesy retry against `current` is the one in flight. Only
    /// meaningful while `state() == AttemptInFlight`.
    bool attemptUsesPending() const { return attempt_uses_pending_; }
    /// Bounded count of individual ambiguous connection attempts (against
    /// `pending` or the `current`-as-courtesy retry alike) since the last
    /// confirmed connection; resets to zero on `confirmConnected()`. The
    /// coordinator enters `BootstrapRecovery` once this reaches its own
    /// limit (recommended: 3) -- it counts attempts, not full
    /// pending-then-courtesy cycles.
    uint32_t roundsWithoutSuccess() const { return rounds_without_success_; }

    /// Persists a freshly issued credential as `pending`, before any MQTT
    /// use -- the contract's own "persist before use" rule. Never touches
    /// `current`. Resets the round counter for the new credential.
    /// @return ESP_OK after commit + readback, or a storage error.
    esp_err_t storePending(const StoredCredential &credential);

    /// Durably marks that the named credential is about to be handed to the
    /// transport, before the first byte reaches it. Must be called and
    /// confirmed `ESP_OK` before `NetworkSession::start()`/`init()` runs
    /// with this credential's secret.
    /// @param usingPending True to attempt `pending`; false for the
    ///        `current`-as-courtesy retry (only valid while `current` exists).
    /// @return ESP_OK after commit + readback, ESP_ERR_INVALID_STATE if the
    ///         named credential does not exist, or a storage error.
    esp_err_t beginAttempt(bool usingPending);

    /// The broker's CONNACK was observed and accepted for the credential
    /// named by the most recent `beginAttempt()`. Promotes it to `current`
    /// and discards whichever credential was not just confirmed.
    /// @return ESP_OK after commit + readback, or a storage error.
    esp_err_t confirmConnected();

    /// The attempt failed in a way the transport proved happened strictly
    /// before any authentication could occur (the TLS handshake itself
    /// failed). Neither credential is touched or discarded; only the round
    /// bookkeeping advances, because this outcome is not ambiguous.
    /// @return ESP_OK after commit + readback, or a storage error.
    esp_err_t reportPreAuthFailure();

    /// The attempt's outcome cannot be proven either way (CONNACK lost, the
    /// broker refused the CONNECT after possibly authenticating it, a
    /// reboot mid-attempt). Never discards the credential that was in
    /// flight -- the only safe assumption is "it may already be the only
    /// valid one".
    /// @return ESP_OK after commit + readback, or a storage error.
    esp_err_t reportAmbiguous();

    /// Neither credential confirmed after the coordinator's own bounded
    /// round limit: give up on both and require a fresh bootstrap request.
    /// @return ESP_OK after commit + readback, or a storage error.
    esp_err_t enterBootstrapRecovery();

    /// The bootstrap route itself rejected this device (401/403): stop
    /// requesting until an external state change (new claim/rebind). Any
    /// stored credential is left exactly as it was -- a provisioning fault
    /// is not evidence that a working credential stopped working.
    /// @return ESP_OK after commit + readback, or a storage error.
    esp_err_t enterProvisioningFault();

    /// Discards both credentials and every attempt/round counter, returning
    /// to `Empty`, without closing the NVS namespace handle or touching the
    /// partition's own init state (unlike a `end()`+`nvs_flash_erase_partition()`
    /// +`init()` cycle, which does both and is reserved for tests that mean
    /// to simulate an actual reboot). Intended for an explicit unclaim/
    /// factory-reset flow, where the device must stop trusting every
    /// credential it holds immediately.
    /// @return ESP_OK after commit + readback, or a storage error.
    esp_err_t eraseAll();

private:
    esp_err_t commitAndVerify();
    esp_err_t load();

    nvs_handle_t handle_ = 0;
    bool initialized_ = false;
    CredentialState state_ = CredentialState::Empty;
    StoredCredential current_{};
    StoredCredential pending_{};
    bool attempt_uses_pending_ = true;
    uint32_t rounds_without_success_ = 0;
    mutable uint8_t record_[2048]{};
};

// --- Monotonic clock ----------------------------------------------------------

/// Monotonic microsecond clock, injected so backoff/round timing never
/// depends on NTP or the wall clock, and so tests can control time without
/// waiting.
class MonotonicClock {
public:
    virtual ~MonotonicClock() = default;
    virtual int64_t nowUs() const = 0;
};

/// `esp_timer_get_time()`-backed production clock.
class EspMonotonicClock final : public MonotonicClock {
public:
    int64_t nowUs() const override;
};

// --- Recovery coordinator -----------------------------------------------------

/// What the coordinator wants the caller to do next. The coordinator itself
/// never touches HTTP or `NetworkSession` -- the caller (section 6/7's
/// integration layer) is the only thing that does, so the coordinator stays
/// testable against fakes alone.
enum class CoordinatorAction : uint8_t {
    Idle,                        ///< Nothing to do now; poll again later.
    RequestBootstrap,            ///< Call the bootstrap route now.
    ConnectWithPending,          ///< Attempt MQTT with `pending`.
    ConnectWithCurrentCourtesy,  ///< Ambiguous retries on `pending` exhausted
                                 ///< this round; try `current` once, as a courtesy.
};

/// Deterministic #47-B recovery logic, fully testable against fake
/// store/client/trust/clock interfaces -- see
/// docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md for the transition table this
/// implements. Owns no HTTP call and no `NetworkSession` call itself.
class MqttCredentialCoordinator {
public:
    struct Config {
        const char *base_url = nullptr;
        /// This unit's bootstrap identity -- the path parameter and the
        /// subject of the Authorization token. Not necessarily the same
        /// string as the device UUID the response must echo back.
        const char *hardware_id = nullptr;
        const char *bootstrap_token = nullptr;
        /// This device's own canonical UUID and current binding UUID, used
        /// only to validate the response's `device_id`/`hardware_binding_id`
        /// actually identify this device (see parseDeviceCredentialResponse()).
        const char *expected_device_id = nullptr;
        const char *expected_hardware_binding_id = nullptr;
        uint32_t timeout_ms = 10000;
        /// Bounded individual connection attempts (recommended: 3) before
        /// BootstrapRecovery -- see MqttCredentialStore::roundsWithoutSuccess().
        uint32_t max_rounds_before_recovery = 3;
        uint32_t initial_backoff_ms = 5000;
        uint32_t max_backoff_ms = 900000; // 15 min
    };

    MqttCredentialCoordinator(MqttCredentialStore &store, MqttBootstrapClient &client, TrustResolver &trust,
                              MonotonicClock &clock, const Config &config);

    /// Drives one step: may call the bootstrap client (never MQTT). Call at
    /// least every few seconds from the application task.
    /// @return The action the caller must now perform; `Idle` if none.
    CoordinatorAction poll();

    /// The credential to use for `ConnectWithPending`/
    /// `ConnectWithCurrentCourtesy` -- valid only right after `poll()`
    /// returns one of those actions.
    const StoredCredential &credentialToTry() const;

    // Outcome reporting from the NetworkSession integration layer.
    void reportConnected();
    void reportPreAuthFailure();
    void reportAmbiguous();

    /// Honors a `Retry-After` from a `429`, or applies the configured
    /// exponential backoff with jitter otherwise.
    void scheduleNextAttempt(bool have_retry_after, uint32_t retry_after_s);

private:
    esp_err_t doBootstrap();

    MqttCredentialStore &store_;
    MqttBootstrapClient &client_;
    TrustResolver &trust_;
    MonotonicClock &clock_;
    Config config_;
    int64_t next_action_at_us_ = 0;
    uint32_t backoff_ms_ = 0;
    /// Transient (not persisted -- a reboot safely restarts the round at
    /// "try pending first", which the contract always allows anyway): true
    /// once `pending` has been tried during the current ambiguous round, so
    /// the next poll() offers the `current`-as-courtesy action instead of
    /// retrying `pending` a second time in the same round.
    bool pending_tried_this_round_ = false;
    static constexpr size_t kResponseBytes = kMaxResponseBytes;
    char response_buffer_[kResponseBytes]{};
};

} // namespace pixels_mqtt
