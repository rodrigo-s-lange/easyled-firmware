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
// specifics through injected interfaces; NetworkSession itself is untouched
// by this round (see docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md, "Seção 5"/
// "Seção 7"). See that document for the full contract and state tables this
// implements, and for what still depends on platform values (trust_ref
// table, bootstrap endpoint, bootstrap token delivery) not yet provided.

namespace pixels_mqtt {

constexpr size_t kUuidBytes = 36;
constexpr size_t kMaxHardwareIdBytes = 64; // OpenAPI: "^[0-9a-f]{1,64}$"
constexpr size_t kMaxPasswordBytes = 128;
constexpr size_t kMaxBrokerUriBytes = 191;
constexpr size_t kMaxTopicBytes = 191;
constexpr size_t kMaxTrustRefBytes = 63;
constexpr size_t kMaxExpiresAtBytes = 35; // "YYYY-MM-DDTHH:MM:SS.ffffffZ" is 27
constexpr size_t kMaxResponseBytes = 2048; // generous over the ~1.3 KiB fixture

/// Validates the #47-B bootstrap path parameter `hardware_id`: lower-hex
/// only, 1 to 64 characters -- the OpenAPI contract's own pattern,
/// `^[0-9a-f]{1,64}$` (`claimability`/`https-credential`/`mqtt-credential`
/// all share it). This is the physical unit identifier used only in the
/// bootstrap request path; it is never a UUID and never
/// `hardware_binding_id` (the UUID the response body must echo back, bound
/// to the *current* hardware binding, which changes on a rebind while
/// `hardware_id` never does). Never derive one from the other.
/// @return true iff `value` is non-null and matches the pattern exactly.
bool isValidHardwareId(const char *value);

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
    /// device_id, hardware_binding_id or generation did not match what the
    /// caller (this device, from its own durable identity) expected --
    /// generation is an exact-match authority token (OpenAPI: "never an
    /// ordering comparison"), not a freshness/ordering check: a response
    /// for any generation other than the caller's own current one is
    /// rejected the same way, whether older or newer.
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
/// a `generation` that is zero or does not exactly equal `expected_generation`
/// (checked before trust_ref is ever resolved, before anything is persisted,
/// before any connection is attempted); a `credential.status` other than
/// exactly `"pending"`; an unrecognized `credential.kind`; an `activation`
/// other than exactly `"first_successful_mqtt_authentication"`;
/// `username`/`client_id` not both identical to `device_id`; an empty or
/// over-128-byte `password`; a `broker_uri` that is not exactly `wss://`
/// with no userinfo; `verify_server != true`; or an empty, over-bound, or
/// `+`/`#`-bearing topic. A consumed field that is duplicated, missing, or
/// the wrong JSON type is rejected the same way. Unknown members are
/// accepted (forward compatibility). Never allocates from the heap.
/// @param json UTF-8 response body bytes; no trailing NUL required.
/// @param size Exact byte count.
/// @param expected_device_id This device's own canonical UUID.
/// @param expected_hardware_binding_id This device's own current binding UUID.
/// @param expected_generation This device's own current hardware-binding
///        generation (OpenAPI `ClaimResponse.generation`), an exact-match
///        authority token, never zero for any real binding.
/// @param out Receives the validated response on success only.
/// @param error Receives a stable rejection category; `None` on success.
/// @return ESP_OK, or ESP_ERR_INVALID_ARG/SIZE.
esp_err_t parseDeviceCredentialResponse(const char *json, size_t size,
                                        const char *expected_device_id,
                                        const char *expected_hardware_binding_id,
                                        uint32_t expected_generation,
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
    /// This unit's physical bootstrap identifier -- the path parameter.
    /// Lower-hex, 1 to 64 characters (isValidHardwareId()); never a UUID,
    /// never `hardware_binding_id`.
    const char *hardware_id = nullptr;
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

/// Test-only fault-injection point inside the next durable write, so a test
/// can prove recovery without needing real power loss. Production code
/// never sets this away from `None`. See
/// docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md, "Seção 3 -- fault injection".
enum class StoreTestFault : uint8_t {
    None,
    /// The write is never attempted at all (as if the caller crashed
    /// before even starting it). RAM and NVS are both untouched.
    BeforeWrite,
    /// `nvs_set_blob()` for the candidate slot is skipped and replaced by a
    /// synthetic failure, as if the write itself failed partway through.
    DuringWrite,
    /// `nvs_set_blob()` runs for real, but `nvs_commit()` is replaced by a
    /// synthetic failure.
    DuringCommit,
    /// The write and commit both run for real (the data is durably on
    /// flash), but the post-commit readback comparison is forced to report
    /// a mismatch anyway -- proves this layer never trusts a write it has
    /// not itself re-read and verified, independent of whatever guarantee
    /// NVS's own commit already gives.
    DuringReadback,
};

/// NVS-backed durable store for exactly one `current` and one `pending`
/// credential, plus the state machine and attempt bookkeeping coordinating
/// their use. Uses the dedicated `easyled` partition, its own namespace.
///
/// Durability model: every transition writes one complete, self-contained
/// A/B slot record (schema + sequence + state + attempt flag + round
/// counter + both full credentials + CRC) with a single `nvs_set_blob()` +
/// `nvs_commit()` + readback, always to the slot NOT currently active, never
/// touching the other slot. RAM state is updated only after that single
/// write is itself verified, so a failure anywhere in it leaves both RAM and
/// the active slot exactly as they were -- there is never a window where a
/// not-yet-durable state is visible to a caller. On load, both slots are
/// validated independently (magic/schema/CRC, then state/credential
/// invariants) and the valid one with the higher sequence number wins; if
/// only one is valid, that one wins unconditionally; if neither is, loading
/// fails closed. This is deliberately not a separate "active slot" pointer
/// key, which would just recreate the same non-atomic window one level up.
/// Single control-task use only.
class MqttCredentialStore {
public:
    ~MqttCredentialStore();

    /// Opens the namespace and loads the winning slot (see class comment).
    /// A reboot found mid-attempt (`AttemptInFlight`) is reconciled here to
    /// `AmbiguousActivation` -- the only safe reading of "we do not know
    /// whether the broker saw it before power was lost". No partition erase
    /// or format migration is attempted.
    /// @return ESP_OK, or ESP_ERR_INVALID_RESPONSE when neither slot is
    ///         valid -- this never fabricates a credential to recover from it.
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
    /// Bounded count of fully completed rounds without a confirmed
    /// connection (a round is `pending` first, then `current` as a courtesy
    /// when one exists; with only `pending`, one ambiguous attempt against
    /// it already completes the round) since the last confirmed connection;
    /// resets to zero on `confirmConnected()`. The coordinator enters
    /// `BootstrapRecovery` once this reaches its own limit (recommended: 3
    /// *completed rounds*, not individual attempts).
    uint32_t roundsWithoutSuccess() const { return rounds_without_success_; }

    /// Persists a freshly issued credential as `pending`, before any MQTT
    /// use -- the contract's own "persist before use" rule. Never touches
    /// `current`. Resets the round counter for the new credential.
    /// @return ESP_OK after a verified single-slot commit, or a storage
    ///         error -- RAM is unchanged on any non-ESP_OK return.
    esp_err_t storePending(const StoredCredential &credential);

    /// Durably marks that the named credential is about to be handed to the
    /// transport, before the first byte reaches it. Must be called and
    /// confirmed `ESP_OK` before `NetworkSession::start()`/`init()` runs
    /// with this credential's secret.
    /// @param usingPending True to attempt `pending`; false for the
    ///        `current`-as-courtesy retry (only valid while `current` exists).
    /// @return ESP_OK after a verified single-slot commit,
    ///         ESP_ERR_INVALID_STATE if the named credential does not
    ///         exist, or a storage error -- RAM is unchanged on any
    ///         non-ESP_OK return.
    esp_err_t beginAttempt(bool usingPending);

    /// The broker's CONNACK was observed and accepted for the credential
    /// named by the most recent `beginAttempt()`. Promotes it to `current`
    /// and discards whichever credential was not just confirmed.
    /// @return ESP_OK after a verified single-slot commit, or a storage
    ///         error -- RAM is unchanged on any non-ESP_OK return.
    esp_err_t confirmConnected();

    /// The attempt failed in a way the transport proved happened strictly
    /// before any authentication could occur. Neither credential is
    /// touched or discarded. Kept for a future, genuinely evidence-backed
    /// pre-auth signal; nothing in this codebase calls it today -- see
    /// docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md, "Seção 5".
    /// @return ESP_OK after a verified single-slot commit, or a storage
    ///         error -- RAM is unchanged on any non-ESP_OK return.
    esp_err_t reportPreAuthFailure();

    /// The attempt's outcome cannot be proven either way (CONNACK lost, the
    /// broker refused the CONNECT after possibly authenticating it, a
    /// reboot mid-attempt). Never discards the credential that was in
    /// flight -- the only safe assumption is "it may already be the only
    /// valid one".
    /// @param completesRound True iff this was the last attempt of the
    ///        current round (the only attempt, when there is no `current`
    ///        to try as a courtesy; otherwise the courtesy attempt itself)
    ///        -- only then does `roundsWithoutSuccess()` advance. The
    ///        caller (the coordinator) decides this; the store only records
    ///        it atomically alongside the state transition.
    /// @return ESP_OK after a verified single-slot commit, or a storage
    ///         error -- RAM is unchanged on any non-ESP_OK return.
    esp_err_t reportAmbiguous(bool completesRound);

    /// Neither credential confirmed after the coordinator's own bounded
    /// round limit: give up on both and require a fresh bootstrap request.
    /// @return ESP_OK after a verified single-slot commit, or a storage
    ///         error -- RAM is unchanged on any non-ESP_OK return.
    esp_err_t enterBootstrapRecovery();

    /// The bootstrap route itself rejected this device (401/403): stop
    /// requesting until an external state change (new claim/rebind). Any
    /// stored credential is left exactly as it was -- a provisioning fault
    /// is not evidence that a working credential stopped working.
    /// @return ESP_OK after a verified single-slot commit, or a storage
    ///         error -- RAM is unchanged on any non-ESP_OK return.
    esp_err_t enterProvisioningFault();

    /// Discards both credentials and every attempt/round counter, returning
    /// to `Empty`. Intended for an explicit unclaim/factory-reset flow,
    /// where the device must stop trusting every credential it holds
    /// immediately.
    /// @return ESP_OK after a verified single-slot commit, or a storage
    ///         error -- RAM is unchanged on any non-ESP_OK return.
    esp_err_t eraseAll();

    /// Test-only: see StoreTestFault. Cleared back to None automatically
    /// after the next write attempt, whether or not that attempt was itself
    /// the faulted one. Production code never calls this.
    void setFaultInjectionForTest(StoreTestFault fault) { fault_for_test_ = fault; }

private:
    esp_err_t commitTransition(CredentialState new_state, bool new_attempt_uses_pending, uint32_t new_rounds,
                               const StoredCredential &new_current, const StoredCredential &new_pending);
    esp_err_t load();

    nvs_handle_t handle_ = 0;
    bool initialized_ = false;
    CredentialState state_ = CredentialState::Empty;
    StoredCredential current_{};
    StoredCredential pending_{};
    bool attempt_uses_pending_ = true;
    uint32_t rounds_without_success_ = 0;
    int active_slot_ = -1; // -1 until load() finds a winning slot; 0 or 1 after
    uint32_t next_sequence_ = 1;
    StoreTestFault fault_for_test_ = StoreTestFault::None;
    mutable uint8_t record_[3072]{};
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
        /// subject of the Authorization token. Lower-hex, 1-64 characters
        /// (isValidHardwareId()); never a UUID, never the same string as
        /// `expected_hardware_binding_id` below.
        const char *hardware_id = nullptr;
        const char *bootstrap_token = nullptr;
        /// This device's own canonical UUID and current binding UUID, used
        /// only to validate the response's `device_id`/`hardware_binding_id`
        /// actually identify this device (see parseDeviceCredentialResponse()).
        const char *expected_device_id = nullptr;
        const char *expected_hardware_binding_id = nullptr;
        /// This device's own current hardware-binding generation -- an
        /// exact-match authority token checked by the parser before trust_ref
        /// is resolved, before anything is persisted or connected with.
        uint32_t expected_generation = 0;
        uint32_t timeout_ms = 10000;
        /// Bounded fully-completed rounds (recommended: 3) before
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

    // Outcome reporting from the NetworkSession integration layer. Each
    // propagates the store's own durable-write result: on anything but
    // ESP_OK, no round/backoff bookkeeping advances and no state
    // transition is assumed to have happened -- the caller should simply
    // try reporting again (or let the next poll() retry the connection
    // attempt itself). A caller that ignores this return risks the
    // coordinator believing progress occurred when nothing was actually
    // made durable.
    esp_err_t reportConnected();
    esp_err_t reportPreAuthFailure();
    esp_err_t reportAmbiguous();

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

// --- Session transport seam (orchestration only; not wired to a real
// NetworkSession this round -- see docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md,
// "Seção 7") -----------------------------------------------------------------

/// The four MQTT-session-relevant fields of one credential, plus the
/// resolved CA, in the shape a transport needs to (re)start a session.
/// Buffers referenced here must stay alive until the next call.
struct MqttSessionIdentity {
    const char *broker_uri = nullptr;
    const char *client_id = nullptr;
    const char *username = nullptr;
    const char *password = nullptr;
    const char *command_topic = nullptr;
    const char *server_ca_pem = nullptr; // resolved via TrustResolver; never null in practice
};

/// Minimal seam a session controller drives credential swaps through --
/// small enough for a production NetworkSession to satisfy via a thin
/// adapter, and for a test to fake outright with no real Wi-Fi/MQTT stack.
/// Deliberately has no "PreAuthFailure" outcome: see Seção 5 of the doc for
/// why no implementation of this seam can honestly produce one today.
class MqttTransportSession {
public:
    enum class Outcome {
        StillTrying, ///< No conclusive event yet; call poll() again later.
        Connected,   ///< A real CONNACK was accepted for the current identity.
        Ambiguous,   ///< Anything else that is not "still trying" -- see Seção 5.
    };

    virtual ~MqttTransportSession() = default;
    /// Ends whatever session is running (safe if none), then starts a new
    /// one with `identity`.
    /// @return ESP_OK once the new session has been asked to start, or an
    ///         immediate setup error (never proof of a connection outcome).
    virtual esp_err_t restart(const MqttSessionIdentity &identity) = 0;
    /// Ends the current session, if any. Idempotent.
    virtual void end() = 0;
    /// Non-blocking outcome sample for the session started by the most
    /// recent restart().
    virtual Outcome poll() = 0;
};

/// Drives one #47-B credential swap at a time: asks the coordinator what to
/// do, durably marks the attempt before touching the transport, restarts
/// the transport with the resolved identity, samples its outcome, and
/// reports that outcome back to the coordinator durably. Fully testable
/// against a fake MqttTransportSession and the coordinator's own fakes --
/// see validation_mqtt_credentials/. Not wired to a real NetworkSession by
/// main.cpp this round (Seção 7): doing so safely requires resolving how a
/// product boot brings up Wi-Fi before any MQTT identity exists, which
/// NetworkSession::init() requires eagerly today, and that is a
/// NetworkSession-level question, not a credential-lifecycle one.
class MqttSessionController {
public:
    MqttSessionController(MqttCredentialCoordinator &coordinator, MqttCredentialStore &store, TrustResolver &trust,
                          MqttTransportSession &transport);

    /// Call once per control-loop tick. Never blocks.
    /// @return ESP_OK, or the first durable-store write failure encountered
    ///         -- the caller should simply call tick() again later; nothing
    ///         is left half-applied either in the store or in `transport`.
    esp_err_t tick();

    /// True once the credential this controller is currently using has an
    /// accepted CONNACK for this tick.
    bool ready() const { return ready_; }

private:
    MqttCredentialCoordinator &coordinator_;
    MqttCredentialStore &store_;
    TrustResolver &trust_;
    MqttTransportSession &transport_;
    bool attempt_active_ = false;
    bool ready_ = false;
};

} // namespace pixels_mqtt
