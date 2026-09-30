#include "source.h"

#include <Preferences.h>

#include "fatvol.h"
#include "goo.h"
#include "monitor.h"
#include "storage.h"
#include "httpstream.h"

namespace source {
namespace {

File s_file;
Preferences s_prefs;

void persist() {
  s_prefs.putUChar("kind", static_cast<uint8_t>(s_file.kind));
  s_prefs.putString("name", s_file.name);
  s_prefs.putUInt("size", s_file.size);
  s_prefs.putUInt("mtime", s_file.mtime);
  s_prefs.putString("url", s_file.url);
}

void apply() {
  if (s_file.kind == Kind::Stream) stream::open(s_file.url, s_file.size);
  else stream::close();
  bool present = s_file.kind != Kind::None;
  fatvol::configure(present ? s_file.name : String(), present ? s_file.size : 0, s_file.mtime);
  goo::reset(present ? s_file.size : 0);
  monitor::onFileChanged();
}

}  // namespace

void begin() {
  s_prefs.begin("drive", false);
  File f;
  f.kind = static_cast<Kind>(s_prefs.getUChar("kind", 0));
  f.name = s_prefs.getString("name", "");
  f.size = s_prefs.getUInt("size", 0);
  f.mtime = s_prefs.getUInt("mtime", 0);
  f.url = s_prefs.getString("url", "");
  bool valid = (f.kind == Kind::Flash && f.size <= storage::capacity()) ||
               (f.kind == Kind::Stream && !f.url.isEmpty());
  if (!valid || f.name.isEmpty()) f = File();
  s_file = f;
  apply();
}

const File& file() { return s_file; }

void set(const File& f) {
  s_file = f;
  persist();
  apply();
}

ReadResult read(uint32_t offset, void* buf, uint32_t len, Mode mode) {
  if (len == 0) return ReadResult::Ok;
  if (offset >= s_file.size || len > s_file.size - offset) return ReadResult::Error;
  switch (s_file.kind) {
    case Kind::Flash:
      return storage::read(offset, buf, len) ? ReadResult::Ok : ReadResult::Error;
    case Kind::Stream:
      return stream::read(offset, buf, len, mode);
    default:
      return ReadResult::Error;
  }
}

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Flash: return "flash";
    case Kind::Stream: return "stream";
    default: return "none";
  }
}

}  // namespace source
