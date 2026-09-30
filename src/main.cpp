// SLA-USB: the ESP32-S3's native USB port acts as a USB stick for a resin
// printer. The stick holds exactly one file, either uploaded over Wi-Fi into
// flash or streamed on demand from an HTTP server. Each new file replaces
// the previous one.
//
// Serial console (COM port via CH343, 115200 baud):
//   status             show USB / file / Wi-Fi state
//   ssid <name>        set Wi-Fi network (then: pass, reboot)
//   pass <password>
//   reboot

#include <Arduino.h>

#include "fatvol.h"
#include "goo.h"
#include "monitor.h"
#include "source.h"
#include "storage.h"
#include "httpstream.h"
#include "usbdrive.h"
#include "web.h"

namespace {

String s_pendingSsid;

void handleCommand(String line) {
  line.trim();
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);

  if (cmd == "status") {
    web::printStatus(Serial);
  } else if (cmd == "ssid" && !arg.isEmpty()) {
    s_pendingSsid = arg;
    Serial.printf("SSID set to '%s'. Now: pass <password> (or 'pass' for an open network)\n", arg.c_str());
  } else if (cmd == "pass") {
    if (s_pendingSsid.isEmpty()) {
      Serial.println("Set the SSID first: ssid <name>");
      return;
    }
    web::setWifi(s_pendingSsid, arg);
    Serial.println("Saved. Type 'reboot' to connect.");
  } else if (cmd == "reboot") {
    ESP.restart();
  } else if (!cmd.isEmpty()) {
    Serial.println("Commands: status | ssid <name> | pass <password> | reboot");
  }
}

void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (!line.isEmpty()) handleCommand(line);
      line = "";
    } else if (c >= 0x20 && c < 0x7F && line.length() < 200) {  // drop noise from port open
      line += c;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("\nSLA-USB starting");

  monitor::begin();
  if (!storage::begin()) Serial.println("WARNING: flash drive partition missing; only streaming will work");
  fatvol::begin();
  stream::begin();
  source::begin();
  const source::File& f = source::file();
  Serial.printf("File: %s (%s), PSRAM free: %u KB\n", f.kind == source::Kind::None ? "(none)" : f.name.c_str(),
                source::kindName(f.kind), ESP.getFreePsram() / 1024);

  if (f.kind == source::Kind::Stream) {
    // Streamed file: bring up Wi-Fi first and cache the file header, so the
    // printer's first look at the drive (preview, file info) is instant.
    web::begin();
    bool head = stream::waitForHead(4 * stream::kChunk, 10000);
    if (!head) monitor::logf(monitor::Kind::Error, "stream: file server not reachable yet");
    usbdrive::begin();
  } else {
    usbdrive::begin();
    web::begin();
  }
  Serial.println("Ready. Type 'status' for details.");
}

void loop() {
  web::loop();
  usbdrive::loop();
  goo::poll();
  pollSerial();
  delay(1);
}
