#include "webview_bridge.h"

#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_result_functions.h>

#include <atomic>
#include <format>

#include "engine_availability.h"

namespace {
constexpr auto kErrorInvalidArgs = "invalidArguments";

constexpr auto kMethodLoadUrl = "loadUrl";
constexpr auto kMethodLoadStringContent = "loadStringContent";
constexpr auto kMethodReload = "reload";
constexpr auto kMethodStop = "stop";
constexpr auto kMethodGoBack = "goBack";
constexpr auto kMethodGoForward = "goForward";
constexpr auto kMethodAddScriptToExecuteOnDocumentCreated =
    "addScriptToExecuteOnDocumentCreated";
constexpr auto kMethodRemoveScriptToExecuteOnDocumentCreated =
    "removeScriptToExecuteOnDocumentCreated";
constexpr auto kMethodExecuteScript = "executeScript";
constexpr auto kMethodPostWebMessage = "postWebMessage";
constexpr auto kMethodSetSize = "setSize";
constexpr auto kMethodSetVisible = "setVisible";
constexpr auto kMethodSetParentWindow = "setParentWindow";
constexpr auto kMethodSetUserAgent = "setUserAgent";
constexpr auto kMethodSetBackgroundColor = "setBackgroundColor";
constexpr auto kMethodSetZoomFactor = "setZoomFactor";
constexpr auto kMethodSetShowFpsOverlay = "setShowFpsOverlay";
constexpr auto kMethodOpenDevTools = "openDevTools";
constexpr auto kMethodSuspend = "suspend";
constexpr auto kMethodResume = "resume";
constexpr auto kMethodSetVirtualHostNameMapping = "setVirtualHostNameMapping";
constexpr auto kMethodClearVirtualHostNameMapping =
    "clearVirtualHostNameMapping";
constexpr auto kMethodClearCookies = "clearCookies";
constexpr auto kMethodSetCookie = "setCookie";
constexpr auto kMethodGetCookies = "getCookies";
constexpr auto kMethodClearCache = "clearCache";
constexpr auto kMethodSetCacheDisabled = "setCacheDisabled";
constexpr auto kMethodSetPopupWindowPolicy = "setPopupWindowPolicy";
constexpr auto kMethodSetNavigationBlocklist = "setNavigationBlocklist";
constexpr auto kMethodSetNewWindowDelegateEnabled =
    "setNewWindowDelegateEnabled";
constexpr auto kMethodSetInterceptedAcceleratorKeys =
    "setInterceptedAcceleratorKeys";
constexpr auto kMethodRequestFocus = "requestFocus";

constexpr auto kEventType = "type";
constexpr auto kEventValue = "value";

constexpr auto kErrorNotSupported = "not_supported";
constexpr auto kScriptFailed = "script_failed";
constexpr auto kMethodFailed = "method_failed";

static const std::optional<std::vector<WebviewAcceleratorKey>>
GetAcceleratorKeys(const flutter::EncodableValue* args) {
  const auto* list = std::get_if<flutter::EncodableList>(args);
  if (!list) {
    return std::nullopt;
  }

  std::vector<WebviewAcceleratorKey> keys;
  keys.reserve(list->size());
  for (const auto& value : *list) {
    const auto* map = std::get_if<flutter::EncodableMap>(&value);
    if (!map) {
      return std::nullopt;
    }
    const auto virtual_key = map->find(flutter::EncodableValue("virtualKey"));
    const auto control = map->find(flutter::EncodableValue("control"));
    const auto shift = map->find(flutter::EncodableValue("shift"));
    const auto alt = map->find(flutter::EncodableValue("alt"));
    if (virtual_key == map->end() || control == map->end() ||
        shift == map->end() || alt == map->end()) {
      return std::nullopt;
    }
    const auto virtual_key_value = std::get_if<int32_t>(&virtual_key->second);
    const auto control_value = std::get_if<bool>(&control->second);
    const auto shift_value = std::get_if<bool>(&shift->second);
    const auto alt_value = std::get_if<bool>(&alt->second);
    if (!virtual_key_value || !control_value || !shift_value || !alt_value ||
        *virtual_key_value < 0 || *virtual_key_value > 0xFFFF) {
      return std::nullopt;
    }
    keys.push_back({static_cast<UINT>(*virtual_key_value), *control_value,
                    *shift_value, *alt_value});
  }
  return keys;
}

static const std::optional<std::vector<std::string>> GetStringListFromMap(
    const flutter::EncodableMap& map, const std::string& key) {
  const auto it = map.find(flutter::EncodableValue(key));
  if (it == map.end()) {
    return std::vector<std::string>();
  }
  const auto* list = std::get_if<flutter::EncodableList>(&it->second);
  if (!list) {
    return std::nullopt;
  }
  std::vector<std::string> values;
  values.reserve(list->size());
  for (const auto& value : *list) {
    const auto str = std::get_if<std::string>(&value);
    if (!str) {
      return std::nullopt;
    }
    values.push_back(*str);
  }
  return values;
}

}  // namespace

namespace {
// A plain per-process counter for channel naming -- no longer sourced from
// flutter::TextureRegistrar, since this webview isn't registered as a
// Flutter texture anymore.
int64_t NextWebviewId() {
  static std::atomic<int64_t> next_id{0};
  return next_id.fetch_add(1);
}
}  // namespace

WebviewBridge::WebviewBridge(flutter::BinaryMessenger* messenger,
                             std::unique_ptr<Webview> webview)
    : webview_(std::move(webview)), webview_id_(NextWebviewId()) {
  const auto method_channel_name =
      std::format("io.jns.webview.win/{}", webview_id_);
  method_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          messenger, method_channel_name,
          &flutter::StandardMethodCodec::GetInstance());
  method_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    HandleMethodCall(call, std::move(result));
  });

  const auto event_channel_name =
      std::format("io.jns.webview.win/{}/events", webview_id_);
  event_channel_ =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          messenger, event_channel_name,
          &flutter::StandardMethodCodec::GetInstance());

  auto handler = std::make_unique<
      flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
      [this](const flutter::EncodableValue* arguments,
             std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&&
                 events) {
        event_sink_ = std::move(events);
        RegisterEventHandlers();
        return nullptr;
      },
      [this](const flutter::EncodableValue* arguments) {
        event_sink_ = nullptr;
        return nullptr;
      });

  event_channel_->SetStreamHandler(std::move(handler));
}

WebviewBridge::~WebviewBridge() = default;

void WebviewBridge::Dispose(std::function<void()> completion) {
  method_channel_->SetMethodCallHandler(nullptr);
  event_channel_->SetStreamHandler(nullptr);
  event_sink_.reset();
  webview_->Close();
  completion();
}

void WebviewBridge::RegisterEventHandlers() {
  webview_->OnUrlChanged([this](const std::string& url) {
    const auto event = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kEventType),
         flutter::EncodableValue("urlChanged")},
        {flutter::EncodableValue(kEventValue), flutter::EncodableValue(url)},
    });
    EmitEvent(event);
  });

  webview_->OnLoadError([this](COREWEBVIEW2_WEB_ERROR_STATUS web_status) {
    const auto event = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kEventType),
         flutter::EncodableValue("onLoadError")},
        {flutter::EncodableValue(kEventValue),
         flutter::EncodableValue(static_cast<int>(web_status))},
    });
    EmitEvent(event);
  });

  webview_->OnLoadingStateChanged([this](WebviewLoadingState state) {
    const auto event = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kEventType),
         flutter::EncodableValue("loadingStateChanged")},
        {flutter::EncodableValue(kEventValue),
         flutter::EncodableValue(static_cast<int>(state))},
    });
    EmitEvent(event);
  });

  webview_->OnDownloadEvent([this](WebviewDownloadEvent webviewDownloadEvent) {
    const auto event = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kEventType),
         flutter::EncodableValue("downloadEvent")},
        {flutter::EncodableValue(kEventValue),
         flutter::EncodableValue(flutter::EncodableMap{
             {flutter::EncodableValue("kind"),
              flutter::EncodableValue(
                  static_cast<int>(webviewDownloadEvent.kind))},
             {flutter::EncodableValue("url"),
              flutter::EncodableValue(webviewDownloadEvent.url)},
             {flutter::EncodableValue("resultFilePath"),
              flutter::EncodableValue(webviewDownloadEvent.resultFilePath)},
             {flutter::EncodableValue("bytesReceived"),
              flutter::EncodableValue(webviewDownloadEvent.bytesReceived)},
             {flutter::EncodableValue("totalBytesToReceive"),
              flutter::EncodableValue(
                  webviewDownloadEvent.totalBytesToReceive)},
         })}});
    EmitEvent(event);
  });

  webview_->OnHistoryChanged([this](WebviewHistoryChanged historyChanged) {
    const auto event = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kEventType),
         flutter::EncodableValue("historyChanged")},
        {flutter::EncodableValue(kEventValue),
         flutter::EncodableValue(flutter::EncodableMap{
             {flutter::EncodableValue("canGoBack"),
              flutter::EncodableValue(
                  static_cast<bool>(historyChanged.can_go_back))},
             {flutter::EncodableValue("canGoForward"),
              flutter::EncodableValue(
                  static_cast<bool>(historyChanged.can_go_forward))},
         })},
    });
    EmitEvent(event);
  });

  webview_->OnDevtoolsProtocolEvent([this](const std::string& json) {
    const auto event = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kEventType),
         flutter::EncodableValue("securityStateChanged")},
        {flutter::EncodableValue(kEventValue), flutter::EncodableValue(json)}});
    EmitEvent(event);
  });

  webview_->OnDocumentTitleChanged([this](const std::string& title) {
    const auto event = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kEventType),
         flutter::EncodableValue("titleChanged")},
        {flutter::EncodableValue(kEventValue), flutter::EncodableValue(title)},
    });
    EmitEvent(event);
  });

  webview_->OnWebMessageReceived([this](const std::string& message) {
    const auto event = flutter::EncodableValue(
        flutter::EncodableMap{{flutter::EncodableValue(kEventType),
                               flutter::EncodableValue("webMessageReceived")},
                              {flutter::EncodableValue(kEventValue), message}});
    EmitEvent(event);
  });

  webview_->OnPermissionRequested(
      [this](const std::string& url, WebviewPermissionKind kind,
             bool is_user_initiated,
             Webview::WebviewPermissionRequestedCompleter completer) {
        OnPermissionRequested(url, kind, is_user_initiated, completer);
      });

  webview_->OnNewWindowRequested(
      [this](const std::string& url, bool is_user_initiated,
             WebviewKeyModifiers modifiers,
             Webview::NewWindowRequestedCompleter completer) {
        OnNewWindowRequested(url, is_user_initiated, modifiers,
                             std::move(completer));
      });

  webview_->OnNavigationBlocked([this](const std::string& url,
                                       bool isUserInitiated,
                                       bool isRedirected) {
    const auto event = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue(kEventType),
         flutter::EncodableValue("navigationBlocked")},
        {flutter::EncodableValue(kEventValue),
         flutter::EncodableValue(flutter::EncodableMap{
             {flutter::EncodableValue("url"), flutter::EncodableValue(url)},
             {flutter::EncodableValue("isUserInitiated"),
              flutter::EncodableValue(isUserInitiated)},
             {flutter::EncodableValue("isRedirected"),
              flutter::EncodableValue(isRedirected)},
         })}});
    EmitEvent(event);
  });

  webview_->OnContainsFullScreenElementChanged(
      [this](bool contains_fullscreen_element) {
        const auto event = flutter::EncodableValue(flutter::EncodableMap{
            {flutter::EncodableValue(kEventType),
             flutter::EncodableValue("containsFullScreenElementChanged")},
            {flutter::EncodableValue(kEventValue),
             contains_fullscreen_element}});
        EmitEvent(event);
      });

  webview_->OnAcceleratorKeyPressed(
      [this](WebviewAcceleratorKeyEvent accelerator_event) {
        const char* kind = "keyDown";
        switch (accelerator_event.kind) {
          case COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP:
            kind = "keyUp";
            break;
          case COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN:
            kind = "systemKeyDown";
            break;
          case COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_UP:
            kind = "systemKeyUp";
            break;
          case COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN:
          default:
            break;
        }
        const auto event = flutter::EncodableValue(flutter::EncodableMap{
            {flutter::EncodableValue(kEventType),
             flutter::EncodableValue("acceleratorKeyPressed")},
            {flutter::EncodableValue(kEventValue),
             flutter::EncodableValue(flutter::EncodableMap{
                 {flutter::EncodableValue("virtualKey"),
                  flutter::EncodableValue(
                      static_cast<int32_t>(accelerator_event.virtual_key))},
                 {flutter::EncodableValue("kind"), flutter::EncodableValue(kind)},
                 {flutter::EncodableValue("control"),
                  flutter::EncodableValue(accelerator_event.control)},
                 {flutter::EncodableValue("shift"),
                  flutter::EncodableValue(accelerator_event.shift)},
                 {flutter::EncodableValue("alt"),
                  flutter::EncodableValue(accelerator_event.alt)},
                 {flutter::EncodableValue("wasKeyDown"),
                  flutter::EncodableValue(accelerator_event.was_key_down)},
                 {flutter::EncodableValue("isKeyReleased"),
                  flutter::EncodableValue(accelerator_event.is_key_released)},
             })}});
        EmitEvent(event);
      });
}

void WebviewBridge::OnPermissionRequested(
    const std::string& url,
    WebviewPermissionKind permissionKind,
    bool isUserInitiated,
    Webview::WebviewPermissionRequestedCompleter completer) {
  auto args = std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
      {"url", url},
      {"isUserInitiated", isUserInitiated},
      {"permissionKind", static_cast<int>(permissionKind)}});

  method_channel_->InvokeMethod(
      "permissionRequested", std::move(args),
      std::make_unique<flutter::MethodResultFunctions<flutter::EncodableValue>>(
          [completer](const flutter::EncodableValue* result) {
            auto allow = std::get_if<bool>(result);
            if (allow != nullptr) {
              return completer(*allow ? WebviewPermissionState::Allow
                                      : WebviewPermissionState::Deny);
            }
            completer(WebviewPermissionState::Default);
          },
          [completer](const std::string& error_code,
                      const std::string& error_message,
                      const flutter::EncodableValue* error_details) {
            completer(WebviewPermissionState::Default);
          },
          [completer]() { completer(WebviewPermissionState::Default); }));
}

void WebviewBridge::OnNewWindowRequested(
    const std::string& url, bool is_user_initiated,
    WebviewKeyModifiers modifiers,
    Webview::NewWindowRequestedCompleter completer) {
  auto args = std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
      {"url", url},
      {"isUserInitiated", is_user_initiated},
      {"modifiers",
       flutter::EncodableValue(flutter::EncodableMap{
           {"ctrl", modifiers.control},
           {"shift", modifiers.shift},
           {"alt", modifiers.alt},
       })}});

  method_channel_->InvokeMethod(
      "newWindowRequested", std::move(args),
      std::make_unique<flutter::MethodResultFunctions<flutter::EncodableValue>>(
          [completer](const flutter::EncodableValue* result) {
            const auto decision = std::get_if<int32_t>(result);
            // WebviewNavigationDecision: allow = 0, reject = 1.
            completer(decision == nullptr || *decision == 0);
          },
          [completer](const std::string& error_code,
                      const std::string& error_message,
                      const flutter::EncodableValue* error_details) {
            completer(true);
          },
          [completer]() { completer(true); }));
}

void WebviewBridge::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto& method_name = method_call.method_name();

  // setInterceptedAcceleratorKeys: List<{virtualKey, control, shift, alt}>
  if (method_name.compare(kMethodSetInterceptedAcceleratorKeys) == 0) {
    auto keys = GetAcceleratorKeys(method_call.arguments());
    if (!keys) {
      return result->Error(kErrorInvalidArgs);
    }
    webview_->SetInterceptedAcceleratorKeys(std::move(*keys));
    return result->Success();
  }

  // requestFocus
  if (method_name.compare(kMethodRequestFocus) == 0) {
    if (webview_->RequestFocus()) {
      return result->Success();
    }
    return result->Error(kMethodFailed, "Requesting focus failed.");
  }

  // setSize: [double x, double y, double width, double height, double scale_factor]
  // x/y are relative to the parent window's client area -- see
  // Webview::SetBounds.
  if (method_name.compare(kMethodSetSize) == 0) {
    const flutter::EncodableList* list =
        std::get_if<flutter::EncodableList>(method_call.arguments());
    if (!list || list->size() != 5) {
      return result->Error(kErrorInvalidArgs);
    }
    const auto x = std::get_if<double>(&(*list)[0]);
    const auto y = std::get_if<double>(&(*list)[1]);
    const auto width = std::get_if<double>(&(*list)[2]);
    const auto height = std::get_if<double>(&(*list)[3]);
    const auto scale_factor = std::get_if<double>(&(*list)[4]);
    if (x && y && width && height && scale_factor) {
      webview_->SetBounds(static_cast<long>(*x), static_cast<long>(*y),
                          static_cast<size_t>(*width),
                          static_cast<size_t>(*height),
                          static_cast<float>(*scale_factor));
      return result->Success();
    }
    return result->Error(kErrorInvalidArgs);
  }

  // setVisible: bool
  if (method_name.compare(kMethodSetVisible) == 0) {
    if (const auto visible = std::get_if<bool>(method_call.arguments())) {
      webview_->SetVisible(*visible);
      return result->Success();
    }
    return result->Error(kErrorInvalidArgs);
  }

  // setParentWindow: int (raw HWND value, e.g. a native window's handle
  // reinterpret_cast through intptr_t to an int64 on the Dart side). The
  // standard codec encodes a small Dart int as int32 rather than int64 --
  // window handles are often small enough to hit this -- so both must be
  // accepted or the call fails with invalidArguments for small HWNDs.
  if (method_name.compare(kMethodSetParentWindow) == 0) {
    std::optional<int64_t> handle;
    if (const auto handle64 = std::get_if<int64_t>(method_call.arguments())) {
      handle = *handle64;
    } else if (const auto handle32 =
                   std::get_if<int32_t>(method_call.arguments())) {
      handle = *handle32;
    }
    if (handle) {
      const auto new_parent =
          reinterpret_cast<HWND>(static_cast<intptr_t>(*handle));
      return result->Success(webview_->SetParentWindow(new_parent));
    }
    return result->Error(kErrorInvalidArgs);
  }

  // loadUrl: string
  if (method_name.compare(kMethodLoadUrl) == 0) {
    if (const auto url = std::get_if<std::string>(method_call.arguments())) {
      webview_->LoadUrl(*url);
      return result->Success();
    }
    return result->Error(kErrorInvalidArgs);
  }

  // loadStringContent: string
  if (method_name.compare(kMethodLoadStringContent) == 0) {
    if (const auto content =
            std::get_if<std::string>(method_call.arguments())) {
      webview_->LoadStringContent(*content);
      return result->Success();
    }
    return result->Error(kErrorInvalidArgs);
  }

  // reload
  if (method_name.compare(kMethodReload) == 0) {
    if (webview_->Reload()) {
      return result->Success();
    }
    return result->Error(kMethodFailed);
  }

  // stop
  if (method_name.compare(kMethodStop) == 0) {
    if (webview_->Stop()) {
      return result->Success();
    }
    return result->Error(kMethodFailed);
  }

  // goBack
  if (method_name.compare(kMethodGoBack) == 0) {
    if (webview_->GoBack()) {
      return result->Success();
    }
    return result->Error(kMethodFailed);
  }

  // goForward
  if (method_name.compare(kMethodGoForward) == 0) {
    if (webview_->GoForward()) {
      return result->Success();
    }
    return result->Error(kMethodFailed);
  }

  // suspend
  if (method_name.compare(kMethodSuspend) == 0) {
    webview_->Suspend();
    return result->Success();
  }

  // resume
  if (method_name.compare(kMethodResume) == 0) {
    webview_->Resume();
    return result->Success();
  }

  // setVirtualHostNameMapping [string hostName, string path, int accessKind]
  if (method_name.compare(kMethodSetVirtualHostNameMapping) == 0) {
    const flutter::EncodableList* list =
        std::get_if<flutter::EncodableList>(method_call.arguments());
    if (!list || list->size() != 3) {
      return result->Error(kErrorInvalidArgs);
    }

    const auto hostName = std::get_if<std::string>(&(*list)[0]);
    const auto path = std::get_if<std::string>(&(*list)[1]);
    const auto accessKind = std::get_if<int32_t>(&(*list)[2]);

    if (hostName && path && accessKind) {
      webview_->SetVirtualHostNameMapping(
          *hostName, *path,
          static_cast<WebviewHostResourceAccessKind>(*accessKind));
      return result->Success();
    }
    return result->Error(kErrorInvalidArgs);
  }

  // clearVirtualHostNameMapping: string
  if (method_name.compare(kMethodClearVirtualHostNameMapping) == 0) {
    if (const auto hostName =
            std::get_if<std::string>(method_call.arguments())) {
      if (webview_->ClearVirtualHostNameMapping(*hostName)) {
        return result->Success();
      }
    }
    return result->Error(kErrorInvalidArgs);
  }

  if (method_name.compare(kMethodAddScriptToExecuteOnDocumentCreated) == 0) {
    if (const auto script = std::get_if<std::string>(method_call.arguments())) {
      std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
          shared_result = std::move(result);

      webview_->AddScriptToExecuteOnDocumentCreated(
          *script, [shared_result](bool success, const std::string& script_id) {
            if (success) {
              shared_result->Success(script_id);
            } else {
              shared_result->Error(kScriptFailed, "Executing script failed.");
            }
          });
      return;
    }
    return result->Error(kErrorInvalidArgs);
  }

  if (method_name.compare(kMethodRemoveScriptToExecuteOnDocumentCreated) == 0) {
    if (const auto script_id =
            std::get_if<std::string>(method_call.arguments())) {
      std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
          shared_result = std::move(result);

      webview_->RemoveScriptToExecuteOnDocumentCreated(*script_id);
      shared_result->Success();
      return;
    }
    return result->Error(kErrorInvalidArgs);
  }

  // executeScript: string
  if (method_name.compare(kMethodExecuteScript) == 0) {
    if (const auto script = std::get_if<std::string>(method_call.arguments())) {
      std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
          shared_result = std::move(result);

      webview_->ExecuteScript(
          *script,
          [shared_result](bool success, const std::string& json_result) {
            if (success) {
              shared_result->Success(json_result);
            } else {
              shared_result->Error(kScriptFailed, "Executing script failed.");
            }
          });
      return;
    }
    return result->Error(kErrorInvalidArgs);
  }

  // postWebMessage: string
  if (method_name.compare(kMethodPostWebMessage) == 0) {
    if (const auto message =
            std::get_if<std::string>(method_call.arguments())) {
      if (webview_->PostWebMessage(*message)) {
        return result->Success();
      }
      return result->Error(kErrorNotSupported, "Posting the message failed.");
    }
    return result->Error(kErrorInvalidArgs);
  }

  // setUserAgent: string
  if (method_name.compare(kMethodSetUserAgent) == 0) {
    if (const auto user_agent =
            std::get_if<std::string>(method_call.arguments())) {
      if (webview_->SetUserAgent(*user_agent)) {
        return result->Success();
      }
      return result->Error(kErrorNotSupported,
                           "Setting the user agent failed.");
    }
    return result->Error(kErrorInvalidArgs);
  }

  // setBackgroundColor: int
  if (method_name.compare(kMethodSetBackgroundColor) == 0) {
    if (const auto color = std::get_if<int32_t>(method_call.arguments())) {
      if (webview_->SetBackgroundColor(*color)) {
        return result->Success();
      }
      return result->Error(kErrorNotSupported,
                           "Setting the background color failed.");
    }
    return result->Error(kErrorInvalidArgs);
  }

  // setZoomFactor: double
  if (method_name.compare(kMethodSetZoomFactor) == 0) {
    if (const auto factor = std::get_if<double>(method_call.arguments())) {
      if (webview_->SetZoomFactor(*factor)) {
        return result->Success();
      }
      return result->Error(kErrorNotSupported,
                           "Setting the zoom factor failed.");
    }
    return result->Error(kErrorInvalidArgs);
  }

  // setShowFpsOverlay: bool
  if (method_name.compare(kMethodSetShowFpsOverlay) == 0) {
    if (const auto show = std::get_if<bool>(method_call.arguments())) {
      if (webview_->SetShowFpsOverlay(*show)) {
        return result->Success();
      }
      return result->Error(kErrorNotSupported,
                           "Setting the FPS overlay failed.");
    }
    return result->Error(kErrorInvalidArgs);
  }

  // openDevTools
  if (method_name.compare(kMethodOpenDevTools) == 0) {
    if (webview_->OpenDevTools()) {
      return result->Success();
    }
    return result->Error(kMethodFailed);
  }

  // clearCookies
  if (method_name.compare(kMethodClearCookies) == 0) {
    if (webview_->ClearCookies()) {
      return result->Success();
    }
    return result->Error(kMethodFailed);
  }

  // setCookie: {"name": string, "value": string, "domain": string,
  //             "path": string, "secure": bool, "httpOnly": bool,
  //             "expires": double (optional, seconds since epoch UTC)}
  if (method_name.compare(kMethodSetCookie) == 0) {
    const auto* map = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!map) {
      return result->Error(kErrorInvalidArgs);
    }

    const auto name = map->find(flutter::EncodableValue("name"));
    const auto cookieValue = map->find(flutter::EncodableValue("value"));
    const auto domain = map->find(flutter::EncodableValue("domain"));
    const auto path = map->find(flutter::EncodableValue("path"));
    const auto secure = map->find(flutter::EncodableValue("secure"));
    const auto httpOnly = map->find(flutter::EncodableValue("httpOnly"));

    if (name != map->end() && cookieValue != map->end() &&
        domain != map->end() && path != map->end() && secure != map->end() &&
        httpOnly != map->end()) {
      const auto nameValue = std::get_if<std::string>(&name->second);
      const auto cookieValueValue = std::get_if<std::string>(&cookieValue->second);
      const auto domainValue = std::get_if<std::string>(&domain->second);
      const auto pathValue = std::get_if<std::string>(&path->second);
      const auto secureValue = std::get_if<bool>(&secure->second);
      const auto httpOnlyValue = std::get_if<bool>(&httpOnly->second);

      if (nameValue && cookieValueValue && domainValue && pathValue &&
          secureValue && httpOnlyValue) {
        std::optional<double> expires;
        const auto expiresIt = map->find(flutter::EncodableValue("expires"));
        if (expiresIt != map->end()) {
          if (const auto expiresValue = std::get_if<double>(&expiresIt->second)) {
            expires = *expiresValue;
          }
        }

        std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
            shared_result = std::move(result);
        webview_->SetCookie(*nameValue, *cookieValueValue, *domainValue,
                            *pathValue, *secureValue, *httpOnlyValue, expires,
                            [shared_result](bool success) {
                              if (success) {
                                shared_result->Success();
                              } else {
                                shared_result->Error(
                                    kMethodFailed, "Setting the cookie failed.");
                              }
                            });
        return;
      }
    }
    return result->Error(kErrorInvalidArgs);
  }

  // getCookies: string (URL to scope cookies to; empty/null for all cookies)
  if (method_name.compare(kMethodGetCookies) == 0) {
    const auto uri = std::get_if<std::string>(method_call.arguments());
    std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
        shared_result = std::move(result);

    webview_->GetCookies(
        uri ? *uri : std::string(),
        [shared_result](bool success, std::vector<WebviewCookie> cookies) {
          if (!success) {
            shared_result->Error(kMethodFailed, "Getting cookies failed.");
            return;
          }
          flutter::EncodableList list;
          list.reserve(cookies.size());
          for (const auto& cookie : cookies) {
            list.push_back(flutter::EncodableValue(flutter::EncodableMap{
                {flutter::EncodableValue("name"),
                 flutter::EncodableValue(cookie.name)},
                {flutter::EncodableValue("value"),
                 flutter::EncodableValue(cookie.value)},
                {flutter::EncodableValue("domain"),
                 flutter::EncodableValue(cookie.domain)},
                {flutter::EncodableValue("path"),
                 flutter::EncodableValue(cookie.path)},
                {flutter::EncodableValue("httpOnly"),
                 flutter::EncodableValue(cookie.http_only)},
                {flutter::EncodableValue("secure"),
                 flutter::EncodableValue(cookie.secure)},
                {flutter::EncodableValue("session"),
                 flutter::EncodableValue(cookie.session)},
                {flutter::EncodableValue("expires"),
                 flutter::EncodableValue(cookie.expires)},
            }));
          }
          shared_result->Success(flutter::EncodableValue(list));
        });
    return;
  }

  // clearCache
  if (method_name.compare(kMethodClearCache) == 0) {
    if (webview_->ClearCache()) {
      return result->Success();
    }
    return result->Error(kMethodFailed);
  }

  // setCacheDisabled: bool
  if (method_name.compare(kMethodSetCacheDisabled) == 0) {
    if (const auto disabled = std::get_if<bool>(method_call.arguments())) {
      if (webview_->SetCacheDisabled(*disabled)) {
        return result->Success();
      }
    }
    return result->Error(kErrorInvalidArgs);
  }

  // setPopupWindowPolicy: {"policy": int, "showAddressBar": bool,
  //                         "addressBarHiddenUrlPatterns": List<String>}
  if (method_name.compare(kMethodSetPopupWindowPolicy) == 0) {
    const auto* map = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!map) {
      return result->Error(kErrorInvalidArgs);
    }

    const auto policy_it = map->find(flutter::EncodableValue("policy"));
    const auto show_bar_it =
        map->find(flutter::EncodableValue("showAddressBar"));
    if (policy_it == map->end() || show_bar_it == map->end()) {
      return result->Error(kErrorInvalidArgs);
    }
    const auto index = std::get_if<int32_t>(&policy_it->second);
    const auto show_address_bar = std::get_if<bool>(&show_bar_it->second);
    auto hidden_patterns =
        GetStringListFromMap(*map, "addressBarHiddenUrlPatterns");
    if (!index || !show_address_bar || !hidden_patterns) {
      return result->Error(kErrorInvalidArgs);
    }

    WebviewPopupWindowPolicy policy;
    switch (*index) {
      case 1:
        policy = WebviewPopupWindowPolicy::Deny;
        break;
      case 2:
        policy = WebviewPopupWindowPolicy::ShowInSameWindow;
        break;
      default:
        policy = WebviewPopupWindowPolicy::Allow;
        break;
    }
    webview_->SetPopupWindowPolicy(policy, *show_address_bar,
                                   std::move(*hidden_patterns));
    return result->Success();
  }

  // setNavigationBlocklist: {"exactUrls": List<String>,
  //                          "urlPrefixes": List<String>}
  if (method_name.compare(kMethodSetNavigationBlocklist) == 0) {
    const auto* map =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!map) {
      return result->Error(kErrorInvalidArgs);
    }

    auto exact_urls = GetStringListFromMap(*map, "exactUrls");
    auto url_prefixes = GetStringListFromMap(*map, "urlPrefixes");
    if (!exact_urls || !url_prefixes) {
      return result->Error(kErrorInvalidArgs);
    }

    webview_->SetNavigationBlocklist(std::move(*exact_urls),
                                     std::move(*url_prefixes));
    return result->Success();
  }

  if (method_name.compare(kMethodSetNewWindowDelegateEnabled) == 0) {
    const auto enabled = std::get_if<bool>(method_call.arguments());
    if (!enabled) {
      return result->Error(kErrorInvalidArgs);
    }
    webview_->SetNewWindowDelegateEnabled(*enabled);
    return result->Success();
  }

  result->NotImplemented();
}
