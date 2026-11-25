#pragma once

// A monitor's identity that survives Windows renumbering its outputs.
//
// DXGI and GDI name outputs \.\DISPLAY1, \.\DISPLAY2, ... in the order
// Windows brings them up: a reboot, a driver update or a DisplayPort monitor
// waking from sleep can hand the same name to another screen. The monitor
// device path from QueryDisplayConfig names the physical monitor on its
// connector instead, so a choice saved with it keeps meaning that screen.

#include <string>

namespace clipture::platform {

// "monitor:" + the lower-case monitor device path of the output whose GDI name
// is `gdiName` (\.\DISPLAYn); empty when Windows reports none.
std::string stableMonitorId(const wchar_t* gdiName);

// True for ids made by stableMonitorId; anything else is a legacy GDI name or
// "primary".
bool isStableMonitorId(const std::string& id);

// Builds the id from a device path; split out so the format is testable.
std::string stableMonitorIdFromDevicePath(const std::wstring& devicePath);

}  // namespace clipture::platform
