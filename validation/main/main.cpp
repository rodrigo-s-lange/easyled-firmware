#include "message_buffer.hpp"
#include "network_session.hpp"
#include "firmware_contract.hpp"
#include "pixels_payload_adapter.hpp"
#include "pixels_command_executor.hpp"
#include "pixels_control_runtime.hpp"
#include "pixels_durable_storage.hpp"
#include "esp_pixels_composer.hpp"
#include "esp_pixels_composer_adapter_esp_pixels.hpp"
#include "esp_pixels_fx.hpp"
#include "esp_pixels_fx_ids.hpp"

#include <climits>
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

// --- Versioned fixtures (validation/fixtures/*.provisional.json), embedded
// via EMBED_TXTFILES so the on-disk files stay inspectable/diffable while
// this binary still uses their exact bytes -- never a second copy pasted
// into source. EMBED_TXTFILES appends one NUL past the real file content;
// fixtureSize() below subtracts it back out so every exact-byte check
// (SHA-256, command parsing) sees precisely the versioned file's own bytes.
extern "C" {
extern const uint8_t apply_config_command_provisional_json_start[] asm(
    "_binary_apply_config_command_provisional_json_start");
extern const uint8_t apply_config_command_provisional_json_end[] asm(
    "_binary_apply_config_command_provisional_json_end");
extern const uint8_t config_payload_bench_gpio16_50_provisional_json_start[] asm(
    "_binary_config_payload_bench_gpio16_50_provisional_json_start");
extern const uint8_t config_payload_bench_gpio16_50_provisional_json_end[] asm(
    "_binary_config_payload_bench_gpio16_50_provisional_json_end");
extern const uint8_t activate_preset_command_provisional_json_start[] asm(
    "_binary_activate_preset_command_provisional_json_start");
extern const uint8_t activate_preset_command_provisional_json_end[] asm(
    "_binary_activate_preset_command_provisional_json_end");
extern const uint8_t preset_payload_provisional_json_start[] asm(
    "_binary_preset_payload_provisional_json_start");
extern const uint8_t preset_payload_provisional_json_end[] asm(
    "_binary_preset_payload_provisional_json_end");
extern const uint8_t event_command_applied_provisional_json_start[] asm(
    "_binary_event_command_applied_provisional_json_start");
extern const uint8_t event_command_applied_provisional_json_end[] asm(
    "_binary_event_command_applied_provisional_json_end");
extern const uint8_t event_checksum_mismatch_rejected_provisional_json_start[] asm(
    "_binary_event_checksum_mismatch_rejected_provisional_json_start");
extern const uint8_t event_checksum_mismatch_rejected_provisional_json_end[] asm(
    "_binary_event_checksum_mismatch_rejected_provisional_json_end");
extern const uint8_t event_command_failed_provisional_json_start[] asm(
    "_binary_event_command_failed_provisional_json_start");
extern const uint8_t event_command_failed_provisional_json_end[] asm(
    "_binary_event_command_failed_provisional_json_end");
}

namespace {

const char *fixtureBytes(const uint8_t *start) { return reinterpret_cast<const char *>(start); }
size_t fixtureSize(const uint8_t *start, const uint8_t *end)
{
    return static_cast<size_t>(end - start) - 1; // drop EMBED_TXTFILES' own trailing NUL
}

#define FIXTURE_BYTES(name) fixtureBytes(name##_start)
#define FIXTURE_SIZE(name) fixtureSize(name##_start, name##_end)

constexpr char kDeviceId[] = "11111111-1111-4111-8111-111111111111";
constexpr char kApplyCommandId[] = "22222222-2222-4222-8222-222222222222";
constexpr char kActorId[] = "33333333-3333-4333-8333-333333333333";
constexpr char kPresetId[] = "55555555-5555-4555-8555-555555555555";
constexpr char kActivateCommandId[] = "66666666-6666-4666-8666-666666666666";
constexpr char kConfigHash[] = "b62868aef25369d8b00db8dbc6af34c7c82b060b5570b0b7b24859a7d5547c5a";
constexpr char kPresetHash[] = "c9be5621fa0109c6acbf4461a47191b80b3b6ebae2d50714d5c56172f7cc5174";
pixels_payload::Candidate payload_candidate{};

int checks = 0, failures = 0;
void check(bool value, int line)
{
    ++checks;
    if (!value) { ++failures; std::printf("FAILED check %d (line %d)\n", checks, line); }
}
#define CHECK(expr) check((expr), __LINE__)

// --- Fakes / injected boundaries --------------------------------------------

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

// Records every store() call (never just the last one) and every find()
// lookup, with an injectable one-shot store failure -- needed to prove a
// terminal-store failure after a successful activation rolls back instead
// of ever reporting "applied but not saved".
class RecordingTerminalJournal final : public pixels_firmware::TerminalJournal {
public:
    esp_err_t find(const char *command_id, pixels_firmware::TerminalRecord &record) const override
    {
        ++find_calls_;
        for (int i = 0; i < count_; ++i) {
            if (std::strcmp(command_id, records_[i].command_id) == 0) {
                record = records_[i];
                return ESP_OK;
            }
        }
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t store(const pixels_firmware::TerminalRecord &record) override
    {
        ++store_calls_;
        if (fail_next_store_) { fail_next_store_ = false; return ESP_FAIL; }
        for (int i = 0; i < count_; ++i) {
            if (std::strcmp(record.command_id, records_[i].command_id) == 0) {
                records_[i] = record; // upsert: a later terminal outcome replaces an earlier one
                return ESP_OK;
            }
        }
        if (count_ >= kMaxRecords) { return ESP_ERR_NO_MEM; }
        records_[count_++] = record;
        return ESP_OK;
    }
    void failNextStore() { fail_next_store_ = true; }
    int storeCalls() const { return store_calls_; }
    int findCalls() const { return find_calls_; }
private:
    static constexpr int kMaxRecords = 4;
    pixels_firmware::TerminalRecord records_[kMaxRecords]{};
    int count_ = 0;
    int store_calls_ = 0;
    mutable int find_calls_ = 0;
    bool fail_next_store_ = false;
};

// Serves exact fixture bytes keyed by the command's own resource
// (config_url for apply_config, preset_id for activate_preset), with an
// injectable one-shot resolution failure and a call counter -- needed to
// prove zero payload calls on duplicate replay.
class FakePayloadSource final : public pixels_executor::PayloadSource {
public:
    void serveConfig(const char *resource_url, const char *bytes, size_t size, const uint8_t checksum[32])
    {
        config_url_ = resource_url;
        config_bytes_ = bytes;
        config_size_ = size;
        std::memcpy(config_checksum_, checksum, 32);
    }
    void servePreset(const char *preset_id, const char *bytes, size_t size, const uint8_t checksum[32])
    {
        preset_id_ = preset_id;
        preset_bytes_ = bytes;
        preset_size_ = size;
        std::memcpy(preset_checksum_, checksum, 32);
    }
    void failNextResolve() { fail_next_ = true; }
    void returnOversizedOnce() { oversized_next_ = true; }
    int resolveCalls() const { return resolve_calls_; }

    esp_err_t resolve(const pixels_firmware::Command &command, char *buffer, size_t capacity, size_t &size,
                      uint8_t expected_checksum[32]) override
    {
        ++resolve_calls_;
        if (fail_next_) { fail_next_ = false; return ESP_FAIL; }
        if (oversized_next_) {
            oversized_next_ = false;
            size = capacity + 1;
            std::memset(expected_checksum, 0, 32);
            return ESP_OK;
        }
        const char *bytes = nullptr;
        size_t source_size = 0;
        const uint8_t *checksum = nullptr;
        if (command.type == pixels_firmware::CommandType::ApplyConfig && config_url_ != nullptr &&
            std::strcmp(command.resource_url, config_url_) == 0) {
            bytes = config_bytes_; source_size = config_size_; checksum = config_checksum_;
        } else if (command.type == pixels_firmware::CommandType::ActivatePreset && preset_id_ != nullptr &&
                   std::strcmp(command.preset_id, preset_id_) == 0) {
            bytes = preset_bytes_; source_size = preset_size_; checksum = preset_checksum_;
        }
        if (bytes == nullptr || source_size > capacity) { return ESP_ERR_NOT_FOUND; }
        std::memcpy(buffer, bytes, source_size);
        size = source_size;
        std::memcpy(expected_checksum, checksum, 32);
        return ESP_OK;
    }
private:
    const char *config_url_ = nullptr;
    const char *config_bytes_ = nullptr;
    size_t config_size_ = 0;
    uint8_t config_checksum_[32]{};
    const char *preset_id_ = nullptr;
    const char *preset_bytes_ = nullptr;
    size_t preset_size_ = 0;
    uint8_t preset_checksum_[32]{};
    bool fail_next_ = false;
    bool oversized_next_ = false;
    int resolve_calls_ = 0;
};

// Records every published event's own bytes AND call order, with an
// injectable one-shot publish failure.
class RecordingEventSink final : public pixels_executor::EventSink {
public:
    esp_err_t publish(const char *event_json, size_t size) override
    {
        ++publish_calls_;
        if (fail_at_call_ == publish_calls_) { fail_at_call_ = -1; return ESP_FAIL; }
        if (count_ < kMaxRecords && size < kMaxEventBytes) {
            std::memcpy(records_[count_], event_json, size + 1);
            ++count_;
        }
        return ESP_OK;
    }
    void failNextPublish() { fail_at_call_ = publish_calls_ + 1; }
    // Fails exactly the Nth publish() call (1-indexed) this instance ever
    // receives, one-shot -- needed to target a specific event (e.g. the
    // terminal `command_applied` publish, not whichever intermediate one
    // happens to be first) precisely instead of just "the next call".
    void failAtCall(int n) { fail_at_call_ = n; }
    int publishCalls() const { return publish_calls_; }
    int recordCount() const { return count_; }
    // Deliberately fails loudly (empty string, never found by strstr) on an
    // out-of-range index instead of returning a stale slot from an earlier
    // scenario that reused this same instance -- a wrong index must show up
    // as a failed assertion, not a silent pass against leftover data.
    const char *record(int index) const
    {
        static const char kEmpty[] = "";
        return (index >= 0 && index < count_) ? records_[index] : kEmpty;
    }
    bool anyContains(const char *needle) const
    {
        for (int i = 0; i < count_; ++i) {
            if (std::strstr(records_[i], needle) != nullptr) { return true; }
        }
        return false;
    }
    void reset() { count_ = 0; publish_calls_ = 0; fail_at_call_ = -1; }
private:
    static constexpr int kMaxRecords = 8;
    static constexpr size_t kMaxEventBytes = pixels_firmware::kMaxEventBytes;
    char records_[kMaxRecords][kMaxEventBytes]{};
    int count_ = 0;
    int publish_calls_ = 0;
    int fail_at_call_ = -1;
};

class FixedTimestampSource final : public pixels_executor::TimestampSource {
public:
    void set(const char *timestamp) { std::strncpy(current_, timestamp, sizeof(current_) - 1); }
    void now(char out[36]) override { std::memcpy(out, current_, sizeof(current_)); }
private:
    char current_[36] = "2026-09-22T12:00:00Z";
};

class FakeActiveTargetProbe final : public pixels_executor::ActiveTargetProbe {
public:
    void setActive(const char *preset_id, const uint8_t checksum[32])
    {
        active_ = true;
        std::strncpy(active_preset_id_, preset_id, sizeof(active_preset_id_) - 1);
        std::memcpy(active_checksum_, checksum, 32);
    }
    void clear() { active_ = false; }
    bool alreadyActive(const char *preset_id, const uint8_t checksum[32]) const override
    {
        return active_ && std::strcmp(preset_id, active_preset_id_) == 0 &&
               std::memcmp(checksum, active_checksum_, 32) == 0;
    }
private:
    bool active_ = false;
    char active_preset_id_[37]{};
    uint8_t active_checksum_[32]{};
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

// The one DurableStaging implementation these gates use: stages raw bytes
// (append-only, exactly like a reboot-recoverable implementation would),
// then on activateAtFrameBoundary() re-parses those staged bytes through
// pixels_payload_adapter (never trusting an in-memory Candidate that would
// not itself survive a reboot) and applies the result to a REAL Composer
// instance over FakeSink -- proving a fixture's own bytes genuinely reach
// rendered pixels, not just a parsed struct. Keeps the PRIOR applied scene
// (segments + count) so rollback() after a successful activation can
// restore it exactly. Does not reconfigure FakeSink's own fixed one-output/
// 50-pixel topology at runtime -- Composer::build() only ever runs once in
// these gates, matching this delivery's own explicitly deferred "final NVS
// key/capacity policy"/output reconfiguration scope; a candidate's own
// `outputs[]` are validated by pixels_payload_adapter but not re-applied to
// a physical sink here.
class ComposerBackedDurableStaging final : public pixels_firmware::DurableStaging {
public:
    explicit ComposerBackedDurableStaging(esp_pixels_composer::Composer &composer) : composer_(composer) {}

    void failNextBegin() { fail_begin_ = true; }
    void failNextAppend() { fail_append_ = true; }
    void failNextCommit() { fail_commit_ = true; }
    void failNextActivate() { fail_activate_ = true; }
    void failNextRollback() { fail_rollback_ = true; }
    int activateCalls() const { return activate_calls_; }
    int finalizeCalls() const { return finalize_calls_; }
    int rollbackCalls() const { return rollback_calls_; }

    esp_err_t begin(const pixels_firmware::Command &command, size_t total_size) override
    {
        if (fail_begin_) { fail_begin_ = false; return ESP_FAIL; }
        if (total_size > sizeof(staged_bytes_)) { return ESP_ERR_INVALID_SIZE; }
        command_type_ = command.type;
        staged_size_ = 0;
        committed_ = false;
        activation_applied_this_txn_ = false;
        return ESP_OK;
    }
    esp_err_t append(const void *bytes, size_t size) override
    {
        if (fail_append_) { fail_append_ = false; return ESP_FAIL; }
        if (staged_size_ + size > sizeof(staged_bytes_)) { return ESP_ERR_INVALID_SIZE; }
        std::memcpy(staged_bytes_ + staged_size_, bytes, size);
        staged_size_ += size;
        return ESP_OK;
    }
    esp_err_t verifyAndCommit(const uint8_t expected[pixels_firmware::kSha256Bytes]) override
    {
        if (fail_commit_) { fail_commit_ = false; return ESP_FAIL; }
        if (pixels_firmware::verifySha256(staged_bytes_, staged_size_, expected) != ESP_OK) {
            return ESP_ERR_INVALID_CRC;
        }
        committed_ = true;
        return ESP_OK;
    }
    esp_err_t activateAtFrameBoundary() override
    {
        ++activate_calls_;
        if (fail_activate_) { fail_activate_ = false; return ESP_FAIL; }
        if (!committed_) { return ESP_ERR_INVALID_STATE; }
        // `scratch_candidate_` is a class member, never a local -- `Candidate`
        // is dominated by AtScene's own MAX_SEGMENTS array, and this method
        // is already called several frames deep (Executor::execute() ->
        // here -> parseConfigPayload()'s own cJSON recursion) on a small
        // FreeRTOS task stack; a local one here caused a real stack-overflow
        // crash under QEMU (observed directly: LoadProhibited a few frames
        // in) before this fix.
        scratch_candidate_ = pixels_payload::Candidate{};
        pixels_payload::SchemaError schema_error = pixels_payload::SchemaError::None;
        const esp_err_t parsed = (command_type_ == pixels_firmware::CommandType::ApplyConfig)
            ? pixels_payload::parseConfigPayload(staged_bytes_, staged_size_, scratch_candidate_, schema_error)
            : pixels_payload::parsePresetPayload(staged_bytes_, staged_size_, scratch_candidate_, schema_error);
        if (parsed != ESP_OK) { return parsed; } // re-validated at activation time, never trusted from memory
        // Keep the CURRENTLY live scene as the rollback target BEFORE
        // overwriting it -- applyConfig() itself is transactional/adopted
        // at the next renderFrame(), so a fresh Composer's very first
        // activation has nothing prior to preserve (prior_count_ == 0).
        prior_count_ = live_count_;
        for (uint8_t i = 0; i < live_count_; ++i) { prior_segments_[i] = live_segments_[i]; }
        const esp_err_t applied = composer_.applyConfig(scratch_candidate_.scene.segments, scratch_candidate_.scene.count);
        if (applied != ESP_OK) { return applied; }
        activation_applied_this_txn_ = true; // only from here on did this transaction touch live state
        (void)composer_.renderFrame(0); // adopt the candidate now, at this "frame boundary"
        live_count_ = scratch_candidate_.scene.count;
        for (uint8_t i = 0; i < live_count_; ++i) { live_segments_[i] = scratch_candidate_.scene.segments[i]; }
        return ESP_OK;
    }
    esp_err_t finalize() override
    {
        ++finalize_calls_;
        committed_ = false;
        staged_size_ = 0;
        activation_applied_this_txn_ = false;
        return ESP_OK;
    }
    esp_err_t rollback() override
    {
        ++rollback_calls_;
        if (fail_rollback_) { fail_rollback_ = false; return ESP_FAIL; }
        committed_ = false;
        staged_size_ = 0;
        // A failed activateAtFrameBoundary() (e.g. it never got past its own
        // injected/parse/applyConfig failure) never touched live state at
        // all -- re-applying `prior_*` here would incorrectly stomp an
        // unrelated, still-live scene with a stale one from a PREVIOUS
        // transaction. Only undo when THIS transaction actually mutated it.
        if (!activation_applied_this_txn_) { return ESP_OK; }
        activation_applied_this_txn_ = false;
        const esp_err_t applied = composer_.applyConfig(prior_segments_, prior_count_);
        if (applied != ESP_OK) { return applied; }
        (void)composer_.renderFrame(0);
        live_count_ = prior_count_;
        for (uint8_t i = 0; i < live_count_; ++i) { live_segments_[i] = prior_segments_[i]; }
        return ESP_OK;
    }

private:
    esp_pixels_composer::Composer &composer_;
    char staged_bytes_[pixels_payload::kMaxPayloadBytes]{};
    size_t staged_size_ = 0;
    bool committed_ = false;
    bool activation_applied_this_txn_ = false;
    pixels_firmware::CommandType command_type_ = pixels_firmware::CommandType::ApplyConfig;
    esp_pixels_composer::SegmentSpec live_segments_[esp_pixels_composer::MAX_SEGMENTS]{};
    uint8_t live_count_ = 0;
    esp_pixels_composer::SegmentSpec prior_segments_[esp_pixels_composer::MAX_SEGMENTS]{};
    uint8_t prior_count_ = 0;
    bool fail_begin_ = false, fail_append_ = false, fail_commit_ = false, fail_activate_ = false;
    int activate_calls_ = 0;
    int finalize_calls_ = 0;
    bool fail_rollback_ = false;
    int rollback_calls_ = 0;
    pixels_payload::Candidate scratch_candidate_{};
};

class RecordingActivationTarget final : public pixels_storage::ActivationTarget {
public:
    esp_err_t activateAtFrameBoundary(pixels_firmware::CommandType type,
                                      const char *payload, size_t size) override
    {
        ++activate_calls_;
        if (payload == nullptr || size == 0 || size > pixels_payload::kMaxPayloadBytes) {
            return ESP_ERR_INVALID_SIZE;
        }
        type_ = type;
        size_ = size;
        fingerprint_ = fingerprint(payload, size);
        active_ = true;
        return ESP_OK;
    }

    esp_err_t clearAtFrameBoundary() override
    {
        ++clear_calls_;
        active_ = false;
        size_ = 0;
        return ESP_OK;
    }

    void resetObservation() { activate_calls_ = 0; clear_calls_ = 0; }
    bool matches(pixels_firmware::CommandType type, const char *payload, size_t size) const
    {
        return active_ && type_ == type && size_ == size && fingerprint_ == fingerprint(payload, size);
    }
    int activateCalls() const { return activate_calls_; }

private:
    static uint32_t fingerprint(const char *bytes, size_t size)
    {
        uint32_t value = 2166136261u;
        for (size_t i = 0; i < size; ++i) {
            value ^= static_cast<uint8_t>(bytes[i]);
            value *= 16777619u;
        }
        return value;
    }

    bool active_ = false;
    pixels_firmware::CommandType type_ = pixels_firmware::CommandType::ApplyConfig;
    size_t size_ = 0;
    uint32_t fingerprint_ = 0;
    int activate_calls_ = 0;
    int clear_calls_ = 0;
};

void runDurableStorageChecks(const uint8_t config_digest[32], const uint8_t preset_digest[32])
{
    using namespace pixels_firmware;

    // This erases only the QEMU validation image's dedicated test partition.
    // Production classes never erase or auto-format storage.
    CHECK(nvs_flash_erase_partition(pixels_storage::kPartitionLabel) == ESP_OK);

    static pixels_storage::NvsTerminalJournal journal;
    CHECK(journal.init() == ESP_OK);
    static TerminalRecord record{};
    std::memcpy(record.command_id, kApplyCommandId, sizeof(kApplyCommandId));
    static constexpr char kTerminalJson[] =
        "{\"event_type\":\"command_applied\",\"command_id\":\"22222222-2222-4222-8222-222222222222\"}";
    record.event_size = sizeof(kTerminalJson) - 1;
    std::memcpy(record.event_json, kTerminalJson, sizeof(kTerminalJson));
    CHECK(journal.store(record) == ESP_OK);
    static TerminalRecord recovered{};
    CHECK(journal.find(kApplyCommandId, recovered) == ESP_OK &&
          recovered.event_size == record.event_size &&
          std::memcmp(recovered.event_json, record.event_json, record.event_size + 1) == 0);
    journal.end();
    CHECK(journal.init() == ESP_OK);
    CHECK(journal.find(kApplyCommandId, recovered) == ESP_OK); // survives a new boot scan

    static TerminalRecord conflicting{};
    conflicting = record;
    conflicting.event_json[15] = 'X';
    CHECK(journal.store(conflicting) == ESP_ERR_INVALID_STATE); // terminal IDs are immutable

    // Fill one complete ring after the original entry: the ninth new ID
    // evicts the oldest record, while every retained record survives scan.
    static TerminalRecord ring_record{};
    for (unsigned i = 0; i < pixels_storage::kTerminalCapacity; ++i) {
        std::snprintf(ring_record.command_id, sizeof(ring_record.command_id),
                      "10000000-0000-4000-8000-%012u", i);
        std::snprintf(ring_record.event_json, sizeof(ring_record.event_json), "{\"terminal\":%u}", i);
        ring_record.event_size = static_cast<uint16_t>(std::strlen(ring_record.event_json));
        CHECK(journal.store(ring_record) == ESP_OK);
    }
    CHECK(journal.find(kApplyCommandId, recovered) == ESP_ERR_NOT_FOUND);
    journal.end();
    CHECK(journal.init() == ESP_OK);
    CHECK(journal.find("10000000-0000-4000-8000-000000000007", recovered) == ESP_OK);

    static RecordingActivationTarget target;
    static pixels_storage::NvsDurableStaging staging;
    CHECK(staging.init(target, journal) == ESP_ERR_NOT_FOUND); // blank partition is usable

    static Command apply_command{};
    ParseError parse_error = ParseError::None;
    CHECK(parseCommand(FIXTURE_BYTES(apply_config_command_provisional_json),
                       FIXTURE_SIZE(apply_config_command_provisional_json), apply_command, parse_error) == ESP_OK);
    const char *config = FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json);
    const size_t config_size = FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json);
    CHECK(staging.begin(apply_command, config_size) == ESP_OK);
    CHECK(staging.append(config, config_size / 2) == ESP_OK);
    CHECK(staging.append(config + config_size / 2, config_size - config_size / 2) == ESP_OK);
    CHECK(staging.verifyAndCommit(config_digest) == ESP_OK);
    CHECK(staging.activateAtFrameBoundary() == ESP_OK);
    CHECK(staging.hasActive() && target.matches(CommandType::ApplyConfig, config, config_size));
    CHECK(journal.store(record) == ESP_OK);

    // A reboot after terminal success but before finalize() reconciles through
    // the journal, keeps the new active bytes and safely cleans rollback data.
    staging.end();
    target.resetObservation();
    CHECK(staging.init(target, journal) == ESP_OK);
    CHECK(target.activateCalls() == 1 && target.matches(CommandType::ApplyConfig, config, config_size));

    static Command preset_command{};
    parse_error = ParseError::None;
    CHECK(parseCommand(FIXTURE_BYTES(activate_preset_command_provisional_json),
                       FIXTURE_SIZE(activate_preset_command_provisional_json), preset_command, parse_error) == ESP_OK);
    const char *preset = FIXTURE_BYTES(preset_payload_provisional_json);
    const size_t preset_size = FIXTURE_SIZE(preset_payload_provisional_json);
    CHECK(staging.begin(preset_command, preset_size) == ESP_OK);
    CHECK(staging.append(preset, preset_size) == ESP_OK);
    CHECK(staging.verifyAndCommit(preset_digest) == ESP_OK);
    CHECK(staging.activateAtFrameBoundary() == ESP_OK);
    CHECK(staging.activePresetMatches(kPresetId, preset_digest));
    CHECK(target.matches(CommandType::ActivatePreset, preset, preset_size));

    // A post-activation failure can still restore the previous durable/live
    // config because the older A/B slot has not been discarded yet.
    CHECK(staging.rollback() == ESP_OK);
    CHECK(target.matches(CommandType::ApplyConfig, config, config_size));
    CHECK(!staging.activePresetMatches(kPresetId, preset_digest));

    // A reboot after activation but before terminal success must restore the
    // prior active config; the duplicate command remains safe to retry.
    CHECK(staging.begin(preset_command, preset_size) == ESP_OK);
    CHECK(staging.append(preset, preset_size) == ESP_OK);
    CHECK(staging.verifyAndCommit(preset_digest) == ESP_OK);
    CHECK(staging.activateAtFrameBoundary() == ESP_OK);
    staging.end();
    target.resetObservation();
    CHECK(staging.init(target, journal) == ESP_OK);
    CHECK(target.activateCalls() == 1 && target.matches(CommandType::ApplyConfig, config, config_size));

    // A reboot between candidate commit and activation restores the prior
    // active state and discards the unactivated pending candidate.
    CHECK(staging.begin(preset_command, preset_size) == ESP_OK);
    CHECK(staging.append(preset, preset_size) == ESP_OK);
    CHECK(staging.verifyAndCommit(preset_digest) == ESP_OK);
    staging.end();
    target.resetObservation();
    CHECK(staging.init(target, journal) == ESP_OK);
    CHECK(target.activateCalls() == 1 && target.matches(CommandType::ApplyConfig, config, config_size));

    // Complete the preset transaction, then prove preset identity/checksum
    // and exact bytes survive another boot.
    CHECK(staging.begin(preset_command, preset_size) == ESP_OK);
    CHECK(staging.append(preset, preset_size) == ESP_OK);
    CHECK(staging.verifyAndCommit(preset_digest) == ESP_OK);
    CHECK(staging.activateAtFrameBoundary() == ESP_OK);
    static TerminalRecord preset_terminal{};
    std::memcpy(preset_terminal.command_id, preset_command.command_id,
                sizeof(preset_terminal.command_id));
    static constexpr char kPresetTerminalJson[] =
        "{\"event_type\":\"command_applied\",\"command_id\":\"55555555-5555-4555-8555-555555555555\"}";
    preset_terminal.event_size = sizeof(kPresetTerminalJson) - 1;
    std::memcpy(preset_terminal.event_json, kPresetTerminalJson, sizeof(kPresetTerminalJson));
    CHECK(journal.store(preset_terminal) == ESP_OK);
    CHECK(staging.finalize() == ESP_OK);
    staging.end();
    target.resetObservation();
    CHECK(staging.init(target, journal) == ESP_OK);
    CHECK(target.activateCalls() == 1 && target.matches(CommandType::ActivatePreset, preset, preset_size));
    CHECK(staging.activePresetMatches(kPresetId, preset_digest));

    // The declared 8192-byte limit is exercised against real NVS rather
    // than inferred from the smaller contract fixtures.
    static constexpr char kMaxPayloadHash[] =
        "f8ca02c69621dd84cd1212ebfd7d6cdc9ba6ad658854f29567723531912d1a35";
    static uint8_t max_payload_digest[32]{};
    static char max_chunk[256]{};
    std::memset(max_chunk, 'A', sizeof(max_chunk));
    CHECK(parseSha256(kMaxPayloadHash, max_payload_digest) == ESP_OK);
    CHECK(staging.begin(apply_command, pixels_payload::kMaxPayloadBytes) == ESP_OK);
    for (size_t offset = 0; offset < pixels_payload::kMaxPayloadBytes; offset += sizeof(max_chunk)) {
        CHECK(staging.append(max_chunk, sizeof(max_chunk)) == ESP_OK);
    }
    CHECK(staging.verifyAndCommit(max_payload_digest) == ESP_OK);
    CHECK(staging.rollback() == ESP_OK); // pending-only rollback keeps the active preset
    CHECK(staging.activePresetMatches(kPresetId, preset_digest));

    staging.end();
    nvs_handle_t corrupt_handle = 0;
    CHECK(nvs_open_from_partition(pixels_storage::kPartitionLabel, "easyled_stage",
                                  NVS_READWRITE, &corrupt_handle) == ESP_OK);
    static constexpr uint8_t kCorruptRecord[] = {0xde, 0xad, 0xbe, 0xef};
    CHECK(nvs_set_blob(corrupt_handle, "active_a", kCorruptRecord, sizeof(kCorruptRecord)) == ESP_OK);
    CHECK(nvs_set_blob(corrupt_handle, "active_b", kCorruptRecord, sizeof(kCorruptRecord)) == ESP_OK);
    CHECK(nvs_commit(corrupt_handle) == ESP_OK);
    nvs_close(corrupt_handle);
    CHECK(staging.init(target, journal) == ESP_ERR_INVALID_RESPONSE); // corrupt is not reported as blank

    journal.end();
    CHECK(nvs_open_from_partition(pixels_storage::kPartitionLabel, "easyled_journal",
                                  NVS_READWRITE, &corrupt_handle) == ESP_OK);
    CHECK(nvs_set_blob(corrupt_handle, "j0", kCorruptRecord, sizeof(kCorruptRecord)) == ESP_OK);
    CHECK(nvs_commit(corrupt_handle) == ESP_OK);
    nvs_close(corrupt_handle);
    CHECK(journal.init() == ESP_ERR_INVALID_RESPONSE); // never risk replay after journal corruption
}

void runControlRuntimeChecks(const uint8_t config_digest[32], const uint8_t preset_digest[32])
{
    static const pixels_control::EmbeddedResource resources[] = {
        {pixels_firmware::CommandType::ApplyConfig,
         "https://example.invalid/api/v1/devices/11111111-1111-4111-8111-111111111111/config/13",
         FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json),
         FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json), config_digest},
        {pixels_firmware::CommandType::ActivatePreset, kPresetId,
         FIXTURE_BYTES(preset_payload_provisional_json),
         FIXTURE_SIZE(preset_payload_provisional_json), preset_digest},
    };
    static pixels_control::EmbeddedPayloadSource source(resources, 2);
    static char resolved[1024]{};
    static uint8_t resolved_digest[pixels_firmware::kSha256Bytes]{};
    size_t resolved_size = 0;
    pixels_firmware::Command command{};
    pixels_firmware::ParseError parse_error = pixels_firmware::ParseError::None;
    CHECK(pixels_firmware::parseCommand(FIXTURE_BYTES(apply_config_command_provisional_json),
                                        FIXTURE_SIZE(apply_config_command_provisional_json),
                                        command, parse_error) == ESP_OK);
    CHECK(source.resolve(command, resolved, sizeof(resolved), resolved_size, resolved_digest) == ESP_OK);
    CHECK(resolved_size == FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json) &&
          std::memcmp(resolved, FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json),
                      resolved_size) == 0 &&
          std::memcmp(resolved_digest, config_digest, sizeof(resolved_digest)) == 0);
    CHECK(source.resolve(command, resolved, resolved_size - 1, resolved_size, resolved_digest) ==
          ESP_ERR_INVALID_SIZE);
    command.resource_url[0] = 'x';
    CHECK(source.resolve(command, resolved, sizeof(resolved), resolved_size, resolved_digest) ==
          ESP_ERR_NOT_FOUND);

    command = pixels_firmware::Command{};
    CHECK(pixels_firmware::parseCommand(FIXTURE_BYTES(activate_preset_command_provisional_json),
                                        FIXTURE_SIZE(activate_preset_command_provisional_json),
                                        command, parse_error) == ESP_OK);
    CHECK(source.resolve(command, resolved, sizeof(resolved), resolved_size, resolved_digest) == ESP_OK &&
          resolved_size == FIXTURE_SIZE(preset_payload_provisional_json) &&
          std::memcmp(resolved_digest, preset_digest, sizeof(resolved_digest)) == 0);

    char timestamp[36]{};
    pixels_control::UtcTimestampSource timestamps;
    timestamps.now(timestamp);
    CHECK(std::strlen(timestamp) == 20 && timestamp[4] == '-' && timestamp[10] == 'T' &&
          timestamp[19] == 'Z');

    // This is the production activation adapter over a real managed Composer,
    // with only the hardware transport replaced by FakeSink. Confirmation is
    // obtained from reclaim(), not from a timing guess.
    static FakeSink sink;
    static esp_pixels_composer::Composer composer;
    CHECK(composer.build(sink) == ESP_OK);
    CHECK(composer.start(2) == ESP_OK);
    static constexpr pixels_payload::OutputCandidate topology[] = {
        {.gpio = 16, .pixel_count = 50, .order = esp_pixels::PixelOrder::GRB},
    };
    static pixels_control::ComposerActivationTarget target(composer, topology, 1);
    const size_t config_size = FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json);
    std::memcpy(resolved, FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json), config_size);
    resolved[config_size] = '\0';
    char *gpio = std::strstr(resolved, "\"gpio\":16");
    CHECK(gpio != nullptr);
    if (gpio != nullptr) { gpio[8] = '7'; }
    CHECK(target.activateAtFrameBoundary(
              pixels_firmware::CommandType::ApplyConfig,
              resolved, config_size) == ESP_ERR_NOT_SUPPORTED);
    CHECK(target.activateAtFrameBoundary(
              pixels_firmware::CommandType::ApplyConfig,
              FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json),
              FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json)) == ESP_OK);
    CHECK(target.activateAtFrameBoundary(
              pixels_firmware::CommandType::ActivatePreset,
              FIXTURE_BYTES(preset_payload_provisional_json),
              FIXTURE_SIZE(preset_payload_provisional_json)) == ESP_OK);
    CHECK(sink.pixel(0).r == 0x00 && sink.pixel(0).g == 0x37 && sink.pixel(0).b == 0xDC);
    CHECK(target.clearAtFrameBoundary() == ESP_OK);
    CHECK(composer.end() == ESP_OK);
}

} // namespace

extern "C" void app_main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // never lose diagnostic output to a panic/reboot mid-buffer
    using Buffer = pixels_network::MessageBuffer<8>;
    using Result = Buffer::Result;
    static Buffer buffer;

    check(buffer.append("abc", 3, 0, 8) == Result::Incomplete, __LINE__);
    check(buffer.append("defgh", 5, 3, 8) == Result::Complete, __LINE__);
    check(buffer.size() == 8 && std::memcmp(buffer.data(), "abcdefgh\0", 9) == 0, __LINE__);
    check(buffer.append("x", 1, 8, 8) == Result::Rejected, __LINE__);
    check(buffer.append("x", 1, 0, 9) == Result::Rejected, __LINE__);
    check(buffer.append("x", 1, 1, 2) == Result::Rejected, __LINE__);
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete, __LINE__);
    check(buffer.append("c", 1, 3, 4) == Result::Rejected, __LINE__);
    check(buffer.append("cd", 2, 2, 4) == Result::Rejected, __LINE__);
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete, __LINE__);
    check(buffer.append("cd", 2, 2, 5) == Result::Rejected, __LINE__);
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete, __LINE__);
    buffer.reset();
    check(buffer.append("cd", 2, 2, 4) == Result::Rejected, __LINE__);
    check(buffer.append(nullptr, 1, 0, 1) == Result::Rejected, __LINE__);
    check(buffer.append("a", -1, 0, 1) == Result::Rejected, __LINE__);
    check(buffer.append("a", 1, -1, 1) == Result::Rejected, __LINE__);
    check(buffer.append("a", INT_MAX, INT_MAX, INT_MAX) == Result::Rejected, __LINE__);
    check(buffer.append(nullptr, 0, 0, 1) == Result::Rejected, __LINE__);
    check(buffer.append(nullptr, 0, 0, 0) == Result::Complete, __LINE__);
    check(buffer.size() == 0 && buffer.data()[0] == '\0', __LINE__);
    static const char binary[] = {'a', '\0', 'b'};
    check(buffer.append(binary, 3, 0, 3) == Result::Complete, __LINE__);
    check(buffer.size() == 3 && std::memcmp(buffer.data(), binary, 3) == 0, __LINE__);
    check(buffer.append("ab", 2, 0, 4) == Result::Incomplete, __LINE__);
    check(buffer.append("ok", 2, 0, 2) == Result::Complete, __LINE__);
    check(buffer.size() == 2 && std::memcmp(buffer.data(), "ok", 2) == 0, __LINE__);

    using namespace pixels_firmware;

    // --- Versioned fixtures: exact-byte hash re-verification -------------
    // (the fixture FILES are the source of truth now, not string literals)
    static uint8_t config_digest[kSha256Bytes]{};
    static uint8_t preset_digest[kSha256Bytes]{};
    CHECK(parseSha256(kConfigHash, config_digest) == ESP_OK);
    CHECK(parseSha256(kPresetHash, preset_digest) == ESP_OK);
    runDurableStorageChecks(config_digest, preset_digest);
    runControlRuntimeChecks(config_digest, preset_digest);
    CHECK(verifySha256(FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json),
                       FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json), config_digest) == ESP_OK);
    CHECK(verifySha256(FIXTURE_BYTES(preset_payload_provisional_json),
                       FIXTURE_SIZE(preset_payload_provisional_json), preset_digest) == ESP_OK);

    // --- Command envelope parsing (unchanged contract, now from fixture files) ---
    static Command command;
    ParseError parse_error = ParseError::InvalidJson;
    CHECK(parseCommand(FIXTURE_BYTES(apply_config_command_provisional_json),
                       FIXTURE_SIZE(apply_config_command_provisional_json), command, parse_error) == ESP_OK);
    CHECK(parse_error == ParseError::None && command.type == CommandType::ApplyConfig &&
          command.config_version == 13 && command.has_checksum &&
          std::strcmp(command.command_id, kApplyCommandId) == 0 &&
          std::strcmp(command.actor_id, kActorId) == 0);
    CHECK(verifySha256(FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json),
                       FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json), command.checksum) == ESP_OK);
    CHECK(parseCommand(FIXTURE_BYTES(activate_preset_command_provisional_json),
                       FIXTURE_SIZE(activate_preset_command_provisional_json), command, parse_error) == ESP_OK);
    CHECK(command.type == CommandType::ActivatePreset && command.duration_ms == 0 &&
          std::strcmp(command.preset_id, kPresetId) == 0 &&
          std::strcmp(command.transition, "instant") == 0);

    static constexpr char kUnsupported[] =
        "{\"command_id\":\"22222222-2222-4222-8222-222222222222\",\"type\":\"dance\",\"requested_at\":\"2026-09-22T12:00:01Z\",\"requested_by\":{\"actor_type\":\"product-admin\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"x\",\"payload\":{}}";
    CHECK(parseCommand(kUnsupported, sizeof(kUnsupported) - 1, command, parse_error) == ESP_ERR_NOT_SUPPORTED &&
          parse_error == ParseError::UnsupportedCommand);
    static constexpr char kDuplicateField[] =
        "{\"command_id\":\"22222222-2222-4222-8222-222222222222\",\"command_id\":\"22222222-2222-4222-8222-222222222222\",\"type\":\"reboot\",\"requested_at\":\"2026-09-22T12:00:01Z\",\"requested_by\":{\"actor_type\":\"tenant-operator\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"x\",\"payload\":{\"reason\":\"maintenance\"}}";
    CHECK(parseCommand(kDuplicateField, sizeof(kDuplicateField) - 1, command, parse_error) == ESP_ERR_INVALID_ARG &&
          parse_error == ParseError::InvalidEnvelope);

    // extractCommandId(): the executor's own bounded pre-check.
    static char extracted_id[kUuidBytes + 1]{};
    CHECK(extractCommandId(FIXTURE_BYTES(apply_config_command_provisional_json),
                           FIXTURE_SIZE(apply_config_command_provisional_json), extracted_id) == ESP_OK &&
          std::strcmp(extracted_id, kApplyCommandId) == 0);
    CHECK(extractCommandId("{}", 2, extracted_id) == ESP_ERR_INVALID_ARG);

    // --- Event serialization (unchanged contract, now cross-checked
    // against the versioned event fixture files too) -----------------------
    static char event_json[kMaxEventBytes];
    size_t event_size = 0;
    Event applied{EventType::CommandApplied, "2026-09-22T12:00:05Z", kDeviceId, kApplyCommandId};
    CHECK(serializeEvent(applied, event_json, sizeof(event_json), event_size) == ESP_OK);
    CHECK(event_size == FIXTURE_SIZE(event_command_applied_provisional_json) &&
          std::memcmp(event_json, FIXTURE_BYTES(event_command_applied_provisional_json), event_size) == 0);
    static char short_event[32]{};
    CHECK(serializeEvent(applied, short_event, sizeof(short_event), event_size) == ESP_ERR_INVALID_SIZE &&
          event_size == 0);
    Event mismatch{EventType::CommandRejected, "2026-09-22T12:00:03Z", kDeviceId, kApplyCommandId,
                   "checksum_mismatch", "downloaded payload checksum mismatch"};
    CHECK(serializeEvent(mismatch, event_json, sizeof(event_json), event_size) == ESP_OK);
    CHECK(event_size == FIXTURE_SIZE(event_checksum_mismatch_rejected_provisional_json) &&
          std::memcmp(event_json, FIXTURE_BYTES(event_checksum_mismatch_rejected_provisional_json), event_size) == 0);
    Event failed{EventType::CommandFailed, "2026-09-22T12:00:04Z", kDeviceId, kApplyCommandId,
                 "activation_failed", "activation failed"};
    CHECK(serializeEvent(failed, event_json, sizeof(event_json), event_size) == ESP_OK);
    CHECK(event_size == FIXTURE_SIZE(event_command_failed_provisional_json) &&
          std::memcmp(event_json, FIXTURE_BYTES(event_command_failed_provisional_json), event_size) == 0);

    static OneRecordJournal journal;
    static TerminalRecord terminal{};
    std::memcpy(terminal.command_id, kApplyCommandId, sizeof(kApplyCommandId));
    terminal.event_size = static_cast<uint16_t>(event_size);
    std::memcpy(terminal.event_json, event_json, event_size + 1);
    CHECK(journal.store(terminal) == ESP_OK);
    static char replay[kMaxEventBytes];
    size_t replay_size = 0;
    CHECK(replayTerminal(journal, kApplyCommandId, replay, sizeof(replay), replay_size) == ESP_OK);
    CHECK(replay_size == event_size && std::memcmp(replay, event_json, event_size + 1) == 0);
    CHECK(replayTerminal(journal, kPresetId, replay, sizeof(replay), replay_size) == ESP_ERR_NOT_FOUND);

    static pixels_network::NetworkSession network_probe;
    CHECK(!network_probe.status().running && !network_probe.status().wifi_connected);
    static_assert(esp_pixels_composer_adapter_esp_pixels::EspPixelsSink::kMaxOutputs == 2);
    CHECK(esp_pixels_fx::describe(esp_pixels_fx::FX_RAINBOW) != nullptr);

    {
        static FakeSink sink;
        static esp_pixels_composer::Composer composer;
        esp_pixels_fx::FxParams parameters{};
        parameters.color[0] = {4, 8, 12};
        CHECK(composer.build(sink) == ESP_OK);
        CHECK(composer.addSegment(0, 50, esp_pixels_fx::FX_SOLID, parameters) == ESP_OK);
        CHECK(composer.renderFrame(1000) == ESP_OK);
        CHECK(sink.pixel(0).r == 4 && sink.pixel(0).g == 8 && sink.pixel(0).b == 12 &&
              sink.pixel(49).r == 4 && sink.pixel(49).g == 8 && sink.pixel(49).b == 12);
        CHECK(composer.end() == ESP_OK);
    }

    // =========================================================================
    // pixels_payload_adapter: exact fixture parsing, unknown-field acceptance,
    // and a compact representative-failure table.
    // =========================================================================
    {
        pixels_payload::SchemaError schema_error = pixels_payload::SchemaError::None;
        CHECK(pixels_payload::parseConfigPayload(FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json),
                                                 FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json),
                                                 payload_candidate, schema_error) == ESP_OK);
        CHECK(payload_candidate.has_outputs && payload_candidate.output_count == 1 && payload_candidate.outputs[0].gpio == 16 &&
              payload_candidate.outputs[0].pixel_count == 50 &&
              payload_candidate.outputs[0].order == esp_pixels::PixelOrder::GRB);
        CHECK(payload_candidate.scene.brightness == 255 && payload_candidate.scene.count == 1);
        CHECK(payload_candidate.scene.segments[0].id == 1 && payload_candidate.scene.segments[0].effect_id == esp_pixels_fx::FX_RAINBOW &&
              payload_candidate.scene.segments[0].start == 0 && payload_candidate.scene.segments[0].stop == 50 &&
              payload_candidate.scene.segments[0].params.palette == 0 && payload_candidate.scene.segments[0].params.speed == 96 &&
              payload_candidate.scene.segments[0].params.intensity == 128 && payload_candidate.scene.segments[0].brightness == 255 &&
              payload_candidate.scene.segments[0].blend == esp_pixels_composer::BlendMode::Replace &&
              payload_candidate.scene.segments[0].opacity == 255);
        // future_field-style forward compatibility: an unrelated unknown
        // member anywhere must never be rejected.
        static constexpr char kWithUnknown[] =
            "{\"ledmap\":null,\"outputs\":[{\"slot_index\":0,\"type\":\"digital_out\",\"config\":{\"gpio\":16,\"pixel_count\":50,\"strip_type\":\"WS2812B\",\"color_order\":\"GRB\",\"reversed\":false}}],\"inputs\":[],\"segments\":[],\"global_brightness\":10,\"future_thing\":{\"x\":1}}";
        CHECK(pixels_payload::parseConfigPayload(kWithUnknown, sizeof(kWithUnknown) - 1, payload_candidate, schema_error) ==
              ESP_OK);
        CHECK(payload_candidate.scene.brightness == 10 && payload_candidate.scene.count == 0 && payload_candidate.output_count == 1);

        CHECK(pixels_payload::parsePresetPayload(FIXTURE_BYTES(preset_payload_provisional_json),
                                                 FIXTURE_SIZE(preset_payload_provisional_json), payload_candidate,
                                                 schema_error) == ESP_OK);
        CHECK(!payload_candidate.has_outputs && payload_candidate.scene.count == 1 &&
              payload_candidate.scene.segments[0].id == 1 &&
              payload_candidate.scene.segments[0].effect_id == esp_pixels_fx::FX_SOLID &&
              payload_candidate.scene.segments[0].params.color[0].r == 0x00 &&
              payload_candidate.scene.segments[0].params.color[0].g == 0x40 &&
              payload_candidate.scene.segments[0].params.color[0].b == 0xFF &&
              payload_candidate.scene.segments[0].brightness == 220);
    }
    {
        // Compact representative-failure table: type/range/duplicate/
        // unsupported for consumed members, on top of the happy path above.
#define VALID_OUTPUT_JSON "[{\"slot_index\":0,\"type\":\"digital_out\",\"config\":{\"gpio\":16,\"pixel_count\":50,\"strip_type\":\"WS2812B\",\"color_order\":\"GRB\",\"reversed\":false}}]"
        struct Case { const char *json; pixels_payload::SchemaError expected; const char *why; };
        static const Case cases[] = {
            {"{\"ledmap\":null,\"outputs\":[],\"inputs\":[],\"segments\":[],\"global_brightness\":\"255\"}",
             pixels_payload::SchemaError::Malformed, "global_brightness wrong type (string)"},
            {"{\"ledmap\":null,\"outputs\":[],\"inputs\":[],\"segments\":[],\"global_brightness\":10.5}",
             pixels_payload::SchemaError::OutOfRange, "global_brightness fractional"},
            {"{\"ledmap\":null,\"outputs\":[],\"inputs\":[],\"segments\":[],\"global_brightness\":256}",
             pixels_payload::SchemaError::OutOfRange, "global_brightness out of 0..255"},
            {"{\"ledmap\":null,\"outputs\":[],\"inputs\":[],\"segments\":[],\"global_brightness\":1}",
             pixels_payload::SchemaError::OutOfRange, "config requires at least one output"},
            {"{\"ledmap\":null,\"global_brightness\":1,\"outputs\":[],\"outputs\":[],\"inputs\":[],\"segments\":[]}",
             pixels_payload::SchemaError::Malformed, "duplicate outputs member"},
            {"{\"ledmap\":{},\"outputs\":[],\"inputs\":[],\"segments\":[],\"global_brightness\":1}",
             pixels_payload::SchemaError::Unsupported, "non-null ledmap"},
            {"{\"ledmap\":null,\"outputs\":[],\"inputs\":[1],\"segments\":[],\"global_brightness\":1}",
             pixels_payload::SchemaError::Unsupported, "non-empty inputs"},
            {"{\"ledmap\":null,\"outputs\":[{\"slot_index\":0,\"type\":\"digital_out\",\"config\":{\"gpio\":16,\"pixel_count\":50,\"strip_type\":\"WS2812B\",\"color_order\":\"GRB\",\"reversed\":true}}],\"inputs\":[],\"segments\":[],\"global_brightness\":1}",
             pixels_payload::SchemaError::Unsupported, "physical reversed:true"},
            {"{\"ledmap\":null,\"outputs\":[{\"slot_index\":0,\"type\":\"digital_out\",\"config\":{\"gpio\":16,\"pixel_count\":0,\"strip_type\":\"WS2812B\",\"color_order\":\"GRB\",\"reversed\":false}}],\"inputs\":[],\"segments\":[],\"global_brightness\":1}",
             pixels_payload::SchemaError::OutOfRange, "pixel_count == 0"},
            {"{\"ledmap\":null,\"outputs\":[{\"slot_index\":0,\"type\":\"digital_out\",\"config\":{\"gpio\":16,\"pixel_count\":50,\"strip_type\":\"WS2811\",\"color_order\":\"GRB\",\"reversed\":false}}],\"inputs\":[],\"segments\":[],\"global_brightness\":1}",
             pixels_payload::SchemaError::Unsupported, "unsupported strip_type"},
            {"{\"ledmap\":null,\"outputs\":" VALID_OUTPUT_JSON ",\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":4,\"is_2d\":false,\"start_px\":0,\"stop_px\":10,\"config\":{},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"global_brightness\":1}",
             pixels_payload::SchemaError::Unsupported, "2D-only effect id on a 1D segment"},
            {"{\"ledmap\":null,\"outputs\":" VALID_OUTPUT_JSON ",\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":250,\"is_2d\":false,\"start_px\":0,\"stop_px\":10,\"config\":{},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"global_brightness\":1}",
             pixels_payload::SchemaError::Unsupported, "unregistered effect id"},
            {"{\"ledmap\":null,\"outputs\":" VALID_OUTPUT_JSON ",\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":0,\"is_2d\":true,\"start_px\":0,\"stop_px\":10,\"config\":{},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"global_brightness\":1}",
             pixels_payload::SchemaError::Unsupported, "is_2d:true"},
            {"{\"ledmap\":null,\"outputs\":" VALID_OUTPUT_JSON ",\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":0,\"is_2d\":false,\"start_px\":0,\"stop_px\":10,\"config\":{\"mirror\":false},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"global_brightness\":1}",
             pixels_payload::SchemaError::Unsupported, "present segment mirror field, even false"},
            {"{\"ledmap\":null,\"outputs\":" VALID_OUTPUT_JSON ",\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":0,\"is_2d\":false,\"start_px\":0,\"stop_px\":10,\"config\":{\"colors\":[\"0040FF\"]},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"global_brightness\":1}",
             pixels_payload::SchemaError::Malformed, "color missing leading #"},
            {"{\"ledmap\":null,\"outputs\":" VALID_OUTPUT_JSON ",\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":0,\"is_2d\":false,\"start_px\":10,\"stop_px\":5,\"config\":{},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"global_brightness\":1}",
             pixels_payload::SchemaError::OutOfRange, "start_px >= stop_px"},
            {"{\"ledmap\":null,\"outputs\":" VALID_OUTPUT_JSON ",\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":0,\"is_2d\":false,\"start_px\":0,\"stop_px\":10,\"config\":{},\"updated_at\":\"2026-09-22T13:00:00Z\"},{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":0,\"is_2d\":false,\"start_px\":10,\"stop_px\":20,\"config\":{},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"global_brightness\":1}",
             pixels_payload::SchemaError::Malformed, "duplicate segment_index"},
            {"{\"ledmap\":null,\"outputs\":" VALID_OUTPUT_JSON ",\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":2,\"is_2d\":false,\"start_px\":0,\"stop_px\":10,\"config\":{\"speed\":1,\"speed\":2},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"global_brightness\":1}",
             pixels_payload::SchemaError::Malformed, "duplicate optional speed"},
            {"{\"ledmap\":null,\"outputs\":" VALID_OUTPUT_JSON ",\"inputs\":[],\"segments\":[{\"device_id\":\"11111111-1111-4111-8111-111111111111\",\"segment_index\":0,\"effect_id\":0,\"is_2d\":false,\"start_px\":0,\"stop_px\":51,\"config\":{},\"updated_at\":\"2026-09-22T13:00:00Z\"}],\"global_brightness\":1}",
             pixels_payload::SchemaError::OutOfRange, "segment exceeds configured topology"},
            {"{\"ledmap\":null,\"outputs\":[{\"slot_index\":0,\"type\":\"digital_out\",\"config\":{\"gpio\":34,\"pixel_count\":50,\"strip_type\":\"WS2812B\",\"color_order\":\"GRB\",\"reversed\":false}}],\"inputs\":[],\"segments\":[],\"global_brightness\":1}",
             pixels_payload::SchemaError::OutOfRange, "input-only GPIO"},
        };
        for (const Case &c : cases) {
            pixels_payload::SchemaError schema_error = pixels_payload::SchemaError::None;
            const esp_err_t err = pixels_payload::parseConfigPayload(c.json, std::strlen(c.json), payload_candidate,
                                                                     schema_error);
            check(err != ESP_OK && schema_error == c.expected, __LINE__);
            if (err == ESP_OK || schema_error != c.expected) { std::printf("  case: %s\n", c.why); }
        }
#undef VALID_OUTPUT_JSON
        // A preset payload redefining outputs is rejected the same way.
        static constexpr char kPresetWithOutputs[] =
            "{\"segments\":[],\"outputs\":[{\"slot_index\":0,\"type\":\"digital_out\",\"config\":{\"gpio\":16,\"pixel_count\":50,\"strip_type\":\"WS2812B\",\"color_order\":\"GRB\",\"reversed\":false}}]}";
        pixels_payload::SchemaError schema_error = pixels_payload::SchemaError::None;
        CHECK(pixels_payload::parsePresetPayload(kPresetWithOutputs, sizeof(kPresetWithOutputs) - 1,
                                                 payload_candidate,
                                                 schema_error) == ESP_ERR_NOT_SUPPORTED &&
              schema_error == pixels_payload::SchemaError::Unsupported);
    }

    // =========================================================================
    // pixels_command_executor: end-to-end scenario table, through a REAL
    // Composer/FakeSink via ComposerBackedDurableStaging.
    // =========================================================================
    {
        static FakeSink sink;
        static esp_pixels_composer::Composer composer;
        CHECK(composer.build(sink) == ESP_OK);

        static RecordingTerminalJournal journal2;
        static ComposerBackedDurableStaging staging(composer);
        static FakePayloadSource payload_source;
        static RecordingEventSink event_sink;
        static FixedTimestampSource timestamps;
        static FakeActiveTargetProbe active_probe;
        payload_source.serveConfig(
            "https://example.invalid/api/v1/devices/11111111-1111-4111-8111-111111111111/config/13",
            FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json),
            FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json), config_digest);
        payload_source.servePreset(kPresetId, FIXTURE_BYTES(preset_payload_provisional_json),
                                   FIXTURE_SIZE(preset_payload_provisional_json), preset_digest);
        static pixels_executor::Executor executor(journal2, staging, payload_source, event_sink, timestamps,
                                                   active_probe, kDeviceId);

        // --- Happy path: apply_config -----------------------------------
        event_sink.reset();
        CHECK(executor.execute(FIXTURE_BYTES(apply_config_command_provisional_json),
                               FIXTURE_SIZE(apply_config_command_provisional_json)) == ESP_OK);
        CHECK(event_sink.recordCount() == 3); // received, accepted, applied
        CHECK(std::strstr(event_sink.record(0), "command_received") != nullptr);
        CHECK(std::strstr(event_sink.record(1), "command_accepted") != nullptr);
        CHECK(std::strstr(event_sink.record(2), "command_applied") != nullptr);
        CHECK(staging.finalizeCalls() == 1);
        CHECK(composer.renderFrame(2000) == ESP_OK); // Rainbow is time-driven; any frame proves it is live
        CHECK(staging.activateCalls() == 1);

        // --- Duplicate replay: byte-for-byte, zero payload/staging calls ---
        event_sink.reset();
        const int resolve_before = payload_source.resolveCalls();
        const int activate_before = staging.activateCalls();
        const int find_before = journal2.findCalls();
        CHECK(executor.execute(FIXTURE_BYTES(apply_config_command_provisional_json),
                               FIXTURE_SIZE(apply_config_command_provisional_json)) == ESP_OK);
        CHECK(payload_source.resolveCalls() == resolve_before && staging.activateCalls() == activate_before);
        CHECK(journal2.findCalls() == find_before + 1); // a duplicate needs one journal lookup, not two
        CHECK(event_sink.recordCount() == 1); // only the replayed terminal record, nothing else
        CHECK(std::strstr(event_sink.record(0), "command_applied") != nullptr &&
              std::strstr(event_sink.record(0), kApplyCommandId) != nullptr);

        // --- activate_preset: cache miss (real staging/activation) --------
        event_sink.reset();
        CHECK(executor.execute(FIXTURE_BYTES(activate_preset_command_provisional_json),
                               FIXTURE_SIZE(activate_preset_command_provisional_json)) == ESP_OK);
        CHECK(event_sink.recordCount() == 3 && std::strstr(event_sink.record(2), "command_applied") != nullptr);
        CHECK(composer.renderFrame(2100) == ESP_OK);
        // The preset's raw #0040FF is scaled by its own segment brightness
        // (220, see the fixture) via Composer's nscale8((channel*(scale+1))>>8)
        // before blending -- global scene brightness stays 255 (a preset
        // never sets it) so that pass is a no-op. 0x40*221>>8=55(0x37),
        // 0xFF*221>>8=220(0xDC).
        CHECK(sink.pixel(0).r == 0x00 && sink.pixel(0).g == 0x37 && sink.pixel(0).b == 0xDC);
        CHECK(staging.activateCalls() == 2);

        // --- activate_preset: already-active new command id -> no-op,
        //     never restarts the animation (activateCalls() unchanged) ---
        active_probe.setActive(kPresetId, preset_digest);
        static constexpr char kSecondActivate[] =
            "{\"command_id\":\"77777777-7777-4777-8777-777777777777\",\"type\":\"activate_preset\",\"requested_at\":\"2026-09-22T13:02:00Z\",\"requested_by\":{\"actor_type\":\"tenant-operator\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"55555555-5555-4555-8555-555555555555\",\"payload\":{\"preset_id\":\"55555555-5555-4555-8555-555555555555\",\"transition\":\"instant\",\"duration_ms\":0}}";
        event_sink.reset();
        const int activate_before2 = staging.activateCalls();
        CHECK(executor.execute(kSecondActivate, sizeof(kSecondActivate) - 1) == ESP_OK);
        CHECK(staging.activateCalls() == activate_before2); // no restart
        CHECK(event_sink.recordCount() == 3 && std::strstr(event_sink.record(0), "command_received") != nullptr &&
              std::strstr(event_sink.record(1), "command_accepted") != nullptr &&
              std::strstr(event_sink.record(2), "command_applied") != nullptr &&
              std::strstr(event_sink.record(2), "77777777-7777-4777-8777-777777777777") != nullptr);
        active_probe.clear();

        // --- Checksum mismatch: rejected before staging --------------------
        static FakePayloadSource bad_checksum_source;
        bad_checksum_source.serveConfig(
            "https://example.invalid/api/v1/devices/11111111-1111-4111-8111-111111111111/config/13",
            FIXTURE_BYTES(config_payload_bench_gpio16_50_provisional_json),
            FIXTURE_SIZE(config_payload_bench_gpio16_50_provisional_json), config_digest);
        static RecordingTerminalJournal journal3;
        static RecordingEventSink event_sink2;
        static pixels_executor::Executor bad_checksum_executor(journal3, staging, bad_checksum_source, event_sink2,
                                                                timestamps, active_probe, kDeviceId);
        static constexpr char kBadEnvelopeChecksum[] =
            "{\"command_id\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\",\"type\":\"apply_config\",\"requested_at\":\"2026-09-22T12:00:01Z\",\"requested_by\":{\"actor_type\":\"product-admin\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"config-version-13-bad\",\"payload\":{\"config_version\":13,\"config_url\":\"https://example.invalid/api/v1/devices/11111111-1111-4111-8111-111111111111/config/13\",\"checksum\":\"062868aef25369d8b00db8dbc6af34c7c82b060b5570b0b7b24859a7d5547c5a\"}}";
        const int activate_before3 = staging.activateCalls();
        CHECK(bad_checksum_executor.execute(kBadEnvelopeChecksum, sizeof(kBadEnvelopeChecksum) - 1) == ESP_OK);
        CHECK(staging.activateCalls() == activate_before3); // never staged
        CHECK(event_sink2.anyContains("checksum_mismatch") && event_sink2.anyContains("command_rejected"));

        // --- A resolver cannot authorize an out-of-bounds payload size ----
        static constexpr char kOversizedApply[] =
            "{\"command_id\":\"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb\",\"type\":\"apply_config\",\"requested_at\":\"2026-09-22T12:00:01Z\",\"requested_by\":{\"actor_type\":\"product-admin\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"config-version-13-oversized\",\"payload\":{\"config_version\":13,\"config_url\":\"https://example.invalid/api/v1/devices/11111111-1111-4111-8111-111111111111/config/13\",\"checksum\":\"b62868aef25369d8b00db8dbc6af34c7c82b060b5570b0b7b24859a7d5547c5a\"}}";
        event_sink.reset();
        payload_source.returnOversizedOnce();
        const int activate_before_oversized = staging.activateCalls();
        CHECK(executor.execute(kOversizedApply, sizeof(kOversizedApply) - 1) == ESP_OK);
        CHECK(staging.activateCalls() == activate_before_oversized &&
              event_sink.anyContains("command_failed") && event_sink.anyContains("resolve_failed"));

        // --- Staging/activation failure -> rollback, no applied -----------
        static RecordingTerminalJournal journal4;
        static RecordingEventSink event_sink3;
        static pixels_executor::Executor fail_executor(journal4, staging, payload_source, event_sink3, timestamps,
                                                        active_probe, kDeviceId);
        const esp_pixels_composer::CRGB before_pixel = sink.pixel(0);
        staging.failNextActivate();
        CHECK(fail_executor.execute(FIXTURE_BYTES(apply_config_command_provisional_json),
                                    FIXTURE_SIZE(apply_config_command_provisional_json)) == ESP_OK);
        CHECK(event_sink3.anyContains("command_failed") && event_sink3.anyContains("activation_failed"));
        CHECK(!event_sink3.anyContains("command_applied"));
        CHECK(composer.renderFrame(2200) == ESP_OK);
        CHECK(sink.pixel(0).r == before_pixel.r && sink.pixel(0).g == before_pixel.g &&
              sink.pixel(0).b == before_pixel.b); // rolled back to the prior live scene, unchanged

        // A rollback error is surfaced as the terminal reason; it is never
        // silently relabeled as an ordinary activation failure.
        static constexpr char kRollbackFailureApply[] =
            "{\"command_id\":\"cccccccc-cccc-4ccc-8ccc-cccccccccccc\",\"type\":\"apply_config\",\"requested_at\":\"2026-09-22T12:00:01Z\",\"requested_by\":{\"actor_type\":\"product-admin\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"config-version-13-rollback\",\"payload\":{\"config_version\":13,\"config_url\":\"https://example.invalid/api/v1/devices/11111111-1111-4111-8111-111111111111/config/13\",\"checksum\":\"b62868aef25369d8b00db8dbc6af34c7c82b060b5570b0b7b24859a7d5547c5a\"}}";
        event_sink3.reset();
        staging.failNextActivate();
        staging.failNextRollback();
        CHECK(fail_executor.execute(kRollbackFailureApply, sizeof(kRollbackFailureApply) - 1) == ESP_OK);
        CHECK(event_sink3.anyContains("command_failed") && event_sink3.anyContains("rollback_failed") &&
              !event_sink3.anyContains("command_applied"));

        // --- Terminal-store failure AFTER successful activation: rollback,
        //     never "applied but not saved" ------------------------------
        static RecordingTerminalJournal journal5;
        static RecordingEventSink event_sink4;
        static pixels_executor::Executor store_fail_executor(journal5, staging, payload_source, event_sink4,
                                                              timestamps, active_probe, kDeviceId);
        const esp_pixels_composer::CRGB before_store_fail = sink.pixel(0);
        journal5.failNextStore();
        CHECK(store_fail_executor.execute(FIXTURE_BYTES(apply_config_command_provisional_json),
                                          FIXTURE_SIZE(apply_config_command_provisional_json)) == ESP_OK);
        CHECK(event_sink4.anyContains("command_failed") && event_sink4.anyContains("terminal_store_failed"));
        CHECK(!event_sink4.anyContains("command_applied"));
        CHECK(composer.renderFrame(2300) == ESP_OK);
        CHECK(sink.pixel(0).r == before_store_fail.r && sink.pixel(0).g == before_store_fail.g &&
              sink.pixel(0).b == before_store_fail.b);

        // --- Terminal publish failure AFTER a successful durable apply:
        //     command stays applied, its exact result remains replayable ---
        static RecordingTerminalJournal journal6;
        static RecordingEventSink event_sink5;
        static pixels_executor::Executor publish_fail_executor(journal6, staging, payload_source, event_sink5,
                                                                timestamps, active_probe, kDeviceId);
        // Calls 1/2 are the command_received/command_accepted intermediates;
        // call 3 is the terminal command_applied publish -- fail that one
        // specifically, since intermediate-publish failures are already
        // covered by "best effort, ignored" semantics elsewhere.
        event_sink5.failAtCall(3);
        static constexpr char kThirdApply[] =
            "{\"command_id\":\"88888888-8888-4888-8888-888888888888\",\"type\":\"apply_config\",\"requested_at\":\"2026-09-22T12:05:00Z\",\"requested_by\":{\"actor_type\":\"product-admin\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"config-version-13\",\"payload\":{\"config_version\":13,\"config_url\":\"https://example.invalid/api/v1/devices/11111111-1111-4111-8111-111111111111/config/13\",\"checksum\":\"b62868aef25369d8b00db8dbc6af34c7c82b060b5570b0b7b24859a7d5547c5a\"}}";
        CHECK(publish_fail_executor.execute(kThirdApply, sizeof(kThirdApply) - 1) == ESP_OK);
        static TerminalRecord stored_after_publish_failure{};
        CHECK(journal6.find("88888888-8888-4888-8888-888888888888", stored_after_publish_failure) == ESP_OK);
        CHECK(std::strstr(stored_after_publish_failure.event_json, "command_applied") != nullptr);
        // Replayable afterward, even though the FIRST publish attempt failed.
        event_sink5.reset();
        CHECK(publish_fail_executor.execute(kThirdApply, sizeof(kThirdApply) - 1) == ESP_OK);
        CHECK(event_sink5.recordCount() == 1 &&
              std::strstr(event_sink5.record(0), "command_applied") != nullptr);

        // --- reboot/ota: parsed, never executed ----------------------------
        static RecordingTerminalJournal journal7;
        static RecordingEventSink event_sink6;
        static pixels_executor::Executor reboot_executor(journal7, staging, payload_source, event_sink6, timestamps,
                                                          active_probe, kDeviceId);
        static constexpr char kReboot[] =
            "{\"command_id\":\"99999999-9999-4999-8999-999999999999\",\"type\":\"reboot\",\"requested_at\":\"2026-09-22T12:06:00Z\",\"requested_by\":{\"actor_type\":\"tenant-operator\",\"actor_id\":\"33333333-3333-4333-8333-333333333333\"},\"idempotency_key\":\"x\",\"payload\":{\"reason\":\"maintenance\"}}";
        const int activate_before4 = staging.activateCalls();
        CHECK(reboot_executor.execute(kReboot, sizeof(kReboot) - 1) == ESP_OK);
        CHECK(staging.activateCalls() == activate_before4);
        CHECK(event_sink6.anyContains("command_rejected") && event_sink6.anyContains("not_implemented"));

        // --- No assertion/panic/leak across repeated commands --------------
        for (int i = 0; i < 20; ++i) {
            event_sink.reset();
            CHECK(executor.execute(FIXTURE_BYTES(apply_config_command_provisional_json),
                                   FIXTURE_SIZE(apply_config_command_provisional_json)) == ESP_OK);
        }
        CHECK(composer.renderFrame(3000) == ESP_OK);

        CHECK(composer.end() == ESP_OK);
    }

    for (;;) {
        std::printf("NETWORK_CONTRACT_VALIDATION checks=%d failures=%d RESULT=%s\n",
                    checks, failures, failures == 0 ? "ALL_PASS" : "FAIL");
        std::fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
