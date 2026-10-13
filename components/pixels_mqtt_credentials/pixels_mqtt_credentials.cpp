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

esp_err_t parseDeviceCredentialResponse(const char *json, size_t size, const char *expected_device_id,
                                        const char *expected_hardware_binding_id, DeviceCredentialResponse &out,
                                        ParseError &error)
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

    if (std::strcmp(device_id, expected_device_id) != 0 ||
        std::strcmp(hardware_binding_id, expected_hardware_binding_id) != 0) {
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
        request.hardware_id == nullptr || !uuid(request.hardware_id) ||
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

namespace {
constexpr const char *kNamespace = "easyled_mqtt"; // <=15 chars: NVS_KEY_NAME_MAX_SIZE-1
constexpr const char *kHeaderKey = "hdr";
constexpr const char *kCurrentKey = "current";
constexpr const char *kPendingKey = "pending";
constexpr uint32_t kHeaderMagic = 0x43524448; // "HDRC" little-endian
constexpr uint32_t kCredentialMagic = 0x44524543; // "CERD" little-endian
constexpr uint8_t kSchema = 1;

#pragma pack(push, 1)
struct WireHeader {
    uint32_t magic;
    uint8_t schema;
    uint8_t state;
    uint8_t attempt_uses_pending;
    uint8_t reserved;
    uint32_t rounds_without_success;
    uint32_t crc; // over every byte above, computed last
};
struct WireCredential {
    uint32_t magic;
    uint8_t schema;
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
    wire.magic = kCredentialMagic;
    wire.schema = kSchema;
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

// Writes `bytes`, commits, and reads it back byte-for-byte before trusting
// the write succeeded -- the same discipline as pixels_durable_storage,
// beyond whatever integrity check NVS already does internally.
esp_err_t writeBlobVerified(nvs_handle_t handle, const char *key, const void *bytes, size_t size, uint8_t *scratch,
                           size_t scratch_capacity)
{
    if (size > scratch_capacity) { return ESP_ERR_INVALID_SIZE; }
    esp_err_t err = nvs_set_blob(handle, key, bytes, size);
    if (err != ESP_OK) { return err; }
    err = nvs_commit(handle);
    if (err != ESP_OK) { return err; }
    size_t verify_size = scratch_capacity;
    err = nvs_get_blob(handle, key, scratch, &verify_size);
    if (err != ESP_OK) { return err; }
    if (verify_size != size || std::memcmp(scratch, bytes, size) != 0) { return ESP_ERR_INVALID_RESPONSE; }
    return ESP_OK;
}

} // namespace

MqttCredentialStore::~MqttCredentialStore()
{
    end();
}

esp_err_t MqttCredentialStore::load()
{
    WireHeader header{};
    size_t header_size = sizeof(header);
    esp_err_t err = nvs_get_blob(handle_, kHeaderKey, &header, &header_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        state_ = CredentialState::Empty;
        current_ = StoredCredential{};
        pending_ = StoredCredential{};
        attempt_uses_pending_ = true;
        rounds_without_success_ = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) { return err; }
    if (header_size != sizeof(header) || header.magic != kHeaderMagic || header.schema != kSchema ||
        header.state > static_cast<uint8_t>(CredentialState::ProvisioningFault) || !crcValid(header)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    state_ = static_cast<CredentialState>(header.state);
    attempt_uses_pending_ = header.attempt_uses_pending != 0;
    rounds_without_success_ = header.rounds_without_success;

    WireCredential wire{};
    size_t wire_size = sizeof(wire);
    err = nvs_get_blob(handle_, kCurrentKey, &wire, &wire_size);
    if (err == ESP_OK) {
        if (wire_size != sizeof(wire) || wire.magic != kCredentialMagic || wire.schema != kSchema ||
            !crcValid(wire)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        wireToCredential(wire, current_);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        current_ = StoredCredential{};
    } else {
        return err;
    }

    wire_size = sizeof(wire);
    err = nvs_get_blob(handle_, kPendingKey, &wire, &wire_size);
    if (err == ESP_OK) {
        if (wire_size != sizeof(wire) || wire.magic != kCredentialMagic || wire.schema != kSchema ||
            !crcValid(wire)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        wireToCredential(wire, pending_);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        pending_ = StoredCredential{};
    } else {
        return err;
    }

    // A reboot found mid-attempt cannot know whether the broker saw it
    // before power was lost -- the only safe reading is "ambiguous", never
    // "safe to retry the same way" and never "assume failure".
    if (state_ == CredentialState::AttemptInFlight) { state_ = CredentialState::AmbiguousActivation; }
    return ESP_OK;
}

esp_err_t MqttCredentialStore::commitAndVerify()
{
    WireHeader header{};
    header.magic = kHeaderMagic;
    header.schema = kSchema;
    header.state = static_cast<uint8_t>(state_);
    header.attempt_uses_pending = attempt_uses_pending_ ? 1 : 0;
    header.rounds_without_success = rounds_without_success_;
    sealWithCrc(header);
    esp_err_t err = writeBlobVerified(handle_, kHeaderKey, &header, sizeof(header), record_, sizeof(record_));
    if (err != ESP_OK) { return err; }

    WireCredential wire{};
    credentialToWire(current_, wire);
    sealWithCrc(wire);
    err = writeBlobVerified(handle_, kCurrentKey, &wire, sizeof(wire), record_, sizeof(record_));
    if (err != ESP_OK) { return err; }

    credentialToWire(pending_, wire);
    sealWithCrc(wire);
    return writeBlobVerified(handle_, kPendingKey, &wire, sizeof(wire), record_, sizeof(record_));
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
    pending_ = credential;
    state_ = CredentialState::PendingUnused;
    rounds_without_success_ = 0;
    const esp_err_t err = commitAndVerify();
    if (err != ESP_OK) { return err; }
    return ESP_OK;
}

esp_err_t MqttCredentialStore::beginAttempt(bool usingPending)
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    if (usingPending ? !pending_.valid : !current_.valid) { return ESP_ERR_INVALID_STATE; }
    attempt_uses_pending_ = usingPending;
    state_ = CredentialState::AttemptInFlight;
    return commitAndVerify();
}

esp_err_t MqttCredentialStore::confirmConnected()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    if (attempt_uses_pending_) {
        current_ = pending_;
        pending_ = StoredCredential{};
    }
    // else: the courtesy retry on `current` succeeded; `current` stays, and
    // any still-stored `pending` (now proven never to have worked within
    // this round) is also cleared -- there is no third slot to keep it in.
    pending_ = attempt_uses_pending_ ? pending_ : StoredCredential{};
    state_ = CredentialState::CurrentConfirmed;
    rounds_without_success_ = 0;
    return commitAndVerify();
}

esp_err_t MqttCredentialStore::reportPreAuthFailure()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    // Proven to have happened before authentication: neither credential is
    // touched. Only restore a sensible resting state from AttemptInFlight.
    state_ = pending_.valid ? CredentialState::PendingUnused : CredentialState::CurrentConfirmed;
    return commitAndVerify();
}

esp_err_t MqttCredentialStore::reportAmbiguous()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    state_ = CredentialState::AmbiguousActivation;
    ++rounds_without_success_;
    return commitAndVerify();
}

esp_err_t MqttCredentialStore::enterBootstrapRecovery()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    state_ = CredentialState::BootstrapRecovery;
    return commitAndVerify();
}

esp_err_t MqttCredentialStore::enterProvisioningFault()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    state_ = CredentialState::ProvisioningFault;
    return commitAndVerify();
}

esp_err_t MqttCredentialStore::eraseAll()
{
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    state_ = CredentialState::Empty;
    current_ = StoredCredential{};
    pending_ = StoredCredential{};
    attempt_uses_pending_ = true;
    rounds_without_success_ = 0;
    return commitAndVerify();
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
    case 401:
        store_.enterProvisioningFault();
        scheduleNextAttempt(false, 0);
        return ESP_FAIL;
    case 403:
        store_.enterProvisioningFault();
        scheduleNextAttempt(false, 0);
        return ESP_FAIL;
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
                                                  config_.expected_hardware_binding_id, parsed, parse_error);
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
    return store_.storePending(credential);
}

void MqttCredentialCoordinator::reportConnected()
{
    store_.confirmConnected();
    pending_tried_this_round_ = false;
    backoff_ms_ = config_.initial_backoff_ms;
    next_action_at_us_ = clock_.nowUs();
}

void MqttCredentialCoordinator::reportPreAuthFailure()
{
    // Proven to have happened before authentication: not an ambiguous
    // outcome, so it does not consume a round attempt against either
    // credential's "tried this round" bookkeeping either.
    store_.reportPreAuthFailure();
    scheduleNextAttempt(false, 0);
}

void MqttCredentialCoordinator::reportAmbiguous()
{
    const bool was_pending = store_.attemptUsesPending();
    store_.reportAmbiguous();
    if (was_pending) {
        // Pending just had its ambiguous attempt; offer the current-as-
        // courtesy action next, instead of retrying pending immediately.
        pending_tried_this_round_ = true;
    } else {
        // The courtesy attempt on `current` also ended ambiguously: this
        // round is now fully complete either way.
        pending_tried_this_round_ = false;
    }
    if (store_.roundsWithoutSuccess() >= config_.max_rounds_before_recovery) {
        store_.enterBootstrapRecovery();
        pending_tried_this_round_ = false;
    }
    scheduleNextAttempt(false, 0);
}

} // namespace pixels_mqtt
