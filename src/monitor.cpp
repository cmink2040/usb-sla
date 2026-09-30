#include "monitor.h"

#include <stdarg.h>

#include <vector>

#include "fatvol.h"
#include "goo.h"
#include "source.h"
#include "httpstream.h"

namespace monitor {
namespace {

constexpr uint32_t kEvents = 300;
constexpr uint32_t kCoalesceMs = 3000;
constexpr uint32_t kCoverageChunk = stream::kChunk;

struct Event {
  uint32_t id;
  uint32_t t0, t1;  // millis
  Kind kind;
  fatvol::Region region;
  uint32_t a, b;    // read/write: byte range (file offset for file reads, disk LBA*512 otherwise)
  uint32_t count;   // coalesced operations
  char text[80];
};

Event* s_ev = nullptr;
uint32_t s_nextId = 1;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Read statistics (written by the USB task).
volatile uint32_t s_lastFileOff = 0, s_maxFileOff = 0, s_lastReadMs = 0;
volatile uint64_t s_fileBytesRead = 0, s_diskBytesRead = 0;
volatile bool s_anyFileRead = false;
uint8_t* s_coverage = nullptr;  // bit per 64 KB chunk of the file read by the printer
uint32_t s_coverageBits = 0;

Event& slot(uint32_t id) { return s_ev[id % kEvents]; }

// Appends an event (or extends the last one). Caller holds s_mux.
Event& push(Kind kind, fatvol::Region region, uint32_t a, uint32_t b) {
  Event& e = slot(s_nextId);
  e.id = s_nextId++;
  e.t0 = e.t1 = millis();
  e.kind = kind;
  e.region = region;
  e.a = a;
  e.b = b;
  e.count = 1;
  e.text[0] = 0;
  return e;
}

void record(Kind kind, uint32_t lba, uint32_t len) {
  uint32_t fileOff = 0;
  fatvol::Region region = fatvol::classify(lba, &fileOff);
  bool isFile = region == fatvol::Region::File;
  uint32_t a = isFile ? fileOff : lba * fatvol::kSectorSize;
  uint32_t b = a + len;
  uint32_t now = millis();

  if (kind == Kind::Read) {
    s_diskBytesRead += len;
    if (isFile) {
      s_lastFileOff = fileOff;
      if (b > s_maxFileOff) s_maxFileOff = b;
      s_fileBytesRead += len;
      s_lastReadMs = now;
      s_anyFileRead = true;
      if (s_coverage)
        for (uint32_t c = a / kCoverageChunk; c <= (b - 1) / kCoverageChunk && c < s_coverageBits; c++)
          s_coverage[c >> 3] |= 1 << (c & 7);
    }
  }

  portENTER_CRITICAL(&s_mux);
  Event& last = slot(s_nextId - 1);
  if (s_nextId > 1 && last.kind == kind && last.region == region && last.b == a && now - last.t1 < kCoalesceMs) {
    last.b = b;
    last.t1 = now;
    last.count++;
  } else {
    push(kind, region, a, b);
  }
  portEXIT_CRITICAL(&s_mux);
}

String jsonEscape(const char* s) {
  String out;
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { out += '\\'; out += *s; }
    else if ((uint8_t)*s < 0x20) out += ' ';
    else out += *s;
  }
  return out;
}

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Info: return "info";
    case Kind::Usb: return "usb";
    case Kind::Read: return "read";
    case Kind::Write: return "write";
    case Kind::File: return "file";
    case Kind::Error: return "error";
  }
  return "?";
}

}  // namespace

void begin() {
  s_ev = static_cast<Event*>(heap_caps_calloc(kEvents, sizeof(Event), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_ev) s_ev = static_cast<Event*>(calloc(kEvents, sizeof(Event)));
}

void logf(Kind kind, const char* fmt, ...) {
  char text[sizeof(Event::text)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  Serial.printf("[%lu] %s: %s\n", (unsigned long)(millis() / 1000), kindName(kind), text);
  portENTER_CRITICAL(&s_mux);
  Event& e = push(kind, fatvol::Region::Free, 0, 0);
  memcpy(e.text, text, sizeof text);
  portEXIT_CRITICAL(&s_mux);
}

void onHostRead(uint32_t lba, uint32_t len) { record(Kind::Read, lba, len); }
void onHostWrite(uint32_t lba, uint32_t len) { record(Kind::Write, lba, len); }

void onFileChanged() {
  s_lastFileOff = s_maxFileOff = s_lastReadMs = 0;
  s_fileBytesRead = 0;
  s_anyFileRead = false;
  uint32_t size = source::file().size;
  uint8_t* old = s_coverage;
  s_coverage = nullptr;
  free(old);
  s_coverageBits = (size + kCoverageChunk - 1) / kCoverageChunk;
  if (s_coverageBits) s_coverage = static_cast<uint8_t*>(calloc((s_coverageBits + 7) / 8, 1));
}

String eventsJson(uint32_t since) {
  std::vector<Event> copy;
  copy.reserve(100);  // no allocation inside the critical section
  portENTER_CRITICAL(&s_mux);
  uint32_t first = s_nextId > kEvents ? s_nextId - kEvents : 1;
  if (since < first) since = first;
  for (uint32_t id = since; id < s_nextId && copy.size() < 100; id++) copy.push_back(slot(id));
  uint32_t next = s_nextId;
  portEXIT_CRITICAL(&s_mux);

  uint32_t now = millis();
  String j = "{\"next\":" + String(next) + ",\"events\":[";
  bool firstOut = true;
  for (const Event& e : copy) {
    if (!firstOut) j += ',';
    firstOut = false;
    j += "{\"id\":" + String(e.id) + ",\"ago\":" + String(now - e.t1) + ",\"dur\":" + String(e.t1 - e.t0);
    j += ",\"kind\":\"" + String(kindName(e.kind)) + "\"";
    if (e.kind == Kind::Read || e.kind == Kind::Write) {
      j += ",\"region\":\"" + String(fatvol::regionName(e.region)) + "\",\"a\":" + String(e.a) + ",\"b\":" + String(e.b);
      j += ",\"n\":" + String(e.count);
      if (e.region == fatvol::Region::File) {
        j += ",\"la\":" + String(goo::layerAt(e.a)) + ",\"lb\":" + String(goo::layerAt(e.b - 1));
      }
    } else {
      j += ",\"text\":\"" + jsonEscape(e.text) + "\"";
    }
    j += '}';
  }
  return j + "]}";
}

String progressJson() {
  uint32_t now = millis();
  int32_t layer = s_anyFileRead ? goo::layerAt(s_lastFileOff) : -1;
  String j = "{\"fileOffset\":" + String(s_lastFileOff);
  j += ",\"maxOffset\":" + String(s_maxFileOff);
  j += ",\"fileBytesRead\":" + String((double)s_fileBytesRead, 0);
  j += ",\"diskBytesRead\":" + String((double)s_diskBytesRead, 0);
  j += ",\"msSinceFileRead\":" + (s_anyFileRead ? String(now - s_lastReadMs) : String("null"));
  j += ",\"layer\":" + String(layer);
  return j + "}";
}

String mapJson(uint32_t buckets) {
  uint32_t size = source::file().size;
  if (!size || !buckets) return "{\"size\":0,\"map\":\"\"}";
  buckets = min<uint32_t>(buckets, 1000);
  bool isStream = source::file().kind == source::Kind::Stream;
  std::vector<uint8_t> cached;
  if (isStream) stream::cachedChunks(cached);

  auto hasBit = [](const uint8_t* bits, uint32_t nbits, uint32_t i) { return i < nbits && (bits[i >> 3] >> (i & 7)) & 1; };
  String map;
  map.reserve(buckets);
  for (uint32_t i = 0; i < buckets; i++) {
    uint32_t a = (uint64_t)size * i / buckets, b = (uint64_t)size * (i + 1) / buckets;
    uint32_t c0 = a / kCoverageChunk, c1 = b > a ? (b - 1) / kCoverageChunk : c0;
    bool avail = !isStream, read = false;
    for (uint32_t c = c0; c <= c1; c++) {
      if (isStream && hasBit(cached.data(), cached.size() * 8, c)) avail = true;
      if (s_coverage && hasBit(s_coverage, s_coverageBits, c)) read = true;
    }
    map += char('0' + (avail ? 1 : 0) + (read ? 2 : 0));
  }
  return "{\"size\":" + String(size) + ",\"map\":\"" + map + "\"}";
}

}  // namespace monitor
