#pragma once

#include <flutter/event_channel.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <flutter/texture_registrar.h>

#include <functional>
#include <memory>

#include "engine_availability.h"
#include "graphics_context.h"
#include "texture_bridge.h"
#include "webview.h"

class WebviewBridge {
 public:
  WebviewBridge(flutter::BinaryMessenger* messenger,
                flutter::TextureRegistrar* texture_registrar,
                GraphicsContext* graphics_context,
                std::unique_ptr<Webview> webview);
  ~WebviewBridge();

  // Stops event/capture activity and releases the Flutter texture only after
  // the engine confirms it is no longer in use.
  void Dispose(std::function<void()> completion);

  TextureBridge* texture_bridge() const { return texture_bridge_.get(); }

  int64_t texture_id() const { return texture_id_; }

 private:
  // Declared in reverse teardown order: Flutter texture must stop referring to
  // TextureBridge before TextureBridge and Webview are released.
  std::unique_ptr<Webview> webview_;
  std::unique_ptr<TextureBridge> texture_bridge_;
  std::unique_ptr<flutter::TextureVariant> flutter_texture_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      method_channel_;

  flutter::TextureRegistrar* texture_registrar_;
  int64_t texture_id_;

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
      Webview::NewWindowRequestedCompleter completer);
};
