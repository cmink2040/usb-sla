#include "goo.h"

#include "monitor.h"
#include "source.h"

namespace goo {
namespace {

// Fixed V3.0 header layout (big-endian).
constexpr uint32_t kStringsSize = 188;
constexpr uint32_t kNumbersOffset = 195310;  // after both previews
constexpr uint32_t kHeaderEnd = 195474;
constexpr uint32_t kPrintTimeOffset = 195446;
constexpr uint32_t kLayerHeaderSize = 70;    // per-layer settings + data size
constexpr uint32_t kEndingSize = 11;
constexpr uint32_t kLayersPerPoll = 256;

Info s_info;
uint32_t s_size = 0;
uint32_t* s_offsets = nullptr;
volatile uint32_t s_known = 0;
uint32_t s_next = 0;  // file offset of the next layer to find
bool s_done = true;

uint32_t be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
float beFloat(const uint8_t* p) {
  uint32_t v = be32(p);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
String cstr(const uint8_t* p, size_t n) {
  String s;
  for (size_t i = 0; i < n && p[i]; i++) s += (char)p[i];
  return s;
}

// Returns false while the data is not available yet.
bool parseHeader() {
  uint8_t a[kStringsSize], b[kHeaderEnd - kNumbersOffset];
  source::ReadResult r = source::read(0, a, sizeof a, source::Mode::Peek);
  if (r == source::ReadResult::Ok) r = source::read(kNumbersOffset, b, sizeof b, source::Mode::Peek);
  if (r == source::ReadResult::NotReady) return false;
  if (r == source::ReadResult::Error || memcmp(a + 4, "\x07\0\0\0DLP\0", 8) != 0) {
    s_done = true;  // not a .goo file
    return true;
  }
  Info in;
  in.version = cstr(a, 4);
  in.software = cstr(a + 12, 32);
  in.softwareVersion = cstr(a + 44, 24);
  in.created = cstr(a + 68, 24);
  in.machine = cstr(a + 92, 32);
  in.layers = be32(b);
  in.resX = be16(b + 4);
  in.resY = be16(b + 6);
  in.layerHeight = beFloat(b + 22);
  in.exposure = beFloat(b + 26);
  const uint8_t* t = b + (kPrintTimeOffset - kNumbersOffset);
  in.printTime = be32(t);
  in.volumeMl = beFloat(t + 4) / 1000.0f;
  in.grams = beFloat(t + 8);
  in.layerTable = be32(t + 24);
  if (in.layers == 0 || in.layers > 200000 || in.layerTable < kHeaderEnd || in.layerTable >= s_size) {
    monitor::logf(monitor::Kind::Error, "goo: header looks invalid");
    s_done = true;
    return true;
  }
  in.valid = true;
  s_info = in;
  s_offsets = static_cast<uint32_t*>(heap_caps_malloc(in.layers * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_offsets) s_offsets = static_cast<uint32_t*>(malloc(in.layers * 4));
  s_next = in.layerTable;
  monitor::logf(monitor::Kind::Info, "goo: %u layers, %.3f mm, %.1f s exposure, %s", in.layers, in.layerHeight,
                in.exposure, in.machine.c_str());
  return true;
}

}  // namespace

void reset(uint32_t fileSize) {
  s_known = 0;
  free(s_offsets);
  s_offsets = nullptr;
  s_info = Info();
  s_size = fileSize;
  s_next = 0;
  s_done = fileSize < kHeaderEnd + kEndingSize;
}

void poll() {
  if (s_done) return;
  if (!s_info.valid && (!parseHeader() || s_done)) return;
  if (!s_offsets) {
    s_done = true;
    return;
  }
  for (uint32_t i = 0; i < kLayersPerPoll; i++) {
    if (s_known >= s_info.layers || s_next + kLayerHeaderSize > s_size) {
      s_done = true;
      if (s_known == s_info.layers) monitor::logf(monitor::Kind::Info, "goo: layer table complete");
      return;
    }
    uint8_t h[kLayerHeaderSize];
    source::ReadResult r = source::read(s_next, h, sizeof h, source::Mode::Peek);
    if (r == source::ReadResult::NotReady) return;
    if (r == source::ReadResult::Error || h[64] != '\r' || h[65] != '\n') {
      monitor::logf(monitor::Kind::Error, "goo: layer %u header not found at %u", (unsigned)s_known, s_next);
      s_done = true;
      return;
    }
    s_offsets[s_known] = s_next;
    s_known = s_known + 1;  // publish after the offset is stored
    s_next += kLayerHeaderSize + be32(h + 66) + 2;
  }
}

const Info& info() { return s_info; }
uint32_t knownLayers() { return s_known; }
bool complete() { return s_done; }

int32_t layerAt(uint32_t off) {
  uint32_t n = s_known;
  if (!n || off < s_offsets[0]) return -1;
  if (off >= s_next && n < s_info.layers) return -1;  // beyond what we have walked
  uint32_t lo = 0, hi = n;  // last offset <= off
  while (hi - lo > 1) {
    uint32_t mid = (lo + hi) / 2;
    if (s_offsets[mid] <= off) lo = mid;
    else hi = mid;
  }
  return lo;
}

}  // namespace goo
