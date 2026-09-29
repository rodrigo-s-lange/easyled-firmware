#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace pixels_firmware {

constexpr size_t kUuidBytes = 36;
constexpr size_t kSha256Bytes = 32;
constexpr size_t kSha256HexBytes = 64;
constexpr size_t kMaxEventBytes = 512;

enum class CommandType : uint8_t {
    ApplyConfig,
    ActivatePreset,
    Reboot,
    Ota,
};

enum class ParseError : uint8_t {
    None,
    MessageTooLarge,
    InvalidJson,
    InvalidEnvelope,
    UnsupportedCommand,
    InvalidPayload,
};

/// Parsed subset of the provisional EasyLED command contract. Unknown JSON
/// fields are ignored; every consumed field is type/range checked. Strings are
/// owned by this value and do not refer to the input buffer.
struct Command {
    CommandType type = CommandType::ApplyConfig;
    char command_id[kUuidBytes + 1]{};
    char requested_at[36]{};
    char actor_type[24]{};
    char actor_id[kUuidBytes + 1]{};
    char idempotency_key[97]{};

    uint32_t config_version = 0;
    char resource_url[257]{};
    uint8_t checksum[kSha256Bytes]{};
    bool has_checksum = false;

    char preset_id[kUuidBytes + 1]{};
    char transition[17]{};
    uint32_t duration_ms = 0;

    char reason[65]{};
    char firmware_version[33]{};
};

/// Extracts ONLY the envelope's own `command_id`, without validating or
/// parsing anything else (type, payload, actor, timestamp). Intended for an
/// executor's own terminal-journal deduplication check, before deciding
/// whether the full envelope needs parsing/revalidating at all. Still
/// rejects a malformed/oversized message or an invalid UUID -- this is a
/// bounded extraction, not a lax pre-scan.
/// @param json UTF-8 JSON bytes; no trailing NUL is required.
/// @param size Exact byte count, at most 1024.
/// @param command_id Receives the canonical UUID on success only.
/// @return ESP_OK or ESP_ERR_INVALID_ARG/SIZE.
esp_err_t extractCommandId(const char *json, size_t size, char command_id[kUuidBytes + 1]);

/// Parses one complete, non-retained command message.
/// @param json UTF-8 JSON bytes; no trailing NUL is required.
/// @param size Exact byte count, at most 1024.
/// @param command Receives an owned parsed command on success only.
/// @param error Receives a stable local rejection category.
/// @return ESP_OK or ESP_ERR_INVALID_ARG/SIZE/NOT_SUPPORTED.
esp_err_t parseCommand(const char *json, size_t size, Command &command, ParseError &error);

/// Returns the stable wire-oriented reason code for a parse category.
/// @param error Parse category.
/// @return Static NUL-terminated reason code.
const char *parseErrorCode(ParseError error);

/// Computes SHA-256 over exact bytes and compares it in constant time.
/// @param bytes Payload bytes; may be null only when size is zero.
/// @param size Exact byte count, including any whitespace/newline supplied.
/// @param expected Expected 32-byte digest.
/// @return ESP_OK on match, ESP_ERR_INVALID_ARG or ESP_ERR_INVALID_CRC.
esp_err_t verifySha256(const void *bytes, size_t size, const uint8_t expected[kSha256Bytes]);

/// Parses a 64-character hexadecimal SHA-256 representation.
/// @param text NUL-terminated hexadecimal value.
/// @param digest Receives 32 decoded bytes on success only.
/// @return ESP_OK or ESP_ERR_INVALID_ARG.
esp_err_t parseSha256(const char *text, uint8_t digest[kSha256Bytes]);

enum class EventType : uint8_t {
    CommandReceived,
    CommandAccepted,
    CommandRejected,
    CommandApplied,
    CommandFailed,
};

/// Values used to serialize one compact event. `reason_code` and `message`
/// are required only for rejected/failed events and ignored otherwise.
struct Event {
    EventType type = EventType::CommandReceived;
    const char *event_at = nullptr;
    const char *device_id = nullptr;
    const char *command_id = nullptr;
    const char *reason_code = nullptr;
    const char *message = nullptr;
};

/// Serializes the provisional event contract without logging its contents.
/// @param event Event fields; IDs and timestamp are validated.
/// @param output Destination buffer.
/// @param capacity Destination capacity, including the trailing NUL.
/// @param size Receives JSON byte count, excluding the trailing NUL.
/// @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, or ESP_ERR_INVALID_SIZE.
esp_err_t serializeEvent(const Event &event, char *output, size_t capacity, size_t &size);

/// Exact terminal event retained for command-id deduplication. Persist the
/// serialized bytes so a duplicate republishes the original timestamp/reason.
struct TerminalRecord {
    char command_id[kUuidBytes + 1]{};
    uint16_t event_size = 0;
    char event_json[kMaxEventBytes]{};
};

/// Product-owned durable terminal-command journal. The contract component
/// borrows this interface and never chooses NVS keys, capacity or retention.
class TerminalJournal {
public:
    virtual ~TerminalJournal() = default;
    /// Finds a terminal result without modifying it.
    /// @param command_id Canonical UUID.
    /// @param record Receives the original terminal event on success only.
    /// @return ESP_OK, ESP_ERR_NOT_FOUND, or a storage error.
    virtual esp_err_t find(const char *command_id, TerminalRecord &record) const = 0;
    /// Stores a terminal result durably before it may be acknowledged.
    /// @param record Complete terminal result.
    /// @return ESP_OK only after recoverable commit, or a storage error.
    virtual esp_err_t store(const TerminalRecord &record) = 0;
};

/// Copies a duplicate command's durable terminal event for republication.
/// No validation, download, staging or activation callback is invoked here.
/// @param journal Durable journal.
/// @param command_id Command UUID to find.
/// @param output Destination for exact original event bytes.
/// @param capacity Destination capacity.
/// @param size Receives copied byte count.
/// @return ESP_OK, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_ARG/SIZE, or storage error.
esp_err_t replayTerminal(const TerminalJournal &journal, const char *command_id,
                         char *output, size_t capacity, size_t &size);

/// Product-owned transaction for downloaded configuration/preset bytes. The
/// implementation must keep the current active state recoverable until commit
/// and frame-boundary activation both succeed.
class DurableStaging {
public:
    virtual ~DurableStaging() = default;
    /// Starts a candidate and discards any incomplete prior candidate safely.
    /// @param command Parsed command owning this transaction.
    /// @param total_size Expected payload length.
    /// @return ESP_OK or a validation/storage error.
    virtual esp_err_t begin(const Command &command, size_t total_size) = 0;
    /// Appends exact downloaded bytes in order.
    /// @param bytes Next bytes.
    /// @param size Byte count.
    /// @return ESP_OK or a storage error.
    virtual esp_err_t append(const void *bytes, size_t size) = 0;
    /// Verifies the exact-byte digest and commits a reboot-recoverable candidate.
    /// @param expected Expected SHA-256.
    /// @return ESP_OK only after durable verification/commit.
    virtual esp_err_t verifyAndCommit(const uint8_t expected[kSha256Bytes]) = 0;
    /// Activates the committed candidate atomically at a render frame boundary.
    /// @return ESP_OK only when active state matches the durable candidate.
    virtual esp_err_t activateAtFrameBoundary() = 0;
    /// Finalizes an activated transaction after its applied terminal result is durable.
    /// A failure leaves recovery metadata intact for boot-time reconciliation.
    /// @return ESP_OK when obsolete rollback data is removed, or a storage error.
    virtual esp_err_t finalize() = 0;
    /// Restores/preserves the previous durable active state after any failure.
    /// @return ESP_OK when recovery is complete, or a storage/runtime error.
    virtual esp_err_t rollback() = 0;
};

} // namespace pixels_firmware
