#include "web.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

#include "fatvol.h"
#include "goo.h"
#include "index_html.h"
#include "monitor.h"
#include "source.h"
#include "storage.h"
#include "httpstream.h"
#include "usbdrive.h"

namespace web {
namespace {

constexpr char kHostname[] = "sla-usb";
constexpr char kSetupSsid[] = "SLA-USB-Setup";
constexpr uint32_t kConnectTimeoutMs = 15000;

WebServer s_server(80);
Preferences s_prefs;

struct Upload {
  bool active = false;
  bool detached = false;  // we unplugged the printer for this upload
  String name;
  String error;
  int errorCode = 0;
  uint32_t bytes = 0;
  uint32_t startMs = 0;
} s_up;

struct Firmware {
  String error;
  uint32_t bytes = 0;
} s_fw;

String q(const String& s) {
  String out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { char esc[8]; snprintf(esc, sizeof esc, "\\u%04x", c); out += esc; }
    else out += c;
  }
  return out + "\"";
}
String b(bool v) { return v ? "true" : "false"; }
String num(float v, int decimals) { return isfinite(v) ? String(v, decimals) : String("null"); }

void sendJson(int code, const String& body) { s_server.send(code, "application/json", body); }
void sendError(int code, const String& msg) { sendJson(code, "{\"error\":" + q(msg) + "}"); }

bool forced() { return s_server.arg("force") == "1"; }

bool refuseIfPrinting() {
  if (!usbdrive::hostBusy() || forced()) return false;
  sendError(409, "The printer is reading the drive (probably printing). Retry with force to do it anyway.");
  return true;
}

uint32_t now() {
  time_t t = time(nullptr);
  return t > 1600000000 ? t : 0;
}

String sanitizeName(const String& in) {
  String name = in;
  int slash = max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
  if (slash >= 0) name = name.substring(slash + 1);
  String out;
  for (char c : name) {
    if ((uint8_t)c < 0x20 || strchr("\\/:*?\"<>|", c)) c = '_';
    out += c;
  }
  out.trim();
  while (out.startsWith(".")) out.remove(0, 1);
  if (out.isEmpty()) out = "print.goo";
  if (out.length() > 120) out = out.substring(out.length() - 120);
  return out;
}

String nameFromUrl(const String& url) {
  String path = url;
  int cut = path.indexOf('?');
  if (cut >= 0) path = path.substring(0, cut);
  String raw = path.substring(path.lastIndexOf('/') + 1), out;
  for (size_t i = 0; i < raw.length(); i++) {
    if (raw[i] == '%' && i + 2 < raw.length()) {
      out += (char)strtol(raw.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += raw[i];
    }
  }
  return out;
}

source::File makeFile(source::Kind kind, const String& name, uint32_t size, const String& url = String()) {
  source::File f;
  f.kind = kind;
  f.name = name;
  f.size = size;
  f.mtime = now();
  f.url = url;
  return f;
}

void handleIndex() { s_server.send_P(200, "text/html", kIndexHtml); }

void handleStatus() {
  const source::File& f = source::file();
  uint32_t sinceRead = usbdrive::msSinceHostRead();

  String j = "{\"uptime\":" + String(millis() / 1000);
  j += ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"psram\":" + String(ESP.getFreePsram());

  j += ",\"usb\":{\"attached\":" + b(usbdrive::attached()) + ",\"restarting\":" + b(usbdrive::restarting()) + ",\"hostConnected\":" + b(usbdrive::hostConnected());
  j += ",\"busy\":" + b(usbdrive::hostBusy()) + ",\"sectorsRead\":" + String(usbdrive::sectorsRead());
  j += ",\"msSinceRead\":" + (sinceRead == UINT32_MAX ? String("null") : String(sinceRead)) + "}";

  j += ",\"file\":";
  if (f.kind == source::Kind::None) {
    j += "null";
  } else {
    j += "{\"kind\":\"" + String(source::kindName(f.kind)) + "\",\"name\":" + q(f.name);
    j += ",\"size\":" + String(f.size) + ",\"mtime\":" + String(f.mtime) + ",\"url\":" + q(f.url) + "}";
  }

  const goo::Info& g = goo::info();
  j += ",\"goo\":";
  if (!g.valid) {
    j += "null";
  } else {
    j += "{\"layers\":" + String(g.layers) + ",\"known\":" + String(goo::knownLayers());
    j += ",\"complete\":" + b(goo::complete());
    j += ",\"resX\":" + String(g.resX) + ",\"resY\":" + String(g.resY);
    j += ",\"layerHeight\":" + num(g.layerHeight, 4) + ",\"exposure\":" + num(g.exposure, 2);
    j += ",\"printTime\":" + String(g.printTime) + ",\"volumeMl\":" + num(g.volumeMl, 2) + ",\"grams\":" + num(g.grams, 2);
    j += ",\"machine\":" + q(g.machine) + ",\"software\":" + q(g.software + " " + g.softwareVersion);
    j += ",\"created\":" + q(g.created) + "}";
  }

  j += ",\"progress\":" + monitor::progressJson();

  if (f.kind == source::Kind::Stream) {
    stream::Stats st = stream::stats();
    j += ",\"stream\":{\"slots\":" + String(st.slots) + ",\"cached\":" + String(st.cached);
    j += ",\"chunk\":" + String(stream::kChunk) + ",\"fetched\":" + String(st.fetched);
    j += ",\"errors\":" + String(st.errors) + ",\"kbps\":" + String(st.kbps) + ",\"hostWaits\":" + String(st.hostWaits);
    j += ",\"lastError\":" + q(st.lastError);
    j += ",\"lastErrorAgo\":" + (st.lastErrorAgoMs == UINT32_MAX ? String("null") : String(st.lastErrorAgoMs)) + "}";
  }

  fatvol::Layout L = fatvol::layout();
  j += ",\"volume\":{\"bytes\":" + String((double)L.totalSectors * fatvol::kSectorSize, 0);
  j += ",\"cluster\":" + String(L.clusterBytes) + ",\"clusters\":" + String(L.clusters);
  j += ",\"overlaySectors\":" + String(L.overlaySectors) + ",\"maxFile\":" + String(fatvol::maxFileSize());
  j += ",\"flashCapacity\":" + String(storage::capacity()) + "}";

  j += ",\"upload\":{\"active\":" + b(s_up.active) + ",\"bytes\":" + String(s_up.bytes) + ",\"name\":" + q(s_up.name) + "}";

  j += ",\"wifi\":{\"ssid\":" + q(WiFi.SSID());
  j += ",\"ip\":" + q(WiFi.isConnected() ? WiFi.localIP().toString() : "");
  j += ",\"rssi\":" + String(WiFi.isConnected() ? WiFi.RSSI() : 0);
  j += ",\"setupAp\":" + b(WiFi.getMode() & WIFI_MODE_AP) + "}";
  j += "}";
  sendJson(200, j);
}

void handleEvents() { sendJson(200, monitor::eventsJson(s_server.arg("since").toInt())); }

void handleMap() {
  int n = s_server.arg("n").toInt();
  sendJson(200, monitor::mapJson(n > 0 ? n : 400));
}

// Raw RGB565 (big-endian) preview image from the .goo header.
void handlePreview() {
  if (!goo::info().valid) {
    sendError(404, "No .goo preview available");
    return;
  }
  bool big = s_server.arg("size") == "big";
  uint32_t off = big ? goo::kBigPreviewOffset : goo::kSmallPreviewOffset;
  uint32_t len = big ? goo::kBigPreviewSize : goo::kSmallPreviewSize;
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buf) buf = static_cast<uint8_t*>(malloc(len));
  if (!buf) {
    sendError(500, "Out of memory");
    return;
  }
  source::ReadResult r = source::ReadResult::NotReady;
  for (uint32_t t0 = millis(); r == source::ReadResult::NotReady && millis() - t0 < 5000;)
    r = source::read(off, buf, len, source::Mode::Wait);
  if (r == source::ReadResult::Ok) {
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.setContentLength(len);
    s_server.send(200, "application/octet-stream", "");
    s_server.sendContent(reinterpret_cast<const char*>(buf), len);
  } else {
    sendError(503, "Preview not available yet");
  }
  free(buf);
}

void failUpload(int code, const String& msg) {
  if (s_up.error.isEmpty()) {
    s_up.error = msg;
    s_up.errorCode = code;
  }
}

// Streams the multipart body straight into the flash partition.
void handleUploadData() {
  HTTPUpload& up = s_server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      s_up = Upload();
      if (up.filename.isEmpty()) { failUpload(400, "No file in request"); return; }
      if (usbdrive::hostBusy() && !forced()) {
        failUpload(409, "The printer is reading the drive (probably printing). Retry with force to replace the file anyway.");
        return;
      }
      s_up.active = true;
      s_up.name = sanitizeName(up.filename);
      s_up.startMs = millis();
      usbdrive::detach();
      s_up.detached = true;
      source::set(source::File());  // the old file is gone from here on
      monitor::logf(monitor::Kind::File, "upload started: %s", s_up.name.c_str());
      if (!storage::beginWrite()) failUpload(500, "Flash storage unavailable");
      break;

    case UPLOAD_FILE_WRITE:
      if (!s_up.active || !s_up.error.isEmpty()) return;
      if (!storage::write(up.buf, up.currentSize)) {
        failUpload(507, "File too large for the flash drive (" + String(storage::capacity() / 1048576.0, 1) +
                            " MB). Use streaming for big files.");
        return;
      }
      s_up.bytes += up.currentSize;
      break;

    case UPLOAD_FILE_END: {
      if (!s_up.active) return;
      uint32_t size = storage::endWrite();
      if (s_up.error.isEmpty()) {
        source::set(makeFile(source::Kind::Flash, s_up.name, size));
        uint32_t ms = millis() - s_up.startMs;
        monitor::logf(monitor::Kind::File, "upload done: %u bytes in %.1f s (%u KB/s)", size, ms / 1000.0,
                      ms ? size / ms * 1000 / 1024 : 0);
      }
      break;
    }

    case UPLOAD_FILE_ABORTED:
      if (s_up.active) storage::endWrite();
      failUpload(400, "Upload aborted");
      monitor::logf(monitor::Kind::Error, "upload aborted by client");
      if (s_up.detached) usbdrive::attach();
      s_up.active = s_up.detached = false;
      break;
  }
}

void handleUploadDone() {
  if (!s_up.error.isEmpty() && s_up.active) source::set(source::File());
  if (s_up.detached) usbdrive::attach();
  s_up.active = s_up.detached = false;

  const source::File& f = source::file();
  if (!s_up.error.isEmpty()) {
    monitor::logf(monitor::Kind::Error, "upload failed: %s", s_up.error.c_str());
    sendError(s_up.errorCode ? s_up.errorCode : 500, s_up.error);
  } else if (f.kind != source::Kind::Flash) {
    sendError(400, "No file received");
  } else {
    sendJson(200, "{\"ok\":true,\"name\":" + q(f.name) + ",\"size\":" + String(f.size) + "}");
  }
}

void handleStream() {
  String url = s_server.arg("url");
  url.trim();
  if (url.isEmpty()) {
    sendError(400, "url is required");
    return;
  }
  if (refuseIfPrinting()) return;
  uint32_t size = 0;
  String err;
  if (!stream::probe(url, size, err)) {
    sendError(502, "Could not use " + url + ": " + err);
    return;
  }
  if (size > fatvol::maxFileSize()) {
    sendError(413, "File is larger than the drive");
    return;
  }
  String name = s_server.arg("name");
  name = sanitizeName(name.isEmpty() ? nameFromUrl(url) : name);

  usbdrive::detach();
  source::set(makeFile(source::Kind::Stream, name, size, url));
  monitor::logf(monitor::Kind::File, "streaming %s (%u bytes)", name.c_str(), size);
  usbdrive::attach();
  sendJson(200, "{\"ok\":true,\"name\":" + q(name) + ",\"size\":" + String(size) + "}");
}

void handleClear() {
  if (refuseIfPrinting()) return;
  usbdrive::detach();
  source::set(source::File());
  monitor::logf(monitor::Kind::File, "drive cleared");
  usbdrive::attach();
  sendJson(200, "{\"ok\":true}");
}

void handleReplug() {
  if (refuseIfPrinting()) return;
  usbdrive::detach();
  delay(1000);
  usbdrive::attach();
  sendJson(200, "{\"ok\":true}");
}

void handleWifi() {
  String ssid = s_server.arg("ssid");
  if (ssid.isEmpty()) {
    sendError(400, "SSID is required");
    return;
  }
  setWifi(ssid, s_server.arg("pass"));
  sendJson(200, "{\"ok\":true}");
  delay(500);
  ESP.restart();
}

void handleFirmwareData() {
  HTTPUpload& up = s_server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      s_fw = Firmware();
      if (usbdrive::hostBusy() && !forced()) {
        s_fw.error = "The printer is reading the drive (probably printing); updating would restart the device.";
        return;
      }
      monitor::logf(monitor::Kind::Info, "firmware update started");
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) s_fw.error = String("Update.begin: ") + Update.errorString();
      break;
    case UPLOAD_FILE_WRITE:
      if (!s_fw.error.isEmpty()) return;
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        s_fw.error = String("write: ") + Update.errorString();
        Update.abort();
      }
      s_fw.bytes += up.currentSize;
      break;
    case UPLOAD_FILE_END:
      if (s_fw.error.isEmpty() && !Update.end(true)) s_fw.error = String("end: ") + Update.errorString();
      break;
    case UPLOAD_FILE_ABORTED:
      Update.abort();
      s_fw.error = "aborted";
      break;
  }
}

void handleFirmwareDone() {
  if (!s_fw.error.isEmpty()) {
    monitor::logf(monitor::Kind::Error, "firmware update failed: %s", s_fw.error.c_str());
    sendError(400, s_fw.error);
    return;
  }
  monitor::logf(monitor::Kind::Info, "firmware updated (%u bytes), restarting", s_fw.bytes);
  sendJson(200, "{\"ok\":true,\"bytes\":" + String(s_fw.bytes) + "}");
  delay(500);
  usbdrive::detach();
  ESP.restart();
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
    monitor::logf(monitor::Kind::Info, "Wi-Fi connected: http://%s/", WiFi.localIP().toString().c_str());
  else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && WiFi.getMode() == WIFI_STA)
    Serial.println("Wi-Fi disconnected");
}

}  // namespace

void begin() {
  s_prefs.begin("wifi", false);
  String ssid = s_prefs.getString("ssid");
  String pass = s_prefs.getString("pass");

  WiFi.onEvent(onWifiEvent);
  WiFi.setHostname(kHostname);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  bool connected = false;
  if (!ssid.isEmpty()) {
    Serial.printf("Connecting to Wi-Fi '%s'...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    connected = WiFi.waitForConnectResult(kConnectTimeoutMs) == WL_CONNECTED;
  }
  if (!connected) {
    // Keep trying the saved network in the background, but also offer a
    // setup access point so Wi-Fi can be configured from a phone.
    WiFi.mode(ssid.isEmpty() ? WIFI_AP : WIFI_AP_STA);
    WiFi.softAP(kSetupSsid);
    monitor::logf(monitor::Kind::Info, "setup access point '%s': http://%s/", kSetupSsid,
                  WiFi.softAPIP().toString().c_str());
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  if (MDNS.begin(kHostname)) MDNS.addService("http", "tcp", 80);

  s_server.on("/", HTTP_GET, handleIndex);
  s_server.on("/api/status", HTTP_GET, handleStatus);
  s_server.on("/api/events", HTTP_GET, handleEvents);
  s_server.on("/api/map", HTTP_GET, handleMap);
  s_server.on("/api/preview", HTTP_GET, handlePreview);
  s_server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  s_server.on("/api/stream", HTTP_POST, handleStream);
  s_server.on("/api/clear", HTTP_POST, handleClear);
  s_server.on("/api/replug", HTTP_POST, handleReplug);
  s_server.on("/api/wifi", HTTP_POST, handleWifi);
  s_server.on("/api/firmware", HTTP_POST, handleFirmwareDone, handleFirmwareData);
  s_server.onNotFound([] { sendError(404, "Not found"); });
  s_server.begin();
}

void loop() { s_server.handleClient(); }

void setWifi(const String& ssid, const String& pass) {
  s_prefs.putString("ssid", ssid);
  s_prefs.putString("pass", pass);
}

void printStatus(Print& out) {
  const source::File& f = source::file();
  out.printf("USB: %s, host %s, %u sectors read, printer %s\n", usbdrive::attached() ? "attached" : "detached",
             usbdrive::hostConnected() ? "connected" : "NOT connected", usbdrive::sectorsRead(),
             usbdrive::hostBusy() ? "reading (busy)" : "idle");
  if (f.kind == source::Kind::None) out.println("File: (drive empty)");
  else out.printf("File: %s (%u bytes, %s)\n", f.name.c_str(), f.size, source::kindName(f.kind));
  const goo::Info& g = goo::info();
  if (g.valid) out.printf("GOO: %u layers (%u indexed), %.3f mm, %s\n", g.layers, goo::knownLayers(), g.layerHeight, g.machine.c_str());
  out.printf("Flash drive capacity: %u bytes\n", storage::capacity());
  if (WiFi.isConnected()) out.printf("Wi-Fi: '%s' %s RSSI %d\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else out.println("Wi-Fi: not connected");
  if (WiFi.getMode() & WIFI_MODE_AP) out.printf("Setup AP: '%s' %s\n", kSetupSsid, WiFi.softAPIP().toString().c_str());
}

}  // namespace web
