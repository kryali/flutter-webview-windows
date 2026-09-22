#pragma once

#include <flutter/event_channel.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <functional>
#include <memory>

#include "engine_availability.h"
#include "webview.h"

class WebviewBridge {
 public:
  WebviewBridge(flutter::BinaryMessenger* messenger,
                std::unique_ptr<Webview> webview);
  ~WebviewBridge();

  // Stops event handling and closes the webview.
  void Dispose(std::function<void()> completion);

  int64_t webview_id() const { return webview_id_; }

 private:
  std::unique_ptr<Webview> webview_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      method_channel_;

  int64_t webview_id_;

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void RegisterEventHandlers();

  template <typename T>
  void EmitEvent(const T& value) {
    if (event_sink_ && webview_windows::PluginAlive()) {
      event_sink_->Success(value);
    }
  }

  void OnPermissionRequested(
      const std::string& url, WebviewPermissionKind permissionKind,
      bool is_user_initiated,
      Webview::WebviewPermissionRequestedCompleter completer);
  void OnNewWindowRequested(
      const std::string& url, bool is_user_initiated,
      WebviewKeyModifiers modifiers,
      Webview::NewWindowRequestedCompleter completer);
};
