// Deterministic validation for docs/MQTT-SESSION-INTEGRATION-TASK.md:
// connecting the already-tested pixels_mqtt_credentials controller to a real
// pixels_network::NetworkSession through pixels_mqtt_session_adapter. No
// real Wi-Fi, broker, backend or secret exists anywhere below.
//
// NetworkSession is driven the same way validation/'s own NetworkSession
// section already does: init()/initWifi()/configureMqtt() are called, but
// start() never is, and every esp_mqtt_event_t is hand-built and fed through
// the public event() test seam (see network_session.hpp). The adapter's
// clock is injected (pixels_mqtt::MonotonicClock), exactly like
// MqttCredentialCoordinator's own clock, so the connect-timeout and
// post-confirmation reconnect-deadline paths are provable without a real
// multi-second/minute wait.
#include "network_session.hpp"
#include "pixels_mqtt_credentials.hpp"
#include "pixels_mqtt_session_adapter.hpp"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {

using namespace pixels_network;
using namespace pixels_mqtt;
using namespace pixels_mqtt_session;

int checks = 0, failures = 0;
void check(bool value, int line)
{
    ++checks;
    if (!value) { ++failures; std::printf("FAILED check %d (line %d)\n", checks, line); }
}
#define CHECK(expr) check((expr), __LINE__)

// --- Fakes, local to this target (same non-sharing convention as
// pixels_mqtt_credentials.cpp / validation_mqtt_credentials' own fakes). ----

class FakeClock final : public MonotonicClock {
public:
    int64_t nowUs() const override { return now_us_; }
    int64_t now_us_ = 1'000'000;
};

class FakeBootstrapClient final : public MqttBootstrapClient {
public:
    esp_err_t requestCredential(const BootstrapRequest &request, BootstrapResult &result) override
    {
        ++calls;
        if (fail_transport) { return ESP_FAIL; }
        result = BootstrapResult{};
        result.status_code = 200;
        if (body != nullptr) {
            if (body_size > request.capacity) { return ESP_ERR_INVALID_SIZE; }
            std::memcpy(request.destination, body, body_size);
            result.size = body_size;
        }
        return ESP_OK;
    }
    int calls = 0;
    bool fail_transport = false;
    const char *body = nullptr;
    size_t body_size = 0;
};

constexpr char kDeviceId[] = "5b0f2d4e-8c1a-4f3b-9d6e-2a7c1b9e4f10";
constexpr char kHardwareBindingId[] = "c1d2e3f4-0a1b-4c2d-8e3f-4a5b6c7d8e9f";
constexpr char kHardwareId[] = "a1b2c3d4e5f6";
constexpr uint32_t kGeneration = 2;
constexpr char kTrustRef[] = "easyled-broker-ca-v1";
constexpr char kCaPem[] = "-----BEGIN CERTIFICATE-----\nSYNTHETIC-TEST-ONLY\n-----END CERTIFICATE-----\n";

// One hand-built #47-B bootstrap response body, exactly the schema
// pixels_mqtt_credentials.cpp's parser requires. `credential_id` and
// `password` vary per call so each rotation in these tests is visibly a
// different identity.
int buildBootstrapBody(char *out, size_t capacity, const char *credential_id, const char *password,
                       const char *topic_cmd)
{
    return std::snprintf(out, capacity,
        "{\n"
        "  \"device_id\": \"%s\",\n"
        "  \"hardware_binding_id\": \"%s\",\n"
        "  \"generation\": %u,\n"
        "  \"credential\": {\n"
        "    \"id\": \"%s\",\n"
        "    \"status\": \"pending\",\n"
        "    \"kind\": \"initial_provisioning\",\n"
        "    \"activation\": \"first_successful_mqtt_authentication\",\n"
        "    \"expires_at\": \"2026-10-11T12:15:00.123456Z\"\n"
        "  },\n"
        "  \"mqtt\": {\n"
        "    \"username\": \"%s\",\n"
        "    \"client_id\": \"%s\",\n"
        "    \"password\": \"%s\",\n"
        "    \"broker_uri\": \"wss://mqtt.example.invalid:8085/mqtt\",\n"
        "    \"tls\": {\"trust_ref\": \"%s\", \"verify_server\": true},\n"
        "    \"topics\": {\n"
        "      \"publish\": {\"state\": \"tenants/t1/devices/d1/state\","
        " \"event\": \"tenants/t1/devices/d1/event\","
        " \"telemetry\": \"tenants/t1/devices/d1/telemetry\"},\n"
        "      \"subscribe\": {\"cmd\": \"%s\"}\n"
        "    }\n"
        "  }\n"
        "}\n",
        kDeviceId, kHardwareBindingId, static_cast<unsigned>(kGeneration), credential_id, kDeviceId, kDeviceId,
        password, kTrustRef, topic_cmd);
}

StoredCredential makeCredential(const char *credential_id, const char *password, const char *topic_cmd)
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
    std::strcpy(c.topic_cmd, topic_cmd);
    std::strcpy(c.expires_at, "2026-10-11T12:15:00Z");
    return c;
}

// --- NetworkSession event() test seam helpers (same pattern as
// validation/main/main.cpp's own NetworkSession section). -------------------
int g_fake_subscribe_next_id = 100;
char g_last_subscribed_topic[256]{};
int fakeSubscribe(const char *topic, int)
{
    std::strncpy(g_last_subscribed_topic, topic, sizeof(g_last_subscribed_topic) - 1);
    return g_fake_subscribe_next_id++;
}

esp_mqtt_event_t subackEvent(int msg_id, uint8_t &grant_byte)
{
    esp_mqtt_event_t ev{};
    ev.msg_id = msg_id;
    ev.data = reinterpret_cast<char *>(&grant_byte);
    ev.data_len = 1;
    return ev;
}

} // namespace

extern "C" void app_main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // =========================================================================
    // Section 1 -- NetworkSession Wi-Fi/MQTT split (acceptance checks 2, 3, 5,
    // 10). Laboratory init()/command/PUBACK behavior itself (check 1) and
    // render continuity while offline (check 17) are unchanged code paths,
    // already proven by the frozen validation/ target -- not re-tested here.
    // =========================================================================
    {
        static NetworkSession session;
        NetworkSession::WifiConfig wifi_cfg{"validation-ssid", "validation-password"};
        NetworkSession::MqttConfig mqtt_cfg{};
        mqtt_cfg.broker_uri = "wss://broker.invalid/mqtt";
        mqtt_cfg.client_id = "validation-client";
        mqtt_cfg.username = "validation-user";
        mqtt_cfg.password = "validation-password";
        mqtt_cfg.command_topic = "devices/session-validation/commands-v1";

        // Section 1's own ordering check: no MQTT identity can be configured
        // before Wi-Fi is even initialized.
        CHECK(session.configureMqtt(mqtt_cfg) == ESP_ERR_INVALID_STATE);
        CHECK(session.restartMqtt(mqtt_cfg) == ESP_ERR_INVALID_STATE);

        // Wi-Fi-only init/start is valid without any MQTT credential/client.
        CHECK(session.initWifi(wifi_cfg) == ESP_OK);
        NetworkSession::Config legacy_after_wifi{};
        legacy_after_wifi.ssid = wifi_cfg.ssid;
        legacy_after_wifi.wifi_password = wifi_cfg.wifi_password;
        legacy_after_wifi.broker_uri = mqtt_cfg.broker_uri;
        legacy_after_wifi.client_id = mqtt_cfg.client_id;
        legacy_after_wifi.username = mqtt_cfg.username;
        legacy_after_wifi.password = mqtt_cfg.password;
        legacy_after_wifi.command_topic = mqtt_cfg.command_topic;
        CHECK(session.init(legacy_after_wifi) == ESP_ERR_INVALID_STATE);
        CHECK(session.publish("x/y", "z", 1) < 0); // no client at all yet; never crashes
        CHECK(session.poll() == ESP_ERR_INVALID_STATE); // start() never called in this seam -- see header

        // Configuring MQTT now (still no start()) must succeed, and a second
        // configureMqtt() while a client already exists must be refused --
        // restartMqtt() is the only way to replace an existing identity.
        CHECK(session.configureMqtt(mqtt_cfg) == ESP_OK);
        CHECK(session.configureMqtt(mqtt_cfg) == ESP_ERR_INVALID_STATE);

        session.setSubscribeOverrideForTest(fakeSubscribe);
        esp_mqtt_event_t connected{};
        session.event(MQTT_EVENT_CONNECTED, connected);
        const int subscribe_id = g_fake_subscribe_next_id - 1;
        uint8_t granted = 1;
        auto suback = subackEvent(subscribe_id, granted);
        session.event(MQTT_EVENT_SUBSCRIBED, suback);
        CHECK(session.status().subscribed);
        CHECK(std::strcmp(g_last_subscribed_topic, "devices/session-validation/commands-v1") == 0);

        const uint32_t epoch_before_restart = session.status().connection_epoch;
        const int tracked_msg_id = session.publish("devices/session-validation/commands-v1", "payload", 7);
        CHECK(tracked_msg_id >= 0);

        // A deliberate identity restart: new command topic, no disconnect
        // event ever delivered for the old client. Wi-Fi-level readiness
        // (wifi_ready_) must survive it -- restartMqtt() never requires
        // initWifi() again.
        NetworkSession::MqttConfig rotated_cfg = mqtt_cfg;
        rotated_cfg.command_topic = "devices/session-validation/commands-v2";
        rotated_cfg.password = "validation-password-v2";
        CHECK(session.restartMqtt(rotated_cfg) == ESP_OK);

        // The restart is a session boundary: the old tracked publish is
        // Unknown even though no DISCONNECTED callback was ever delivered,
        // and incomplete inbound state/subscription are cleared.
        CHECK(session.publishStatus(tracked_msg_id) == NetworkSession::PublishAck::Unknown);
        CHECK(!session.status().subscribed);
        CHECK(session.status().connection_epoch != epoch_before_restart);

        // The new identity's own CONNECTED+SUBACK cycle subscribes the NEW
        // topic, never the old one -- the restart fully took effect.
        session.event(MQTT_EVENT_CONNECTED, connected);
        const int subscribe_id_2 = g_fake_subscribe_next_id - 1;
        uint8_t granted_2 = 1;
        auto suback_2 = subackEvent(subscribe_id_2, granted_2);
        session.event(MQTT_EVENT_SUBSCRIBED, suback_2);
        CHECK(session.status().subscribed);
        CHECK(std::strcmp(g_last_subscribed_topic, "devices/session-validation/commands-v2") == 0);

        // endMqtt() alone must leave Wi-Fi-level readiness intact: a fresh
        // configureMqtt() afterward succeeds with no initWifi() repeated.
        CHECK(session.endMqtt() == ESP_OK);
        CHECK(session.configureMqtt(mqtt_cfg) == ESP_OK);

        CHECK(session.end() == ESP_OK);
        // After end(), the session is back to its pre-initWifi() state.
        CHECK(session.configureMqtt(mqtt_cfg) == ESP_ERR_INVALID_STATE);
    }

    // =========================================================================
    // Section 2 -- NetworkSessionMqttTransport adapter against a real
    // NetworkSession (acceptance checks 6, 7, 8, 9). The shared clock proves
    // the connect-timeout/ambiguous paths deterministically, without a real
    // wait.
    // =========================================================================
    {
        static NetworkSession session;
        static FakeClock clock;
        static NetworkSessionMqttTransport adapter(session, clock);
        CHECK(session.initWifi({"validation-ssid", "validation-password"}) == ESP_OK);
        session.setSubscribeOverrideForTest(fakeSubscribe);

        char username[kUuidBytes + 1], client_id[kUuidBytes + 1], password[64], broker[64], topic[64];
        std::strcpy(username, kDeviceId);
        std::strcpy(client_id, kDeviceId);
        std::strcpy(password, "SYNTHETIC-FIRST-IDENTITY");
        std::strcpy(broker, "wss://mqtt.example.invalid:8085/mqtt");
        std::strcpy(topic, "tenants/t1/devices/d1/cmd-v1");
        MqttSessionIdentity identity{};
        identity.broker_uri = broker;
        identity.client_id = client_id;
        identity.username = username;
        identity.password = password;
        identity.command_topic = topic;
        identity.server_ca_pem = kCaPem;

        char overlong_client_id[kUuidBytes + 2];
        std::memset(overlong_client_id, 'a', sizeof(overlong_client_id) - 1);
        overlong_client_id[sizeof(overlong_client_id) - 1] = '\0';
        MqttSessionIdentity invalid_identity = identity;
        invalid_identity.client_id = overlong_client_id;
        CHECK(adapter.restart(invalid_identity) == ESP_ERR_INVALID_ARG);

        CHECK(adapter.restart(identity) == ESP_OK);
        CHECK(adapter.poll() == MqttTransportSession::Outcome::StillTrying);

        // Check 6: the adapter owns these strings. Clobbering the source
        // buffers right after restart() must not change the live MQTT
        // configuration -- proven by the SUBACK-topic observed below still
        // matching the ORIGINAL (pre-clobber) command topic. Each buffer
        // keeps its own guaranteed NUL terminator within bounds, so a later
        // strnlen()-based validation of these now-clobbered-but-still-valid
        // strings (e.g. inside restart() calls below that reuse `identity`)
        // never reads past the array.
        std::memset(username, 'X', sizeof(username) - 2);
        username[sizeof(username) - 1] = '\0';
        std::memset(password, 'X', sizeof(password) - 2);
        password[sizeof(password) - 1] = '\0';
        std::memset(topic, 'X', sizeof(topic) - 2);
        topic[sizeof(topic) - 1] = '\0';

        // Check 7: CONNACK produces Connected even before any SUBACK.
        esp_mqtt_event_t connected{};
        session.event(MQTT_EVENT_CONNECTED, connected);
        CHECK(adapter.poll() == MqttTransportSession::Outcome::Connected);
        CHECK(!session.status().subscribed); // SUBACK not sent yet -- CONNACK alone promoted it
        const int subscribe_id = g_fake_subscribe_next_id - 1;
        uint8_t granted = 1;
        auto suback = subackEvent(subscribe_id, granted);
        session.event(MQTT_EVENT_SUBSCRIBED, suback);
        CHECK(session.status().subscribed);
        CHECK(std::strcmp(g_last_subscribed_topic, "tenants/t1/devices/d1/cmd-v1") == 0); // the ORIGINAL topic

        // Monitoring phase: no new connect, no new disconnect -- StillTrying.
        CHECK(adapter.poll() == MqttTransportSession::Outcome::StillTrying);

        // Check 8a: a disconnect before a NEW attempt's CONNACK is Ambiguous.
        // `identity` itself (still pointing at the now-clobbered-but-valid
        // `username`/`password`/`topic` buffers from check 6) is reused
        // unchanged -- restart() only cares that the pointers are valid for
        // the duration of this call, which they are; its own string
        // content is irrelevant to this outcome check.
        {
            CHECK(adapter.restart(identity) == ESP_OK);
            esp_mqtt_event_t disconnected{};
            session.event(MQTT_EVENT_DISCONNECTED, disconnected);
            CHECK(adapter.poll() == MqttTransportSession::Outcome::Ambiguous);
        }

        // Check 8b: a transport error before CONNACK is Ambiguous too.
        {
            CHECK(adapter.restart(identity) == ESP_OK);
            esp_mqtt_event_t error_event{};
            session.event(MQTT_EVENT_ERROR, error_event);
            CHECK(adapter.poll() == MqttTransportSession::Outcome::Ambiguous);
        }

        // Check 8c: a monotonic attempt timeout with no event at all.
        {
            CHECK(adapter.restart(identity) == ESP_OK);
            CHECK(adapter.poll() == MqttTransportSession::Outcome::StillTrying);
            clock.now_us_ += 21LL * 1000 * 1000; // past the adapter's 20 s initial-connect timeout
            CHECK(adapter.poll() == MqttTransportSession::Outcome::Ambiguous);
        }

        // Check 9: a stale connection count from an EARLIER attempt cannot
        // confirm a NEW one. Reconnect cleanly first (so `connections_` is
        // nonzero), then start a fresh attempt and confirm poll() does NOT
        // report Connected merely because connections_ is already nonzero --
        // only a FRESH increment past the new baseline counts.
        {
            CHECK(adapter.restart(identity) == ESP_OK);
            session.event(MQTT_EVENT_CONNECTED, connected);
            CHECK(adapter.poll() == MqttTransportSession::Outcome::Connected);
            const uint32_t connections_before_rotation = session.status().connections;
            CHECK(connections_before_rotation > 0);

            CHECK(adapter.restart(identity) == ESP_OK); // new attempt; no new event fed at all
            CHECK(session.status().connections == connections_before_rotation); // stale count, unchanged
            CHECK(adapter.poll() == MqttTransportSession::Outcome::StillTrying); // never falsely Connected
        }

        CHECK(session.end() == ESP_OK);
    }

    // =========================================================================
    // Section 3 -- ProvisionedTopics rotation (acceptance check 16): the
    // whole topic set copies atomically from the credential, never aliases
    // it. Laboratory mode never calls this function at all (main.cpp), so
    // its own topics are trivially unaffected -- a code-level fact, not
    // re-tested here.
    // =========================================================================
    {
        StoredCredential source = makeCredential("11111111-1111-4111-8111-111111111111",
                                                  "irrelevant-for-this-section", "tenants/t1/devices/d1/cmd");
        ProvisionedTopics topics{};
        rotateProvisionedTopics(source, topics);
        CHECK(std::strcmp(topics.state, "tenants/t1/devices/d1/state") == 0);
        CHECK(std::strcmp(topics.event, "tenants/t1/devices/d1/event") == 0);
        CHECK(std::strcmp(topics.telemetry, "tenants/t1/devices/d1/telemetry") == 0);
        CHECK(std::strcmp(topics.cmd, "tenants/t1/devices/d1/cmd") == 0);

        std::memset(&source, 0, sizeof(source)); // clobber the source after the copy
        CHECK(std::strcmp(topics.state, "tenants/t1/devices/d1/state") == 0); // unaffected: a copy, not an alias
        CHECK(std::strcmp(topics.cmd, "tenants/t1/devices/d1/cmd") == 0);
    }

    // =========================================================================
    // Section 4 -- full integration: MqttSessionController driving the REAL
    // NetworkSessionMqttTransport adapter over a real NetworkSession, backed
    // by a real MqttCredentialStore against the actual NVS partition.
    // Acceptance checks 4, 11, 14, 15. Checks 12/13 (durable-failure retry
    // without re-polling/restarting transport; incomplete provisioning
    // fail-closed) reuse the #47-B state machine unchanged and are already
    // proven against fakes in validation_mqtt_credentials/ -- not duplicated
    // here against the real adapter, since nothing about that logic depends
    // on which MqttTransportSession implementation is behind it.
    // =========================================================================
    CHECK(nvs_flash_erase_partition("easyled") == ESP_OK);
    {
        static MqttCredentialStore store;
        CHECK(store.init() == ESP_OK);
        static FakeBootstrapClient bootstrap_client;
        static const TableTrustResolver::Entry kEntries[] = {{kTrustRef, kCaPem}};
        static TableTrustResolver trust(kEntries, 1);
        static FakeClock clock;
        static NetworkSession session;
        CHECK(session.initWifi({"validation-ssid", "validation-password"}) == ESP_OK);
        session.setSubscribeOverrideForTest(fakeSubscribe);
        static NetworkSessionMqttTransport adapter(session, clock);

        MqttCredentialCoordinator::Config coordinator_config{};
        coordinator_config.base_url = "https://api.example.invalid";
        coordinator_config.hardware_id = kHardwareId;
        coordinator_config.bootstrap_token = "bootstrap-test-token";
        coordinator_config.expected_device_id = kDeviceId;
        coordinator_config.expected_hardware_binding_id = kHardwareBindingId;
        coordinator_config.expected_generation = kGeneration;
        static MqttCredentialCoordinator coordinator(store, bootstrap_client, trust, clock, coordinator_config);
        static MqttSessionController controller(coordinator, store, trust, adapter);

        // Check 4: a credential is durably pending before the adapter's
        // first restart() -- proven here by the end-to-end result: the
        // store only ever reaches AttemptInFlight (beginAttempt() requires
        // an already-valid pending/current) and the adapter's SUBACK-topic
        // capture below matches the bootstrap body, never a placeholder.
        char body[2048];
        const int written = buildBootstrapBody(body, sizeof(body), "0e9d8c7b-6a5f-4e3d-9c2b-1a0f9e8d7c6b",
                                               "SYNTHETIC-COLD-START", "tenants/t1/devices/d1/cmd-cold");
        bootstrap_client.body = body;
        bootstrap_client.body_size = static_cast<size_t>(written);

        CHECK(controller.tick() == ESP_OK); // Empty -> bootstrap -> storePending -> beginAttempt -> adapter.restart()
        CHECK(store.state() == CredentialState::AttemptInFlight && store.hasPending());
        CHECK(bootstrap_client.calls == 1);

        esp_mqtt_event_t connected{};
        session.event(MQTT_EVENT_CONNECTED, connected);
        CHECK(controller.tick() == ESP_OK); // adapter observes Connected; reported durably
        CHECK(store.state() == CredentialState::CurrentConfirmed && controller.ready());
        CHECK(std::strcmp(g_last_subscribed_topic, "tenants/t1/devices/d1/cmd-cold") == 0);

        // Check 15a: brief post-CONNACK loss reconnects with the SAME
        // credential, never re-bootstrapping and never re-reporting the
        // automatic reconnect CONNACK as a new promotion.
        const int bootstrap_calls_before_loss = bootstrap_client.calls;
        esp_mqtt_event_t disconnected{};
        session.event(MQTT_EVENT_DISCONNECTED, disconnected);
        clock.now_us_ += 5LL * 1000 * 1000; // well inside the 60 s reconnect deadline
        CHECK(controller.tick() == ESP_OK);
        CHECK(controller.ready()); // still considered current; no ambiguous report yet
        session.event(MQTT_EVENT_CONNECTED, connected); // ESP-MQTT's own auto-reconnect succeeding
        CHECK(controller.tick() == ESP_OK);
        CHECK(controller.ready() && store.state() == CredentialState::CurrentConfirmed);
        CHECK(bootstrap_client.calls == bootstrap_calls_before_loss); // never re-bootstrapped for a brief loss

        // Check 11: a rotation while already ready() must preempt monitoring
        // on this very tick(), choosing the NEW pending over the old
        // current (pending first, current only as courtesy -- the existing
        // #47-B rule, unchanged).
        CHECK(store.storePending(makeCredential("5e5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e", "SYNTHETIC-ROTATED",
                                                 "tenants/t1/devices/d1/cmd-rotated")) == ESP_OK);
        clock.now_us_ += 1LL * 1000 * 1000;
        CHECK(controller.tick() == ESP_OK);
        CHECK(store.state() == CredentialState::AttemptInFlight && store.attemptUsesPending() && !controller.ready());
        session.event(MQTT_EVENT_CONNECTED, connected);
        CHECK(controller.tick() == ESP_OK);
        CHECK(controller.ready() &&
              std::strcmp(store.current().credential_id, "5e5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e") == 0);
        CHECK(std::strcmp(g_last_subscribed_topic, "tenants/t1/devices/d1/cmd-rotated") == 0);

        // Check 15b: a bounded persistent post-confirmation failure (no
        // reconnect within the deadline) converts into the existing
        // ambiguous/current retry path, never a tight loop. Round 1 loses
        // the already-confirmed session via the monitoring deadline; the
        // confirmConnected() that would otherwise reset the round counter
        // never happens again after this point, so rounds 2 and 3 are each
        // a fresh attempt against `current` that never connects at all
        // (governed by the adapter's initial-connect timeout, not the
        // monitoring deadline) -- genuinely consecutive failures, exactly
        // what the existing round limit counts. A generous clock jump
        // between rounds clears the coordinator's own backoff gate
        // regardless of its exact jittered value.
        CHECK(controller.ready());
        session.event(MQTT_EVENT_DISCONNECTED, disconnected);
        CHECK(controller.tick() == ESP_OK); // registers the loss; deadline timer starts now
        CHECK(controller.ready()); // not yet past the deadline
        clock.now_us_ += 61LL * 1000 * 1000; // past the 60 s post-confirmation reconnect deadline
        CHECK(controller.tick() == ESP_OK); // adapter reports Ambiguous; durably recorded (round 1 of 3)
        CHECK(!controller.ready() && store.state() == CredentialState::AmbiguousActivation);

        for (int round = 0; round < 2; ++round) {
            clock.now_us_ += 1000LL * 1000 * 1000; // clear any jittered backoff
            CHECK(controller.tick() == ESP_OK); // ConnectWithCurrentCourtesy -> beginAttempt() + restart()
            CHECK(store.state() == CredentialState::AttemptInFlight && !store.attemptUsesPending());
            clock.now_us_ += 21LL * 1000 * 1000; // past the adapter's 20 s initial-connect timeout; no CONNECTED fed
            CHECK(controller.tick() == ESP_OK); // Ambiguous again -> reported (rounds 2 and 3 of 3)
            CHECK(!controller.ready());
        }
        CHECK(store.state() == CredentialState::BootstrapRecovery); // bounded round limit reached, no tight loop
        CHECK(store.hasCurrent()); // the last confirmed credential is preserved until a replacement confirms

        // Reset to a clean slate for a focused cold-boot-reconnect check
        // (check 14), independent of the BootstrapRecovery state reached
        // above.
        CHECK(store.eraseAll() == ESP_OK);
        CHECK(store.storePending(makeCredential("7a7a7a7a-7a7a-4a7a-8a7a-7a7a7a7a7a7a", "SYNTHETIC-COLD-RECONNECT",
                                                 "tenants/t1/devices/d1/cmd-coldboot")) == ESP_OK);
        CHECK(store.beginAttempt(true) == ESP_OK);
        CHECK(store.confirmConnected() == ESP_OK);
        CHECK(store.state() == CredentialState::CurrentConfirmed);

        // Check 14: reload from NVS (simulating reboot) with only
        // CurrentConfirmed, then confirm a controller reconnects with
        // `current` WITHOUT requesting a new credential first. Reusing the
        // same `session`/`adapter` is equivalent to a real reboot for this
        // purpose: restartMqtt() unconditionally tears down whatever
        // identity (if any) is already configured before applying the new
        // one, and the adapter's own restart() unconditionally recaptures
        // its baseline -- neither carries forward any state from before.
        store.end();
        CHECK(store.init() == ESP_OK); // "reboot": reload from NVS
        CHECK(store.state() == CredentialState::CurrentConfirmed && !store.hasPending());

        static MqttSessionController reboot_controller(coordinator, store, trust, adapter);
        const int bootstrap_calls_before_reboot = bootstrap_client.calls;
        CHECK(reboot_controller.tick() == ESP_OK); // cold-boot reconnect with `current`, no bootstrap
        CHECK(store.state() == CredentialState::AttemptInFlight && !store.attemptUsesPending());
        CHECK(bootstrap_client.calls == bootstrap_calls_before_reboot); // never requested a new credential
        session.event(MQTT_EVENT_CONNECTED, connected);
        CHECK(reboot_controller.tick() == ESP_OK);
        CHECK(reboot_controller.ready() && store.state() == CredentialState::CurrentConfirmed);
        CHECK(std::strcmp(g_last_subscribed_topic, "tenants/t1/devices/d1/cmd-coldboot") == 0);

        store.end();
        session.end();
    }

    // =========================================================================
    // No secret ever reaches a log line or a status-style accessor: audited
    // by code inspection (same basis as validation_mqtt_credentials/), not a
    // QEMU check. NetworkSession::Status/Config and MqttSessionIdentity
    // never carry anything beyond what this file's own synthetic constants
    // already are.
    // =========================================================================

    std::printf("MQTT_SESSION_VALIDATION checks=%d failures=%d RESULT=%s\n", checks, failures,
               failures == 0 ? "ALL_PASS" : "FAIL");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
