#pragma once
#include <Arduino.h>

// Wi-Fi, mDNS and the HTTP UI/API: upload or stream the print file,
// live monitoring, firmware updates.
namespace web {

void begin();
void loop();

void setWifi(const String& ssid, const String& pass);  // saved; applied after reboot
void printStatus(Print& out);

}  // namespace web
