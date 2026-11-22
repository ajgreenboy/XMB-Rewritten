// Phase 1 Linux port: no-op stubs for the Windows-only DesktopPadDriver delivery
// half of desktop mode. The pure Mapper and its tests live in desktop_pad.cpp and
// are cross-platform; the driver that reads XInput pads and injects input through
// SendInput is Windows-only (desktop_pad_win.cpp, kept in the Windows build).
// These stubs let the shell construct a driver and call setActive() without any
// pad input or injection on non-Windows platforms. Nothing here is ever a real pad.
#include "desktop_pad.h"

#include <utility>

namespace xmb::desktoppad {

DesktopPadDriver::DesktopPadDriver(Hooks hooks, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)) {}

DesktopPadDriver::~DesktopPadDriver() = default;

void DesktopPadDriver::setActive(bool active) {
    active_ = active;
}

quint64 DesktopPadDriver::oskPid() const { return 0; }

void DesktopPadDriver::toggleKeyboard() {}

void DesktopPadDriver::deliver(const std::vector<Action>&) {}

// CLI delivery probes are Windows-only; report "did nothing" (success) on other platforms.
int runPadSink(const QString&, int) { return 0; }
int runPadInject(const QString&, const QString&) { return 0; }
int runKeyboardCheck(const QString&) { return 0; }

}  // namespace xmb::desktoppad
