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
    uint32_t generation = 2;
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
                        const char *expected_binding_id = kHardwareBindingId)
{
    static char buffer[2048];
    const int written = buildResponse(buffer, sizeof(buffer), f);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(buffer)) { return ESP_ERR_INVALID_SIZE; }
    return parseDeviceCredentialResponse(buffer, static_cast<size_t>(written), expected_device_id,
                                         expected_binding_id, out, error);
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

StoredCredential makeCredential(const char *credential_id, const char *password)
{
    StoredCredential c{};
    c.valid = true;
    std::strcpy(c.credential_id, credential_id);
    std::strcpy(c.hardware_binding_id, kHardwareBindingId);
    c.generation = 2;
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
        CHECK(parseDeviceCredentialResponse(fixtureBytes(), fixtureSize(), kDeviceId, kHardwareBindingId, response,
                                            error) == ESP_OK &&
              error == ParseError::None);
        CHECK(std::strcmp(response.device_id, kDeviceId) == 0 &&
              std::strcmp(response.hardware_binding_id, kHardwareBindingId) == 0 && response.generation == 2 &&
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
                                            response, error) == ESP_ERR_INVALID_ARG &&
              error == ParseError::IdentityMismatch);
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
                                            kHardwareBindingId, response, error) == ESP_ERR_INVALID_ARG &&
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
                                            kHardwareBindingId, response, error) == ESP_ERR_INVALID_ARG &&
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
                                            response, error) == ESP_ERR_INVALID_ARG &&
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
    CHECK(nvs_flash_erase_partition("easyled") == ESP_OK); // QEMU image's own dedicated test partition only
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK && store.state() == CredentialState::Empty && !store.hasCurrent() &&
              !store.hasPending());

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
        // mid-ambiguity keeps exactly that reading.
        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(store.reportAmbiguous() == ESP_OK);
        CHECK(store.state() == CredentialState::AmbiguousActivation && store.hasPending() &&
              std::strcmp(store.pending().password, "second-secret-xyz") == 0);
        CHECK(store.roundsWithoutSuccess() == 1);
        store.end();
        CHECK(store.init() == ESP_OK);
        CHECK(store.state() == CredentialState::AmbiguousActivation && store.hasPending() && store.hasCurrent());

        // Two more ambiguous rounds (3 total): the coordinator (tested
        // below) is what actually moves to BootstrapRecovery at the round
        // limit; the store itself only counts.
        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(store.reportAmbiguous() == ESP_OK);
        CHECK(store.roundsWithoutSuccess() == 2);
        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(store.reportAmbiguous() == ESP_OK);
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

    // Corruption fails closed, never fabricates a credential.
    {
        nvs_handle_t handle = 0;
        CHECK(nvs_open_from_partition("easyled", "easyled_mqtt", NVS_READWRITE, &handle) == ESP_OK);
        static constexpr uint8_t kGarbage[] = {0xde, 0xad, 0xbe, 0xef};
        CHECK(nvs_set_blob(handle, "hdr", kGarbage, sizeof(kGarbage)) == ESP_OK);
        CHECK(nvs_commit(handle) == ESP_OK);
        nvs_close(handle);
        static MqttCredentialStore corrupt_store;
        CHECK(corrupt_store.init() == ESP_ERR_INVALID_RESPONSE);
    }

    // =========================================================================
    // Coordinator: new credential tried first, old only as courtesy, three
    // rounds then BootstrapRecovery, HTTP status policy, timeout, Retry-After.
    // =========================================================================
    CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        static FakeBootstrapClient client;
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver trust(kEntries, 1);
        static FakeClock clock;
        MqttCredentialCoordinator::Config config{};
        config.base_url = "https://api.example.invalid";
        config.hardware_id = kDeviceId;
        config.bootstrap_token = "bootstrap-test-token";
        config.expected_device_id = kDeviceId;
        config.expected_hardware_binding_id = kHardwareBindingId;
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
        coordinator.reportConnected();
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
        coordinator.reportAmbiguous(); // CONNACK lost on the new credential
        CHECK(store.state() == CredentialState::AmbiguousActivation);
        clock.now_us_ += 1000000LL * 3600; // well past backoff

        // Old tried only as courtesy: pending already tried this round, so
        // the SAME poll() now offers current instead of retrying pending.
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithCurrentCourtesy);
        CHECK(std::strcmp(coordinator.credentialToTry().credential_id, kCredentialId) == 0); // the OLD one
        CHECK(store.beginAttempt(/*usingPending=*/false) == ESP_OK);
        coordinator.reportConnected(); // the courtesy attempt succeeds
        CHECK(store.state() == CredentialState::CurrentConfirmed &&
              std::strcmp(store.current().credential_id, kCredentialId) == 0 && !store.hasPending());

        // Three bounded connection attempts without success -> BootstrapRecovery
        // (never a tight loop -- each one waits out the backoff first).
        // `roundsWithoutSuccess()` counts individual ambiguous attempts, be
        // they against `pending` or the `current`-as-courtesy retry.
        StoredCredential third = makeCredential("3c3c3c3c-3c3c-4c3c-8c3c-3c3c3c3c3c3c", "third-secret-bbbb");
        CHECK(store.storePending(third) == ESP_OK);
        clock.now_us_ += 1000000LL * 3600;
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending); // attempt 1
        CHECK(store.beginAttempt(true) == ESP_OK);
        coordinator.reportAmbiguous();
        CHECK(store.roundsWithoutSuccess() == 1 && store.state() == CredentialState::AmbiguousActivation);
        clock.now_us_ += 1000000LL * 3600;
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithCurrentCourtesy); // attempt 2, old as courtesy
        CHECK(store.beginAttempt(false) == ESP_OK);
        coordinator.reportAmbiguous();
        CHECK(store.roundsWithoutSuccess() == 2 && store.state() == CredentialState::AmbiguousActivation);
        clock.now_us_ += 1000000LL * 3600;
        CHECK(coordinator.poll() == CoordinatorAction::ConnectWithPending); // attempt 3, new credential again
        CHECK(store.beginAttempt(true) == ESP_OK);
        coordinator.reportAmbiguous(); // third attempt: coordinator must now escalate
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

    // --- HTTP status policy: 401/403/429/500/503/404-405, timeout, Retry-After ---
    CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        static FakeBootstrapClient client;
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver trust(kEntries, 1);
        static FakeClock clock;
        MqttCredentialCoordinator::Config config{};
        config.base_url = "https://api.example.invalid";
        config.hardware_id = kDeviceId;
        config.bootstrap_token = "bootstrap-test-token";
        config.expected_device_id = kDeviceId;
        config.expected_hardware_binding_id = kHardwareBindingId;
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
    // No secret ever reaches a log line or a status-style accessor: audited
    // by code inspection (grep over this component's own sources finds no
    // ESP_LOG*/printf of a `password`/`bootstrap_token` field; see
    // docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md), not a QEMU check -- there is no
    // log sink to assert against in this harness. CredentialState/the public
    // CoordinatorAction/ parse ParseError enums never carry a secret byte.
    // =========================================================================

    std::printf("MQTT_CREDENTIALS_VALIDATION checks=%d failures=%d RESULT=%s\n", checks, failures,
               failures == 0 ? "ALL_PASS" : "FAIL");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
