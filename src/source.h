#pragma once
#include <Arduino.h>

// The one file on the drive and where its bytes come from: the flash
// partition (uploaded) or an HTTP URL (streamed on demand).
namespace source {

enum class Kind : uint8_t { None = 0, Flash = 1, Stream = 2 };
enum class ReadResult : uint8_t { Ok, NotReady, Error };

// How a read may wait for streamed data that is not cached yet.
enum class Mode : uint8_t {
  Host,  // printer read: moves the read-ahead cursor, waits briefly
  Wait,  // e.g. web preview: waits briefly, leaves the cursor alone
  Peek,  // never waits or requests anything
};

struct File {
  Kind kind = Kind::None;
  String name;
  uint32_t size = 0;
  uint32_t mtime = 0;  // unix time
  String url;          // Stream only
};

void begin();  // restores the last file from NVS
const File& file();
void set(const File& f);  // persists; reconfigures the volume and metadata

ReadResult read(uint32_t offset, void* buf, uint32_t len, Mode mode);

const char* kindName(Kind k);

}  // namespace source
