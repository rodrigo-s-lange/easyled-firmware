#include "firmware_contract.hpp"

#include <climits>
#include <cstring>
#include "cJSON.h"
#include "psa/crypto.h"

namespace pixels_firmware {
namespace {

constexpr size_t kMaxCommandBytes = 1024;

const cJSON *member(const cJSON *object, const char *name)
{
    if (!cJSON_IsObject(object)) { return nullptr; }
    const cJSON *found = nullptr;
    for (const cJSON *item = object->child; item != nullptr; item = item->next) {
        if (item->string != nullptr && std::strcmp(item->string, name) == 0) {
            if (found != nullptr) { return nullptr; } // consumed duplicate is invalid
            found = item;
        }
    }
    return found;
}

bool copyString(const cJSON *item, char *output, size_t capacity, bool allow_empty = false)
{
    if (!cJSON_IsString(item) || item->valuestring == nullptr || capacity == 0) { return false; }
    const size_t length = strnlen(item->valuestring, capacity);
    if (length >= capacity || (!allow_empty && length == 0)) { return false; }
    std::memcpy(output, item->valuestring, length + 1);
    return true;
}

bool isHex(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

uint8_t hexValue(char value)
{
    if (value >= '0' && value <= '9') { return static_cast<uint8_t>(value - '0'); }
    if (value >= 'a' && value <= 'f') { return static_cast<uint8_t>(value - 'a' + 10); }
    return static_cast<uint8_t>(value - 'A' + 10);
}

bool uuid(const char *value)
{
    if (value == nullptr || std::strlen(value) != kUuidBytes) { return false; }
    for (size_t i = 0; i < kUuidBytes; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') { return false; }
        } else if (!isHex(value[i])) {
            return false;
        }
    }
    return true;
}

bool timestamp(const char *value)
{
    if (value == nullptr) { return false; }
    const size_t length = std::strlen(value);
    if (length < 20 || length > 35 || value[length - 1] != 'Z' ||
        value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') {
        return false;
    }
    for (size_t i = 0; i < 19; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) { continue; }
        if (value[i] < '0' || value[i] > '9') { return false; }
    }
    if (length > 20) {
        if (value[19] != '.' || length == 21) { return false; }
        for (size_t i = 20; i + 1 < length; ++i) {
            if (value[i] < '0' || value[i] > '9') { return false; }
        }
    }
    return true;
}

bool httpsUrl(const char *value)
{
    return value != nullptr && std::strncmp(value, "https://", 8) == 0 && value[8] != '\0' &&
           std::strchr(value + 8, '@') == nullptr;
}

bool unsignedValue(const cJSON *item, uint32_t minimum, uint32_t maximum, uint32_t &value)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < static_cast<double>(minimum) ||
        item->valuedouble > static_cast<double>(maximum)) {
        return false;
    }
    const uint32_t candidate = static_cast<uint32_t>(item->valuedouble);
    if (item->valuedouble != static_cast<double>(candidate)) { return false; }
    value = candidate;
    return true;
}

bool parseEnvelope(const cJSON *root, Command &command, const cJSON *&payload)
{
    char type[24]{};
    const cJSON *requested_by = member(root, "requested_by");
    payload = member(root, "payload");
    return cJSON_IsObject(root) &&
        copyString(member(root, "command_id"), command.command_id, sizeof(command.command_id)) &&
        uuid(command.command_id) &&
        copyString(member(root, "type"), type, sizeof(type)) &&
        copyString(member(root, "requested_at"), command.requested_at, sizeof(command.requested_at)) &&
        timestamp(command.requested_at) &&
        cJSON_IsObject(requested_by) &&
        copyString(member(requested_by, "actor_type"), command.actor_type, sizeof(command.actor_type)) &&
        copyString(member(requested_by, "actor_id"), command.actor_id, sizeof(command.actor_id)) &&
        uuid(command.actor_id) &&
        copyString(member(root, "idempotency_key"), command.idempotency_key,
                   sizeof(command.idempotency_key)) &&
        cJSON_IsObject(payload) &&
        ((std::strcmp(type, "apply_config") == 0 && (command.type = CommandType::ApplyConfig, true)) ||
         (std::strcmp(type, "activate_preset") == 0 && (command.type = CommandType::ActivatePreset, true)) ||
         (std::strcmp(type, "reboot") == 0 && (command.type = CommandType::Reboot, true)) ||
         (std::strcmp(type, "ota") == 0 && (command.type = CommandType::Ota, true)));
}

bool parsePayload(const cJSON *payload, Command &command)
{
    char checksum[kSha256HexBytes + 1]{};
    switch (command.type) {
    case CommandType::ApplyConfig:
        if (!unsignedValue(member(payload, "config_version"), 1, UINT32_MAX,
                           command.config_version) ||
            !copyString(member(payload, "config_url"), command.resource_url,
                        sizeof(command.resource_url)) || !httpsUrl(command.resource_url) ||
            !copyString(member(payload, "checksum"), checksum, sizeof(checksum)) ||
            parseSha256(checksum, command.checksum) != ESP_OK) {
            return false;
        }
        command.has_checksum = true;
        return true;
    case CommandType::ActivatePreset:
        return copyString(member(payload, "preset_id"), command.preset_id,
                          sizeof(command.preset_id)) && uuid(command.preset_id) &&
            copyString(member(payload, "transition"), command.transition,
                       sizeof(command.transition)) &&
            unsignedValue(member(payload, "duration_ms"), 0, UINT32_MAX,
                          command.duration_ms);
    case CommandType::Reboot:
        return copyString(member(payload, "reason"), command.reason, sizeof(command.reason));
    case CommandType::Ota:
        if (!copyString(member(payload, "firmware_version"), command.firmware_version,
                        sizeof(command.firmware_version)) ||
            !copyString(member(payload, "artifact_url"), command.resource_url,
                        sizeof(command.resource_url)) || !httpsUrl(command.resource_url) ||
            !copyString(member(payload, "checksum"), checksum, sizeof(checksum)) ||
            parseSha256(checksum, command.checksum) != ESP_OK) {
            return false;
        }
        command.has_checksum = true;
        return true;
    }
    return false;
}

const char *eventName(EventType type)
{
    switch (type) {
    case EventType::CommandReceived: return "command_received";
    case EventType::CommandAccepted: return "command_accepted";
    case EventType::CommandRejected: return "command_rejected";
    case EventType::CommandApplied: return "command_applied";
    case EventType::CommandFailed: return "command_failed";
    }
    return nullptr;
}

bool terminalReason(EventType type)
{
    return type == EventType::CommandRejected || type == EventType::CommandFailed;
}

bool boundedText(const char *value, size_t maximum)
{
    return value != nullptr && value[0] != '\0' && strnlen(value, maximum + 1) <= maximum;
}

} // namespace

esp_err_t parseCommand(const char *json, size_t size, Command &command, ParseError &error)
{
    error = ParseError::InvalidJson;
    if (json == nullptr || size == 0) { return ESP_ERR_INVALID_ARG; }
    if (size > kMaxCommandBytes) {
        error = ParseError::MessageTooLarge;
        return ESP_ERR_INVALID_SIZE;
    }
    const char *end = nullptr;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (root == nullptr || end != json + size) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    Command parsed{};
    const cJSON *payload = nullptr;
    if (!parseEnvelope(root, parsed, payload)) {
        const cJSON *type = member(root, "type");
        if (cJSON_IsString(type) && type->valuestring != nullptr &&
            std::strcmp(type->valuestring, "apply_config") != 0 &&
            std::strcmp(type->valuestring, "activate_preset") != 0 &&
            std::strcmp(type->valuestring, "reboot") != 0 &&
            std::strcmp(type->valuestring, "ota") != 0) {
            error = ParseError::UnsupportedCommand;
            cJSON_Delete(root);
            return ESP_ERR_NOT_SUPPORTED;
        }
        error = ParseError::InvalidEnvelope;
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!parsePayload(payload, parsed)) {
        error = ParseError::InvalidPayload;
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);
    command = parsed;
    error = ParseError::None;
    return ESP_OK;
}

const char *parseErrorCode(ParseError error)
{
    switch (error) {
    case ParseError::None: return "none";
    case ParseError::MessageTooLarge: return "message_too_large";
    case ParseError::InvalidJson: return "invalid_json";
    case ParseError::InvalidEnvelope: return "invalid_envelope";
    case ParseError::UnsupportedCommand: return "unsupported_command";
    case ParseError::InvalidPayload: return "invalid_payload";
    }
    return "invalid_json";
}

esp_err_t parseSha256(const char *text, uint8_t digest[kSha256Bytes])
{
    if (text == nullptr || digest == nullptr || std::strlen(text) != kSha256HexBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t parsed[kSha256Bytes]{};
    for (size_t i = 0; i < kSha256Bytes; ++i) {
        if (!isHex(text[i * 2]) || !isHex(text[i * 2 + 1])) { return ESP_ERR_INVALID_ARG; }
        parsed[i] = static_cast<uint8_t>((hexValue(text[i * 2]) << 4) | hexValue(text[i * 2 + 1]));
    }
    std::memcpy(digest, parsed, sizeof(parsed));
    return ESP_OK;
}

esp_err_t verifySha256(const void *bytes, size_t size, const uint8_t expected[kSha256Bytes])
{
    if ((bytes == nullptr && size != 0) || expected == nullptr) { return ESP_ERR_INVALID_ARG; }
    uint8_t actual[kSha256Bytes]{};
    size_t actual_size = 0;
    static const uint8_t empty = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256,
                         bytes == nullptr ? &empty : static_cast<const uint8_t *>(bytes), size,
                         actual, sizeof(actual), &actual_size) != PSA_SUCCESS ||
        actual_size != sizeof(actual)) {
        return ESP_FAIL;
    }
    uint8_t difference = 0;
    for (size_t i = 0; i < kSha256Bytes; ++i) { difference |= actual[i] ^ expected[i]; }
    return difference == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t serializeEvent(const Event &event, char *output, size_t capacity, size_t &size)
{
    size = 0;
    const char *name = eventName(event.type);
    if (name == nullptr || output == nullptr || capacity == 0 ||
        !timestamp(event.event_at) || !uuid(event.device_id) || !uuid(event.command_id) ||
        (terminalReason(event.type) &&
         (!boundedText(event.reason_code, 48) || !boundedText(event.message, 192)))) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    if (root == nullptr || payload == nullptr) {
        cJSON_Delete(payload);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    bool ok = cJSON_AddStringToObject(root, "event_type", name) != nullptr &&
        cJSON_AddStringToObject(root, "event_at", event.event_at) != nullptr &&
        cJSON_AddStringToObject(root, "device_id", event.device_id) != nullptr &&
        cJSON_AddStringToObject(root, "command_id", event.command_id) != nullptr;
    if (ok && terminalReason(event.type)) {
        ok = cJSON_AddStringToObject(payload, "reason_code", event.reason_code) != nullptr &&
             cJSON_AddStringToObject(payload, "message", event.message) != nullptr;
    }
    if (!ok) {
        cJSON_Delete(payload);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "payload", payload);
    payload = nullptr;
    if (capacity > INT_MAX ||
        !cJSON_PrintPreallocated(root, output, static_cast<int>(capacity), false)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    size = std::strlen(output);
    return ESP_OK;
}

esp_err_t replayTerminal(const TerminalJournal &journal, const char *command_id,
                         char *output, size_t capacity, size_t &size)
{
    size = 0;
    if (!uuid(command_id) || output == nullptr) { return ESP_ERR_INVALID_ARG; }
    TerminalRecord record{};
    const esp_err_t err = journal.find(command_id, record);
    if (err != ESP_OK) { return err; }
    if (std::strcmp(record.command_id, command_id) != 0 || record.event_size == 0 ||
        record.event_size >= kMaxEventBytes || record.event_json[record.event_size] != '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (capacity <= record.event_size) { return ESP_ERR_INVALID_SIZE; }
    std::memcpy(output, record.event_json, record.event_size + 1);
    size = record.event_size;
    return ESP_OK;
}

} // namespace pixels_firmware
