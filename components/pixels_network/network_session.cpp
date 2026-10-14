#include "network_session.hpp"

#include <cstring>
#include <ctime>
#include "esp32libfun_wifi_sta.hpp"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_wifi.h"

namespace pixels_network {
namespace {
bool text(const char *s, size_t maximum)
{
    return s != nullptr && s[0] != '\0' && strnlen(s, maximum + 1) <= maximum;
}
bool topic(const char *s)
{
    return text(s, 192) && std::strpbrk(s, "+#") == nullptr;
}
} // namespace

esp_err_t NetworkSession::init(const Config &config)
{
    if (client_ != nullptr) { return ESP_ERR_INVALID_STATE; }
    if (!text(config.ssid, 32) || !text(config.broker_uri, 255) ||
        std::strncmp(config.broker_uri, "wss://", 6) != 0 ||
        config.broker_uri[6] == '\0' || std::strchr(config.broker_uri, '@') != nullptr ||
        !text(config.client_id, 64) || !text(config.username, 64) ||
        !text(config.password, 256) || !topic(config.command_topic) ||
        (config.wifi_password != nullptr && strnlen(config.wifi_password, 65) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    next_tracked_slot_ = 0;
    connection_epoch_ = 1;
    acknowledged_ = 0;
    for (auto &slot : tracked_) { slot.state = 0; }
    esp_mqtt_client_config_t cfg{};
    cfg.broker.address.uri = config.broker_uri;
    if (config.server_ca_pem != nullptr) {
        cfg.broker.verification.certificate = config.server_ca_pem;
    } else {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.credentials.client_id = config.client_id;
    cfg.credentials.username = config.username;
    cfg.credentials.authentication.password = config.password;
    cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    cfg.session.keepalive = 30;
    cfg.network.timeout_ms = 3000;
    cfg.network.reconnect_timeout_ms = 3000;
    cfg.task.stack_size = 6144;
    cfg.buffer.size = 512; // Large messages intentionally exercise fragment assembly.
    cfg.outbox.limit = 4096;
    client_ = esp_mqtt_client_init(&cfg);
    if (client_ == nullptr) { return ESP_ERR_NO_MEM; }
    const esp_err_t err = esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY,
                                                       &NetworkSession::onEvent, this);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        return err;
    }
    messages_ = xQueueCreateStatic(kQueueLength, sizeof(Message), queue_storage_, &queue_control_);
    if (messages_ == nullptr) {
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    config_ = config;
    return ESP_OK;
}

esp_err_t NetworkSession::start()
{
    if (client_ == nullptr || running_.load()) { return ESP_ERR_INVALID_STATE; }
    const esp_err_t err = esp32libfun::wifi.begin(config_.ssid, config_.wifi_password);
    if (err != ESP_OK) { return err; }
    running_ = true;
    wifi_retry_ms_ = 1000;
    next_wifi_retry_us_ = esp_timer_get_time() + 10000000; // Let the initial association finish.
    next_mqtt_retry_us_ = 0;
    return ESP_OK;
}

esp_err_t NetworkSession::poll()
{
    if (!running_.load()) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    if (!esp32libfun::wifi.isConnected()) {
        subscribed_ = false;
        if (now < next_wifi_retry_us_) { return ESP_OK; }
        next_wifi_retry_us_ = now + static_cast<int64_t>(wifi_retry_ms_) * 1000;
        if (wifi_retry_ms_ < 30000) {
            wifi_retry_ms_ = wifi_retry_ms_ > 15000 ? 30000 : wifi_retry_ms_ * 2;
        }
        // WifiSta owns stack/profile initialization. It deliberately has no
        // reconnect policy; use the SDK's nonblocking reconnect on that profile.
        return esp_wifi_connect();
    }
    wifi_retry_ms_ = 1000;
    next_wifi_retry_us_ = now + 1000000;
    // TLS must validate certificate dates. SNTP/bootstrap clock ownership stays
    // with the application; there is no insecure fallback on missing time.
    if (std::time(nullptr) < 1704067200) { return ESP_OK; } // 2024-01-01.
    if (now < next_mqtt_retry_us_) { return ESP_OK; }
    if (!mqtt_started_) {
        next_mqtt_retry_us_ = now + 3000000;
        const esp_err_t err = esp_mqtt_client_start(client_);
        mqtt_started_ = (err == ESP_OK);
        return err;
    }
    if (reconnect_requested_.exchange(false)) {
        next_mqtt_retry_us_ = now + 3000000;
        const esp_err_t err = esp_mqtt_client_reconnect(client_);
        if (err != ESP_OK) { reconnect_requested_ = true; }
        return err;
    }
    return ESP_OK; // MQTT transport reconnection is owned by ESP-MQTT.
}

esp_err_t NetworkSession::stop()
{
    running_ = false;
    subscribed_ = false;
    if (mqtt_started_) {
        const esp_err_t err = esp_mqtt_client_stop(client_);
        if (err != ESP_OK) { return err; }
        mqtt_started_ = false;
    }
    // stop() is also a session boundary even when the SDK does not deliver a
    // DISCONNECTED event (for example, a caller stopping an idle client).
    ++connection_epoch_;
    reconnect_requested_ = false;
    incoming_.reset(); // MQTT task has stopped before accessing its buffer.
    if (messages_ != nullptr) { xQueueReset(messages_); }
    return esp32libfun::wifi.disconnect();
}

esp_err_t NetworkSession::end()
{
    if (client_ == nullptr) { return ESP_OK; }
    const esp_err_t err = stop();
    if (err != ESP_OK) { return err; }
    const esp_err_t destroyed = esp_mqtt_client_destroy(client_);
    if (destroyed != ESP_OK) { return destroyed; }
    client_ = nullptr;
    vQueueDelete(messages_);
    messages_ = nullptr;
    config_ = Config{};
    subscribe_override_ = nullptr;
    for (auto &slot : tracked_) { slot.state = 0; }
    return ESP_OK;
}

uint32_t NetworkSession::trackedState(int msg_id, uint32_t epoch, bool acknowledged)
{
    return (static_cast<uint32_t>(msg_id) & kTrackedIdMask) |
        ((epoch & kTrackedEpochMask) << 16) |
        (acknowledged ? kTrackedAcknowledged : 0u);
}

int NetworkSession::publish(const char *destination, const char *payload, size_t size, bool retained)
{
    // subscribed_ alone gates this: it can only be true after a real
    // CONNECTED+granted-SUBACK sequence, which in production never happens
    // without running_ also being true. Not also requiring running_ here
    // lets a test drive the state machine through event() alone (see its
    // own comment) without a real start()/Wi-Fi.
    const uint32_t enqueue_epoch = connection_epoch_.load();
    if (!subscribed_.load() || !topic(destination) ||
        size > kMaxMessageBytes || (payload == nullptr && size != 0)) { return -1; }
    // ESP-MQTT interprets len=0 as strlen(data); null explicitly means empty.
    const int msg_id = esp_mqtt_client_enqueue(client_, destination, size == 0 ? nullptr : payload,
                                               static_cast<int>(size), 1, retained ? 1 : 0, true);
    if (msg_id > 0) {
        TrackedPublish &slot = tracked_[next_tracked_slot_];
        next_tracked_slot_ = (next_tracked_slot_ + 1) % kMaxTrackedPublishes;
        // Capture the epoch from before enqueue. If a disconnect races the
        // SDK call, current_epoch will already differ when the caller asks
        // for status, so this publication correctly becomes Unknown.
        slot.state.store(trackedState(msg_id, enqueue_epoch, false));
    }
    return msg_id;
}

void NetworkSession::setSubscribeOverrideForTest(int (*subscribe)(const char *topic, int qos))
{
    subscribe_override_ = subscribe;
}

NetworkSession::PublishAck NetworkSession::publishStatus(int msg_id) const
{
    if (msg_id < 0) { return PublishAck::Unknown; }
    const uint32_t current_epoch = connection_epoch_.load() & kTrackedEpochMask;
    for (const auto &slot : tracked_) {
        const uint32_t state = slot.state.load();
        if ((state & kTrackedIdMask) != (static_cast<uint32_t>(msg_id) & kTrackedIdMask) ||
            ((state >> 16) & kTrackedEpochMask) != current_epoch) {
            continue;
        }
        return (state & kTrackedAcknowledged) != 0
            ? PublishAck::Acknowledged : PublishAck::Pending;
    }
    return PublishAck::Unknown;
}

bool NetworkSession::receive(Message &message)
{
    return messages_ != nullptr && xQueueReceive(messages_, &message, 0) == pdTRUE;
}

NetworkSession::Status NetworkSession::status() const
{
    return {running_.load(), esp32libfun::wifi.isConnected(), subscribed_.load(),
            connections_.load(), rejected_.load(), overflow_.load(), errors_.load(),
            acknowledged_.load()};
}

void NetworkSession::onEvent(void *arg, esp_event_base_t, int32_t id, void *data)
{
    if (arg != nullptr && data != nullptr) {
        static_cast<NetworkSession *>(arg)->event(id, *static_cast<esp_mqtt_event_t *>(data));
    }
}

void NetworkSession::event(int32_t id, esp_mqtt_event_t &event)
{
    // client_ (not running_) is the guard: it is false only before init()
    // or after end(), so a test may drive this state machine via init()
    // alone, with no real start()/Wi-Fi. In production the callback is
    // registered in init() and only ever fires while client_ is non-null.
    if (client_ == nullptr) { return; }
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        ++connections_;
        subscribed_ = false;
        incoming_.reset();
        subscribe_id_ = subscribe_override_ != nullptr
            ? subscribe_override_(config_.command_topic, 1)
            : esp_mqtt_client_subscribe(client_, config_.command_topic, 1);
        if (subscribe_id_ < 0) { reconnect_requested_ = true; }
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (subscribe_id_ >= 0 && event.msg_id == subscribe_id_) {
            // A SUBACK event also carries broker rejection (0x80). Connection
            // alone is not proof that the device may receive its commands.
            const bool granted = event.data != nullptr && event.data_len == 1 &&
                static_cast<uint8_t>(event.data[0]) == 1 &&
                (event.error_handle == nullptr || event.error_handle->error_type == MQTT_ERROR_TYPE_NONE);
            subscribed_ = granted;
            if (!granted) { ++errors_; reconnect_requested_ = true; }
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        subscribed_ = false;
        subscribe_id_ = -1;
        incoming_.reset();
        // Any publish tracked before this point becomes Unknown to
        // publishStatus() regardless of whether the SDK's own outbox still
        // holds and eventually redelivers it -- the application decides
        // whether/how to retry rather than this session guessing.
        ++connection_epoch_;
        break;
    case MQTT_EVENT_ERROR:
        ++errors_;
        // No ESP-MQTT error_type proves a failure happened strictly before
        // authentication: mqtt_message_receive() (the CONNACK-wait path, run
        // only after CONNECT has already been sent) dispatches the exact
        // same MQTT_ERROR_TYPE_TCP_TRANSPORT on a read/write failure as the
        // pre-CONNECT transport-connect path does. A credential-lifecycle
        // caller must therefore treat every MQTT_EVENT_ERROR here as
        // ambiguous -- see docs/MQTT-CREDENTIAL-LIFECYCLE-47B.md, "Seção 5".
        break;
    case MQTT_EVENT_PUBLISHED:
    {
        // PUBACK belongs to the current connection only. A single-word CAS
        // makes a duplicate idempotent and cannot acknowledge a slot while
        // publish() is atomically replacing it.
        const uint32_t current_epoch = connection_epoch_.load() & kTrackedEpochMask;
        for (auto &slot : tracked_) {
            uint32_t state = slot.state.load();
            while ((state & kTrackedIdMask) ==
                       (static_cast<uint32_t>(event.msg_id) & kTrackedIdMask) &&
                   ((state >> 16) & kTrackedEpochMask) == current_epoch) {
                if ((state & kTrackedAcknowledged) != 0) { break; }
                if (slot.state.compare_exchange_weak(
                        state, state | kTrackedAcknowledged)) {
                    ++acknowledged_;
                    return;
                }
            }
        }
        break;
    }
    case MQTT_EVENT_DATA: {
        if (!subscribed_.load() || event.retain) {
            incoming_.reset();
            ++rejected_; // Commands must be fresh publications, never retained snapshots.
            break;
        }
        if (event.current_data_offset == 0 &&
            (event.topic == nullptr || event.topic_len < 0 ||
             static_cast<size_t>(event.topic_len) != std::strlen(config_.command_topic) ||
             std::memcmp(event.topic, config_.command_topic, event.topic_len) != 0)) {
            incoming_.reset();
            ++rejected_;
            break;
        }
        const auto result = incoming_.append(event.data, event.data_len,
                                             event.current_data_offset, event.total_data_len);
        if (result == MessageBuffer<kMaxMessageBytes>::Result::Rejected) { ++rejected_; }
        if (result == MessageBuffer<kMaxMessageBytes>::Result::Complete) {
            delivery_.size = incoming_.size();
            std::memcpy(delivery_.data, incoming_.data(), delivery_.size + 1);
            if (xQueueSend(messages_, &delivery_, 0) != pdTRUE) { ++overflow_; }
        }
        break;
    }
    default:
        break;
    }
}

} // namespace pixels_network
