#include "pixels_command_executor.hpp"

#include <cstring>

namespace pixels_executor {

namespace {
using namespace pixels_firmware;
} // namespace

Executor::Executor(TerminalJournal &journal, DurableStaging &staging, PayloadSource &payload_source,
                   EventSink &event_sink, TimestampSource &timestamp_source,
                   ActiveTargetProbe &active_probe, const char *device_id)
    : journal_(journal), staging_(staging), payload_source_(payload_source), event_sink_(event_sink),
      timestamp_source_(timestamp_source), active_probe_(active_probe), device_id_(device_id)
{
}

void Executor::publishIntermediate(EventType type, const char *command_id)
{
    char timestamp[36]{};
    timestamp_source_.now(timestamp);
    Event event{type, timestamp, device_id_, command_id};
    size_t size = 0;
    if (serializeEvent(event, event_scratch_, sizeof(event_scratch_), size) != ESP_OK) { return; }
    (void)event_sink_.publish(event_scratch_, size); // best-effort: never aborts the transaction
}

esp_err_t Executor::storeAndPublishTerminal(EventType type, const char *command_id, const char *reason_code,
                                            const char *message)
{
    char timestamp[36]{};
    timestamp_source_.now(timestamp);
    Event event{type, timestamp, device_id_, command_id, reason_code, message};
    terminal_scratch_ = TerminalRecord{};
    std::memcpy(terminal_scratch_.command_id, command_id, sizeof(terminal_scratch_.command_id));
    size_t size = 0;
    esp_err_t err = serializeEvent(event, terminal_scratch_.event_json, sizeof(terminal_scratch_.event_json), size);
    if (err != ESP_OK) { return err; }
    terminal_scratch_.event_size = static_cast<uint16_t>(size);
    err = journal_.store(terminal_scratch_);
    if (err != ESP_OK) { return err; }
    // Enqueue failure never undoes this already-durable store.
    (void)event_sink_.publish(terminal_scratch_.event_json, terminal_scratch_.event_size);
    return ESP_OK;
}

esp_err_t Executor::rollbackAndFail(const char *command_id, const char *reason_code, const char *message)
{
    if (staging_.rollback() != ESP_OK) {
        return storeAndPublishTerminal(EventType::CommandFailed, command_id, ReasonCode::kRollbackFailed,
                                       "rollback could not restore the prior durable/live state");
    }
    return storeAndPublishTerminal(EventType::CommandFailed, command_id, reason_code, message);
}

esp_err_t Executor::execute(const char *json, size_t size)
{
    char command_id[kUuidBytes + 1]{};
    if (extractCommandId(json, size, command_id) != ESP_OK) {
        return ESP_ERR_INVALID_ARG; // no command_id at all: no terminal record can be addressed
    }

    // Step 2: duplicate replay -- exact stored bytes, no re-execution.
    size_t replay_size = 0;
    const esp_err_t found =
        replayTerminal(journal_, command_id, replay_scratch_, sizeof(replay_scratch_), replay_size);
    if (found == ESP_OK) {
        (void)event_sink_.publish(replay_scratch_, replay_size);
        return ESP_OK;
    }
    if (found != ESP_ERR_NOT_FOUND) {
        return found; // a genuine journal storage error -- cannot proceed confidently either way
    }

    // Step 3: full parse. A failure here still has a valid command_id (step
    // 1 already extracted it), so it is still addressable as a rejection.
    ParseError parse_error = ParseError::None;
    if (parseCommand(json, size, command_, parse_error) != ESP_OK) {
        return storeAndPublishTerminal(EventType::CommandRejected, command_id, parseErrorCode(parse_error),
                                       "envelope or payload failed validation");
    }
    publishIntermediate(EventType::CommandReceived, command_id);

    if (command_.type == CommandType::Reboot || command_.type == CommandType::Ota) {
        return storeAndPublishTerminal(EventType::CommandRejected, command_id,
                                       parseErrorCode(ParseError::UnsupportedCommand),
                                       "reboot/ota are parsed but not executed by this delivery");
    }

    // Step 4: resolve, checksum, validate -- all before touching durable/live state.
    size_t payload_size = 0;
    uint8_t expected_checksum[kSha256Bytes]{};
    if (payload_source_.resolve(command_, payload_buffer_, sizeof(payload_buffer_), payload_size,
                                expected_checksum) != ESP_OK) {
        return storeAndPublishTerminal(EventType::CommandFailed, command_id, ReasonCode::kResolveFailed,
                                       "resource resolution failed");
    }
    if (payload_size == 0 || payload_size > sizeof(payload_buffer_)) {
        return storeAndPublishTerminal(EventType::CommandFailed, command_id, ReasonCode::kResolveFailed,
                                       "resource source returned an invalid payload size");
    }

    // The command envelope owns apply_config's digest. A resolver may return
    // cache metadata, but it must never be able to replace the requested
    // checksum and authorize different bytes. activate_preset has no digest
    // in its envelope yet, so its resolver remains authoritative.
    const uint8_t *authoritative_checksum = command_.type == CommandType::ApplyConfig
        ? command_.checksum : expected_checksum;

    if (command_.type == CommandType::ActivatePreset &&
        active_probe_.alreadyActive(command_.preset_id, expected_checksum)) {
        // Required successful no-op: skip checksum/validation/staging/
        // activation entirely, never restarting the running animation.
        publishIntermediate(EventType::CommandAccepted, command_id);
        return storeAndPublishTerminal(EventType::CommandApplied, command_id, nullptr, nullptr);
    }

    if (verifySha256(payload_buffer_, payload_size, authoritative_checksum) != ESP_OK) {
        return storeAndPublishTerminal(EventType::CommandRejected, command_id, ReasonCode::kChecksumMismatch,
                                       "downloaded payload checksum mismatch");
    }

    pixels_payload::SchemaError schema_error = pixels_payload::SchemaError::None;
    const esp_err_t schema_err = (command_.type == CommandType::ApplyConfig)
        ? pixels_payload::parseConfigPayload(payload_buffer_, payload_size, candidate_, schema_error)
        : pixels_payload::parsePresetPayload(payload_buffer_, payload_size, candidate_, schema_error);
    if (schema_err != ESP_OK) {
        return storeAndPublishTerminal(EventType::CommandRejected, command_id,
                                       pixels_payload::schemaErrorCode(schema_error),
                                       "payload failed schema validation");
    }

    // Step 5.
    publishIntermediate(EventType::CommandAccepted, command_id);

    // Step 6: begin -> append -> verifyAndCommit.
    if (staging_.begin(command_, payload_size) != ESP_OK) {
        return rollbackAndFail(command_id, ReasonCode::kStagingFailed, "staging begin failed");
    }
    if (staging_.append(payload_buffer_, payload_size) != ESP_OK) {
        return rollbackAndFail(command_id, ReasonCode::kStagingFailed, "staging append failed");
    }
    if (staging_.verifyAndCommit(authoritative_checksum) != ESP_OK) {
        return rollbackAndFail(command_id, ReasonCode::kStagingFailed, "staging commit failed");
    }

    // Step 7.
    if (staging_.activateAtFrameBoundary() != ESP_OK) {
        return rollbackAndFail(command_id, ReasonCode::kActivationFailed, "activation failed");
    }

    // Step 8: an "applied but not saved" outcome is forbidden -- a failed
    // terminal store after a successful activation rolls the durable/live
    // state back and reports failure instead.
    const esp_err_t stored = storeAndPublishTerminal(EventType::CommandApplied, command_id, nullptr, nullptr);
    if (stored == ESP_OK) {
        // Terminal success is already durable. Finalization only removes the
        // retained rollback slot/pending marker; if it fails, boot recovery
        // reconciles them against that terminal journal record.
        (void)staging_.finalize();
        return ESP_OK;
    }
    return rollbackAndFail(command_id, ReasonCode::kTerminalStoreFailed,
                           "activated but the terminal record could not be stored durably");
}

} // namespace pixels_executor
