#pragma once

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wil/com.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "webview.h"

struct WebviewCreationError {
  HRESULT hr;
  std::string message;

  explicit WebviewCreationError(HRESULT hr, std::string message)
      : hr(hr), message(message) {}

  static std::unique_ptr<WebviewCreationError> create(
      HRESULT hr, const std::string message) {
    return std::make_unique<WebviewCreationError>(hr, message);
  }
};

class WebviewHost {
 public:
  typedef std::function<void(std::unique_ptr<Webview>,
                             std::unique_ptr<WebviewCreationError>)>
      WebviewCreationCallback;
  typedef std::function<void(wil::com_ptr<ICoreWebView2Controller>,
                             std::unique_ptr<WebviewCreationError>)>
      ControllerCreationCallback;

  static std::unique_ptr<WebviewHost> Create(
      std::optional<std::wstring> user_data_directory = std::nullopt,
      std::optional<std::wstring> browser_exe_path = std::nullopt,
      std::optional<std::string> arguments = std::nullopt);

  // hwnd is the parent window the controller renders into (a sub-rectangle
  // of it, via Webview::SetBounds) -- typically Flutter's own top-level
  // window, shared by every tab's controller and multiplexed with
  // SetBounds/SetVisible rather than one native window per tab.
  void CreateWebview(HWND hwnd, bool owns_window,
                     WebviewCreationCallback callback);

  // Same environment used to create every Webview's controller, so popup
  // windows created by Webview share cookies/session with their opener.
  ICoreWebView2Environment3* environment() const { return webview_env_.get(); }

 private:
  wil::com_ptr<ICoreWebView2Environment3> webview_env_;

  explicit WebviewHost(wil::com_ptr<ICoreWebView2Environment3> webview_env);
  void CreateWebViewController(HWND hwnd, ControllerCreationCallback callback);
};
