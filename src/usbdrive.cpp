#include "usbdrive.h"

#include <USB.h>
#include <USBMSC.h>

#include "esp32-hal-tinyusb.h"
#include "fatvol.h"
#include "monitor.h"

namespace usbdrive {
namespace {

// Reads within this long after (re)attach are the printer scanning the
// drive, not printing.
constexpr uint32_t kSettleMs = 15000;
constexpr uint32_t kBusyWindowMs = 20000;

USBMSC s_msc;
volatile bool s_attached = false;
volatile uint32_t s_attachedAt = 0;
volatile uint32_t s_lastReadAt = 0;
volatile bool s_everRead = false;
volatile uint32_t s_sectorsRead = 0;
bool s_wasMounted = false;
uint32_t s_restartAt = 0;

int32_t onRead(uint32_t lba, uint32_t offset, void* buf, uint32_t size) {
  if (!s_attached) return -1;
  int32_t n = fatvol::read(lba, offset, static_cast<uint8_t*>(buf), size);
  if (n <= 0) return n;  // 0 = streamed data not here yet; TinyUSB asks again
  s_sectorsRead += n / fatvol::kSectorSize;
  monitor::onHostRead(lba + offset / fatvol::kSectorSize, n);
  if (millis() - s_attachedAt > kSettleMs) {
    s_lastReadAt = millis();
    s_everRead = true;
  }
  return n;
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t size) {
  if (!s_attached) return -1;
  int32_t n = fatvol::write(lba, offset, buf, size);
  if (n > 0) monitor::onHostWrite(lba + offset / fatvol::kSectorSize, n);
  return n;
}

bool onStartStop(uint8_t, bool start, bool loadEject) {
  if (loadEject && !start) monitor::logf(monitor::Kind::Usb, "host ejected the drive");
  return true;
}

}  // namespace

void begin() {
  s_msc.vendorID("SLA-USB");
  s_msc.productID("Print Drive");
  s_msc.productRevision("2.0");
  s_msc.onRead(onRead);
  s_msc.onWrite(onWrite);
  s_msc.onStartStop(onStartStop);
  s_msc.mediaPresent(true);
  s_msc.begin(fatvol::sectorCount(), fatvol::kSectorSize);

  USB.manufacturerName("SLA-USB");
  USB.productName("SLA-USB Print Drive");
  USB.begin();
  s_attached = true;
  s_attachedAt = millis();
}

void loop() {
  if (s_restartAt && (int32_t)(millis() - s_restartAt) >= 0) {
    Serial.flush();
    ESP.restart();
  }
  bool mounted = hostConnected();
  if (mounted != s_wasMounted) {
    s_wasMounted = mounted;
    monitor::logf(monitor::Kind::Usb, mounted ? "host connected" : "host disconnected");
  }
}

void detach() {
  if (!s_attached) return;
  s_attached = false;
  s_msc.mediaPresent(false);
  tud_disconnect();
  delay(50);  // let any in-flight transfer finish
  monitor::logf(monitor::Kind::Usb, "drive detached");
}

void attach() {
  if (s_attached || s_restartAt) return;
  // Re-enumerating after tud_disconnect() is unreliable on the S3: hosts
  // reset the device in a loop. A restart is a clean "plug back in"; the
  // file is restored from NVS at boot.
  monitor::logf(monitor::Kind::Usb, "re-plugging: restarting in 1 s");
  s_restartAt = millis() + 1000;  // lets the web response go out first
}

bool restarting() { return s_restartAt != 0; }

bool attached() { return s_attached; }

uint32_t msSinceHostRead() { return s_everRead ? millis() - s_lastReadAt : UINT32_MAX; }

bool hostConnected() { return s_attached && tud_mounted(); }

uint32_t sectorsRead() { return s_sectorsRead; }

bool hostBusy() { return s_attached && msSinceHostRead() < kBusyWindowMs; }

}  // namespace usbdrive
