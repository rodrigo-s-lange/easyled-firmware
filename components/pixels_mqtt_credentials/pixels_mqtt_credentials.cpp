#include "pixels_mqtt_credentials.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "nvs_flash.h"

namespace pixels_mqtt {
namespace {

// --- small bounded JSON helpers (own copy, same pattern as
// pixels_firmware_contract/pixels_payload_adapter -- this component does not
// import their private helpers, exactly like those two do not share them
// with each other). ----------------------------------------------------------

bool isHex(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
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

// Returns null for a missing member, but ALSO for a duplicate one (the
// caller cannot tell them apart from this return value alone) -- every field
// this parser consumes is required, so "missing" and "duplicate" are
// rejected identically anyway.
const cJSON *member(const cJSON *object, const char *name)
{
    if (!cJSON_IsObject(object)) { return nullptr; }
    const cJSON *found = nullptr;
    for (const cJSON *item = object->child; item != nullptr; item = item->next) {
        if (item->string != nullptr && std::strcmp(item->string, name) == 0) {
            if (found != nullptr) { return nullptr; }
            found = item;
        }
    }
    return found;
}

bool copyString(const cJSON *item, char *out, size_t capacity)
{
    if (!cJSON_IsString(item) || item->valuestring == nullptr || capacity == 0) { return false; }
    const size_t len = strnlen(item->valuestring, capacity);
    if (len >= capacity || len == 0) { return false; }
    std::memcpy(out, item->valuestring, len + 1);
    return true;
}

bool stringEquals(const cJSON *item, const char *expected)
{
    return cJSON_IsString(item) && item->valuestring != nullptr && std::strcmp(item->valuestring, expected) == 0;
}

// RFC 8259 permits insignificant whitespace after the top-level JSON value
// (this response is pretty-printed, not minified, and ends with a trailing
// newline) -- cJSON's own `end` only marks where the VALUE finished, so
// anything between there and the buffer's own end must be checked here.
bool onlyTrailingWhitespace(const char *begin, const char *end)
{
    for (; begin != end; ++begin) {
        if (*begin != ' ' && *begin != '\t' && *begin != '\r' && *begin != '\n') { return false; }
    }
    return true;
}

bool topicValid(const char *s)
{
    return s != nullptr && s[0] != '\0' && std::strlen(s) <= kMaxTopicBytes && std::strpbrk(s, "+#") == nullptr;
}

bool wssUrl(const char *s)
{
    // Exactly wss://, no userinfo (an '@' before the first '/' after the
    // scheme would be userinfo) -- the contract: "do not connect and do not
    // persist the credential" for anything else, including mqtts://.
    if (s == nullptr || std::strncmp(s, "wss://", 6) != 0 || s[6] == '\0') { return false; }
    const char *slash = std::strchr(s + 6, '/');
    const char *at = std::strchr(s + 6, '@');
    if (at != nullptr && (slash == nullptr || at < slash)) { return false; }
    return true;
}

// RFC3339 UTC, always ending in 'Z', 0 or 1-6 fractional digits -- format
// only. expires_at is advisory (contract, "Clock: expires_at is advisory");
// this never evaluates it against any clock, only checks its shape.
bool validExpiresAt(const char *s, size_t len)
{
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (len < 20 || s[len - 1] != 'Z') { return false; }
    if (!digit(s[0]) || !digit(s[1]) || !digit(s[2]) || !digit(s[3]) || s[4] != '-') { return false; }
    if (!digit(s[5]) || !digit(s[6]) || s[7] != '-') { return false; }
    if (!digit(s[8]) || !digit(s[9]) || s[10] != 'T') { return false; }
    if (!digit(s[11]) || !digit(s[12]) || s[13] != ':') { return false; }
    if (!digit(s[14]) || !digit(s[15]) || s[16] != ':') { return false; }
    if (!digit(s[17]) || !digit(s[18])) { return false; }
    size_t i = 19;
    if (i < len - 1 && s[i] == '.') {
        ++i;
        const size_t start = i;
        while (i < len - 1 && digit(s[i]) && (i - start) < 6) { ++i; }
        if (i == start) { return false; } // '.' with no digits at all
    }
    return i == len - 1; // whatever remains must be exactly the trailing 'Z'
}

uint32_t crc32(const uint8_t *bytes, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

} // namespace

bool isValidHardwareId(const char *value)
{
    if (value == nullptr) { return false; }
    const size_t len = std::strlen(value);
    if (len == 0 || len > kMaxHardwareIdBytes) { return false; }
    for (size_t i = 0; i < len; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
    }
    return true;
}

esp_err_t parseDeviceCredentialResponse(const char *json, size_t size, const char *expected_device_id,
                                        const char *expected_hardware_binding_id, uint32_t expected_generation,
                                        DeviceCredentialResponse &out, ParseError &error)
{
    out = DeviceCredentialResponse{};
    error = ParseError::Malformed;
    if (json == nullptr || size == 0 || size > kMaxResponseBytes || !uuid(expected_device_id) ||
        !uuid(expected_hardware_binding_id)) {
        error = ParseError::InvalidSize;
        return ESP_ERR_INVALID_SIZE;
    }
    const char *end = nullptr;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (root == nullptr || !onlyTrailingWhitespace(end, json + size) || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char device_id[kUuidBytes + 1]{};
    char hardware_binding_id[kUuidBytes + 1]{};
    uint32_t generation = 0;
    const cJSON *credential = member(root, "credential");
    const cJSON *mqtt = member(root, "mqtt");
    const cJSON *generation_item = member(root, "generation");

    bool ok = copyString(member(root, "device_id"), device_id, sizeof(device_id)) && uuid(device_id) &&
        copyString(member(root, "hardware_binding_id"), hardware_binding_id, sizeof(hardware_binding_id)) &&
        uuid(hardware_binding_id) && cJSON_IsNumber(generation_item) && generation_item->valuedouble > 0 &&
        generation_item->valuedouble <= static_cast<double>(UINT32_MAX) &&
        generation_item->valuedouble == static_cast<double>(static_cast<uint32_t>(generation_item->valuedouble)) &&
        cJSON_IsObject(credential) && cJSON_IsObject(mqtt);
    if (!ok) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    generation = static_cast<uint32_t>(generation_item->valuedouble);

    // Identity AND authority: device_id/hardware_binding_id prove this
    // response is about this device's current binding; generation is an
    // exact-match authority token on top of that (OpenAPI ClaimResponse:
    // "never an ordering comparison") -- a response for any generation
    // other than the caller's own current one is rejected the same way,
    // whether it looks older or newer. Checked before trust_ref is ever
    // resolved (that happens only after this parser returns ESP_OK) and
    // before anything is persisted or connected with.
    if (std::strcmp(device_id, expected_device_id) != 0 ||
        std::strcmp(hardware_binding_id, expected_hardware_binding_id) != 0 ||
        expected_generation == 0 || generation != expected_generation) {
        cJSON_Delete(root);
        error = ParseError::IdentityMismatch;
        return ESP_ERR_INVALID_ARG;
    }

    char credential_id[kUuidBytes + 1]{};
    char expires_at[kMaxExpiresAtBytes + 1]{};
    const cJSON *kind_item = member(credential, "kind");
    ok = copyString(member(credential, "id"), credential_id, sizeof(credential_id)) && uuid(credential_id) &&
        stringEquals(member(credential, "status"), "pending") &&
        (stringEquals(kind_item, "initial_provisioning") || stringEquals(kind_item, "rotation")) &&
        stringEquals(member(credential, "activation"), "first_successful_mqtt_authentication") &&
        copyString(member(credential, "expires_at"), expires_at, sizeof(expires_at)) &&
        validExpiresAt(expires_at, std::strlen(expires_at));
    if (!ok) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    const CredentialKind kind =
        stringEquals(kind_item, "rotation") ? CredentialKind::Rotation : CredentialKind::InitialProvisioning;

    char username[kUuidBytes + 1]{};
    char client_id[kUuidBytes + 1]{};
    char password[kMaxPasswordBytes + 1]{};
    char broker_uri[kMaxBrokerUriBytes + 1]{};
    const cJSON *tls = member(mqtt, "tls");
    const cJSON *topics = member(mqtt, "topics");
    ok = copyString(member(mqtt, "username"), username, sizeof(username)) && uuid(username) &&
        copyString(member(mqtt, "client_id"), client_id, sizeof(client_id)) && uuid(client_id) &&
        std::strcmp(username, device_id) == 0 && std::strcmp(client_id, device_id) == 0 &&
        copyString(member(mqtt, "password"), password, sizeof(password)) &&
        copyString(member(mqtt, "broker_uri"), broker_uri, sizeof(broker_uri)) && wssUrl(broker_uri) &&
        cJSON_IsObject(tls) && cJSON_IsObject(topics);
    if (!ok) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }

    char trust_ref[kMaxTrustRefBytes + 1]{};
    const cJSON *verify_server = member(tls, "verify_server");
    ok = copyString(member(tls, "trust_ref"), trust_ref, sizeof(trust_ref)) &&
        cJSON_IsBool(verify_server) && cJSON_IsTrue(verify_server);
    if (!ok) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }

    const cJSON *publish = member(topics, "publish");
    const cJSON *subscribe = member(topics, "subscribe");
    char topic_state[kMaxTopicBytes + 1]{};
    char topic_event[kMaxTopicBytes + 1]{};
    char topic_telemetry[kMaxTopicBytes + 1]{};
    char topic_cmd[kMaxTopicBytes + 1]{};
    ok = cJSON_IsObject(publish) && cJSON_IsObject(subscribe) &&
        copyString(member(publish, "state"), topic_state, sizeof(topic_state)) && topicValid(topic_state) &&
        copyString(member(publish, "event"), topic_event, sizeof(topic_event)) && topicValid(topic_event) &&
        copyString(member(publish, "telemetry"), topic_telemetry, sizeof(topic_telemetry)) &&
        topicValid(topic_telemetry) &&
        copyString(member(subscribe, "cmd"), topic_cmd, sizeof(topic_cmd)) && topicValid(topic_cmd);
    cJSON_Delete(root);
    if (!ok) { return ESP_ERR_INVALID_ARG; }

    std::memcpy(out.device_id, device_id, sizeof(out.device_id));
    std::memcpy(out.hardware_binding_id, hardware_binding_id, sizeof(out.hardware_binding_id));
    out.generation = generation;
    std::memcpy(out.credential_id, credential_id, sizeof(out.credential_id));
    out.kind = kind;
    std::memcpy(out.expires_at, expires_at, sizeof(out.expires_at));
    std::memcpy(out.username, username, sizeof(out.username));
    std::memcpy(out.client_id, client_id, sizeof(out.client_id));
    std::memcpy(out.password, password, sizeof(out.password));
    std::memcpy(out.broker_uri, broker_uri, sizeof(out.broker_uri));
    std::memcpy(out.trust_ref, trust_ref, sizeof(out.trust_ref));
    std::memcpy(out.topic_state, topic_state, sizeof(out.topic_state));
    std::memcpy(out.topic_event, topic_event, sizeof(out.topic_event));
    std::memcpy(out.topic_telemetry, topic_telemetry, sizeof(out.topic_telemetry));
    std::memcpy(out.topic_cmd, topic_cmd, sizeof(out.topic_cmd));
    error = ParseError::None;
    return ESP_OK;
}

// --- Trust resolver ----------------------------------------------------------

TableTrustResolver::TableTrustResolver(const Entry *entries, size_t count) : entries_(entries), count_(count) {}

esp_err_t TableTrustResolver::resolve(const char *trust_ref, const char **ca_pem) const
{
    if (trust_ref == nullptr || trust_ref[0] == '\0' || ca_pem == nullptr) { return ESP_ERR_INVALID_ARG; }
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].trust_ref != nullptr && std::strcmp(entries_[i].trust_ref, trust_ref) == 0) {
            if (entries_[i].ca_pem == nullptr) { return ESP_ERR_NOT_FOUND; } // never a null-CA fallback
            *ca_pem = entries_[i].ca_pem;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND; // fail closed: unknown trust_ref, no fallback
}

// --- HTTPS bootstrap client ---------------------------------------------------

namespace {

struct BootstrapHttpContext {
    char *destination = nullptr;
    size_t capacity = 0;
    size_t size = 0;
    bool overflow = false;
    bool has_retry_after = false;
    bool invalid_retry_after = false;
    uint32_t retry_after_s = 0;
};

bool sameHeaderName(const char *left, const char *right)
{
    if (left == nullptr || right == nullptr) { return false; }
    size_t i = 0;
    for (; left[i] != '\0' && right[i] != '\0'; ++i) {
        char a = left[i], b = right[i];
        if (a >= 'A' && a <= 'Z') { a = static_cast<char>(a + ('a' - 'A')); }
        if (b >= 'A' && b <= 'Z') { b = static_cast<char>(b + ('a' - 'A')); }
        if (a != b) { return false; }
    }
    return left[i] == '\0' && right[i] == '\0';
}

esp_err_t onBootstrapHttpEvent(esp_http_client_event_t *event)
{
    if (event == nullptr || event->user_data == nullptr) { return ESP_ERR_INVALID_ARG; }
    auto &context = *static_cast<BootstrapHttpContext *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER && sameHeaderName(event->header_key, "Retry-After")) {
        char *parse_end = nullptr;
        const long value = event->header_value == nullptr ? -1 : std::strtol(event->header_value, &parse_end, 10);
        if (event->header_value == nullptr || parse_end == event->header_value || *parse_end != '\0' ||
            value < 0 || value > 86400 || context.has_retry_after) {
            context.invalid_retry_after = true;
        } else {
            context.retry_after_s = static_cast<uint32_t>(value);
            context.has_retry_after = true;
        }
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len == 0) { return ESP_OK; }
    if (event->data == nullptr || event->data_len < 0) { return ESP_ERR_INVALID_RESPONSE; }
    const size_t incoming = static_cast<size_t>(event->data_len);
    if (incoming > context.capacity - context.size) {
        context.overflow = true;
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(context.destination + context.size, event->data, incoming);
    context.size += incoming;
    return ESP_OK;
}

} // namespace

esp_err_t EspMqttBootstrapClient::requestCredential(const BootstrapRequest &request, BootstrapResult &result)
{
    result = BootstrapResult{};
    if (request.base_url == nullptr || std::strncmp(request.base_url, "https://", 8) != 0 ||
        !isValidHardwareId(request.hardware_id) ||
        request.bootstrap_token == nullptr || request.bootstrap_token[0] == '\0' ||
        request.timeout_ms == 0 || request.destination == nullptr || request.capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = std::snprintf(url_, sizeof(url_), "%s/api/v1/device-bootstrap/%s/mqtt-credential",
                                request.base_url, request.hardware_id);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(url_)) { return ESP_ERR_INVALID_SIZE; }
    written = std::snprintf(authorization_, sizeof(authorization_), "Bearer %s", request.bootstrap_token);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(authorization_)) {
        std::memset(authorization_, 0, sizeof(authorization_));
        return ESP_ERR_INVALID_SIZE;
    }

    BootstrapHttpContext context{request.destination, request.capacity};
    esp_http_client_config_t config{};
    config.url = url_;
    config.cert_pem = request.ca_pem;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = static_cast<int>(request.timeout_ms);
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
    config.event_handler = onBootstrapHttpEvent;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.user_data = &context;
    config.skip_cert_common_name_check = false;
    config.crt_bundle_attach = request.ca_pem == nullptr ? esp_crt_bundle_attach : nullptr;
    config.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        std::memset(authorization_, 0, sizeof(authorization_));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_set_header(client, "Authorization", authorization_);
    if (err == ESP_OK) { err = esp_http_client_set_post_field(client, nullptr, 0); }
    if (err == ESP_OK) { err = esp_http_client_perform(client); }
    if (err == ESP_OK) {
        result.status_code = esp_http_client_get_status_code(client);
        result.size = context.size;
        result.has_retry_after = context.has_retry_after;
        result.retry_after_s = context.retry_after_s;
    }
    const esp_err_t cleanup = esp_http_client_cleanup(client);
    std::memset(authorization_, 0, sizeof(authorization_));
    std::memset(url_, 0, sizeof(url_));
    if (context.overflow) { return ESP_ERR_INVALID_SIZE; }
    if (context.invalid_retry_after) { result.has_retry_after = false; } // malformed header: ignore, never invent
    if (err != ESP_OK) { return err; }
    return cleanup;
}

// --- Durable credential store -------------------------------------------------
//
// Atomicity model: two self-contained A/B slots ("slotA"/"slotB"), each one
// complete record (schema + sequence + state + attempt flag + round counter
// + both full credentials + CRC). Every transition writes ONE candidate
// slot -- always the one not currently active -- with a single
// nvs_set_blob() + nvs_commit() + readback, and only mirrors the new values
// into RAM once that single write is itself verified. On load, both slots
// are validated independently and the valid one with the higher sequence
// number wins (ties cannot happen: sequence always increases by exactly one
// per successful write). There is deliberately no separate "which slot is
// active" pointer key -- writing that *after* the slot would just recreate
// a second, smaller non-atomic window one level up.

namespace {
constexpr const char *kNamespace = "easyled_mqtt"; // <=15 chars: NVS_KEY_NAME_MAX_SIZE-1
constexpr const char *kSlotKeys[2] = {"slotA", "slotB"};
constexpr uint32_t kSlotMagic = 0x4d51534cu; // "MQSL"-ish, arbitrary distinct value
constexpr uint8_t kSchema = 2; // format not yet released; no migration from schema 1 needed

#pragma pack(push, 1)
struct WireCredential {
    uint8_t valid;
    char credential_id[kUuidBytes + 1];
    char hardware_binding_id[kUuidBytes + 1];
    uint32_t generation;
    char username[kUuidBytes + 1];
    char client_id[kUuidBytes + 1];
    char password[kMaxPasswordBytes + 1];
    char broker_uri[kMaxBrokerUriBytes + 1];
    char trust_ref[kMaxTrustRefBytes + 1];
    char topic_state[kMaxTopicBytes + 1];
    char topic_event[kMaxTopicBytes + 1];
    char topic_telemetry[kMaxTopicBytes + 1];
    char topic_cmd[kMaxTopicBytes + 1];
    char expires_at[kMaxExpiresAtBytes + 1];
};
struct WireSlot {
    uint32_t magic;
    uint8_t schema;
    uint8_t state;
    uint8_t attempt_uses_pending;
    uint8_t reserved;
    uint32_t sequence;
    uint32_t rounds_without_success;
    WireCredential current;
    WireCredential pending;
    uint32_t crc; // over every byte above, computed last
};
#pragma pack(pop)

template <typename Wire>
void sealWithCrc(Wire &wire)
{
    wire.crc = 0;
    wire.crc = crc32(reinterpret_cast<const uint8_t *>(&wire), sizeof(Wire) - sizeof(wire.crc));
}

template <typename Wire>
bool crcValid(const Wire &wire)
{
    return crc32(reinterpret_cast<const uint8_t *>(&wire), sizeof(Wire) - sizeof(wire.crc)) == wire.crc;
}

void credentialToWire(const StoredCredential &in, WireCredential &wire)
{
    wire = WireCredential{};
    wire.valid = in.valid ? 1 : 0;
    std::memcpy(wire.credential_id, in.credential_id, sizeof(wire.credential_id));
    std::memcpy(wire.hardware_binding_id, in.hardware_binding_id, sizeof(wire.hardware_binding_id));
    wire.generation = in.generation;
    std::memcpy(wire.username, in.username, sizeof(wire.username));
    std::memcpy(wire.client_id, in.client_id, sizeof(wire.client_id));
    std::memcpy(wire.password, in.password, sizeof(wire.password));
    std::memcpy(wire.broker_uri, in.broker_uri, sizeof(wire.broker_uri));
    std::memcpy(wire.trust_ref, in.trust_ref, sizeof(wire.trust_ref));
    std::memcpy(wire.topic_state, in.topic_state, sizeof(wire.topic_state));
    std::memcpy(wire.topic_event, in.topic_event, sizeof(wire.topic_event));
    std::memcpy(wire.topic_telemetry, in.topic_telemetry, sizeof(wire.topic_telemetry));
    std::memcpy(wire.topic_cmd, in.topic_cmd, sizeof(wire.topic_cmd));
    std::memcpy(wire.expires_at, in.expires_at, sizeof(wire.expires_at));
}

void wireToCredential(const WireCredential &wire, StoredCredential &out)
{
    out = StoredCredential{};
    out.valid = wire.valid != 0;
    std::memcpy(out.credential_id, wire.credential_id, sizeof(out.credential_id));
    std::memcpy(out.hardware_binding_id, wire.hardware_binding_id, sizeof(out.hardware_binding_id));
    out.generation = wire.generation;
    std::memcpy(out.username, wire.username, sizeof(out.username));
    std::memcpy(out.client_id, wire.client_id, sizeof(out.client_id));
    std::memcpy(out.password, wire.password, sizeof(out.password));
    std::memcpy(out.broker_uri, wire.broker_uri, sizeof(out.broker_uri));
    std::memcpy(out.trust_ref, wire.trust_ref, sizeof(out.trust_ref));
    std::memcpy(out.topic_state, wire.topic_state, sizeof(out.topic_state));
    std::memcpy(out.topic_event, wire.topic_event, sizeof(out.topic_event));
    std::memcpy(out.topic_telemetry, wire.topic_telemetry, sizeof(out.topic_telemetry));
    std::memcpy(out.topic_cmd, wire.topic_cmd, sizeof(out.topic_cmd));
    std::memcpy(out.expires_at, wire.expires_at, sizeof(out.expires_at));
}

// Whether a (state, attempt_uses_pending, current, pending) tuple is
// internally consistent -- catches a structurally valid (CRC-correct) slot
// that nonetheless claims an impossible combination, which is treated the
// same as CRC corruption: this slot is unusable, fall back to the other one.
bool invariantsOk(CredentialState state, bool attempt_uses_pending, const StoredCredential &current,
                  const StoredCredential &pending)
{
    switch (state) {
    case CredentialState::Empty:
        return !current.valid && !pending.valid;
    case CredentialState::CurrentConfirmed:
        return current.valid && !pending.valid;
    case CredentialState::PendingUnused:
        return pending.valid;
    case CredentialState::AttemptInFlight:
    case CredentialState::AmbiguousActivation:
        return attempt_uses_pending ? pending.valid : current.valid;
    case CredentialState::BootstrapRecovery:
    case CredentialState::ProvisioningFault:
        return true;
    }
    return false;
}

} // namespace

MqttCredentialStore::~MqttCredentialStore()
{
    end();
}

esp_err_t MqttCredentialStore::load()
{
    WireSlot slots[2]{};
    bool present[2] = {false, false};
    bool valid[2] = {false, false};

    for (int i = 0; i < 2; ++i) {
        size_t slot_size = sizeof(slots[i]);
        const esp_err_t err = nvs_get_blob(handle_, kSlotKeys[i], &slots[i], &slot_size);
        if (err == ESP_ERR_NVS_NOT_FOUND) { continue; }
        present[i] = true;
        if (err != ESP_OK) { continue; } // unusable (e.g. wrong stored size): same as corrupt
        if (slot_size != sizeof(slots[i]) || slots[i].magic != kSlotMagic || slots[i].schema != kSchema ||
            slots[i].state > static_cast<uint8_t>(CredentialState::ProvisioningFault) || !crcValid(slots[i])) {
            continue;
        }
        StoredCredential probe_current{}, probe_pending{};
        wireToCredential(slots[i].current, probe_current);
        wireToCredential(slots[i].pending, probe_pending);
        if (!invariantsOk(static_cast<CredentialState>(slots[i].state), slots[i].attempt_uses_pending != 0,
                          probe_current, probe_pending)) {
            continue;
        }
        valid[i] = true;
    }

    if (!valid[0] && !valid[1]) {
        // A key that exists but never validated is corruption, not a fresh
        // store: fail closed rather than silently falling back to Empty.
        if (present[0] || present[1]) { return ESP_ERR_INVALID_RESPONSE; }
        state_ = CredentialState::Empty;
        current_ = StoredCredential{};
        pending_ = StoredCredential{};
        attempt_uses_pending_ = true;
        rounds_without_success_ = 0;
        active_slot_ = -1;
        next_sequence_ = 1;
        return ESP_OK;
    }

    const int winner = (valid[0] && valid[1]) ? (slots[0].sequence >= slots[1].sequence ? 0 : 1)
                                               : (valid[0] ? 0 : 1);
    const WireSlot &slot = slots[winner];
    state_ = static_cast<CredentialState>(slot.state);
    attempt_uses_pending_ = slot.attempt_uses_pending != 0;
    rounds_without_success_ = slot.rounds_without_success;
    wireToCredential(slot.current, current_);
    wireToCredential(slot.pending, pending_);
    active_slot_ = winner;
    next_sequence_ = slot.sequence + 1;

    // A reboot found mid-attempt cannot know whether the broker saw it
    // before power was lost -- the only safe reading is "ambiguous", never
    // "safe to retry the same way" and never "assume failure".
    if (state_ == CredentialState::AttemptInFlight) { state_ = CredentialState::AmbiguousActivation; }
    return ESP_OK;
}

esp_err_t MqttCredentialStore::commitTransition(CredentialState new_state, bool new_attempt_uses_pending,
                                                uint32_t new_rounds, const StoredCredential &new_current,
                                                const StoredCredential &new_pending)
{
    WireSlot slot{};
    slot.magic = kSlotMagic;
    slot.schema = kSchema;
    slot.state = static_cast<uint8_t>(new_state);
    slot.attempt_uses_pending = new_attempt_uses_pending ? 1 : 0;
    slot.sequence = next_sequence_;
    slot.rounds_without_success = new_rounds;
    credentialToWire(new_current, slot.current);
    credentialToWire(new_pending, slot.pending);
    sealWithCrc(slot);

    const int target = (active_slot_ == 0) ? 1 : 0;

    // Fault injection (test-only; see StoreTestFault) happens strictly
    // before any RAM mutation below, so every injected path proves RAM and
    // the active slot are untouched by a failed write.
    esp_err_t err;
    if (fault_for_test_ == StoreTestFault::BeforeWrite) {
        err = ESP_FAIL; // the write is never attempted at all
    } else if (fault_for_test_ == StoreTestFault::DuringWrite) {
        err = ESP_FAIL; // as if nvs_set_blob() itself failed partway through
    } else {
        err = nvs_set_blob(handle_, kSlotKeys[target], &slot, sizeof(slot));
        if (err == ESP_OK) {
            if (fault_for_test_ == StoreTestFault::DuringCommit) {
                // Simulate a crash between the write and the commit. A
                // bare nvs_set_blob() with no following nvs_commit() is not
                // by itself enough to prove nothing survives: NVS may keep
                // an uncommitted write visible to a same-process handle
                // close+reopen (this store's end()+init(), used to model a
                // reboot in tests) even though a real power cut would have
                // lost it. Overwrite the candidate slot with garbage and
                // commit THAT for real, so the slot is unambiguously
                // invalid on any subsequent load() regardless of that
                // visibility detail.
                static constexpr uint8_t kFaultGarbage[4] = {0xde, 0xad, 0xbe, 0xef};
                nvs_set_blob(handle_, kSlotKeys[target], kFaultGarbage, sizeof(kFaultGarbage));
                nvs_commit(handle_);
                err = ESP_FAIL;
            } else {
                err = nvs_commit(handle_);
                if (err == ESP_OK) {
                    size_t verify_size = sizeof(record_);
                    err = nvs_get_blob(handle_, kSlotKeys[target], record_, &verify_size);
                    if (err == ESP_OK) {
                        const bool forced_mismatch = fault_for_test_ == StoreTestFault::DuringReadback;
                        if (forced_mismatch || verify_size != sizeof(slot) ||
                            std::memcmp(record_, &slot, sizeof(slot)) != 0) {
                            err = ESP_ERR_INVALID_RESPONSE;
                        }
                    }
                }
            }
        }
    }
    fault_for_test_ = StoreTestFault::None;
    if (err != ESP_OK) { return err; } // RAM and the active slot are exactly as they were

    active_slot_ = target;
    next_sequence_ = slot.sequence + 1;
    state_ = new_state;
    attempt_uses_pending_ = new_attempt_uses_pending;
    rounds_without_success_ = new_rounds;
    current_ = new_current;
    pending_ = new_pending;
    return ESP_OK;
}

esp_err_t MqttCredentialStore::init()
{
    if (initialized_) { return ESP_ERR_INVALID_STATE; }
    // nvs_flash_erase_partition() de-initializes the partition driver as part
    // of erasing it (see nvs_flash.h); re-initializing here is also what
    // every reboot/reopen needs, since end() only closes our own handle and
    // never tears down the shared partition-level driver state. Idempotent
    // when already initialized, matching pixels_durable_storage's pattern.
    esp_err_t err = nvs_flash_init_partition("easyled");
    if (err != ESP_OK) { return err; }
    err = nvs_open_from_partition("easyled", kNamespace, NVS_READWRITE, &handle_);
    if (err != ESP_OK) { return err; }
    err = load();
    if (err != ESP_OK) {
        nvs_close(handle_);
        handle_ = 0;
        return err;
    }
    initialized_ = true;
    return ESP_OK;
}

void MqttCredentialStore::end()
{
    if (handle_ != 0) { nvs_close(handle_); handle_ = 0; }
    initialized_ = false;
}

esp_err_t MqttCredentialStore::storePending(const StoredCredential &credential)
{
    if (!initialized_ || !credential.valid) { return ESP_ERR_INVALID_STATE; }
    return commitTransition(CredentialState::PendingUnused, attempt_uses_pending_, 0, current_, credential);
}

esp_err_t MqttCredentialStore::beginAttempt(bool usingPending)
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    if (usingPending ? !pending_.valid : !current_.valid) { return ESP_ERR_INVALID_STATE; }
    return commitTransition(CredentialState::AttemptInFlight, usingPending, rounds_without_success_, current_,
                            pending_);
}

esp_err_t MqttCredentialStore::confirmConnected()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const StoredCredential new_current = attempt_uses_pending_ ? pending_ : current_;
    return commitTransition(CredentialState::CurrentConfirmed, attempt_uses_pending_, 0, new_current,
                            StoredCredential{});
}

esp_err_t MqttCredentialStore::reportPreAuthFailure()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    // Proven to have happened before authentication: neither credential is
    // touched. Only restore a sensible resting state from AttemptInFlight.
    const CredentialState resting =
        pending_.valid ? CredentialState::PendingUnused : CredentialState::CurrentConfirmed;
    return commitTransition(resting, attempt_uses_pending_, rounds_without_success_, current_, pending_);
}

esp_err_t MqttCredentialStore::reportAmbiguous(bool completesRound, bool enterBootstrapRecovery)
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    if (enterBootstrapRecovery && !completesRound) { return ESP_ERR_INVALID_ARG; }
    const uint32_t new_rounds = completesRound ? rounds_without_success_ + 1 : rounds_without_success_;
    const CredentialState next_state =
        enterBootstrapRecovery ? CredentialState::BootstrapRecovery : CredentialState::AmbiguousActivation;
    return commitTransition(next_state, attempt_uses_pending_, new_rounds, current_, pending_);
}

esp_err_t MqttCredentialStore::enterBootstrapRecovery()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    return commitTransition(CredentialState::BootstrapRecovery, attempt_uses_pending_, rounds_without_success_,
                            current_, pending_);
}

esp_err_t MqttCredentialStore::enterProvisioningFault()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    return commitTransition(CredentialState::ProvisioningFault, attempt_uses_pending_, rounds_without_success_,
                            current_, pending_);
}

esp_err_t MqttCredentialStore::eraseAll()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    return commitTransition(CredentialState::Empty, true, 0, StoredCredential{}, StoredCredential{});
}

// --- Monotonic clock -----------------------------------------------------------

int64_t EspMonotonicClock::nowUs() const
{
    return esp_timer_get_time();
}

// --- Recovery coordinator -------------------------------------------------------

MqttCredentialCoordinator::MqttCredentialCoordinator(MqttCredentialStore &store, MqttBootstrapClient &client,
                                                      TrustResolver &trust, MonotonicClock &clock,
                                                      const Config &config)
    : store_(store), client_(client), trust_(trust), clock_(clock), config_(config),
      backoff_ms_(config.initial_backoff_ms)
{
}

const StoredCredential &MqttCredentialCoordinator::credentialToTry() const
{
    const CredentialState state = store_.state();
    if (state == CredentialState::AttemptInFlight) {
        return store_.attemptUsesPending() ? store_.pending() : store_.current();
    }
    // Not yet committed via beginAttempt(): mirror poll()'s own choice, so a
    // caller can ask "which credential does that action mean" before it
    // durably marks the attempt.
    if (state == CredentialState::AmbiguousActivation && (!store_.hasPending() || pending_tried_this_round_) &&
        store_.hasCurrent()) {
        return store_.current();
    }
    return store_.pending();
}

void MqttCredentialCoordinator::scheduleNextAttempt(bool have_retry_after, uint32_t retry_after_s)
{
    const int64_t now = clock_.nowUs();
    if (have_retry_after) {
        // Retry-After is normative when present; jitter is still applied so
        // a fleet does not retry in lockstep.
        const uint32_t jitter_pct = 80 + static_cast<uint32_t>(esp_timer_get_time() % 41); // 80..120
        const int64_t delay_us = static_cast<int64_t>(retry_after_s) * 1000000LL * jitter_pct / 100;
        next_action_at_us_ = now + delay_us;
        return;
    }
    const uint32_t jitter_pct = 80 + static_cast<uint32_t>(esp_timer_get_time() % 41); // +/-20%
    const int64_t delay_us = static_cast<int64_t>(backoff_ms_) * 1000LL * jitter_pct / 100;
    next_action_at_us_ = now + delay_us;
    backoff_ms_ = backoff_ms_ >= config_.max_backoff_ms ? config_.max_backoff_ms : backoff_ms_ * 2;
    if (backoff_ms_ > config_.max_backoff_ms) { backoff_ms_ = config_.max_backoff_ms; }
}

CoordinatorAction MqttCredentialCoordinator::poll()
{
    const int64_t now = clock_.nowUs();
    const CredentialState state = store_.state();

    if (state == CredentialState::ProvisioningFault) {
        return CoordinatorAction::Idle; // stays faulted until an external state change; never auto-recovers
    }
    if (state == CredentialState::AttemptInFlight) {
        // The caller (integration layer) owns reporting the outcome; the
        // coordinator never times out an in-flight attempt on its own --
        // doing so here would race whatever NetworkSession is doing.
        return CoordinatorAction::Idle;
    }
    if (now < next_action_at_us_) { return CoordinatorAction::Idle; }

    if (state == CredentialState::Empty || state == CredentialState::BootstrapRecovery) {
        return doBootstrap() == ESP_OK ? CoordinatorAction::ConnectWithPending : CoordinatorAction::Idle;
    }
    if (state == CredentialState::PendingUnused) { return CoordinatorAction::ConnectWithPending; }
    if (state == CredentialState::AmbiguousActivation) {
        // New credential tried first, every round; the old one is offered
        // only once pending has already been tried this same round (never
        // as the plan -- see reportAmbiguous()/reportPreAuthFailure()).
        if (store_.hasPending() && !pending_tried_this_round_) { return CoordinatorAction::ConnectWithPending; }
        if (store_.hasCurrent()) { return CoordinatorAction::ConnectWithCurrentCourtesy; }
        if (store_.hasPending()) { return CoordinatorAction::ConnectWithPending; } // no current to try as courtesy
        return doBootstrap() == ESP_OK ? CoordinatorAction::ConnectWithPending : CoordinatorAction::Idle;
    }
    // CurrentConfirmed with no pending: nothing to do until a rotation is issued.
    return CoordinatorAction::Idle;
}

esp_err_t MqttCredentialCoordinator::doBootstrap()
{
    BootstrapRequest request{};
    request.base_url = config_.base_url;
    request.hardware_id = config_.hardware_id;
    request.bootstrap_token = config_.bootstrap_token;
    request.ca_pem = nullptr; // bootstrap HTTPS uses the IDF certificate bundle
    request.timeout_ms = config_.timeout_ms;
    request.destination = response_buffer_;
    request.capacity = sizeof(response_buffer_);
    BootstrapResult result{};
    const esp_err_t transport_err = client_.requestCredential(request, result);
    if (transport_err != ESP_OK) {
        // Timeout/reset/no response: the request may have been issued and
        // the response merely lost -- ask again after backoff, exactly as a
        // malformed/rejected 200 body would, never a provisioning fault.
        scheduleNextAttempt(false, 0);
        return ESP_FAIL;
    }

    switch (result.status_code) {
    case 200:
        break;
    case 401: {
        // commitTransition() never mutates RAM before a write succeeds, so
        // a failed enterProvisioningFault() call leaves whatever state
        // (current/pending/counters) existed before this call exactly as
        // it was -- there is nothing further to "preserve" here beyond
        // propagating that failure instead of silently discarding it.
        const esp_err_t fault_err = store_.enterProvisioningFault();
        scheduleNextAttempt(false, 0);
        return fault_err == ESP_OK ? ESP_FAIL : fault_err;
    }
    case 403: {
        const esp_err_t fault_err = store_.enterProvisioningFault();
        scheduleNextAttempt(false, 0);
        return fault_err == ESP_OK ? ESP_FAIL : fault_err;
    }
    case 429:
        scheduleNextAttempt(result.has_retry_after, result.retry_after_s);
        return ESP_FAIL;
    case 503:
        next_action_at_us_ = clock_.nowUs() + static_cast<int64_t>(config_.max_backoff_ms) * 1000;
        return ESP_FAIL;
    case 500:
        scheduleNextAttempt(false, 0);
        return ESP_FAIL;
    default:
        // 404/405 (firmware/config bug) or anything else undocumented: do
        // not tight-loop, but do not invent a provisioning fault either.
        scheduleNextAttempt(false, 0);
        return ESP_FAIL;
    }

    DeviceCredentialResponse parsed{};
    ParseError parse_error = ParseError::None;
    esp_err_t err = parseDeviceCredentialResponse(response_buffer_, result.size, config_.expected_device_id,
                                                  config_.expected_hardware_binding_id, config_.expected_generation,
                                                  parsed, parse_error);
    if (err == ESP_OK) {
        const char *ca_pem = nullptr;
        err = trust_.resolve(parsed.trust_ref, &ca_pem); // unknown trust_ref: same as an invalid body
    }
    if (err != ESP_OK) {
        scheduleNextAttempt(false, 0); // invalid body: count as a failed attempt, never persist or use it
        return ESP_FAIL;
    }

    StoredCredential credential{};
    credential.valid = true;
    std::memcpy(credential.credential_id, parsed.credential_id, sizeof(credential.credential_id));
    std::memcpy(credential.hardware_binding_id, parsed.hardware_binding_id, sizeof(credential.hardware_binding_id));
    credential.generation = parsed.generation;
    std::memcpy(credential.username, parsed.username, sizeof(credential.username));
    std::memcpy(credential.client_id, parsed.client_id, sizeof(credential.client_id));
    std::memcpy(credential.password, parsed.password, sizeof(credential.password));
    std::memcpy(credential.broker_uri, parsed.broker_uri, sizeof(credential.broker_uri));
    std::memcpy(credential.trust_ref, parsed.trust_ref, sizeof(credential.trust_ref));
    std::memcpy(credential.topic_state, parsed.topic_state, sizeof(credential.topic_state));
    std::memcpy(credential.topic_event, parsed.topic_event, sizeof(credential.topic_event));
    std::memcpy(credential.topic_telemetry, parsed.topic_telemetry, sizeof(credential.topic_telemetry));
    std::memcpy(credential.topic_cmd, parsed.topic_cmd, sizeof(credential.topic_cmd));
    std::memcpy(credential.expires_at, parsed.expires_at, sizeof(credential.expires_at));
    const esp_err_t store_err = store_.storePending(credential);
    if (store_err != ESP_OK) { scheduleNextAttempt(false, 0); }
    return store_err;
}

esp_err_t MqttCredentialCoordinator::reportConnected()
{
    const esp_err_t err = store_.confirmConnected();
    if (err != ESP_OK) { return err; }
    pending_tried_this_round_ = false;
    backoff_ms_ = config_.initial_backoff_ms;
    next_action_at_us_ = clock_.nowUs();
    return ESP_OK;
}

esp_err_t MqttCredentialCoordinator::reportPreAuthFailure()
{
    // Proven to have happened before authentication: not an ambiguous
    // outcome, so it does not consume a round attempt against either
    // credential's "tried this round" bookkeeping either.
    const esp_err_t err = store_.reportPreAuthFailure();
    scheduleNextAttempt(false, 0); // never a tight loop, regardless of the durable result
    return err;
}

esp_err_t MqttCredentialCoordinator::reportAmbiguous()
{
    // A round is "pending first, current as courtesy second" -- with only
    // pending (no current to fall back on), a single ambiguous attempt
    // against it already completes the round. roundsWithoutSuccess() must
    // only advance on that completion, never on every individual attempt.
    const bool was_pending = store_.attemptUsesPending();
    const bool completes_round = !was_pending || !store_.hasCurrent();
    const uint32_t completed_rounds = store_.roundsWithoutSuccess();
    const bool enter_recovery = completes_round &&
        (config_.max_rounds_before_recovery == 0 ||
         completed_rounds >= config_.max_rounds_before_recovery - 1);
    const esp_err_t err = store_.reportAmbiguous(completes_round, enter_recovery);
    if (err == ESP_OK) {
        if (completes_round) {
            pending_tried_this_round_ = false;
        } else {
            // Pending just had its ambiguous attempt; offer the
            // current-as-courtesy action next, instead of retrying pending
            // immediately within the same round.
            pending_tried_this_round_ = true;
        }
    }
    scheduleNextAttempt(false, 0); // never a tight loop, regardless of the durable result
    return err;
}

// --- Session controller (orchestration only; see MqttTransportSession in
// the header for why this is not wired to a real NetworkSession this
// round) ----------------------------------------------------------------------

MqttSessionController::MqttSessionController(MqttCredentialCoordinator &coordinator, MqttCredentialStore &store,
                                             TrustResolver &trust, MqttTransportSession &transport)
    : coordinator_(coordinator), store_(store), trust_(trust), transport_(transport)
{
}

esp_err_t MqttSessionController::reportPendingResult()
{
    esp_err_t err;
    if (pending_result_ == PendingResult::Connected) {
        err = coordinator_.reportConnected();
        if (err == ESP_OK) { ready_ = true; }
    } else {
        err = coordinator_.reportAmbiguous(); // Outcome/PendingResult have no PreAuthFailure value -- see Seção 5
        if (err == ESP_OK) { ready_ = false; }
    }
    if (err != ESP_OK) { return err; } // pending_result_ stays set; the next tick() retries this same report
    pending_result_ = PendingResult::None;
    return ESP_OK;
}

esp_err_t MqttSessionController::attemptWith(const StoredCredential &credential, bool usingPending)
{
    const char *ca_pem = nullptr;
    if (trust_.resolve(credential.trust_ref, &ca_pem) != ESP_OK) {
        // The coordinator already resolved trust before ever persisting
        // this credential (doBootstrap()); reaching here would mean the
        // trust table shrank underneath an already-persisted/already-
        // confirmed credential. Defensive, not expected: do nothing this
        // tick rather than attempt a connection with no CA.
        return ESP_OK;
    }
    const esp_err_t begin_err = store_.beginAttempt(usingPending);
    if (begin_err != ESP_OK) { return begin_err; }
    // A new attempt is starting: whatever `restart()` below does, the
    // previous session (if any) is no longer the one in use -- ready_ no
    // longer describes "now" regardless of how this new attempt turns out.
    ready_ = false;

    MqttSessionIdentity identity{};
    identity.broker_uri = credential.broker_uri;
    identity.client_id = credential.client_id;
    identity.username = credential.username;
    identity.password = credential.password;
    identity.command_topic = credential.topic_cmd;
    identity.server_ca_pem = ca_pem;
    const esp_err_t restart_err = transport_.restart(identity);
    if (restart_err != ESP_OK) {
        // The attempt is already durably marked in flight; the transport
        // never actually got to try. Conservative, per Seção 5: there is no
        // transport-proven pre-auth evidence anywhere in this seam, so this
        // is reported exactly the same way any other inconclusive outcome is.
        pending_result_ = PendingResult::Ambiguous;
        return reportPendingResult();
    }
    attempt_active_ = true;
    return ESP_OK;
}

esp_err_t MqttSessionController::tick()
{
    // A terminal outcome that could not yet be persisted takes priority
    // over everything else: retry ONLY the durable report, never re-poll
    // or restart the transport while one is owed.
    if (pending_result_ != PendingResult::None) { return reportPendingResult(); }

    if (attempt_active_) {
        const MqttTransportSession::Outcome outcome = transport_.poll();
        if (outcome == MqttTransportSession::Outcome::StillTrying) { return ESP_OK; }
        attempt_active_ = false;
        pending_result_ =
            outcome == MqttTransportSession::Outcome::Connected ? PendingResult::Connected : PendingResult::Ambiguous;
        return reportPendingResult();
    }

    // The coordinator is consulted BEFORE any "just monitor the live
    // session" shortcut: a rotation can durably persist a new `pending`
    // credential at any time, including while `ready_` is still true from
    // the previous identity, and that must still preempt monitoring and
    // restart the transport with the new identity on this very tick --
    // never wait for the old session to first go bad on its own.
    const CoordinatorAction action = coordinator_.poll();
    if (action == CoordinatorAction::ConnectWithPending || action == CoordinatorAction::ConnectWithCurrentCourtesy) {
        return attemptWith(coordinator_.credentialToTry(), action == CoordinatorAction::ConnectWithPending);
    }

    if (ready_) {
        // The coordinator has nothing new to connect with (Idle, or a
        // RequestBootstrap already handled inside poll() itself): a durably
        // confirmed session stays monitored, never idle. Ordinary brief
        // loss is NetworkSession's own reconnect to absorb, silently
        // (transport_.poll() keeps returning StillTrying through it and
        // through any automatic reconnect CONNACK, never re-reporting that
        // as a new credential promotion -- see the real adapter). Only a
        // bounded post-confirmation reconnect/CONNACK deadline with no
        // fresh CONNACK reaches here as Ambiguous, and that must still fall
        // back into the existing conservative ambiguous/current retry path
        // rather than monitor a dead session forever.
        const MqttTransportSession::Outcome outcome = transport_.poll();
        if (outcome != MqttTransportSession::Outcome::Ambiguous) { return ESP_OK; }
        const esp_err_t begin_err = store_.beginAttempt(false); // the only credential a monitored session can be
        if (begin_err != ESP_OK) { return begin_err; } // ready_ untouched; retried next tick()
        pending_result_ = PendingResult::Ambiguous;
        return reportPendingResult(); // clears ready_ once this report itself persists
    }

    // Not ready_, and the coordinator has nothing to decide about CREDENTIAL
    // SELECTION from CurrentConfirmed with no pending -- but that credential
    // still needs its own first reconnect attempt after a reboot -- never
    // bootstrap merely because the device rebooted (Seção 5). Once this
    // succeeds, ready_ monitoring above takes over on every later tick();
    // once it durably reports Ambiguous instead, state leaves
    // CurrentConfirmed and this branch no longer matches, so there is no
    // risk of retrying it in a tight loop with no backoff.
    if (action == CoordinatorAction::Idle && store_.state() == CredentialState::CurrentConfirmed &&
        store_.hasCurrent() && !store_.hasPending()) {
        return attemptWith(store_.current(), false);
    }
    return ESP_OK;
}

} // namespace pixels_mqtt
