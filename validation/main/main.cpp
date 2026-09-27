#include "message_buffer.hpp"
#include "network_session.hpp"
#include "firmware_contract.hpp"
#include "esp_pixels_composer.hpp"
#include "esp_pixels_composer_adapter_esp_pixels.hpp"
#include "esp_pixels_fx.hpp"
#include "esp_pixels_fx_ids.hpp"

#include <climits>
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kDeviceId[] = "11111111-1111-4111-8111-111111111111";
constexpr char kCommandId[] = "22222222-2222-4222-8222-222222222222";
constexpr char kActorId[] = "33333333-3333-4333-8333-333333333333";
constexpr char kPresetId[] = "55555555-5555-4555-8555-555555555555";
constexpr char kConfigHash[] = "b62868aef25369d8b00db8dbc6af34c7c82b060b5570b0b7b24859a7d5547c5a";
constexpr char kPresetHash[] = "c9be5621fa0109c6acbf4461a47191b80b3b6ebae2d50714d5c56172f7cc5174";
constexpr char kConfig[] =
    "{\"ledmap\":null,\"outputs\":[{\"slot_index\":0,\"type\":\"digital_out\",\"config\":{\"gpio\":16,\"pixel_count\":50,\"strip_type\":\"WS2812B\",\"color_order\":\"GRB\",\"reversed\":false}}],\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":2,\"is_2d\":false,\"start_px\":0,\"stop_px\":50,\"config\":{\"palette_id\":0,\"speed\":96,\"intensity\":128,\"brightness\":255,\"blend\":\"replace\",\"opacity\":255},\"updated_at\":\"2026-09-22T13:10:00Z\"}],\"global_brightness\":255}";
constexpr char kPreset[] =
    "{\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":0,\"is_2d\":false,\"start_px\":0,\"stop_px\":50,\"config\":{\"colors\":[\"#0040FF\"],\"brightness\":220,\"blend\":\"replace\",\"opacity\":255},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"outputs\":[]}";
constexpr char kApplyCommand[] =
    "{\"command_id\":\"22222222-2222-4222-8222-222222222222\",\"type\":\"apply_config\",\"requested_at\":\"2026-09-22T12:00:01Z\",\"requested_by\":{\"actor_type\":\"product-admin\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"config-version-13\",\"payload\":{\"config_version\":13,\"config_url\":\"https://example.invalid/api/v1/devices/11111111-1111-4111-8111-111111111111/config/13\",\"checksum\":\"b62868aef25369d8b00db8dbc6af34c7c82b060b5570b0b7b24859a7d5547c5a\"},\"future_field\":true}";
constexpr char kActivateCommand[] =
    "{\"command_id\":\"66666666-6666-4666-8666-666666666666\",\"type\":\"activate_preset\",\"requested_at\":\"2026-09-22T13:01:00.123Z\",\"requested_by\":{\"actor_type\":\"tenant-operator\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"55555555-5555-4555-8555-555555555555\",\"payload\":{\"preset_id\":\"55555555-5555-4555-8555-555555555555\",\"transition\":\"instant\",\"duration_ms\":0}}";

class OneRecordJournal final : public pixels_firmware::TerminalJournal {
public:
    esp_err_t find(const char *command_id, pixels_firmware::TerminalRecord &record) const override
    {
        if (!stored_ || std::strcmp(command_id, record_.command_id) != 0) { return ESP_ERR_NOT_FOUND; }
        record = record_;
        return ESP_OK;
    }
    esp_err_t store(const pixels_firmware::TerminalRecord &record) override
    {
        record_ = record;
        stored_ = true;
        return ESP_OK;
    }
private:
    bool stored_ = false;
    pixels_firmware::TerminalRecord record_{};
};

class FakeSink final : public esp_pixels_composer::PixelFrameSink {
public:
    Capabilities capabilities() const override { return {1, 50, false}; }
    uint8_t outputCount() const override { return 1; }
    uint16_t pixelCount(uint8_t output_slot) const override { return output_slot == 0 ? 50 : 0; }
    esp_err_t beginFrame() override
    {
        if (state_ != 0) { return ESP_ERR_INVALID_STATE; }
        state_ = 1;
        submitted_ = false;
        return ESP_OK;
    }
    esp_err_t submit(uint8_t output_slot, const esp_pixels_composer::CRGB *slice,
                     uint16_t slice_len) override
    {
        if (state_ != 1 || submitted_ || output_slot != 0 || slice == nullptr || slice_len != 50) {
            return ESP_ERR_INVALID_ARG;
        }
        std::memcpy(frame_, slice, sizeof(frame_));
        submitted_ = true;
        return ESP_OK;
    }
    esp_err_t commit() override
    {
        if (state_ != 1 || !submitted_) { return ESP_ERR_INVALID_STATE; }
        state_ = 2;
        return ESP_OK;
    }
    esp_err_t abortFrame() override
    {
        if (state_ != 1) { return ESP_ERR_INVALID_STATE; }
        state_ = 0;
        return ESP_OK;
    }
    esp_err_t wait() override
    {
        if (state_ != 2) { return ESP_ERR_INVALID_STATE; }
        state_ = 0;
        return ESP_OK;
    }
    const esp_pixels_composer::CRGB &pixel(size_t index) const { return frame_[index]; }
private:
    uint8_t state_ = 0;
    bool submitted_ = false;
    esp_pixels_composer::CRGB frame_[50]{};
};

} // namespace

extern "C" void app_main()
{
    using Buffer = pixels_network::MessageBuffer<8>;
    using Result = Buffer::Result;
    static Buffer buffer;
    int checks = 0, failures = 0;
    auto check = [&](bool value) {
        ++checks;
        if (!value) { ++failures; std::printf("FAILED check %d\n", checks); }
    };

    check(buffer.append("abc", 3, 0, 8) == Result::Incomplete);
    check(buffer.append("defgh", 5, 3, 8) == Result::Complete);
    check(buffer.size() == 8 && std::memcmp(buffer.data(), "abcdefgh\0", 9) == 0);
    check(buffer.append("x", 1, 8, 8) == Result::Rejected);
    check(buffer.append("x", 1, 0, 9) == Result::Rejected);
    check(buffer.append("x", 1, 1, 2) == Result::Rejected);
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete);
    check(buffer.append("c", 1, 3, 4) == Result::Rejected);
    check(buffer.append("cd", 2, 2, 4) == Result::Rejected);
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete);
    check(buffer.append("cd", 2, 2, 5) == Result::Rejected);
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete);
    buffer.reset();
    check(buffer.append("cd", 2, 2, 4) == Result::Rejected);
    check(buffer.append(nullptr, 1, 0, 1) == Result::Rejected);
    check(buffer.append("a", -1, 0, 1) == Result::Rejected);
    check(buffer.append("a", 1, -1, 1) == Result::Rejected);
    check(buffer.append("a", INT_MAX, INT_MAX, INT_MAX) == Result::Rejected);
    check(buffer.append(nullptr, 0, 0, 1) == Result::Rejected);
    check(buffer.append(nullptr, 0, 0, 0) == Result::Complete);
    check(buffer.size() == 0 && buffer.data()[0] == '\0');
    const char binary[] = {'a', '\0', 'b'};
    check(buffer.append(binary, 3, 0, 3) == Result::Complete);
    check(buffer.size() == 3 && std::memcmp(buffer.data(), binary, 3) == 0);
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete);
    check(buffer.append("ok", 2, 0, 2) == Result::Complete);
    check(buffer.size() == 2 && std::memcmp(buffer.data(), "ok", 2) == 0);

    using namespace pixels_firmware;
    uint8_t config_digest[kSha256Bytes]{};
    uint8_t preset_digest[kSha256Bytes]{};
    check(parseSha256(kConfigHash, config_digest) == ESP_OK);
    check(parseSha256(kPresetHash, preset_digest) == ESP_OK);
    check(verifySha256(kConfig, sizeof(kConfig) - 1, config_digest) == ESP_OK);
    check(verifySha256(kPreset, sizeof(kPreset) - 1, preset_digest) == ESP_OK);
    check(verifySha256(kPreset, sizeof(kPreset), preset_digest) == ESP_ERR_INVALID_CRC);

    static Command command;
    ParseError parse_error = ParseError::InvalidJson;
    check(parseCommand(kApplyCommand, sizeof(kApplyCommand) - 1, command, parse_error) == ESP_OK);
    check(parse_error == ParseError::None && command.type == CommandType::ApplyConfig &&
          command.config_version == 13 && command.has_checksum &&
          std::strcmp(command.command_id, kCommandId) == 0 &&
          std::strcmp(command.actor_id, kActorId) == 0);
    check(verifySha256(kConfig, sizeof(kConfig) - 1, command.checksum) == ESP_OK);
    check(parseCommand(kActivateCommand, sizeof(kActivateCommand) - 1, command, parse_error) == ESP_OK);
    check(command.type == CommandType::ActivatePreset && command.duration_ms == 0 &&
          std::strcmp(command.preset_id, kPresetId) == 0 &&
          std::strcmp(command.transition, "instant") == 0);

    constexpr char kUnsupported[] =
        "{\"command_id\":\"22222222-2222-4222-8222-222222222222\",\"type\":\"dance\",\"requested_at\":\"2026-09-22T12:00:01Z\",\"requested_by\":{\"actor_type\":\"product-admin\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"x\",\"payload\":{}}";
    check(parseCommand(kUnsupported, sizeof(kUnsupported) - 1, command, parse_error) == ESP_ERR_NOT_SUPPORTED &&
          parse_error == ParseError::UnsupportedCommand);
    constexpr char kDuplicateField[] =
        "{\"command_id\":\"22222222-2222-4222-8222-222222222222\",\"command_id\":\"22222222-2222-4222-8222-222222222222\",\"type\":\"reboot\",\"requested_at\":\"2026-09-22T12:00:01Z\",\"requested_by\":{\"actor_type\":\"tenant-operator\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"x\",\"payload\":{\"reason\":\"maintenance\"}}";
    check(parseCommand(kDuplicateField, sizeof(kDuplicateField) - 1, command, parse_error) == ESP_ERR_INVALID_ARG &&
          parse_error == ParseError::InvalidEnvelope);

    static char event_json[kMaxEventBytes];
    size_t event_size = 0;
    Event applied{EventType::CommandApplied, "2026-09-22T12:00:05Z", kDeviceId, kCommandId};
    check(serializeEvent(applied, event_json, sizeof(event_json), event_size) == ESP_OK);
    constexpr char kExpectedApplied[] =
        "{\"event_type\":\"command_applied\",\"event_at\":\"2026-09-22T12:00:05Z\",\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"command_id\":\"22222222-2222-4222-8222-222222222222\",\"payload\":{}}";
    check(event_size == sizeof(kExpectedApplied) - 1 && std::strcmp(event_json, kExpectedApplied) == 0);
    char short_event[32]{};
    check(serializeEvent(applied, short_event, sizeof(short_event), event_size) == ESP_ERR_INVALID_SIZE &&
          event_size == 0);
    Event failed{EventType::CommandFailed, "2026-09-22T12:00:04Z", kDeviceId, kCommandId,
                 "checksum_mismatch", "downloaded payload checksum mismatch"};
    check(serializeEvent(failed, event_json, sizeof(event_json), event_size) == ESP_OK &&
          std::strstr(event_json, "checksum_mismatch") != nullptr);

    static OneRecordJournal journal;
    TerminalRecord terminal{};
    std::memcpy(terminal.command_id, kCommandId, sizeof(kCommandId));
    terminal.event_size = static_cast<uint16_t>(event_size);
    std::memcpy(terminal.event_json, event_json, event_size + 1);
    check(journal.store(terminal) == ESP_OK);
    static char replay[kMaxEventBytes];
    size_t replay_size = 0;
    check(replayTerminal(journal, kCommandId, replay, sizeof(replay), replay_size) == ESP_OK);
    check(replay_size == event_size && std::memcmp(replay, event_json, event_size + 1) == 0);
    check(replayTerminal(journal, kPresetId, replay, sizeof(replay), replay_size) == ESP_ERR_NOT_FOUND);

    static pixels_network::NetworkSession network_probe;
    check(!network_probe.status().running && !network_probe.status().wifi_connected);
    static_assert(esp_pixels_composer_adapter_esp_pixels::EspPixelsSink::kMaxOutputs == 2);
    check(esp_pixels_fx::describe(esp_pixels_fx::FX_RAINBOW) != nullptr);

    static FakeSink sink;
    static esp_pixels_composer::Composer composer;
    esp_pixels_fx::FxParams parameters{};
    parameters.color[0] = {4, 8, 12};
    check(composer.build(sink) == ESP_OK);
    check(composer.addSegment(0, 50, esp_pixels_fx::FX_SOLID, parameters) == ESP_OK);
    check(composer.renderFrame(1000) == ESP_OK);
    check(sink.pixel(0).r == 4 && sink.pixel(0).g == 8 && sink.pixel(0).b == 12 &&
          sink.pixel(49).r == 4 && sink.pixel(49).g == 8 && sink.pixel(49).b == 12);
    check(composer.end() == ESP_OK);

    for (;;) {
        std::printf("NETWORK_CONTRACT_VALIDATION checks=%d failures=%d RESULT=%s\n",
                    checks, failures, failures == 0 ? "ALL_PASS" : "FAIL");
        std::fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
