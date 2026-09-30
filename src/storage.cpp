#include "storage.h"

#include <esp_partition.h>

namespace storage {
namespace {

constexpr uint32_t kFlashBlock = 65536;
constexpr uint32_t kFlashSector = 4096;

const esp_partition_t* s_part = nullptr;
uint32_t s_pos = 0;
uint32_t s_erasedTo = 0;
bool s_writing = false;

}  // namespace

bool begin() {
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "drive");
  if (!s_part) log_e("partition 'drive' not found");
  return s_part != nullptr;
}

uint32_t capacity() { return s_part ? s_part->size : 0; }

bool beginWrite() {
  if (!s_part) return false;
  s_pos = 0;
  s_erasedTo = 0;
  s_writing = true;
  return true;
}

bool write(const uint8_t* data, size_t len) {
  if (!s_writing || s_pos + len > s_part->size) return false;
  while (s_erasedTo < s_pos + len) {
    bool whole = s_erasedTo % kFlashBlock == 0 && s_erasedTo + kFlashBlock <= s_part->size;
    uint32_t n = whole ? kFlashBlock : kFlashSector;
    if (esp_partition_erase_range(s_part, s_erasedTo, n) != ESP_OK) return false;
    s_erasedTo += n;
  }
  if (esp_partition_write(s_part, s_pos, data, len) != ESP_OK) return false;
  s_pos += len;
  return true;
}

uint32_t endWrite() {
  s_writing = false;
  return s_pos;
}

bool read(uint32_t offset, void* buf, uint32_t len) {
  if (!s_part || offset + len > s_part->size) return false;
  return esp_partition_read(s_part, offset, buf, len) == ESP_OK;
}

}  // namespace storage
