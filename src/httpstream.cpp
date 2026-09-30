#include "httpstream.h"

#include <WiFi.h>
#include <esp_http_client.h>
#include <freertos/semphr.h>

#include "monitor.h"

namespace stream {
namespace {

constexpr uint32_t kCacheBytes = 6 * 1024 * 1024;  // PSRAM
constexpr uint32_t kFallbackBytes = 256 * 1024;    // if there is no PSRAM
constexpr uint32_t kPinnedChunks = 4;              // file header + previews, never evicted
constexpr uint32_t kMaxReadAhead = 48;             // chunks ahead of the printer
constexpr uint32_t kHostWaitMs = 250;
constexpr uint32_t kHttpTimeoutMs = 8000;

enum : uint8_t { kEmpty, kLoading, kReady };

struct Slot {
  int32_t chunk = -1;
  uint8_t state = kEmpty;
  uint32_t lastUse = 0;
};

uint8_t* s_mem = nullptr;
Slot* s_slots = nullptr;
uint32_t s_nSlots = 0;

SemaphoreHandle_t s_mx = nullptr;
SemaphoreHandle_t s_ready = nullptr;  // given whenever a chunk lands
TaskHandle_t s_task = nullptr;

// Guarded by s_mx.
String s_url;
uint32_t s_size = 0;
uint32_t s_chunks = 0;
uint32_t s_gen = 0;  // bumped by open/close; stale downloads are discarded
bool s_active = false;
int32_t s_want = -1;   // chunk the printer is blocked on
uint32_t s_cursor = 0;  // chunk the printer read last

uint32_t s_fetched = 0, s_errors = 0, s_bytes = 0, s_busyMs = 0, s_hostWaits = 0;
String s_lastError;
uint32_t s_lastErrorAt = 0;

struct Lock {
  Lock() { xSemaphoreTake(s_mx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mx); }
};

int32_t findSlot(uint32_t chunk) {
  for (uint32_t i = 0; i < s_nSlots; i++)
    if (s_slots[i].chunk == (int32_t)chunk) return i;
  return -1;
}

bool isReady(uint32_t chunk) {
  int32_t i = findSlot(chunk);
  return i >= 0 && s_slots[i].state == kReady;
}

uint32_t readAhead() {
  uint32_t spare = s_nSlots > kPinnedChunks + 4 ? s_nSlots - kPinnedChunks - 4 : 1;
  return min(spare, kMaxReadAhead);
}

// Next chunk to download: whatever the printer is blocked on, then the file
// header, then the read-ahead window. -1 when nothing useful is missing.
int32_t pickChunk() {
  if (s_want >= 0 && findSlot(s_want) < 0) return s_want;
  for (uint32_t c = 0; c < min(kPinnedChunks, s_chunks); c++)
    if (findSlot(c) < 0) return c;
  uint32_t end = min(s_chunks, s_cursor + readAhead());
  for (uint32_t c = s_cursor; c < end; c++)
    if (findSlot(c) < 0) return c;
  return -1;
}

// Slot to reuse: an empty one, else the least recently used chunk outside
// the pinned header and the read-ahead window.
int32_t pickVictim() {
  int32_t best = -1;
  uint32_t bestUse = UINT32_MAX;
  uint32_t winEnd = s_cursor + readAhead();
  for (uint32_t i = 0; i < s_nSlots; i++) {
    const Slot& s = s_slots[i];
    if (s.state == kEmpty) return i;
    if (s.state != kReady) continue;
    uint32_t c = s.chunk;
    if (c < kPinnedChunks || (c >= s_cursor && c < winEnd)) continue;
    if (s.lastUse < bestUse) {
      best = i;
      bestUse = s.lastUse;
    }
  }
  return best;
}

struct RangeInfo {
  uint32_t total = 0;
};

esp_err_t onHttpEvent(esp_http_client_event_t* e) {
  if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Content-Range") == 0) {
    const char* slash = strchr(e->header_value, '/');
    if (slash) static_cast<RangeInfo*>(e->user_data)->total = strtoul(slash + 1, nullptr, 10);
  }
  return ESP_OK;
}

// GET bytes [from, from+len) of url into buf.
bool httpRange(const String& url, uint32_t from, uint32_t len, uint8_t* buf, uint32_t* total, String& err) {
  RangeInfo info;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = kHttpTimeoutMs;
  cfg.event_handler = onHttpEvent;
  cfg.user_data = &info;
  cfg.buffer_size = 4096;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    err = "invalid URL";
    return false;
  }
  char range[48];
  snprintf(range, sizeof range, "bytes=%u-%u", from, from + len - 1);
  esp_http_client_set_header(c, "Range", range);

  bool ok = false;
  if (esp_http_client_open(c, 0) != ESP_OK) {
    err = "cannot connect to server";
  } else {
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 206) {
      err = status == 200 ? "server does not support Range requests" : "HTTP " + String(status);
    } else {
      uint32_t got = 0;
      while (got < len) {
        int r = esp_http_client_read(c, reinterpret_cast<char*>(buf) + got, len - got);
        if (r <= 0) break;
        got += r;
      }
      ok = got == len;
      if (!ok) err = "connection dropped (" + String(got) + "/" + String(len) + " bytes)";
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  if (total) *total = info.total;
  return ok;
}

void fetchTask(void*) {
  uint32_t backoff = 0;
  for (;;) {
    int32_t chunk = -1, slot = -1;
    uint32_t gen = 0, size = 0;
    String url;
    if (WiFi.isConnected()) {
      Lock lock;
      if (s_active) {
        chunk = pickChunk();
        if (chunk >= 0) slot = pickVictim();
        if (slot >= 0) {
          s_slots[slot].chunk = chunk;
          s_slots[slot].state = kLoading;
          gen = s_gen;
          url = s_url;
          size = s_size;
        }
      }
    }
    if (slot < 0) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }

    uint32_t from = chunk * kChunk;
    uint32_t len = min(kChunk, size - from);
    uint32_t t0 = millis();
    String err;
    bool ok = httpRange(url, from, len, s_mem + slot * kChunk, nullptr, err);
    bool current;
    {
      Lock lock;
      current = gen == s_gen;
      Slot& s = s_slots[slot];
      if (ok && current) {
        s.state = kReady;
        s.lastUse = millis();
        s_fetched++;
        s_bytes += len;
        s_busyMs += millis() - t0;
        if (s_want == chunk) s_want = -1;
      } else {
        s.state = kEmpty;
        s.chunk = -1;
      }
      if (!ok && current) {
        s_errors++;
        s_lastError = err;
        s_lastErrorAt = millis();
      }
    }
    if (ok) {
      backoff = 0;
      xSemaphoreGive(s_ready);
    } else if (current) {
      monitor::logf(monitor::Kind::Error, "stream: chunk %d failed: %s", chunk, err.c_str());
      backoff = min<uint32_t>(backoff ? backoff * 2 : 250, 4000);
      delay(backoff);
    }
  }
}

}  // namespace

void begin() {
  s_mem = static_cast<uint8_t*>(heap_caps_malloc(kCacheBytes, MALLOC_CAP_SPIRAM));
  uint32_t bytes = kCacheBytes;
  if (!s_mem) {
    log_w("no PSRAM, stream cache is only %u KB", kFallbackBytes / 1024);
    bytes = kFallbackBytes;
    s_mem = static_cast<uint8_t*>(malloc(bytes));
  }
  s_nSlots = bytes / kChunk;
  s_slots = new Slot[s_nSlots];
  s_mx = xSemaphoreCreateMutex();
  s_ready = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(fetchTask, "stream", 6144, nullptr, 3, &s_task, 0);
}

bool probe(const String& url, uint32_t& size, String& err) {
  if (!url.startsWith("http://")) {
    err = "Only http:// URLs are supported";
    return false;
  }
  uint8_t one;
  uint32_t total = 0;
  if (!httpRange(url, 0, 1, &one, &total, err)) return false;
  if (total == 0) {
    err = "server did not report the file size";
    return false;
  }
  size = total;
  return true;
}

void open(const String& url, uint32_t size) {
  {
    Lock lock;
    s_gen++;
    s_url = url;
    s_size = size;
    s_chunks = (size + kChunk - 1) / kChunk;
    s_active = size > 0;
    s_want = -1;
    s_cursor = 0;
    for (uint32_t i = 0; i < s_nSlots; i++) s_slots[i] = Slot();
    s_fetched = s_errors = s_bytes = s_busyMs = s_hostWaits = 0;
    s_lastError = "";
  }
  xTaskNotifyGive(s_task);
}

void close() {
  Lock lock;
  if (!s_active) return;
  s_gen++;
  s_active = false;
  for (uint32_t i = 0; i < s_nSlots; i++) s_slots[i] = Slot();
}

source::ReadResult read(uint32_t offset, void* buf, uint32_t len, source::Mode mode) {
  uint32_t first = offset / kChunk, last = (offset + len - 1) / kChunk;
  int attempts = mode == source::Mode::Peek ? 1 : 2;
  for (int attempt = 0; attempt < attempts; attempt++) {
    int32_t missing = -1;
    {
      Lock lock;
      if (!s_active) return source::ReadResult::Error;
      if (mode == source::Mode::Host) s_cursor = first;
      for (uint32_t c = first; c <= last && missing < 0; c++)
        if (!isReady(c)) missing = c;
      if (missing < 0) {
        uint8_t* out = static_cast<uint8_t*>(buf);
        uint32_t pos = offset, left = len;
        while (left) {
          uint32_t c = pos / kChunk, off = pos % kChunk, n = min(left, kChunk - off);
          Slot& s = s_slots[findSlot(c)];
          memcpy(out, s_mem + (&s - s_slots) * kChunk + off, n);
          s.lastUse = millis();
          out += n; pos += n; left -= n;
        }
        return source::ReadResult::Ok;
      }
      if (mode == source::Mode::Peek) return source::ReadResult::NotReady;
      s_want = missing;
      if (mode == source::Mode::Host && attempt == 0) s_hostWaits++;
    }
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_ready, pdMS_TO_TICKS(kHostWaitMs));
  }
  return source::ReadResult::NotReady;
}

bool waitForHead(uint32_t bytes, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  for (;;) {
    uint32_t last;
    {
      Lock lock;
      if (!s_active) return false;
      last = (min(bytes, s_size) - 1) / kChunk;
      bool all = true;
      for (uint32_t c = 0; c <= last && all; c++) all = isReady(c);
      if (all) return true;
    }
    if (millis() - t0 > timeoutMs) return false;
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_ready, pdMS_TO_TICKS(100));
  }
}

Stats stats() {
  Lock lock;
  Stats st;
  st.active = s_active;
  st.slots = s_nSlots;
  for (uint32_t i = 0; i < s_nSlots; i++)
    if (s_slots[i].state == kReady) st.cached++;
  st.fetched = s_fetched;
  st.errors = s_errors;
  st.kbps = s_busyMs ? (uint64_t)s_bytes * 1000 / s_busyMs / 1024 : 0;
  st.hostWaits = s_hostWaits;
  st.lastError = s_lastError;
  if (!s_lastError.isEmpty()) st.lastErrorAgoMs = millis() - s_lastErrorAt;
  return st;
}

void cachedChunks(std::vector<uint8_t>& bits) {
  Lock lock;
  bits.assign((s_chunks + 7) / 8, 0);
  for (uint32_t i = 0; i < s_nSlots; i++) {
    const Slot& s = s_slots[i];
    if (s.state == kReady && (uint32_t)s.chunk < s_chunks) bits[s.chunk >> 3] |= 1 << (s.chunk & 7);
  }
}

}  // namespace stream
