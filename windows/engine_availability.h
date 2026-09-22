#pragma once

#include <flutter_messenger.h>

#include <atomic>

namespace webview_windows {

// Flutter can begin destroying the engine before plugin destruction callbacks
// run. Keep an owned messenger reference solely to determine whether engine
// backed APIs (channels and textures) are still safe to use.
inline std::atomic<FlutterDesktopMessengerRef>& MessengerSlot() {
  static std::atomic<FlutterDesktopMessengerRef> messenger{nullptr};
  return messenger;
}

inline void CaptureMessengerForAvailabilityChecks(
    FlutterDesktopMessengerRef messenger) {
  if (!messenger) {
    return;
  }
  MessengerSlot().store(FlutterDesktopMessengerAddRef(messenger),
                        std::memory_order_release);
}

inline std::atomic<bool>& PluginAliveFlag() {
  static std::atomic<bool> alive{false};
  return alive;
}

inline void SetPluginAlive(bool value) {
  PluginAliveFlag().store(value, std::memory_order_release);
}

inline bool PluginAlive() {
  return PluginAliveFlag().load(std::memory_order_acquire);
}

inline bool EngineAvailable() {
  const auto messenger = MessengerSlot().load(std::memory_order_acquire);
  if (!messenger) {
    return true;
  }
  FlutterDesktopMessengerLock(messenger);
  const bool available = FlutterDesktopMessengerIsAvailable(messenger);
  FlutterDesktopMessengerUnlock(messenger);
  return available;
}

}  // namespace webview_windows
