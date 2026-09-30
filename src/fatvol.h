#pragma once
#include <Arduino.h>

// A synthetic FAT32 volume holding exactly one file. Boot sector, FSInfo,
// FATs and the root directory are generated on the fly; file data comes
// from source::read(). Host writes go to a small RAM overlay and are lost
// on the next configure().
namespace fatvol {

constexpr uint32_t kSectorSize = 512;

void begin();
void configure(const String& name, uint32_t size, uint32_t mtime);  // empty name = no file
uint32_t sectorCount();
uint32_t maxFileSize();

// Returns bytes produced, 0 if streamed data is not ready yet, -1 on error.
int32_t read(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t len);
int32_t write(uint32_t lba, uint32_t offset, const uint8_t* buf, uint32_t len);

enum class Region : uint8_t { Boot, FsInfo, Reserved, Fat, RootDir, File, Free, Overlay };
const char* regionName(Region r);
Region classify(uint32_t lba, uint32_t* fileOffset);

struct Layout {
  uint32_t totalSectors, clusterBytes, reservedSectors, fatSectors, dataStart, clusters;
  uint32_t overlaySectors;
};
Layout layout();

}  // namespace fatvol
