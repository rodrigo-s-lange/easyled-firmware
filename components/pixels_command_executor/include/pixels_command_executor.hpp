#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "firmware_contract.hpp"
#include "pixels_payload_adapter.hpp"

// Synchronous, application-layer executor for the provisional EasyLED
// command contract. Turns a valid `apply_config`/`activate_preset` envelope
// and its exact payload bytes into a validated pixel candidate (via
// pixels_payload_adapter), drives pixels_firmware_contract's own durable
// transaction interfaces (TerminalJournal/DurableStaging) in the required
// order, and emits the expected events -- independent of MQTT callbacks,
// HTTP clients, clock selection and the final NVS key/capacity policy
// through the narrow interfaces below. Call only from the application/
// control task, never from an MQTT callback or the render role.

namespace pixels_executor {

/// Resolves exact payload bytes for a command's own resource (config URL/
/// version, or preset UUID) and related checksum metadata. For
/// `apply_config`, `command.checksum` remains authoritative and the returned
/// digest cannot replace it. For `activate_preset` (which carries no checksum
/// of its own) this is the source of the expected digest. The interface always
/// fills the digest for a uniform bounded API.
class PayloadSource {
public:
    /// Destroys the payload-source interface.
    virtual ~PayloadSource() = default;
    /// @param command Parsed command owning this resolution.
    /// @param buffer Destination for exact resolved bytes.
    /// @param capacity Destination capacity.
    /// @param size Receives resolved byte count on success only.
    /// @param expected_checksum Receives the digest these bytes must match
    ///        on success only.
    /// @return ESP_OK, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_SIZE, or a
    ///         transport-specific error.
    virtual esp_err_t resolve(const pixels_firmware::Command &command, char *buffer, size_t capacity,
                              size_t &size, uint8_t expected_checksum[pixels_firmware::kSha256Bytes]) = 0;
};

/// Publishes exact serialized event bytes for delivery. Enqueue failure
/// never undoes an already committed/applied command -- see Executor's own
/// doc.
class EventSink {
public:
    /// Destroys the event-sink interface.
    virtual ~EventSink() = default;
    /// @param event_json Complete, NUL-terminated serialized event.
    /// @param size Byte count, excluding the trailing NUL.
    /// @return ESP_OK once accepted for publication, or a transport error.
    virtual esp_err_t publish(const char *event_json, size_t size) = 0;
};

/// Supplies each event's own timestamp, in the exact wire format
/// pixels_firmware::serializeEvent() validates (RFC3339, trailing `Z`).
class TimestampSource {
public:
    /// Destroys the timestamp-source interface.
    virtual ~TimestampSource() = default;
    /// @param out Receives a NUL-terminated timestamp; must fit 35 chars + NUL.
    virtual void now(char out[36]) = 0;
};

/// Reports whether the currently active target already matches a preset
/// activation request, allowing the required successful no-op without
/// restarting animation.
class ActiveTargetProbe {
public:
    /// Destroys the active-target probe interface.
    virtual ~ActiveTargetProbe() = default;
    /// @param preset_id Requested preset UUID.
    /// @param checksum Resolved expected checksum for that preset's own payload.
    /// @return true when the live target already matches both exactly.
    virtual bool alreadyActive(const char *preset_id,
                               const uint8_t checksum[pixels_firmware::kSha256Bytes]) const = 0;
};

/// Compact, stable, provisional reason codes this executor may emit through
/// `command_rejected`/`command_failed`, beyond whatever
/// `pixels_firmware::parseErrorCode()`/`pixels_payload::schemaErrorCode()`
/// already return verbatim for envelope/payload rejections.
namespace ReasonCode {
constexpr const char *kChecksumMismatch = "checksum_mismatch"; // reused exactly, per the task brief
constexpr const char *kResolveFailed = "resolve_failed";
constexpr const char *kStagingFailed = "staging_failed";
constexpr const char *kActivationFailed = "activation_failed";
constexpr const char *kTerminalStoreFailed = "terminal_store_failed";
constexpr const char *kRollbackFailed = "rollback_failed";
} // namespace ReasonCode

/// Synchronous command executor. Owns no task, no queue, no MQTT/HTTP
/// knowledge -- one `execute()` call fully resolves one command message.
class Executor {
public:
    /// Constructs an executor over application-owned transport and durability boundaries.
    /// @param journal Durable terminal-command journal (application-owned).
    /// @param staging Durable payload staging/activation transaction (application-owned).
    /// @param payload_source Resolves exact bytes for a command's own resource.
    /// @param event_sink Publishes serialized event bytes.
    /// @param timestamp_source Supplies each event's own timestamp.
    /// @param active_probe Reports whether a requested preset is already active.
    /// @param device_id This device's own canonical UUID, stamped into every
    ///        emitted event; must outlive the executor.
    Executor(pixels_firmware::TerminalJournal &journal, pixels_firmware::DurableStaging &staging,
             PayloadSource &payload_source, EventSink &event_sink, TimestampSource &timestamp_source,
             ActiveTargetProbe &active_probe, const char *device_id);

    /// Executes one complete, non-retained command message end to end.
    ///
    /// Step order (task brief's own required sequence):
    /// 1. Bounded `command_id` extraction only (`extractCommandId()`).
    /// 2. If a terminal journal record already exists for it, republish
    ///    those exact stored bytes and stop -- no parse, resolve, checksum,
    ///    validation, staging or activation.
    /// 3. Full envelope parse (`parseCommand()`), then emit `command_received`.
    /// 4. Resolve exact payload bytes, verify SHA-256, and validate the
    ///    complete candidate (`pixels_payload::parse*Payload()`) -- all
    ///    before touching durable/live state. `reboot`/`ota` stop here with
    ///    `command_rejected`/`unsupported_command` (the same reason code
    ///    `parseErrorCode(ParseError::UnsupportedCommand)` returns for an
    ///    unrecognized envelope type -- parsed, never executed).
    ///    `activate_preset` already matching the live target stops here
    ///    with `command_accepted` followed by a successful terminal result
    ///    -- no staging/activation and no restarted animation.
    /// 5. Emit `command_accepted`.
    /// 6. `staging.begin()` -> ordered `staging.append()` -> `staging.verifyAndCommit()`.
    /// 7. `staging.activateAtFrameBoundary()`.
    /// 8. Serialize and durably store the terminal `command_applied` record,
    ///    publish those exact bytes, then finalize retained rollback metadata.
    ///
    /// Invalid/checksum/unsupported input at step 4 is `command_rejected`
    /// and never calls `rollback()` (nothing was staged yet); a resource
    /// transport/cache failure is `command_failed`. A failure at step 6, 7,
    /// or storing the terminal record at step
    /// 8 is `command_failed`, always followed by `rollback()`, and never
    /// produces `command_applied` -- an "applied but not saved" outcome is
    /// forbidden. Every terminal outcome (rejected, failed, or applied) is
    /// durably stored via the journal BEFORE the corresponding event is
    /// published; a publish failure afterward never undoes the stored
    /// terminal result. An intermediate `command_received`/`command_accepted`
    /// publish failure is ignored -- it never aborts an
    /// otherwise-valid local transaction.
    ///
    /// @param json Raw command envelope bytes.
    /// @param size Exact byte count.
    /// @return ESP_OK once a terminal outcome has been durably stored (and
    ///         an attempt made to publish it) -- covers applied, rejected,
    ///         failed, and a replayed duplicate alike. ESP_ERR_INVALID_ARG
    ///         only when even the envelope's own `command_id` could not be
    ///         extracted (no terminal record can be addressed at all).
    ///         A storage error when the terminal journal itself cannot
    ///         store a rejected/failed result.
    esp_err_t execute(const char *json, size_t size);

private:
    pixels_firmware::TerminalJournal &journal_;
    pixels_firmware::DurableStaging &staging_;
    PayloadSource &payload_source_;
    EventSink &event_sink_;
    TimestampSource &timestamp_source_;
    ActiveTargetProbe &active_probe_;
    const char *device_id_;

    // Fixed-capacity scratch, member-owned (never a function-local `static`,
    // which would alias across a second Executor instance, and never a
    // FreeRTOS task-stack local, per the task brief's own instruction) --
    // `execute()` is the sole entry point and is never reentered on the
    // same instance (control-task-exclusive, see the class doc), so plain
    // members are safe without additional locking.
    pixels_firmware::Command command_{};
    char payload_buffer_[pixels_payload::kMaxPayloadBytes]{};
    pixels_payload::Candidate candidate_{};
    char event_scratch_[pixels_firmware::kMaxEventBytes]{};
    char replay_scratch_[pixels_firmware::kMaxEventBytes]{};
    pixels_firmware::TerminalRecord terminal_scratch_{};

    esp_err_t storeAndPublishTerminal(pixels_firmware::EventType type, const char *command_id,
                                      const char *reason_code, const char *message);
    esp_err_t rollbackAndFail(const char *command_id, const char *reason_code, const char *message);
    void publishIntermediate(pixels_firmware::EventType type, const char *command_id);
};

} // namespace pixels_executor
