#pragma once
#include <Arduino.h>

// The native USB port, presented to the printer as a USB flash drive.
namespace usbdrive {

void begin();
void loop();  // logs host connect/disconnect

// Soft-unplug / re-plug the drive. The printer sees a real removal;
// attach() re-plugs by restarting the device (clean USB enumeration).
void detach();
void attach();
bool attached();
bool restarting();

// True if the printer has read from the drive recently. Printers read the
// file layer by layer while printing, so this means "probably printing".
bool hostBusy();
uint32_t msSinceHostRead();  // UINT32_MAX if never

// Diagnostics: has a USB host enumerated us, and how much has it read.
bool hostConnected();
uint32_t sectorsRead();

}  // namespace usbdrive
