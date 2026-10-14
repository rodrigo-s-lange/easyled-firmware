#include "pixels_mqtt_credentials.hpp"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

extern "C" {
extern const uint8_t device_response_v1_json_start[] asm("_binary_device_response_v1_json_start");
extern const uint8_t device_response_v1_json_end[] asm("_binary_device_response_v1_json_end");
}

namespace {

using namespace pixels_mqtt;

int checks = 0, failures = 0;
void check(bool value, int line)
{
    ++checks;
    if (!value) { ++failures; std::printf("FAILED check %d (line %d)\n", checks, line); }
}
#define CHECK(expr) check((expr), __LINE__)

const char *fixtureBytes() { return reinterpret_cast<const char *>(device_response_v1_json_start); }
size_t fixtureSize()
{
    return static_cast<size_t>(device_response_v1_json_end - device_response_v1_json_start) - 1; // EMBED_TXTFILES NUL
}

constexpr char kDeviceId[] = "5b0f2d4e-8c1a-4f3b-9d6e-2a7c1b9e4f10";
constexpr char kHardwareBindingId[] = "c1d2e3f4-0a1b-4c2d-8e3f-4a5b6c7d8e9f";
constexpr char kCredentialId[] = "0e9d8c7b-6a5f-4e3d-9c2b-1a0f9e8d7c6b";
constexpr char kOtherDeviceId[] = "11111111-1111-4111-8111-111111111111";
constexpr char kTrustRef[] = "easyled-broker-ca-v1";
constexpr char kCaPem[] = "-----BEGIN CERTIFICATE-----\nSYNTHETIC-TEST-ONLY\n-----END CERTIFICATE-----\n";
// The physical bootstrap path parameter: lower-hex, NEVER a UUID, and never
// the same string as kHardwareBindingId above (OpenAPI: "^[0-9a-f]{1,64}$",
// which a UUID's dashes cannot satisfy).
constexpr char kHardwareId[] = "a1b2c3d4e5f6";
constexpr uint32_t kGeneration = 2;

esp_err_t sha256(const void *bytes, size_t size, uint8_t digest[32])
{
    static const psa_status_t init_status = psa_crypto_init();
    if (init_status != PSA_SUCCESS) { return ESP_FAIL; }
    size_t out_size = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, static_cast<const uint8_t *>(bytes), size, digest, 32, &out_size) !=
            PSA_SUCCESS ||
        out_size != 32) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// --- Template for a syntactically-complete, otherwise-valid response, so
// each negative test varies exactly the one field it means to exercise. ----
struct ResponseFields {
    const char *device_id = kDeviceId;
    const char *hardware_binding_id = kHardwareBindingId;
    uint32_t generation = kGeneration;
    const char *credential_id = kCredentialId;
    const char *status = "pending";
    const char *kind = "rotation";
    const char *expires_at = "2026-10-11T12:15:00.123456Z";
    const char *activation = "first_successful_mqtt_authentication";
    const char *username = kDeviceId;
    const char *client_id = kDeviceId;
    const char *password = "SYNTHETIC-NOT-A-CREDENTIAL-0123456789abcdef";
    const char *broker_uri = "wss://mqtt.example.invalid:8085/mqtt";
    const char *trust_ref = kTrustRef;
    bool verify_server = true;
    const char *topic_state = "tenants/t1/devices/d1/state";
    const char *topic_event = "tenants/t1/devices/d1/event";
    const char *topic_telemetry = "tenants/t1/devices/d1/telemetry";
    const char *topic_cmd = "tenants/t1/devices/d1/cmd";
};

int buildResponse(char *out, size_t capacity, const ResponseFields &f)
{
    return std::snprintf(
        out, capacity,
        "{\"device_id\":\"%s\",\"hardware_binding_id\":\"%s\",\"generation\":%u,"
        "\"credential\":{\"id\":\"%s\",\"status\":\"%s\",\"kind\":\"%s\",\"expires_at\":\"%s\","
        "\"activation\":\"%s\"},"
        "\"mqtt\":{\"username\":\"%s\",\"client_id\":\"%s\",\"password\":\"%s\",\"broker_uri\":\"%s\","
        "\"tls\":{\"trust_ref\":\"%s\",\"verify_server\":%s},"
        "\"topics\":{\"publish\":{\"state\":\"%s\",\"event\":\"%s\",\"telemetry\":\"%s\"},"
        "\"subscribe\":{\"cmd\":\"%s\"}}}}",
        f.device_id, f.hardware_binding_id, static_cast<unsigned>(f.generation), f.credential_id, f.status, f.kind,
        f.expires_at,
        f.activation, f.username, f.client_id, f.password, f.broker_uri, f.trust_ref,
        f.verify_server ? "true" : "false", f.topic_state, f.topic_event, f.topic_telemetry, f.topic_cmd);
}

esp_err_t parseTemplate(const ResponseFields &f, DeviceCredentialResponse &out, ParseError &error,
                        const char *expected_device_id = kDeviceId,
                        const char *expected_binding_id = kHardwareBindingId,
                        uint32_t expected_generation = kGeneration)
{
    static char buffer[2048];
    const int written = buildResponse(buffer, sizeof(buffer), f);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(buffer)) { return ESP_ERR_INVALID_SIZE; }
    return parseDeviceCredentialResponse(buffer, static_cast<size_t>(written), expected_device_id,
                                         expected_binding_id, expected_generation, out, error);
}

// --- Fakes -------------------------------------------------------------------

class FakeBootstrapClient final : public MqttBootstrapClient {
public:
    esp_err_t requestCredential(const BootstrapRequest &request, BootstrapResult &result) override
    {
        ++calls;
        last_request = request;
        if (fail_transport) { return ESP_FAIL; }
        result = BootstrapResult{};
        result.status_code = status_code;
        result.has_retry_after = has_retry_after;
        result.retry_after_s = retry_after_s;
        if (status_code == 200 && body != nullptr) {
            if (body_size > request.capacity) { return ESP_ERR_INVALID_SIZE; }
            std::memcpy(request.destination, body, body_size);
            result.size = body_size;
        }
        return ESP_OK;
    }
    void reset() { calls = 0; fail_transport = false; }
    int calls = 0;
    bool fail_transport = false;
    int status_code = 200;
    const char *body = nullptr;
    size_t body_size = 0;
    bool has_retry_after = false;
    uint32_t retry_after_s = 0;
    BootstrapRequest last_request{};
};

class FakeClock final : public MonotonicClock {
public:
    int64_t nowUs() const override { return now_us_; }
    int64_t now_us_ = 1000000;
};

// Fake MQTT transport: never a real NetworkSession, broker or socket. See
// MqttTransportSession in pixels_mqtt_credentials.hpp for why its Outcome
// has no PreAuthFailure value -- Ambiguous is what every transport failure
// reports here, including one standing in for a TCP_TRANSPORT error
// received while waiting for CONNACK (i.e. strictly after CONNECT was
// already sent), which is exactly the #47-B review's explicit concern.
class FakeTransportSession final : public MqttTransportSession {
public:
    esp_err_t restart(const MqttSessionIdentity &identity) override
    {
        ++restart_calls;
        last_identity = identity;
        if (fail_restart) { return ESP_FAIL; }
        outcome_to_report = Outcome::StillTrying;
        return ESP_OK;
    }
    void end() override { ++end_calls; }
    Outcome poll() override { return outcome_to_report; }

    int restart_calls = 0;
    int end_calls = 0;
    bool fail_restart = false;
    MqttSessionIdentity last_identity{};
    Outcome outcome_to_report = Outcome::StillTrying;
};

StoredCredential makeCredential(const char *credential_id, const char *password)
{
    StoredCredential c{};
    c.valid = true;
    std::strcpy(c.credential_id, credential_id);
    std::strcpy(c.hardware_binding_id, kHardwareBindingId);
    c.generation = kGeneration;
    std::strcpy(c.username, kDeviceId);
    std::strcpy(c.client_id, kDeviceId);
    std::strcpy(c.password, password);
    std::strcpy(c.broker_uri, "wss://mqtt.example.invalid:8085/mqtt");
    std::strcpy(c.trust_ref, kTrustRef);
    std::strcpy(c.topic_state, "tenants/t1/devices/d1/state");
    std::strcpy(c.topic_event, "tenants/t1/devices/d1/event");
    std::strcpy(c.topic_telemetry, "tenants/t1/devices/d1/telemetry");
    std::strcpy(c.topic_cmd, "tenants/t1/devices/d1/cmd");
    std::strcpy(c.expires_at, "2026-10-11T12:15:00Z");
    return c;
}

} // namespace

extern "C" void app_main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // =========================================================================
    // Fixture provenance: byte-exact, size + SHA-256, against the documented
    // upstream value (easyled-platform@40e0af25, docs/fixtures/mqtt-provisioning/).
    // =========================================================================
    CHECK(fixtureSize() == 1257);
    uint8_t digest[32]{};
    CHECK(sha256(fixtureBytes(), fixtureSize(), digest) == ESP_OK);
    static constexpr uint8_t kExpectedDigest[32] = {
        0xe9, 0x29, 0x47, 0xae, 0xdc, 0x66, 0x69, 0xd4, 0x4e, 0x00, 0x68, 0x10, 0x35, 0x08, 0xb9, 0xc6,
        0xb7, 0xa8, 0xdb, 0x2a, 0x42, 0x56, 0xb9, 0x87, 0xd2, 0x6e, 0x02, 0xb4, 0xa2, 0x9b, 0x9d, 0xb2,
    };
    CHECK(std::memcmp(digest, kExpectedDigest, 32) == 0);
    CHECK(fixtureBytes()[fixtureSize() - 1] == '\n'); // original fixture ends with a trailing newline

    // =========================================================================
    // Parser: happy path against the real fixture.
    // =========================================================================
    {
        DeviceCredentialResponse response{};
        ParseError error = ParseError::None;
        CHECK(parseDeviceCredentialResponse(fixtureBytes(), fixtureSize(), kDeviceId, kHardwareBindingId,
                                            kGeneration, response, error) == ESP_OK &&
              error == ParseError::None);
        CHECK(std::strcmp(response.device_id, kDeviceId) == 0 &&
              std::strcmp(response.hardware_binding_id, kHardwareBindingId) == 0 &&
              response.generation == kGeneration &&
              std::strcmp(response.credential_id, kCredentialId) == 0 &&
              response.kind == CredentialKind::Rotation &&
              std::strcmp(response.expires_at, "2026-10-11T12:15:00.123456Z") == 0 &&
              std::strcmp(response.username, kDeviceId) == 0 && std::strcmp(response.client_id, kDeviceId) == 0 &&
              std::strcmp(response.password, "SYNTHETIC-NOT-A-CREDENTIAL-0123456789abcdef") == 0 &&
              std::strcmp(response.broker_uri, "wss://mqtt.example.invalid:8085/mqtt") == 0 &&
              std::strcmp(response.trust_ref, kTrustRef) == 0);
        CHECK(std::strstr(response.topic_state, "/state") != nullptr &&
              std::strstr(response.topic_event, "/event") != nullptr &&
              std::strstr(response.topic_telemetry, "/telemetry") != nullptr &&
              std::strstr(response.topic_cmd, "/cmd") != nullptr);

        // Identity mismatch: this device's own identity does not match the response.
        CHECK(parseDeviceCredentialResponse(fixtureBytes(), fixtureSize(), kOtherDeviceId, kHardwareBindingId,
                                            kGeneration, response, error) == ESP_ERR_INVALID_ARG &&
              error == ParseError::IdentityMismatch);
    }

    // =========================================================================
    // generation: an exact-match AUTHORITY TOKEN, never an ordering
    // comparison (OpenAPI ClaimResponse.generation) -- checked before
    // trust_ref is ever resolved (parser-internal; resolution only happens
    // after this parser returns ESP_OK, in doBootstrap()) and before
    // anything is persisted or connected with. Correct accepted; the
    // response's own zero already rejected as Malformed upstream of this
    // check; a caller-side expected generation of zero, and a response
    // generation one below or one above the caller's own, all rejected the
    // same way as IdentityMismatch.
    // =========================================================================
    {
        DeviceCredentialResponse response{};
        ParseError error = ParseError::None;
        ResponseFields fields{};
        fields.generation = kGeneration;
        CHECK(parseTemplate(fields, response, error) == ESP_OK); // correct generation

        fields.generation = 0;
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG &&
              error == ParseError::Malformed); // the response's own generation must be non-zero regardless

        fields.generation = kGeneration - 1; // "previous" (older)
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::IdentityMismatch);

        fields.generation = kGeneration + 1; // "next" (newer) -- never an ordering comparison either
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::IdentityMismatch);

        fields.generation = kGeneration; // the response is correct...
        CHECK(parseTemplate(fields, response, error, kDeviceId, kHardwareBindingId, /*expected_generation=*/0) ==
              ESP_ERR_INVALID_ARG &&
              error == ParseError::IdentityMismatch); // ...but a caller-side 0 never matches anything
    }

    // =========================================================================
    // hardware_id (the bootstrap path parameter) vs hardware_binding_id (the
    // UUID the response echoes back): never the same shape, never derived
    // from each other. OpenAPI pattern: "^[0-9a-f]{1,64}$".
    // =========================================================================
    {
        CHECK(isValidHardwareId(kHardwareId)); // lower-hex, well under 64 chars
        CHECK(!isValidHardwareId(kHardwareBindingId)); // a UUID -- dashes are not lower-hex
        CHECK(!isValidHardwareId(nullptr));
        CHECK(!isValidHardwareId(""));
        CHECK(isValidHardwareId("0")); // single lower-hex digit: the minimum valid length
        static char exactly_64[65];
        std::memset(exactly_64, 'a', 64); exactly_64[64] = '\0';
        CHECK(isValidHardwareId(exactly_64)); // boundary: 64 is the maximum, still accepted
        static char too_long[66];
        std::memset(too_long, 'a', 65); too_long[65] = '\0';
        CHECK(!isValidHardwareId(too_long)); // 65 exceeds the OpenAPI pattern's bound
        CHECK(!isValidHardwareId("A1B2C3")); // uppercase hex is not lower-hex
        CHECK(!isValidHardwareId("a1b2g3")); // 'g' is out of [0-9a-f]

        // The coordinator must pass THIS value through to the bootstrap
        // request untouched, never hardware_binding_id -- proven against
        // the fake client, which records exactly what it received, with no
        // shape validation of its own to mask a conflation bug.
        CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        static FakeBootstrapClient client;
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver trust(kEntries, 1);
        static FakeClock clock;
        MqttCredentialCoordinator::Config config{};
        config.base_url = "https://api.example.invalid";
        config.hardware_id = kHardwareId;
        config.bootstrap_token = "bootstrap-test-token";
        config.expected_device_id = kDeviceId;
        config.expected_hardware_binding_id = kHardwareBindingId;
        config.expected_generation = kGeneration;
        static MqttCredentialCoordinator coordinator(store, client, trust, clock, config);
        char body[2048];
        ResponseFields fields{};
        const int written = buildResponse(body, sizeof(body), fields);
        client.status_code = 200;
        client.body = body;
        client.body_size = static_cast<size_t>(written);
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending);
        CHECK(std::strcmp(client.last_request.hardware_id, kHardwareId) == 0);
        CHECK(std::strcmp(client.last_request.hardware_id, kHardwareBindingId) != 0); // never conflated
        store.end();
    }

    // =========================================================================
    // expires_at: 0, 1 and 6 fractional digits accepted; malformed rejected.
    // =========================================================================
    {
        DeviceCredentialResponse response{};
        ParseError error = ParseError::None;
        ResponseFields fields{};
        fields.expires_at = "2026-10-11T12:15:00Z"; // 0 fractional digits
        CHECK(parseTemplate(fields, response, error) == ESP_OK);
        fields.expires_at = "2026-10-11T12:15:00.5Z"; // 1 fractional digit
        CHECK(parseTemplate(fields, response, error) == ESP_OK);
        fields.expires_at = "2026-10-11T12:15:00.123456Z"; // 6 fractional digits (the real example)
        CHECK(parseTemplate(fields, response, error) == ESP_OK);

        static const char *const kInvalidTimestamps[] = {
            "2026-10-11T12:15:00",          // missing Z
            "2026-10-11T12:15:00+00:00",    // numeric offset, never a Z
            "2026-10-11T12:15:00.1234567Z", // 7 fractional digits
            "2026-10-11T12:15:00.Z",        // '.' with no digits
            "not-a-timestamp",
        };
        for (const char *bad : kInvalidTimestamps) {
            fields.expires_at = bad;
            CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);
        }
    }

    // =========================================================================
    // Missing / duplicate / wrong-type consumed fields.
    // =========================================================================
    {
        DeviceCredentialResponse response{};
        ParseError error = ParseError::None;
        static constexpr char kMissingGeneration[] =
            "{\"device_id\":\"5b0f2d4e-8c1a-4f3b-9d6e-2a7c1b9e4f10\","
            "\"hardware_binding_id\":\"c1d2e3f4-0a1b-4c2d-8e3f-4a5b6c7d8e9f\","
            "\"credential\":{\"id\":\"0e9d8c7b-6a5f-4e3d-9c2b-1a0f9e8d7c6b\",\"status\":\"pending\","
            "\"kind\":\"rotation\",\"expires_at\":\"2026-10-11T12:15:00Z\","
            "\"activation\":\"first_successful_mqtt_authentication\"},"
            "\"mqtt\":{\"username\":\"5b0f2d4e-8c1a-4f3b-9d6e-2a7c1b9e4f10\","
            "\"client_id\":\"5b0f2d4e-8c1a-4f3b-9d6e-2a7c1b9e4f10\",\"password\":\"x\","
            "\"broker_uri\":\"wss://mqtt.example.invalid/mqtt\","
            "\"tls\":{\"trust_ref\":\"easyled-broker-ca-v1\",\"verify_server\":true},"
            "\"topics\":{\"publish\":{\"state\":\"a\",\"event\":\"b\",\"telemetry\":\"c\"},"
            "\"subscribe\":{\"cmd\":\"d\"}}}}";
        CHECK(parseDeviceCredentialResponse(kMissingGeneration, sizeof(kMissingGeneration) - 1, kDeviceId,
                                            kHardwareBindingId, kGeneration, response, error) == ESP_ERR_INVALID_ARG &&
              error == ParseError::Malformed);

        static constexpr char kDuplicateDeviceId[] =
            "{\"device_id\":\"5b0f2d4e-8c1a-4f3b-9d6e-2a7c1b9e4f10\","
            "\"device_id\":\"5b0f2d4e-8c1a-4f3b-9d6e-2a7c1b9e4f10\","
            "\"hardware_binding_id\":\"c1d2e3f4-0a1b-4c2d-8e3f-4a5b6c7d8e9f\",\"generation\":2,"
            "\"credential\":{\"id\":\"0e9d8c7b-6a5f-4e3d-9c2b-1a0f9e8d7c6b\",\"status\":\"pending\","
            "\"kind\":\"rotation\",\"expires_at\":\"2026-10-11T12:15:00Z\","
            "\"activation\":\"first_successful_mqtt_authentication\"},"
            "\"mqtt\":{\"username\":\"5b0f2d4e-8c1a-4f3b-9d6e-2a7c1b9e4f10\","
            "\"client_id\":\"5b0f2d4e-8c1a-4f3b-9d6e-2a7c1b9e4f10\",\"password\":\"x\","
            "\"broker_uri\":\"wss://mqtt.example.invalid/mqtt\","
            "\"tls\":{\"trust_ref\":\"easyled-broker-ca-v1\",\"verify_server\":true},"
            "\"topics\":{\"publish\":{\"state\":\"a\",\"event\":\"b\",\"telemetry\":\"c\"},"
            "\"subscribe\":{\"cmd\":\"d\"}}}}";
        CHECK(parseDeviceCredentialResponse(kDuplicateDeviceId, sizeof(kDuplicateDeviceId) - 1, kDeviceId,
                                            kHardwareBindingId, kGeneration, response, error) == ESP_ERR_INVALID_ARG &&
              error == ParseError::Malformed);

        char buffer[2048];
        // generation as a string, not a number.
        int written = std::snprintf(
            buffer, sizeof(buffer),
            "{\"device_id\":\"%s\",\"hardware_binding_id\":\"%s\",\"generation\":\"2\","
            "\"credential\":{\"id\":\"%s\",\"status\":\"pending\",\"kind\":\"rotation\","
            "\"expires_at\":\"2026-10-11T12:15:00Z\",\"activation\":\"first_successful_mqtt_authentication\"},"
            "\"mqtt\":{\"username\":\"%s\",\"client_id\":\"%s\",\"password\":\"x\","
            "\"broker_uri\":\"wss://mqtt.example.invalid/mqtt\","
            "\"tls\":{\"trust_ref\":\"easyled-broker-ca-v1\",\"verify_server\":true},"
            "\"topics\":{\"publish\":{\"state\":\"a\",\"event\":\"b\",\"telemetry\":\"c\"},"
            "\"subscribe\":{\"cmd\":\"d\"}}}}",
            kDeviceId, kHardwareBindingId, kCredentialId, kDeviceId, kDeviceId);
        CHECK(written > 0 && static_cast<size_t>(written) < sizeof(buffer));
        CHECK(parseDeviceCredentialResponse(buffer, static_cast<size_t>(written), kDeviceId, kHardwareBindingId,
                                            kGeneration, response, error) == ESP_ERR_INVALID_ARG &&
              error == ParseError::Malformed);
    }

    // =========================================================================
    // Password length: 128 accepted, 129 rejected. Non-WSS rejected.
    // verify_server:false rejected. Unknown trust_ref rejected. username/
    // client_id divergent from device_id rejected. Wildcard topics rejected.
    // =========================================================================
    {
        DeviceCredentialResponse response{};
        ParseError error = ParseError::None;
        static char password_128[129];
        static char password_129[130];
        std::memset(password_128, 'A', 128); password_128[128] = '\0';
        std::memset(password_129, 'A', 129); password_129[129] = '\0';

        ResponseFields fields{};
        fields.password = password_128;
        CHECK(parseTemplate(fields, response, error) == ESP_OK);
        fields.password = password_129;
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);

        fields = ResponseFields{};
        fields.broker_uri = "mqtts://mqtt.example.invalid:8085/mqtt"; // contract: implemented only for wss
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);
        fields.broker_uri = "ws://mqtt.example.invalid:8085/mqtt"; // plaintext, never accepted
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);
        fields.broker_uri = "wss://user:pass@mqtt.example.invalid:8085/mqtt"; // userinfo in the URI
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);

        fields = ResponseFields{};
        fields.verify_server = false;
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);

        fields = ResponseFields{};
        fields.username = kOtherDeviceId; // diverges from device_id/client_id
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);
        fields = ResponseFields{};
        fields.client_id = kOtherDeviceId;
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);

        fields = ResponseFields{};
        fields.topic_cmd = "tenants/t1/devices/d1/+";
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);
        fields = ResponseFields{};
        fields.topic_state = "tenants/t1/devices/#";
        CHECK(parseTemplate(fields, response, error) == ESP_ERR_INVALID_ARG && error == ParseError::Malformed);

        // Parser accepts the response (trust_ref is a bare string here); the
        // TRUST RESOLVER is what fails closed for an unknown value, proven
        // next, in the coordinator's own validation path (section below).
        fields = ResponseFields{};
        fields.trust_ref = "unknown-trust-ref";
        CHECK(parseTemplate(fields, response, error) == ESP_OK);
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver resolver(kEntries, 1);
        const char *ca = nullptr;
        CHECK(resolver.resolve("unknown-trust-ref", &ca) == ESP_ERR_NOT_FOUND);
        CHECK(resolver.resolve(kTrustRef, &ca) == ESP_OK && ca == kCaPem);
    }

    // =========================================================================
    // Durable store: persist-before-use, reboot at every transition,
    // corruption fails closed.
    // =========================================================================
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        CHECK(store.eraseAll() == ESP_OK); // clean slate from the previous section, no partition-level reinit
        CHECK(store.state() == CredentialState::Empty && !store.hasCurrent() && !store.hasPending());

        // Persist-before-use: pending exists durably before any attempt.
        const StoredCredential first = makeCredential(kCredentialId, "first-secret-0123456789");
        CHECK(store.storePending(first) == ESP_OK);
        CHECK(store.state() == CredentialState::PendingUnused && store.hasPending() &&
              std::strcmp(store.pending().password, "first-secret-0123456789") == 0 && !store.hasCurrent());

        // Reboot while PendingUnused: survives unchanged.
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(store.state() == CredentialState::PendingUnused &&
              std::strcmp(store.pending().credential_id, kCredentialId) == 0);

        // beginAttempt() durably marks AttemptInFlight BEFORE any transport use.
        CHECK(store.beginAttempt(/*usingPending=*/true) == ESP_OK);
        CHECK(store.state() == CredentialState::AttemptInFlight);

        // Reboot during AttemptInFlight: reconciled to AmbiguousActivation,
        // never silently retried the same way and never assumed failed.
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(store.state() == CredentialState::AmbiguousActivation && store.hasPending());

        // CONNACK accepted: promotes pending to current, drops the old
        // (there was none yet here) -- and the promotion itself survives reboot.
        CHECK(store.confirmConnected() == ESP_OK);
        CHECK(store.state() == CredentialState::CurrentConfirmed && store.hasCurrent() && !store.hasPending() &&
              std::strcmp(store.current().credential_id, kCredentialId) == 0);
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(store.state() == CredentialState::CurrentConfirmed && store.hasCurrent() && !store.hasPending());

        // Rotation: a second credential arrives as pending; current is untouched.
        const StoredCredential second = makeCredential("1a1a1a1a-1a1a-4a1a-8a1a-1a1a1a1a1a1a", "second-secret-xyz");
        CHECK(store.storePending(second) == ESP_OK);
        CHECK(store.hasCurrent() && store.hasPending() &&
              std::strcmp(store.current().credential_id, kCredentialId) == 0);

        // Pre-auth failure (TLS handshake itself failed): neither credential
        // touched; state rests back at PendingUnused (pending still exists).
        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(store.reportPreAuthFailure() == ESP_OK);
        CHECK(store.state() == CredentialState::PendingUnused && store.hasCurrent() && store.hasPending() &&
              std::strcmp(store.current().credential_id, kCredentialId) == 0 &&
              std::strcmp(store.pending().password, "second-secret-xyz") == 0);

        // Ambiguous outcome: never discards the in-flight credential. Reboot
        // mid-ambiguity keeps exactly that reading. Round-completion bool is
        // exercised precisely at the coordinator level further below; here
        // every call simply passes true to keep the arithmetic simple
        // (1, 2, 3).
        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(store.reportAmbiguous(/*completesRound=*/true) == ESP_OK);
        CHECK(store.state() == CredentialState::AmbiguousActivation && store.hasPending() &&
              std::strcmp(store.pending().password, "second-secret-xyz") == 0);
        CHECK(store.roundsWithoutSuccess() == 1);
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(store.state() == CredentialState::AmbiguousActivation && store.hasPending() && store.hasCurrent());

        // Two more ambiguous rounds (3 total): the coordinator (tested
        // below) is what actually moves to BootstrapRecovery at the round
        // limit; the store itself only counts whatever it is told.
        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(store.reportAmbiguous(true) == ESP_OK);
        CHECK(store.roundsWithoutSuccess() == 2);
        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(store.reportAmbiguous(true) == ESP_OK);
        CHECK(store.roundsWithoutSuccess() == 3);

        // Confirmed after ambiguity resets the round counter.
        CHECK(store.confirmConnected() == ESP_OK);
        CHECK(store.state() == CredentialState::CurrentConfirmed && store.roundsWithoutSuccess() == 0 &&
              std::strcmp(store.current().credential_id, "1a1a1a1a-1a1a-4a1a-8a1a-1a1a1a1a1a1a") == 0 &&
              !store.hasPending());

        CHECK(store.enterProvisioningFault() == ESP_OK);
        CHECK(store.state() == CredentialState::ProvisioningFault);
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(store.state() == CredentialState::ProvisioningFault); // reboot does not clear a provisioning fault

        store.end();
    }

    // =========================================================================
    // A/B atomicity: deterministic fault injection at each write boundary,
    // reboot after each, recovery always via the PREVIOUS slot. Never
    // PendingUnused without a valid pending; never CurrentConfirmed without
    // a valid current.
    // =========================================================================
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        CHECK(store.eraseAll() == ESP_OK);
        CHECK(store.state() == CredentialState::Empty);
        const StoredCredential first = makeCredential(kCredentialId, "ab-first-secret-000000000000");
        CHECK(store.storePending(first) == ESP_OK); // slot sequence 1, PendingUnused/first

        const StoredCredential second =
            makeCredential("4d4d4d4d-4d4d-4d4d-8d4d-4d4d4d4d4d4d", "ab-second-secret-111111111111");

        // Fault BEFORE the write is even attempted: RAM is untouched, and a
        // reboot still finds the previous slot (PendingUnused/first).
        store.setFaultInjectionForTest(StoreTestFault::BeforeWrite);
        CHECK(store.storePending(second) != ESP_OK);
        CHECK(store.state() == CredentialState::PendingUnused &&
              std::strcmp(store.pending().credential_id, kCredentialId) == 0);
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(store.state() == CredentialState::PendingUnused &&
              std::strcmp(store.pending().credential_id, kCredentialId) == 0);

        // Fault DURING the write itself: same observable contract.
        store.setFaultInjectionForTest(StoreTestFault::DuringWrite);
        CHECK(store.storePending(second) != ESP_OK);
        CHECK(std::strcmp(store.pending().credential_id, kCredentialId) == 0);
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(std::strcmp(store.pending().credential_id, kCredentialId) == 0);

        // Fault at COMMIT: same observable contract.
        store.setFaultInjectionForTest(StoreTestFault::DuringCommit);
        CHECK(store.storePending(second) != ESP_OK);
        CHECK(std::strcmp(store.pending().credential_id, kCredentialId) == 0);
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(std::strcmp(store.pending().credential_id, kCredentialId) == 0);

        // Fault at READBACK: the write and commit genuinely succeed (the
        // new slot really is durable and internally valid), but this
        // layer's own verification conservatively refuses to trust it
        // THIS instant -- RAM stays on "first" immediately after the call...
        store.setFaultInjectionForTest(StoreTestFault::DuringReadback);
        CHECK(store.storePending(second) != ESP_OK);
        CHECK(std::strcmp(store.pending().credential_id, kCredentialId) == 0);
        // ...but because the data really did make it to flash, a REAL
        // reboot (no fault injected now) correctly recovers the newer,
        // genuinely complete slot -- a transient verification hiccup here
        // causes no permanent data loss.
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(store.state() == CredentialState::PendingUnused &&
              std::strcmp(store.pending().credential_id, second.credential_id) == 0);

        // Never CurrentConfirmed without a valid current: a DuringCommit
        // fault on confirmConnected() must leave RAM at AttemptInFlight,
        // never silently show CurrentConfirmed with a stale/empty current.
        CHECK(store.beginAttempt(true) == ESP_OK);
        store.setFaultInjectionForTest(StoreTestFault::DuringCommit);
        CHECK(store.confirmConnected() != ESP_OK);
        CHECK(store.state() == CredentialState::AttemptInFlight);
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(store.state() == CredentialState::AmbiguousActivation && store.hasPending()); // reboot reconciliation
        CHECK(store.confirmConnected() == ESP_OK); // now succeeds for real, no fault injected
        CHECK(store.state() == CredentialState::CurrentConfirmed && store.hasCurrent() && !store.hasPending());
        store.end();
    }

    // Corruption fails closed, never fabricates a credential: both slots
    // unreadable as valid records.
    {
        nvs_handle_t handle = 0;
        CHECK(nvs_open_from_partition("easyled", "easyled_mqtt", NVS_READWRITE, &handle) == ESP_OK);
        static constexpr uint8_t kGarbage[] = {0xde, 0xad, 0xbe, 0xef};
        CHECK(nvs_set_blob(handle, "slotA", kGarbage, sizeof(kGarbage)) == ESP_OK);
        CHECK(nvs_set_blob(handle, "slotB", kGarbage, sizeof(kGarbage)) == ESP_OK);
        CHECK(nvs_commit(handle) == ESP_OK);
        nvs_close(handle);
        static MqttCredentialStore corrupt_store;
        CHECK(corrupt_store.init() == ESP_ERR_INVALID_RESPONSE);
        // This test's whole point is that recovery is impossible once both
        // slots are corrupt -- so, unlike every other section boundary in
        // this file, a cheap eraseAll() is not an option here (it requires
        // an initialized store, and none exists). Real partition-level
        // erase+reinit is what every following section needs to start from
        // a clean slate again.
        CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
    }

    // =========================================================================
    // Coordinator: new credential tried first, old only as courtesy, three
    // FULLY COMPLETED rounds then BootstrapRecovery, HTTP status policy,
    // timeout, Retry-After. Every reportConnected()/reportAmbiguous() return
    // is checked -- see "durable-failure propagation" below for what
    // happens when the store itself fails one of these durably.
    // =========================================================================
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        CHECK(store.eraseAll() == ESP_OK);
        static FakeBootstrapClient client;
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver trust(kEntries, 1);
        static FakeClock clock;
        MqttCredentialCoordinator::Config config{};
        config.base_url = "https://api.example.invalid";
        config.hardware_id = kHardwareId;
        config.bootstrap_token = "bootstrap-test-token";
        config.expected_device_id = kDeviceId;
        config.expected_hardware_binding_id = kHardwareBindingId;
        config.expected_generation = kGeneration;
        config.timeout_ms = 5000;
        config.max_rounds_before_recovery = 3;
        config.initial_backoff_ms = 5000;
        config.max_backoff_ms = 900000;
        static MqttCredentialCoordinator coordinator(store, client, trust, clock, config);

        // Empty: coordinator requests the bootstrap route on its own.
        char body[2048];
        ResponseFields fields{};
        int written = buildResponse(body, sizeof(body), fields);
        CHECK(written > 0);
        client.status_code = 200;
        client.body = body;
        client.body_size = static_cast<size_t>(written);
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending);
        CHECK(client.calls == 1 && store.state() == CredentialState::PendingUnused);
        CHECK(std::strcmp(coordinator.credentialToTry().credential_id, kCredentialId) == 0);

        // New credential tried first: PendingUnused always yields
        // ConnectWithPending, never a courtesy action, while pending exists.
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending);

        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(coordinator.reportConnected() == ESP_OK);
        CHECK(store.state() == CredentialState::CurrentConfirmed);

        // Rotation + ambiguous outcome: retried with the NEW credential
        // first (ConnectWithPending), never the old one, while pending
        // hasn't been tried yet this round.
        ResponseFields second_fields{};
        second_fields.credential_id = "2b2b2b2b-2b2b-4b2b-8b2b-2b2b2b2b2b2b";
        second_fields.password = "rotated-secret-aaaaaaaaaaaa";
        // Force a rotation the way the application would after a real
        // bootstrap call: store it directly (this test exercises the
        // coordinator's retry ordering, not doBootstrap() again).
        StoredCredential rotated = makeCredential(second_fields.credential_id, second_fields.password);
        CHECK(store.storePending(rotated) == ESP_OK);
        clock.now_us_ += 1000000LL * 3600; // well past any residual backoff
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending);
        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(coordinator.reportAmbiguous() == ESP_OK); // CONNACK lost on the new credential
        CHECK(store.state() == CredentialState::AmbiguousActivation);
        clock.now_us_ += 1000000LL * 3600; // well past backoff

        // Old tried only as courtesy: pending already tried this round, so
        // the SAME poll() now offers current instead of retrying pending.
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithCurrentCourtesy);
        CHECK(std::strcmp(coordinator.credentialToTry().credential_id, kCredentialId) == 0); // the OLD one
        CHECK(store.beginAttempt(/*usingPending=*/false) == ESP_OK);
        CHECK(coordinator.reportConnected() == ESP_OK); // the courtesy attempt succeeds
        CHECK(store.state() == CredentialState::CurrentConfirmed &&
              std::strcmp(store.current().credential_id, kCredentialId) == 0 && !store.hasPending());

        // Three FULLY COMPLETED rounds without success -> BootstrapRecovery
        // (never a tight loop -- each attempt waits out the backoff first).
        // A round is "pending first, current as courtesy second" while
        // `current` exists (it does here), so each round below takes TWO
        // individual ambiguous attempts; roundsWithoutSuccess() only
        // advances on the second (the courtesy one), never on the first.
        // See the dedicated pending-only test further below for the
        // simpler "no current" case, where a single attempt already
        // completes a round.
        StoredCredential third = makeCredential("3c3c3c3c-3c3c-4c3c-8c3c-3c3c3c3c3c3c", "third-secret-bbbb");
        CHECK(store.storePending(third) == ESP_OK);
        for (int round = 1; round <= 3; ++round) {
            clock.now_us_ += 1000000LL * 3600;
            CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending); // new credential first, every round
            CHECK(store.beginAttempt(true) == ESP_OK);
            CHECK(coordinator.reportAmbiguous() == ESP_OK);
            CHECK(store.roundsWithoutSuccess() == static_cast<uint32_t>(round - 1)); // not yet: courtesy still due
            CHECK(store.state() == CredentialState::AmbiguousActivation);

            clock.now_us_ += 1000000LL * 3600;
            CHECK(coordinator.poll() == CoordinatorAction::ConnectWithCurrentCourtesy); // old, only as courtesy
            CHECK(std::strcmp(coordinator.credentialToTry().credential_id, kCredentialId) == 0);
            CHECK(store.beginAttempt(false) == ESP_OK);
            CHECK(coordinator.reportAmbiguous() == ESP_OK);
            if (round < 3) {
                CHECK(store.roundsWithoutSuccess() == static_cast<uint32_t>(round)); // round now completed
                CHECK(store.state() == CredentialState::AmbiguousActivation);
            }
        }
        // The third round's courtesy attempt is what pushes
        // roundsWithoutSuccess to the configured limit and escalates in
        // that same call.
        CHECK(store.state() == CredentialState::BootstrapRecovery);

        clock.now_us_ += 1000000LL * 3600;
        client.reset();
        client.status_code = 200;
        written = buildResponse(body, sizeof(body), fields); // a fresh credential from a fresh bootstrap call
        client.body = body;
        client.body_size = static_cast<size_t>(written);
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending); // recovers via a fresh bootstrap call
        CHECK(client.calls == 1);
        store.end();
    }

    // =========================================================================
    // Round definition, simplest case: with only `pending` (no `current` to
    // fall back on), a single ambiguous attempt against it already
    // completes the round -- three such attempts (three rounds) reach
    // BootstrapRecovery, never six.
    // =========================================================================
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        CHECK(store.eraseAll() == ESP_OK);
        CHECK(store.state() == CredentialState::Empty);
        static FakeBootstrapClient client;
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver trust(kEntries, 1);
        static FakeClock clock;
        MqttCredentialCoordinator::Config config{};
        config.base_url = "https://api.example.invalid";
        config.hardware_id = kHardwareId;
        config.bootstrap_token = "bootstrap-test-token";
        config.expected_device_id = kDeviceId;
        config.expected_hardware_binding_id = kHardwareBindingId;
        config.expected_generation = kGeneration;
        static MqttCredentialCoordinator coordinator(store, client, trust, clock, config);

        char body[2048];
        ResponseFields fields{};
        const int written = buildResponse(body, sizeof(body), fields);
        client.status_code = 200;
        client.body = body;
        client.body_size = static_cast<size_t>(written);
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending); // Empty -> bootstrap -> PendingUnused
        CHECK(!store.hasCurrent());

        for (int round = 1; round <= 3; ++round) {
            clock.now_us_ += 1000000LL * 3600;
            CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending);
            CHECK(store.beginAttempt(true) == ESP_OK);
            CHECK(coordinator.reportAmbiguous() == ESP_OK);
            CHECK(store.roundsWithoutSuccess() == static_cast<uint32_t>(round)); // ONE attempt already completed it
        }
        CHECK(store.state() == CredentialState::BootstrapRecovery);
        store.end();
    }

    // =========================================================================
    // Durable-failure propagation: the coordinator must not advance round
    // bookkeeping, backoff state, or assume any transition happened when
    // the store's own durable write fails. Every caller checks the return.
    // =========================================================================
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        CHECK(store.eraseAll() == ESP_OK);
        static FakeBootstrapClient client;
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver trust(kEntries, 1);
        static FakeClock clock;
        MqttCredentialCoordinator::Config config{};
        config.base_url = "https://api.example.invalid";
        config.hardware_id = kHardwareId;
        config.bootstrap_token = "bootstrap-test-token";
        config.expected_device_id = kDeviceId;
        config.expected_hardware_binding_id = kHardwareBindingId;
        config.expected_generation = kGeneration;
        static MqttCredentialCoordinator coordinator(store, client, trust, clock, config);

        const StoredCredential cred = makeCredential(kCredentialId, "durable-fail-secret-000000");
        CHECK(store.storePending(cred) == ESP_OK);
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending);
        CHECK(store.beginAttempt(true) == ESP_OK);

        store.setFaultInjectionForTest(StoreTestFault::DuringCommit);
        CHECK(coordinator.reportAmbiguous() != ESP_OK); // durable write failed
        CHECK(store.roundsWithoutSuccess() == 0); // bookkeeping did NOT advance
        CHECK(store.state() == CredentialState::AttemptInFlight); // no transition was assumed either

        CHECK(coordinator.reportAmbiguous() == ESP_OK); // fault auto-cleared; retry succeeds for real
        CHECK(store.roundsWithoutSuccess() == 1 && store.state() == CredentialState::AmbiguousActivation);

        // Same guarantee for reportConnected(). Both reportAmbiguous() calls
        // above scheduled backoff (even the failed one, to avoid a tight
        // loop -- see the coordinator's own comment on that call); move the
        // clock well past it before polling again.
        clock.now_us_ += 1000000LL * 3600;
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending);
        CHECK(store.beginAttempt(true) == ESP_OK);
        store.setFaultInjectionForTest(StoreTestFault::DuringCommit);
        CHECK(coordinator.reportConnected() != ESP_OK);
        CHECK(store.state() == CredentialState::AttemptInFlight && !store.hasCurrent());
        CHECK(coordinator.reportConnected() == ESP_OK);
        CHECK(store.state() == CredentialState::CurrentConfirmed && store.hasCurrent());
        store.end();
    }

    // --- HTTP status policy: 401/403/429/500/503/404-405, timeout, Retry-After ---
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        CHECK(store.eraseAll() == ESP_OK);
        static FakeBootstrapClient client;
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver trust(kEntries, 1);
        static FakeClock clock;
        MqttCredentialCoordinator::Config config{};
        config.base_url = "https://api.example.invalid";
        config.hardware_id = kHardwareId;
        config.bootstrap_token = "bootstrap-test-token";
        config.expected_device_id = kDeviceId;
        config.expected_hardware_binding_id = kHardwareBindingId;
        config.expected_generation = kGeneration;
        config.timeout_ms = 5000;
        static MqttCredentialCoordinator coordinator(store, client, trust, clock, config);

        client.status_code = 401;
        CHECK(coordinator.poll() == CoordinatorAction::Idle);
        CHECK(store.state() == CredentialState::ProvisioningFault);

        // 401/403 do not clear on their own; only an external reset does
        // (modeled here as the application observing a new claim/rebind and
        // explicitly wiping stale state). The backoff the 401 case just
        // scheduled is coordinator-internal and transient (never persisted,
        // by design -- see MqttCredentialCoordinator's
        // `pending_tried_this_round_`/backoff fields), so it survives this
        // store-level reset; clear it the same way every other round
        // boundary in this file does, by moving the clock well past it.
        // `eraseAll()` resets just our own records in place -- unlike
        // `end()`+`nvs_flash_erase_partition()`+`init()`, it never tears
        // down and rebuilds the partition's own NVS driver state, which is
        // reserved for the tests that mean to simulate an actual reboot.
        clock.now_us_ += 1000000LL * 3600;
        CHECK(store.eraseAll() == ESP_OK);
        client.status_code = 403;
        CHECK(coordinator.poll() == CoordinatorAction::Idle);
        CHECK(store.state() == CredentialState::ProvisioningFault);

        clock.now_us_ += 1000000LL * 3600;
        CHECK(store.eraseAll() == ESP_OK);
        client.status_code = 429;
        client.has_retry_after = true;
        client.retry_after_s = 30;
        const int64_t before = clock.now_us_;
        CHECK(coordinator.poll() == CoordinatorAction::Idle); // this call is what actually schedules Retry-After
        CHECK(store.state() == CredentialState::Empty); // 429 is not a fault; credentials/state untouched
        // Retry-After jitter is +/-20% (24..36 s for a 30 s value); 20 s is
        // safely below the jitter floor, so this is never yet elapsed.
        clock.now_us_ = before + 20000000LL;
        CHECK(coordinator.poll() == CoordinatorAction::Idle); // Retry-After not yet elapsed
        clock.now_us_ = before + 40000000LL; // safely above the 36 s jitter ceiling
        client.has_retry_after = false;
        client.status_code = 200;
        char body[2048];
        ResponseFields fields{};
        int written = buildResponse(body, sizeof(body), fields);
        client.body = body;
        client.body_size = static_cast<size_t>(written);
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending);

        CHECK(store.eraseAll() == ESP_OK);
        client.reset();
        client.status_code = 500;
        CHECK(coordinator.poll() == CoordinatorAction::Idle);
        CHECK(store.state() == CredentialState::Empty);

        CHECK(store.eraseAll() == ESP_OK);
        client.reset();
        client.status_code = 503;
        CHECK(coordinator.poll() == CoordinatorAction::Idle);
        CHECK(store.state() == CredentialState::Empty);

        CHECK(store.eraseAll() == ESP_OK);
        client.reset();
        client.status_code = 404;
        CHECK(coordinator.poll() == CoordinatorAction::Idle);
        CHECK(store.state() == CredentialState::Empty); // a firmware/config bug, not a credential fault

        CHECK(store.eraseAll() == ESP_OK);
        client.reset();
        client.fail_transport = true; // timeout/reset/no response
        CHECK(coordinator.poll() == CoordinatorAction::Idle);
        CHECK(store.state() == CredentialState::Empty);
        store.end();
    }

    // =========================================================================
    // MqttSessionController: orchestration against a FAKE transport only
    // (never a real NetworkSession, broker, address, token, password or
    // CA). Proves: the identity handed to the transport is built from
    // whichever credential the coordinator actually chose; beginAttempt()
    // happens before restart(); a real Connected outcome promotes
    // durably; and -- the explicit #47-B review point -- ANY transport
    // failure between restart() and Connected (standing in for a
    // TCP_TRANSPORT error received from mqtt_message_receive() while
    // waiting for CONNACK, i.e. strictly after CONNECT was already sent)
    // reports Ambiguous, never anything else: MqttTransportSession::Outcome
    // has no PreAuthFailure value at all, so this is a compile-time
    // guarantee as much as a runtime one.
    // =========================================================================
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        CHECK(store.eraseAll() == ESP_OK);
        static FakeBootstrapClient client;
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver trust(kEntries, 1);
        static FakeClock clock;
        MqttCredentialCoordinator::Config config{};
        config.base_url = "https://api.example.invalid";
        config.hardware_id = kHardwareId;
        config.bootstrap_token = "bootstrap-test-token";
        config.expected_device_id = kDeviceId;
        config.expected_hardware_binding_id = kHardwareBindingId;
        config.expected_generation = kGeneration;
        static MqttCredentialCoordinator coordinator(store, client, trust, clock, config);
        static FakeTransportSession transport;
        static MqttSessionController controller(coordinator, store, trust, transport);

        char body[2048];
        ResponseFields fields{};
        int written = buildResponse(body, sizeof(body), fields);
        client.status_code = 200;
        client.body = body;
        client.body_size = static_cast<size_t>(written);

        CHECK(controller.tick() == ESP_OK); // Empty -> bootstrap -> PendingUnused -> beginAttempt() -> restart()
        CHECK(store.state() == CredentialState::AttemptInFlight && transport.restart_calls == 1 &&
              !controller.ready());
        CHECK(std::strcmp(transport.last_identity.client_id, kDeviceId) == 0 &&
              std::strcmp(transport.last_identity.username, kDeviceId) == 0 &&
              std::strcmp(transport.last_identity.password, "SYNTHETIC-NOT-A-CREDENTIAL-0123456789abcdef") == 0 &&
              std::strcmp(transport.last_identity.broker_uri, "wss://mqtt.example.invalid:8085/mqtt") == 0 &&
              std::strcmp(transport.last_identity.command_topic, "tenants/t1/devices/d1/cmd") == 0 &&
              transport.last_identity.server_ca_pem == kCaPem);

        CHECK(controller.tick() == ESP_OK); // still trying: no change, no extra restart
        CHECK(store.state() == CredentialState::AttemptInFlight && transport.restart_calls == 1 &&
              !controller.ready());

        transport.outcome_to_report = MqttTransportSession::Outcome::Connected;
        CHECK(controller.tick() == ESP_OK); // a real CONNACK was accepted
        CHECK(store.state() == CredentialState::CurrentConfirmed && controller.ready() &&
              std::strcmp(store.current().credential_id, kCredentialId) == 0);

        // Rotation: a new credential arrives; the controller must restart
        // the transport with IT, never the old one.
        ResponseFields rotated_fields{};
        rotated_fields.credential_id = "5e5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e";
        rotated_fields.password = "rotated-secret-cccccccccccc";
        written = buildResponse(body, sizeof(body), rotated_fields);
        client.reset();
        client.status_code = 200;
        client.body = body;
        client.body_size = static_cast<size_t>(written);
        CHECK(store.storePending(makeCredential(rotated_fields.credential_id, rotated_fields.password)) == ESP_OK);
        clock.now_us_ += 1000000LL * 3600;
        CHECK(controller.tick() == ESP_OK);
        CHECK(store.state() == CredentialState::AttemptInFlight &&
              std::strcmp(transport.last_identity.password, rotated_fields.password) == 0 && !controller.ready());

        // The explicit review point.
        transport.outcome_to_report = MqttTransportSession::Outcome::Ambiguous;
        CHECK(controller.tick() == ESP_OK);
        CHECK(store.state() == CredentialState::AmbiguousActivation && !controller.ready());
        CHECK(store.hasPending() &&
              std::strcmp(store.pending().credential_id, rotated_fields.credential_id) == 0); // not discarded
        CHECK(store.hasCurrent() && std::strcmp(store.current().credential_id, kCredentialId) == 0); // old untouched

        // New tried first, old only as courtesy: pending already tried this
        // round, so the next tick() restarts with the OLD identity.
        clock.now_us_ += 1000000LL * 3600;
        CHECK(controller.tick() == ESP_OK);
        CHECK(store.state() == CredentialState::AttemptInFlight &&
              std::strcmp(transport.last_identity.client_id, kDeviceId) == 0 &&
              std::strcmp(transport.last_identity.password, "SYNTHETIC-NOT-A-CREDENTIAL-0123456789abcdef") == 0);

        // The courtesy attempt also ends ambiguously: the round completes.
        transport.outcome_to_report = MqttTransportSession::Outcome::Ambiguous;
        CHECK(controller.tick() == ESP_OK);
        CHECK(store.state() == CredentialState::AmbiguousActivation && store.roundsWithoutSuccess() == 1);

        // A restart() failure itself is reported the same conservative way
        // -- never a special pre-auth path, because none exists in this seam.
        transport.fail_restart = true;
        const int restarts_before = transport.restart_calls;
        clock.now_us_ += 1000000LL * 3600;
        CHECK(controller.tick() == ESP_OK); // beginAttempt() durable; restart() failed -> reported Ambiguous
        CHECK(transport.restart_calls == restarts_before + 1 && store.state() == CredentialState::AmbiguousActivation);
        store.end();
    }

    // =========================================================================
    // No secret ever reaches a log line or a status-style accessor: audited
    // by code inspection (grep over this component's own sources finds no
    // ESP_LOG*/printf of a `password`/`bootstrap_token` field; see
    // docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md), not a QEMU check -- there is no
    // log sink to assert against in this harness. CredentialState/the public
    // CoordinatorAction/ParseError enums never carry a secret byte.
    // =========================================================================

    std::printf("MQTT_CREDENTIALS_VALIDATION checks=%d failures=%d RESULT=%s\n", checks, failures,
               failures == 0 ? "ALL_PASS" : "FAIL");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
