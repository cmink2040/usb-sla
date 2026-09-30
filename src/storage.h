#pragma once
#include <Arduino.h>

// The "drive" flash partition, holding one uploaded file as raw bytes from
// offset 0. The FAT volume around it is synthesized (see fatvol).
namespace storage {

bool begin();
uint32_t capacity();

// Sequential write of a new file, erasing ahead as it goes.
bool beginWrite();
bool write(const uint8_t* data, size_t len);
uint32_t endWrite();  // returns bytes written

bool read(uint32_t offset, void* buf, uint32_t len);

}  // namespace storage
