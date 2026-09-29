#include "pixels_durable_storage.hpp"

#include <cstring>

#include "nvs_flash.h"

namespace pixels_storage {
namespace {

constexpr uint32_t kJournalMagic = 0x4a434c45; // ELCJ, little-endian
constexpr uint32_t kStagingMagic = 0x53544c45; // ELTS, little-endian
constexpr uint8_t kSchema = 1;
constexpr const char *kJournalNamespace = "easyled_journal";
constexpr const char *kStagingNamespace = "easyled_stage";
constexpr const char *kPendingKey = "pending";
constexpr const char *kActiveKeys[] = {"active_a", "active_b"};

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

void put16(uint8_t *out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t *out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) { out[i] = static_cast<uint8_t>(value >> (i * 8)); }
}

void put64(uint8_t *out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) { out[i] = static_cast<uint8_t>(value >> (i * 8)); }
}

uint16_t get16(const uint8_t *in)
{
    return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8);
}

uint32_t get32(const uint8_t *in)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) { value |= static_cast<uint32_t>(in[i]) << (i * 8); }
    return value;
}

uint64_t get64(const uint8_t *in)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) { value |= static_cast<uint64_t>(in[i]) << (i * 8); }
    return value;
}

bool exactString(const char *text, size_t length)
{
    return text != nullptr && std::strlen(text) == length;
}

void journalKey(size_t slot, char key[5])
{
    key[0] = 'j';
    key[1] = static_cast<char>('0' + slot);
    key[2] = '\0';
}

esp_err_t initPartition()
{
    // Product storage is never erased automatically. A layout/version error
    // must be surfaced so the caller can preserve evidence and choose policy.
    return nvs_flash_init_partition(kPartitionLabel);
}

size_t encodeJournal(uint8_t *out, size_t capacity, uint64_t sequence,
                     const pixels_firmware::TerminalRecord &record)
{
    constexpr size_t kFixed = 4 + 1 + 1 + 2 + 8 + 37 + 2;
    if (capacity < kFixed + record.event_size + 1 + 4 ||
        !exactString(record.command_id, pixels_firmware::kUuidBytes) ||
        record.event_size == 0 || record.event_size >= pixels_firmware::kMaxEventBytes ||
        record.event_json[record.event_size] != '\0') {
        return 0;
    }
    size_t at = 0;
    put32(out + at, kJournalMagic); at += 4;
    out[at++] = kSchema;
    out[at++] = 0;
    put16(out + at, record.event_size); at += 2;
    put64(out + at, sequence); at += 8;
    std::memcpy(out + at, record.command_id, 37); at += 37;
    put16(out + at, record.event_size); at += 2;
    std::memcpy(out + at, record.event_json, record.event_size + 1); at += record.event_size + 1;
    put32(out + at, crc32(out, at)); at += 4;
    return at;
}

esp_err_t decodeJournal(const uint8_t *bytes, size_t size, uint64_t &sequence,
                        pixels_firmware::TerminalRecord &record)
{
    constexpr size_t kFixed = 4 + 1 + 1 + 2 + 8 + 37 + 2;
    if (bytes == nullptr || size < kFixed + 1 + 4 || get32(bytes) != kJournalMagic ||
        bytes[4] != kSchema || bytes[5] != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint16_t declared = get16(bytes + 6);
    const uint16_t event_size = get16(bytes + 53);
    const size_t expected = kFixed + static_cast<size_t>(event_size) + 1 + 4;
    if (declared != event_size || event_size == 0 || event_size >= pixels_firmware::kMaxEventBytes ||
        size != expected || bytes[kFixed + event_size] != '\0' ||
        get32(bytes + size - 4) != crc32(bytes, size - 4)) {
        return ESP_ERR_INVALID_CRC;
    }
    pixels_firmware::TerminalRecord decoded{};
    std::memcpy(decoded.command_id, bytes + 16, 37);
    if (!exactString(decoded.command_id, pixels_firmware::kUuidBytes)) { return ESP_ERR_INVALID_RESPONSE; }
    decoded.event_size = event_size;
    std::memcpy(decoded.event_json, bytes + kFixed, event_size + 1);
    sequence = get64(bytes + 8);
    if (sequence == 0 || sequence == UINT64_MAX) { return ESP_ERR_INVALID_RESPONSE; }
    record = decoded;
    return ESP_OK;
}

size_t encodeStaging(uint8_t *out, size_t capacity, uint64_t sequence,
                     const pixels_firmware::Command &command, const uint8_t checksum[32],
                     const char *payload, size_t payload_size)
{
    constexpr size_t kHeader = 130;
    if (out == nullptr || checksum == nullptr || payload == nullptr || payload_size == 0 ||
        payload_size > 8192 || capacity < kHeader + payload_size + 4 || sequence == 0 ||
        sequence == UINT64_MAX ||
        !exactString(command.command_id, pixels_firmware::kUuidBytes) ||
        (command.type == pixels_firmware::CommandType::ActivatePreset &&
         !exactString(command.preset_id, pixels_firmware::kUuidBytes)) ||
        (command.type != pixels_firmware::CommandType::ApplyConfig &&
         command.type != pixels_firmware::CommandType::ActivatePreset)) {
        return 0;
    }
    std::memset(out, 0, kHeader);
    put32(out, kStagingMagic);
    out[4] = kSchema;
    out[5] = static_cast<uint8_t>(command.type);
    put16(out + 6, static_cast<uint16_t>(kHeader));
    put64(out + 8, sequence);
    std::memcpy(out + 16, command.command_id, 37);
    if (command.type == pixels_firmware::CommandType::ActivatePreset) {
        std::memcpy(out + 53, command.preset_id, 37);
    }
    put32(out + 90, command.config_version);
    std::memcpy(out + 94, checksum, 32);
    put32(out + 126, static_cast<uint32_t>(payload_size));
    if (out + kHeader != reinterpret_cast<const uint8_t *>(payload)) {
        std::memcpy(out + kHeader, payload, payload_size);
    }
    put32(out + kHeader + payload_size, crc32(out, kHeader + payload_size));
    return kHeader + payload_size + 4;
}

esp_err_t decodeStaging(const uint8_t *bytes, size_t size, pixels_firmware::CommandType &type,
                        const char *&payload, size_t &payload_size, uint64_t &sequence,
                        char preset_id[37], uint8_t checksum[32])
{
    constexpr size_t kHeader = 130;
    if (bytes == nullptr || size < kHeader + 1 + 4 || get32(bytes) != kStagingMagic ||
        bytes[4] != kSchema || get16(bytes + 6) != kHeader) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const auto decoded_type = static_cast<pixels_firmware::CommandType>(bytes[5]);
    if (decoded_type != pixels_firmware::CommandType::ApplyConfig &&
        decoded_type != pixels_firmware::CommandType::ActivatePreset) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t declared = get32(bytes + 126);
    if (declared == 0 || declared > 8192 || size != kHeader + static_cast<size_t>(declared) + 4 ||
        get32(bytes + size - 4) != crc32(bytes, size - 4)) {
        return ESP_ERR_INVALID_CRC;
    }
    sequence = get64(bytes + 8);
    if (sequence == 0 || sequence == UINT64_MAX) { return ESP_ERR_INVALID_RESPONSE; }
    char command_id[37]{};
    std::memcpy(command_id, bytes + 16, sizeof(command_id));
    if (!exactString(command_id, pixels_firmware::kUuidBytes)) { return ESP_ERR_INVALID_RESPONSE; }
    std::memcpy(preset_id, bytes + 53, 37);
    if (decoded_type == pixels_firmware::CommandType::ActivatePreset &&
        !exactString(preset_id, pixels_firmware::kUuidBytes)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    std::memcpy(checksum, bytes + 94, 32);
    type = decoded_type;
    payload = reinterpret_cast<const char *>(bytes + kHeader);
    payload_size = declared;
    return ESP_OK;
}

} // namespace

NvsTerminalJournal::~NvsTerminalJournal() { end(); }

esp_err_t NvsTerminalJournal::init()
{
    if (initialized_) { return ESP_ERR_INVALID_STATE; }
    esp_err_t err = initPartition();
    if (err != ESP_OK) { return err; }
    err = nvs_open_from_partition(kPartitionLabel, kJournalNamespace, NVS_READWRITE, &handle_);
    if (err != ESP_OK) { return err; }
    initialized_ = true;
    next_sequence_ = 1;
    std::memset(occupied_, 0, sizeof(occupied_));
    std::memset(sequences_, 0, sizeof(sequences_));
    std::memset(command_ids_, 0, sizeof(command_ids_));
    for (size_t slot = 0; slot < kTerminalCapacity; ++slot) {
        char key[5]{};
        journalKey(slot, key);
        size_t size = sizeof(scratch_);
        err = nvs_get_blob(handle_, key, scratch_, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND) { continue; }
        if (err != ESP_OK) { end(); return err; }
        pixels_firmware::TerminalRecord record{};
        uint64_t sequence = 0;
        const esp_err_t decoded = decodeJournal(scratch_, size, sequence, record);
        if (decoded != ESP_OK) { end(); return decoded; }
        occupied_[slot] = true;
        sequences_[slot] = sequence;
        std::memcpy(command_ids_[slot], record.command_id, sizeof(record.command_id));
        if (sequence >= next_sequence_ && sequence != UINT64_MAX) { next_sequence_ = sequence + 1; }
    }
    return ESP_OK;
}

void NvsTerminalJournal::end()
{
    if (handle_ != 0) { nvs_close(handle_); }
    handle_ = 0;
    initialized_ = false;
}

esp_err_t NvsTerminalJournal::find(const char *command_id,
                                   pixels_firmware::TerminalRecord &record) const
{
    if (!initialized_ || handle_ == 0) { return ESP_ERR_INVALID_STATE; }
    if (!exactString(command_id, pixels_firmware::kUuidBytes)) { return ESP_ERR_INVALID_ARG; }
    for (size_t slot = 0; slot < kTerminalCapacity; ++slot) {
        if (!occupied_[slot] || std::strcmp(command_id, command_ids_[slot]) != 0) { continue; }
        char key[5]{};
        journalKey(slot, key);
        size_t size = sizeof(scratch_);
        esp_err_t err = nvs_get_blob(handle_, key, scratch_, &size);
        if (err != ESP_OK) { return err; }
        uint64_t sequence = 0;
        err = decodeJournal(scratch_, size, sequence, record);
        if (err != ESP_OK || sequence != sequences_[slot] ||
            std::strcmp(command_id, record.command_id) != 0) {
            return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t NvsTerminalJournal::store(const pixels_firmware::TerminalRecord &record)
{
    if (!initialized_ || handle_ == 0 || next_sequence_ == 0) { return ESP_ERR_INVALID_STATE; }
    pixels_firmware::TerminalRecord existing{};
    const esp_err_t found = find(record.command_id, existing);
    if (found == ESP_OK) {
        return existing.event_size == record.event_size &&
               std::memcmp(existing.event_json, record.event_json, record.event_size + 1) == 0
            ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (found != ESP_ERR_NOT_FOUND) { return found; }

    size_t target = kTerminalCapacity;
    uint64_t oldest = UINT64_MAX;
    for (size_t slot = 0; slot < kTerminalCapacity; ++slot) {
        if (!occupied_[slot]) { target = slot; break; }
        if (sequences_[slot] < oldest) { oldest = sequences_[slot]; target = slot; }
    }
    if (target >= kTerminalCapacity) { return ESP_ERR_INVALID_STATE; }
    const size_t encoded = encodeJournal(scratch_, sizeof(scratch_), next_sequence_, record);
    if (encoded == 0) { return ESP_ERR_INVALID_ARG; }
    char key[5]{};
    journalKey(target, key);
    const uint32_t expected_crc = crc32(scratch_, encoded);
    esp_err_t err = nvs_set_blob(handle_, key, scratch_, encoded);
    if (err == ESP_OK) { err = nvs_commit(handle_); }
    if (err != ESP_OK) { return err; }
    size_t verify_size = sizeof(scratch_);
    err = nvs_get_blob(handle_, key, scratch_, &verify_size);
    if (err != ESP_OK) { return err; }
    if (verify_size != encoded || crc32(scratch_, verify_size) != expected_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    occupied_[target] = true;
    sequences_[target] = next_sequence_++;
    std::memcpy(command_ids_[target], record.command_id, sizeof(record.command_id));
    return ESP_OK;
}

NvsDurableStaging::~NvsDurableStaging() { end(); }

esp_err_t NvsDurableStaging::eraseKey(const char *key)
{
    const esp_err_t err = nvs_erase_key(handle_, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t NvsDurableStaging::commitAndVerify(const char *key, const uint8_t *bytes, size_t size)
{
    const uint32_t expected_crc = crc32(bytes, size);
    esp_err_t err = nvs_set_blob(handle_, key, bytes, size);
    if (err == ESP_OK) { err = nvs_commit(handle_); }
    if (err != ESP_OK) { return err; }
    size_t verify_size = sizeof(record_);
    err = nvs_get_blob(handle_, key, record_, &verify_size);
    if (err != ESP_OK) { return err; }
    return verify_size == size && crc32(record_, verify_size) == expected_crc
        ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t NvsDurableStaging::loadSlot(int slot, pixels_firmware::CommandType &type,
                                      const char *&payload, size_t &size, uint64_t &sequence,
                                      char preset_id[37], uint8_t checksum[32])
{
    if (slot < 0 || slot > 1) { return ESP_ERR_INVALID_ARG; }
    size_t record_size = sizeof(record_);
    esp_err_t err = nvs_get_blob(handle_, kActiveKeys[slot], record_, &record_size);
    if (err != ESP_OK) { return err; }
    return decodeStaging(record_, record_size, type, payload, size, sequence, preset_id, checksum);
}

esp_err_t NvsDurableStaging::scanActive()
{
    active_slot_ = -1;
    active_sequence_ = 0;
    std::memset(active_preset_id_, 0, sizeof(active_preset_id_));
    std::memset(active_checksum_, 0, sizeof(active_checksum_));
    esp_err_t last_error = ESP_ERR_NOT_FOUND;
    for (int slot = 0; slot < 2; ++slot) {
        pixels_firmware::CommandType type{};
        const char *payload = nullptr;
        size_t size = 0;
        uint64_t sequence = 0;
        char preset_id[37]{};
        uint8_t checksum[32]{};
        const esp_err_t err = loadSlot(slot, type, payload, size, sequence, preset_id, checksum);
        if (err == ESP_ERR_NVS_NOT_FOUND) { continue; }
        if (err != ESP_OK) { last_error = err; continue; }
        if (sequence <= active_sequence_) { continue; }
        active_slot_ = slot;
        active_sequence_ = sequence;
        active_type_ = type;
        std::memcpy(active_preset_id_, preset_id, sizeof(active_preset_id_));
        std::memcpy(active_checksum_, checksum, sizeof(active_checksum_));
    }
    return active_slot_ >= 0 ? ESP_OK : last_error;
}

esp_err_t NvsDurableStaging::init(ActivationTarget &target,
                                  const pixels_firmware::TerminalJournal &journal)
{
    if (initialized_) { return ESP_ERR_INVALID_STATE; }
    esp_err_t err = initPartition();
    if (err != ESP_OK) { return err; }
    err = nvs_open_from_partition(kPartitionLabel, kStagingNamespace, NVS_READWRITE, &handle_);
    if (err != ESP_OK) { return err; }
    initialized_ = true;
    target_ = &target;
    journal_ = &journal;
    resetTransaction();

    bool has_pending = false;
    uint64_t pending_sequence = 0;
    char pending_command_id[37]{};
    size_t pending_size = sizeof(record_);
    err = nvs_get_blob(handle_, kPendingKey, record_, &pending_size);
    if (err == ESP_OK) {
        pixels_firmware::CommandType pending_type{};
        const char *pending_payload = nullptr;
        size_t pending_payload_size = 0;
        char pending_preset_id[37]{};
        uint8_t pending_checksum[32]{};
        err = decodeStaging(record_, pending_size, pending_type, pending_payload,
                            pending_payload_size, pending_sequence,
                            pending_preset_id, pending_checksum);
        if (err != ESP_OK) { end(); return err; }
        std::memcpy(pending_command_id, record_ + 16, sizeof(pending_command_id));
        has_pending = true;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        end();
        return err;
    }

    esp_err_t active_result = scanActive();
    if (has_pending) {
        terminal_scratch_ = pixels_firmware::TerminalRecord{};
        const esp_err_t terminal = journal_->find(pending_command_id, terminal_scratch_);
        const bool terminal_applied = terminal == ESP_OK &&
            std::strstr(terminal_scratch_.event_json,
                        "\"event_type\":\"command_applied\"") != nullptr;
        if (terminal_applied) {
            // The executor durably stored command_applied before finalization.
            // The promoted record must therefore be the selected newest slot.
            if (active_result != ESP_OK || active_sequence_ != pending_sequence) {
                end();
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else if (terminal == ESP_OK || terminal == ESP_ERR_NOT_FOUND) {
            // Power failed before terminal success became durable. Remove a
            // promoted candidate, discard pending, then recover the prior slot.
            if (active_result == ESP_OK && active_sequence_ > pending_sequence) {
                end();
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (active_result == ESP_OK && active_sequence_ == pending_sequence) {
                err = eraseKey(kActiveKeys[active_slot_]);
                if (err != ESP_OK) { end(); return err; }
            }
            err = eraseKey(kPendingKey);
            if (err == ESP_OK) { err = nvs_commit(handle_); }
            if (err != ESP_OK) { end(); return err; }
            active_result = scanActive();
            if (active_result == ESP_ERR_NOT_FOUND) {
                err = target_->clearAtFrameBoundary();
                if (err != ESP_OK) { end(); return err; }
            }
        } else {
            end();
            return terminal;
        }
    }

    if (active_result == ESP_OK) {
        pixels_firmware::CommandType type{};
        const char *payload = nullptr;
        size_t size = 0;
        uint64_t sequence = 0;
        char preset_id[37]{};
        uint8_t checksum[32]{};
        err = loadSlot(active_slot_, type, payload, size, sequence, preset_id, checksum);
        if (err != ESP_OK) { end(); return err; }
        err = target_->activateAtFrameBoundary(type, payload, size);
        if (err != ESP_OK) { end(); return err; }
        const int old_slot = active_slot_ == 0 ? 1 : 0;
        err = eraseKey(kActiveKeys[old_slot]);
        if (err != ESP_OK) { end(); return err; }
    } else if (active_result != ESP_ERR_NOT_FOUND) {
        end();
        return active_result;
    }
    err = eraseKey(kPendingKey);
    if (err == ESP_OK) { err = nvs_commit(handle_); }
    if (err != ESP_OK) { end(); return err; }
    return active_result;
}

void NvsDurableStaging::end()
{
    if (handle_ != 0) { nvs_close(handle_); }
    handle_ = 0;
    initialized_ = false;
    target_ = nullptr;
    journal_ = nullptr;
    active_slot_ = -1;
    active_sequence_ = 0;
    resetTransaction();
}

bool NvsDurableStaging::activePresetMatches(const char *preset_id, const uint8_t checksum[32]) const
{
    return initialized_ && active_slot_ >= 0 &&
           active_type_ == pixels_firmware::CommandType::ActivatePreset &&
           exactString(preset_id, pixels_firmware::kUuidBytes) && checksum != nullptr &&
           std::strcmp(preset_id, active_preset_id_) == 0 &&
           std::memcmp(checksum, active_checksum_, sizeof(active_checksum_)) == 0;
}

void NvsDurableStaging::resetTransaction()
{
    command_ = pixels_firmware::Command{};
    expected_size_ = 0;
    staged_size_ = 0;
    std::memset(staged_checksum_, 0, sizeof(staged_checksum_));
    begun_ = false;
    committed_ = false;
    live_applied_ = false;
    prior_slot_ = -1;
    transaction_slot_ = -1;
}

esp_err_t NvsDurableStaging::finalizePrevious()
{
    if (!live_applied_) {
        const esp_err_t pending = eraseKey(kPendingKey);
        if (pending == ESP_OK) { return nvs_commit(handle_); }
        return pending;
    }
    esp_err_t err = eraseKey(kPendingKey);
    if (err == ESP_OK && prior_slot_ >= 0 && prior_slot_ != active_slot_) {
        err = eraseKey(kActiveKeys[prior_slot_]);
    }
    if (err == ESP_OK) { err = nvs_commit(handle_); }
    if (err == ESP_OK) { resetTransaction(); }
    return err;
}

esp_err_t NvsDurableStaging::begin(const pixels_firmware::Command &command, size_t total_size)
{
    if (!initialized_ || handle_ == 0 || target_ == nullptr) { return ESP_ERR_INVALID_STATE; }
    if (total_size == 0 || total_size > kMaxPayloadBytes ||
        (command.type != pixels_firmware::CommandType::ApplyConfig &&
         command.type != pixels_firmware::CommandType::ActivatePreset)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = finalizePrevious();
    if (err != ESP_OK) { return err; }
    command_ = command;
    expected_size_ = total_size;
    begun_ = true;
    prior_slot_ = active_slot_;
    return ESP_OK;
}

esp_err_t NvsDurableStaging::append(const void *bytes, size_t size)
{
    if (!begun_ || committed_) { return ESP_ERR_INVALID_STATE; }
    if ((bytes == nullptr && size != 0) || size > expected_size_ - staged_size_) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (size != 0) { std::memcpy(record_ + kRecordHeaderBytes + staged_size_, bytes, size); }
    staged_size_ += size;
    return ESP_OK;
}

esp_err_t NvsDurableStaging::verifyAndCommit(const uint8_t expected[32])
{
    if (!begun_ || committed_ || expected == nullptr || staged_size_ != expected_size_ ||
        active_sequence_ == UINT64_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *staged_payload = reinterpret_cast<const char *>(record_ + kRecordHeaderBytes);
    esp_err_t err = pixels_firmware::verifySha256(staged_payload, staged_size_, expected);
    if (err != ESP_OK) { return err; }
    const size_t encoded = encodeStaging(record_, sizeof(record_), active_sequence_ + 1,
                                         command_, expected, staged_payload, staged_size_);
    if (encoded == 0) { return ESP_ERR_INVALID_ARG; }
    // Readback overwrites the same member buffer. Preserve a checksum of the
    // complete encoded record so verification needs no second 8 KiB copy.
    const uint32_t expected_record_crc = crc32(record_, encoded);
    err = nvs_set_blob(handle_, kPendingKey, record_, encoded);
    if (err == ESP_OK) { err = nvs_commit(handle_); }
    if (err != ESP_OK) { return err; }
    size_t read_size = sizeof(record_);
    err = nvs_get_blob(handle_, kPendingKey, record_, &read_size);
    if (err != ESP_OK) { return err; }
    if (read_size != encoded || crc32(record_, read_size) != expected_record_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    pixels_firmware::CommandType type{};
    const char *payload = nullptr;
    size_t payload_size = 0;
    uint64_t sequence = 0;
    char preset_id[37]{};
    uint8_t checksum[32]{};
    err = decodeStaging(record_, read_size, type, payload, payload_size, sequence, preset_id, checksum);
    if (err != ESP_OK || sequence != active_sequence_ + 1 || type != command_.type ||
        payload_size != staged_size_ ||
        std::memcmp(checksum, expected, 32) != 0) {
        return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
    }
    std::memcpy(staged_checksum_, expected, 32);
    committed_ = true;
    return ESP_OK;
}

esp_err_t NvsDurableStaging::activateAtFrameBoundary()
{
    if (!committed_ || live_applied_ || target_ == nullptr) { return ESP_ERR_INVALID_STATE; }
    size_t record_size = sizeof(record_);
    esp_err_t err = nvs_get_blob(handle_, kPendingKey, record_, &record_size);
    if (err != ESP_OK) { return err; }
    pixels_firmware::CommandType type{};
    const char *payload = nullptr;
    size_t payload_size = 0;
    uint64_t sequence = 0;
    char preset_id[37]{};
    uint8_t checksum[32]{};
    err = decodeStaging(record_, record_size, type, payload, payload_size, sequence, preset_id, checksum);
    if (err != ESP_OK || sequence != active_sequence_ + 1 ||
        std::memcmp(checksum, staged_checksum_, 32) != 0) {
        return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
    }
    err = target_->activateAtFrameBoundary(type, payload, payload_size);
    if (err != ESP_OK) { return err; }
    live_applied_ = true;
    transaction_slot_ = active_slot_ == 0 ? 1 : 0;
    // `record_` still contains the exact pending record just activated.
    err = commitAndVerify(kActiveKeys[transaction_slot_], record_, record_size);
    if (err != ESP_OK) { return err; }
    active_slot_ = transaction_slot_;
    active_sequence_ = sequence;
    active_type_ = type;
    std::memcpy(active_preset_id_, preset_id, sizeof(active_preset_id_));
    std::memcpy(active_checksum_, checksum, sizeof(active_checksum_));
    return ESP_OK;
}

esp_err_t NvsDurableStaging::finalize()
{
    if (!initialized_ || !live_applied_) { return ESP_ERR_INVALID_STATE; }
    return finalizePrevious();
}

esp_err_t NvsDurableStaging::rollback()
{
    if (!initialized_ || handle_ == 0 || target_ == nullptr) { return ESP_ERR_INVALID_STATE; }
    if (live_applied_) {
        esp_err_t err = ESP_OK;
        if (prior_slot_ >= 0) {
            pixels_firmware::CommandType type{};
            const char *payload = nullptr;
            size_t size = 0;
            uint64_t sequence = 0;
            char preset_id[37]{};
            uint8_t checksum[32]{};
            err = loadSlot(prior_slot_, type, payload, size, sequence, preset_id, checksum);
            if (err == ESP_OK) { err = target_->activateAtFrameBoundary(type, payload, size); }
            if (err != ESP_OK) { return err; }
        } else {
            err = target_->clearAtFrameBoundary();
            if (err != ESP_OK) { return err; }
        }
        if (transaction_slot_ >= 0) {
            err = eraseKey(kActiveKeys[transaction_slot_]);
            if (err != ESP_OK) { return err; }
        }
        active_slot_ = prior_slot_;
        const esp_err_t scan = scanActive();
        if (active_slot_ < 0 && scan != ESP_ERR_NOT_FOUND) { return scan; }
    }
    esp_err_t err = eraseKey(kPendingKey);
    if (err == ESP_OK) { err = nvs_commit(handle_); }
    if (err == ESP_OK) { resetTransaction(); }
    return err;
}

} // namespace pixels_storage
