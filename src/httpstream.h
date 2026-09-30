#pragma once
#include <Arduino.h>

#include <vector>

#include "source.h"

// Serves a remote file (HTTP Range requests) through a PSRAM chunk cache
// with read-ahead that follows the printer's read position.
namespace stream {

constexpr uint32_t kChunk = 65536;

void begin();  // allocates the cache and starts the fetch task

// Asks the server for the file size. Blocking; call from the web handler.
bool probe(const String& url, uint32_t& size, String& err);

void open(const String& url, uint32_t size);
void close();

source::ReadResult read(uint32_t offset, void* buf, uint32_t len, source::Mode mode);

// Waits until the first `bytes` of the file are cached (file header).
bool waitForHead(uint32_t bytes, uint32_t timeoutMs);

struct Stats {
  bool active = false;
  uint32_t slots = 0;
  uint32_t cached = 0;
  uint32_t fetched = 0;
  uint32_t errors = 0;
  uint32_t kbps = 0;       // average download rate
  uint32_t hostWaits = 0;  // printer reads that had to wait for the network
  String lastError;
  uint32_t lastErrorAgoMs = UINT32_MAX;
};
Stats stats();

// One bit per chunk: is it in the cache right now.
void cachedChunks(std::vector<uint8_t>& bits);

}  // namespace stream
