#pragma once

#include <optional>
#include <string>

class WebviewPlatform {
 public:
  WebviewPlatform() = default;

  // Windowed WebView2 hosting has no special OS/graphics capability
  // requirement (unlike the former Windows.Graphics.Capture-based offscreen
  // path, which needed Windows 10 2004+); the WebView2 Runtime itself is
  // validated separately when the environment is created.
  bool IsSupported() { return true; }

  std::optional<std::wstring> GetDefaultDataDirectory();
};
