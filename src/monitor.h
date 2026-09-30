#pragma once
#include <Arduino.h>

// Activity log and statistics: what the printer reads (coalesced into
// ranges and mapped to FAT regions / file offsets / layers), USB and
// network events, errors.
namespace monitor {

enum class Kind : uint8_t { Info, Usb, Read, Write, File, Error };

void begin();
void logf(Kind kind, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Called from the USB task.
void onHostRead(uint32_t lba, uint32_t len);
void onHostWrite(uint32_t lba, uint32_t len);

void onFileChanged();

String eventsJson(uint32_t since);  // events with id >= since (the last may have grown)
String progressJson();
String mapJson(uint32_t buckets);   // per bucket: 0 none, 1 available, 2 read, 3 both

}  // namespace monitor
