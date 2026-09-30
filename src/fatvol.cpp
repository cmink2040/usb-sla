#include "fatvol.h"

#include <map>
#include <time.h>
#include <vector>

#include "source.h"

namespace fatvol {
namespace {

// Same geometry as a stock 4 GB FAT32 stick known to work in the printer.
constexpr uint32_t kTotalSectors = 7864320;
constexpr uint32_t kSecPerClus = 8;
constexpr uint32_t kClusterBytes = kSectorSize * kSecPerClus;
constexpr uint32_t kFatCopies = 2;
constexpr uint32_t kRootCluster = 2;
constexpr uint32_t kFileCluster = 3;
constexpr uint32_t kEoc = 0x0FFFFFFF;
constexpr uint32_t kMaxOverlaySectors = 2048;  // 1 MB of host writes

uint32_t s_rsvd, s_fatSz, s_dataStart, s_clusters;

uint32_t s_size = 0, s_fileClusters = 0;
uint32_t s_volId = 0;
uint16_t s_date = 0, s_time = 0;
uint8_t s_dir[kClusterBytes];  // root directory cluster

std::map<uint32_t, uint8_t*> s_overlay;
SemaphoreHandle_t s_overlayMx = nullptr;

void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

void computeLayout() {
  s_rsvd = 32;
  s_fatSz = (((kTotalSectors - s_rsvd) / kSecPerClus + 2) * 4 + kSectorSize - 1) / kSectorSize;
  // Align the data area to a cluster boundary, like the Windows formatter.
  while ((s_rsvd + kFatCopies * s_fatSz) % kSecPerClus) s_rsvd++;
  s_dataStart = s_rsvd + kFatCopies * s_fatSz;
  s_clusters = (kTotalSectors - s_dataStart) / kSecPerClus;
}

void bootSector(uint8_t* b) {
  memset(b, 0, kSectorSize);
  b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
  memcpy(b + 3, "MSDOS5.0", 8);
  put16(b + 11, kSectorSize);
  b[13] = kSecPerClus;
  put16(b + 14, s_rsvd);
  b[16] = kFatCopies;
  b[21] = 0xF8;              // fixed media
  put16(b + 24, 63);         // sectors per track
  put16(b + 26, 255);        // heads
  put32(b + 32, kTotalSectors);
  put32(b + 36, s_fatSz);
  put32(b + 44, kRootCluster);
  put16(b + 48, 1);          // FSInfo sector
  put16(b + 50, 6);          // backup boot sector
  b[64] = 0x80;
  b[66] = 0x29;
  put32(b + 67, s_volId);
  memcpy(b + 71, "SLA-USB    ", 11);
  memcpy(b + 82, "FAT32   ", 8);
  b[510] = 0x55; b[511] = 0xAA;
}

void fsInfoSector(uint8_t* b) {
  memset(b, 0, kSectorSize);
  put32(b, 0x41615252);
  put32(b + 484, 0x61417272);
  put32(b + 488, s_clusters - 1 - s_fileClusters);
  put32(b + 492, kFileCluster + s_fileClusters);
  put32(b + 508, 0xAA550000);
}

uint32_t fatEntry(uint32_t c) {
  if (c == 0) return 0x0FFFFFF8;
  if (c == 1 || c == kRootCluster) return kEoc;
  uint32_t end = kFileCluster + s_fileClusters;
  if (c >= kFileCluster && c < end) return c + 1 == end ? kEoc : c + 1;
  return 0;
}

std::vector<uint16_t> utf16(const String& s) {
  std::vector<uint16_t> out;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s.c_str());
  while (*p && out.size() < 255) {
    uint32_t cp = *p++;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    cp &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
    while (extra-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    out.push_back(cp > 0xFFFF ? '_' : cp);
  }
  return out;
}

void shortName(const String& name, uint8_t out[11]) {
  memset(out, ' ', 11);
  int dot = name.lastIndexOf('.');
  String base = dot > 0 ? name.substring(0, dot) : name;
  String ext = dot > 0 ? name.substring(dot + 1) : "";
  auto clean = [](const String& s, size_t max) {
    String r;
    for (char c : s) {
      if (r.length() >= max) break;
      if (c == ' ' || c == '.') continue;
      c = toupper(c);
      r += (isalnum((uint8_t)c) || strchr("$%'-_@~`!(){}^#&", c)) ? c : '_';
    }
    return r;
  };
  String b = clean(base, 6), x = clean(ext, 3);
  if (b.isEmpty()) b = "FILE";
  b += "~1";
  memcpy(out, b.c_str(), b.length());
  memcpy(out + 8, x.c_str(), x.length());
}

uint8_t lfnChecksum(const uint8_t sfn[11]) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++) sum = ((sum & 1) << 7) + (sum >> 1) + sfn[i];
  return sum;
}

void buildRootDir(const String& name) {
  memset(s_dir, 0, sizeof s_dir);
  uint8_t* e = s_dir;
  memcpy(e, "SLA-USB    ", 11);
  e[11] = 0x08;  // volume label
  put16(e + 22, s_time);
  put16(e + 24, s_date);
  e += 32;
  if (name.isEmpty()) return;

  uint8_t sfn[11];
  shortName(name, sfn);
  std::vector<uint16_t> u = utf16(name);
  uint8_t sum = lfnChecksum(sfn);
  static const uint8_t kPos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  int parts = (u.size() + 12) / 13;
  for (int i = parts; i >= 1; i--, e += 32) {
    e[0] = i | (i == parts ? 0x40 : 0);
    e[11] = 0x0F;
    e[13] = sum;
    for (int j = 0; j < 13; j++) {
      size_t k = (i - 1) * 13 + j;
      put16(e + kPos[j], k < u.size() ? u[k] : k == u.size() ? 0x0000 : 0xFFFF);
    }
  }
  memcpy(e, sfn, 11);
  e[11] = 0x20;  // archive
  put16(e + 14, s_time);
  put16(e + 16, s_date);
  put16(e + 18, s_date);
  uint32_t first = s_fileClusters ? kFileCluster : 0;
  put16(e + 20, first >> 16);
  put16(e + 22, s_time);
  put16(e + 24, s_date);
  put16(e + 26, first & 0xFFFF);
  put32(e + 28, s_size);
}

bool overlayGet(uint32_t lba, uint8_t* out) {
  xSemaphoreTake(s_overlayMx, portMAX_DELAY);
  auto it = s_overlay.find(lba);
  bool found = it != s_overlay.end();
  if (found) memcpy(out, it->second, kSectorSize);
  xSemaphoreGive(s_overlayMx);
  return found;
}

// Produces one 512-byte sector.
source::ReadResult sector(uint32_t lba, uint8_t* out) {
  if (overlayGet(lba, out)) return source::ReadResult::Ok;
  if (lba == 0 || lba == 6) {
    bootSector(out);
  } else if (lba == 1 || lba == 7) {
    fsInfoSector(out);
  } else if (lba < s_rsvd) {
    memset(out, 0, kSectorSize);
    if (lba == 2 || lba == 8) { out[510] = 0x55; out[511] = 0xAA; }
  } else if (lba < s_dataStart) {
    uint32_t first = ((lba - s_rsvd) % s_fatSz) * (kSectorSize / 4);
    for (uint32_t i = 0; i < kSectorSize / 4; i++) put32(out + i * 4, fatEntry(first + i));
  } else {
    uint32_t rel = lba - s_dataStart;
    uint32_t c = 2 + rel / kSecPerClus;
    uint32_t within = (rel % kSecPerClus) * kSectorSize;
    if (c == kRootCluster) {
      memcpy(out, s_dir + within, kSectorSize);
    } else if (c >= kFileCluster && c < kFileCluster + s_fileClusters) {
      uint32_t off = (c - kFileCluster) * kClusterBytes + within;
      uint32_t n = off < s_size ? min(kSectorSize, s_size - off) : 0;
      if (n) {
        source::ReadResult r = source::read(off, out, n, source::Mode::Host);
        if (r != source::ReadResult::Ok) return r;
      }
      memset(out + n, 0, kSectorSize - n);
    } else {
      memset(out, 0, kSectorSize);
    }
  }
  return source::ReadResult::Ok;
}

}  // namespace

void begin() {
  computeLayout();
  s_overlayMx = xSemaphoreCreateMutex();
}

void configure(const String& name, uint32_t size, uint32_t mtime) {
  s_size = name.isEmpty() ? 0 : size;
  s_fileClusters = (s_size + kClusterBytes - 1) / kClusterBytes;

  time_t t = mtime > 1600000000 ? mtime : 1767225600;  // fallback 2026-01-01
  struct tm tm;
  gmtime_r(&t, &tm);
  s_date = ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
  s_time = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
  s_volId = mtime ^ size ^ 0x51A0B5B0;  // new serial per file, so hosts drop caches

  buildRootDir(name.isEmpty() ? String() : name);

  xSemaphoreTake(s_overlayMx, portMAX_DELAY);
  for (auto& kv : s_overlay) free(kv.second);
  s_overlay.clear();
  xSemaphoreGive(s_overlayMx);
}

uint32_t sectorCount() { return kTotalSectors; }

uint32_t maxFileSize() { return (s_clusters - 1) * kClusterBytes; }

int32_t read(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t len) {
  uint64_t addr = (uint64_t)lba * kSectorSize + offset;
  uint32_t done = 0;
  uint8_t tmp[kSectorSize];
  while (done < len) {
    uint32_t sec = addr / kSectorSize, so = addr % kSectorSize;
    uint32_t n = min(len - done, kSectorSize - so);
    if (sec >= kTotalSectors) return -1;
    bool whole = so == 0 && n == kSectorSize;
    source::ReadResult r = sector(sec, whole ? buf + done : tmp);
    if (r == source::ReadResult::NotReady) return 0;
    if (r == source::ReadResult::Error) return -1;
    if (!whole) memcpy(buf + done, tmp + so, n);
    done += n;
    addr += n;
  }
  return done;
}

int32_t write(uint32_t lba, uint32_t offset, const uint8_t* buf, uint32_t len) {
  uint64_t addr = (uint64_t)lba * kSectorSize + offset;
  uint32_t done = 0;
  while (done < len) {
    uint32_t sec = addr / kSectorSize, so = addr % kSectorSize;
    uint32_t n = min(len - done, kSectorSize - so);
    if (sec >= kTotalSectors) return -1;
    uint8_t cur[kSectorSize];
    if (n != kSectorSize && sector(sec, cur) != source::ReadResult::Ok) return -1;
    xSemaphoreTake(s_overlayMx, portMAX_DELAY);
    auto it = s_overlay.find(sec);
    uint8_t* slot = it != s_overlay.end() ? it->second : nullptr;
    if (!slot && s_overlay.size() < kMaxOverlaySectors) {
      slot = static_cast<uint8_t*>(heap_caps_malloc(kSectorSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!slot) slot = static_cast<uint8_t*>(malloc(kSectorSize));
      if (slot) s_overlay[sec] = slot;
    }
    if (slot) {
      if (n != kSectorSize) memcpy(slot, cur, kSectorSize);
      memcpy(slot + so, buf + done, n);
    }
    xSemaphoreGive(s_overlayMx);
    if (!slot) return -1;
    done += n;
    addr += n;
  }
  return done;
}

const char* regionName(Region r) {
  switch (r) {
    case Region::Boot: return "boot";
    case Region::FsInfo: return "fsinfo";
    case Region::Reserved: return "reserved";
    case Region::Fat: return "fat";
    case Region::RootDir: return "dir";
    case Region::File: return "file";
    case Region::Free: return "free";
    case Region::Overlay: return "overlay";
  }
  return "?";
}

Region classify(uint32_t lba, uint32_t* fileOffset) {
  if (lba == 0 || lba == 6) return Region::Boot;
  if (lba == 1 || lba == 7) return Region::FsInfo;
  if (lba < s_rsvd) return Region::Reserved;
  if (lba < s_dataStart) return Region::Fat;
  uint32_t rel = lba - s_dataStart;
  uint32_t c = 2 + rel / kSecPerClus;
  if (c == kRootCluster) return Region::RootDir;
  if (c >= kFileCluster && c < kFileCluster + s_fileClusters) {
    if (fileOffset) *fileOffset = (c - kFileCluster) * kClusterBytes + (rel % kSecPerClus) * kSectorSize;
    return Region::File;
  }
  return Region::Free;
}

Layout layout() {
  xSemaphoreTake(s_overlayMx, portMAX_DELAY);
  uint32_t ov = s_overlay.size();
  xSemaphoreGive(s_overlayMx);
  return {kTotalSectors, kClusterBytes, s_rsvd, s_fatSz, s_dataStart, s_clusters, ov};
}

}  // namespace fatvol
