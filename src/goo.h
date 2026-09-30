#pragma once
#include <Arduino.h>

// Metadata of an ELEGOO .goo (V3.0) print file, plus a table of layer
// offsets so a read position in the file can be turned into a layer number.
namespace goo {

constexpr uint32_t kSmallPreviewOffset = 194;   // 116 x 116 RGB565, big-endian
constexpr uint32_t kSmallPreviewSize = 116 * 116 * 2;
constexpr uint32_t kBigPreviewOffset = 27108;   // 290 x 290 RGB565
constexpr uint32_t kBigPreviewSize = 290 * 290 * 2;

struct Info {
  bool valid = false;
  String version, software, softwareVersion, created, machine;
  uint32_t layers = 0;
  uint16_t resX = 0, resY = 0;
  float layerHeight = 0, exposure = 0;
  uint32_t printTime = 0;  // seconds
  float volumeMl = 0, grams = 0;
  uint32_t layerTable = 0;
};

void reset(uint32_t fileSize);

// Parses the header and walks the layer table as far as the data is
// available (streamed files fill in as the cache does). Call from loop().
void poll();

const Info& info();
uint32_t knownLayers();  // layer offsets found so far
bool complete();         // header parsed (or not a .goo) and table walked
int32_t layerAt(uint32_t fileOffset);  // -1 if unknown / before layer 0

}  // namespace goo
