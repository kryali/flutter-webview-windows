#pragma once

#include <WebView2.h>
#include <wil/com.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class WebviewHost;

enum class WebviewLoadingState { None, Loading, NavigationCompleted };

enum class WebviewDownloadEventKind {
  DownloadStarted,
  DownloadCompleted,
  DownloadProgress
};

enum class WebviewPermissionKind {
  Unknown,
  Microphone,
  Camera,
  GeoLocation,
  Notifications,
  OtherSensors,
  ClipboardRead
};

enum class WebviewPermissionState { Default, Allow, Deny };

enum class WebviewPopupWindowPolicy { Allow, Deny, ShowInSameWindow };

enum class WebviewHostResourceAccessKind { Deny, Allow, DenyCors };

struct WebviewHistoryChanged {
  BOOL can_go_back;
  BOOL can_go_forward;
};

struct WebviewDownloadEvent {
  WebviewDownloadEventKind kind;
  std::string url;
  std::string resultFilePath;
  INT64 bytesReceived;
  INT64 totalBytesToReceive;
};

struct WebviewAcceleratorKey {
  UINT virtual_key;
  bool control;
  bool shift;
  bool alt;
};

struct WebviewAcceleratorKeyEvent {
  UINT virtual_key;
  COREWEBVIEW2_KEY_EVENT_KIND kind;
  bool control;
  bool shift;
  bool alt;
  bool was_key_down;
  bool is_key_released;
};

struct WebviewKeyModifiers {
  bool control;
  bool shift;
  bool alt;
};

struct WebviewCookie {
  std::string name;
  std::string value;
  std::string domain;
  std::string path;
  bool http_only;
  bool secure;
  bool session;
  // Seconds since Unix epoch (UTC); meaningless when session is true.
  double expires;
};

struct EventRegistrations {
  EventRegistrationToken source_changed_token_{};
  EventRegistrationToken content_loading_token_{};
  EventRegistrationToken navigation_completed_token_{};
  EventRegistrationToken history_changed_token_{};
  EventRegistrationToken document_title_changed_token_{};
  EventRegistrationToken got_focus_token_{};
  EventRegistrationToken lost_focus_token_{};
  EventRegistrationToken web_message_received_token_{};
  EventRegistrationToken permission_requested_token_{};
  EventRegistrationToken navigation_starting_token_{};
  EventRegistrationToken devtools_protocol_event_token_{};
  EventRegistrationToken new_windows_requested_token_{};
  EventRegistrationToken contains_fullscreen_element_changed_token_{};
  EventRegistrationToken download_starting_token_{};
  EventRegistrationToken download_bytes_received_token_{};
  EventRegistrationToken download_state_changed_token_{};
  EventRegistrationToken accelerator_key_pressed_token_{};
};

class Webview {
 public:
  friend class WebviewHost;

  typedef std::function<void(const std::string&)> UrlChangedCallback;
  typedef std::function<void(WebviewLoadingState)> LoadingStateChangedCallback;
  typedef std::function<void(COREWEBVIEW2_WEB_ERROR_STATUS)>
      OnLoadErrorCallback;
  typedef std::function<void(WebviewHistoryChanged)> HistoryChangedCallback;
  typedef std::function<void(const std::string&)> DevtoolsProtocolEventCallback;
  typedef std::function<void(const std::string&)> DocumentTitleChangedCallback;
  typedef std::function<void(bool)> FocusChangedCallback;
  typedef std::function<void(bool, const std::string&)>
      AddScriptToExecuteOnDocumentCreatedCallback;
  typedef std::function<void(bool, const std::string&)> ScriptExecutedCallback;
  typedef std::function<void(bool)> SetCookieCallback;
  typedef std::function<void(bool, std::vector<WebviewCookie>)>
      GetCookiesCallback;
  typedef std::function<void(const std::string&)> WebMessageReceivedCallback;
  typedef std::function<void(WebviewPermissionState state)>
      WebviewPermissionRequestedCompleter;
  typedef std::function<void(const std::string& url, WebviewPermissionKind kind,
                             bool is_user_initiated,
                             WebviewPermissionRequestedCompleter completer)>
      PermissionRequestedCallback;
  typedef std::function<void(const std::string& url, bool is_user_initiated,
                             bool is_redirected)>
      NavigationBlockedCallback;
  typedef std::function<void(bool allow)> NewWindowRequestedCompleter;
  typedef std::function<void(const std::string& url, bool is_user_initiated,
                             WebviewKeyModifiers modifiers,
                             NewWindowRequestedCompleter completer)>
      NewWindowRequestedCallback;
  typedef std::function<void(bool contains_fullscreen_element)>
      ContainsFullScreenElementChangedCallback;
  typedef std::function<void(WebviewDownloadEvent)> DownloadEventCallback;
  typedef std::function<void(WebviewAcceleratorKeyEvent)>
      AcceleratorKeyPressedCallback;

  ~Webview();

  // Synchronously stops event delivery and closes the WebView2 controller.
  // Safe to call more than once.
  void Close();

  bool IsValid() { return is_valid_; }

  // x/y are relative to hwnd_'s client area. Unlike the former offscreen
  // composition surface, bounds now describe where this webview actually
  // renders on screen, so both position and size must be kept in sync with
  // wherever the Flutter-side widget for it is laid out.
  void SetBounds(long x, long y, size_t width, size_t height,
                 float scale_factor);
  // Toggles native rendering without closing the controller -- used to keep
  // background tabs alive (e.g. behind an IndexedStack) while only the
  // active one is actually shown, since an invisible child controller isn't
  // hidden automatically the way an unpainted Flutter Texture would be.
  void SetVisible(bool visible);
  // Moves this webview to a different parent window (e.g. a tab dragged to
  // another top-level Flutter window), reusing WebView2's own documented
  // reparenting support rather than recreating the controller.
  bool SetParentWindow(HWND new_parent);
  void LoadUrl(const std::string& url);
  void LoadStringContent(const std::string& content);
  bool Stop();
  bool Reload();
  bool GoBack();
  bool GoForward();
  void AddScriptToExecuteOnDocumentCreated(
      const std::string& script,
      AddScriptToExecuteOnDocumentCreatedCallback callback);
  void RemoveScriptToExecuteOnDocumentCreated(const std::string& script_id);
  void ExecuteScript(const std::string& script,
                     ScriptExecutedCallback callback);
  bool PostWebMessage(const std::string& json);
  bool ClearCookies();
  // Uses ICoreWebView2CookieManager (the documented, versioned cookie API)
  // rather than the DevTools Protocol, which Microsoft does not guarantee
  // stable across WebView2 releases.
  void SetCookie(const std::string& name, const std::string& value,
                 const std::string& domain, const std::string& path,
                 bool secure, bool http_only, std::optional<double> expires,
                 SetCookieCallback callback);
  // uri may be empty to fetch all cookies rather than those for one URL.
  void GetCookies(const std::string& uri, GetCookiesCallback callback);
  bool ClearCache();
  bool SetCacheDisabled(bool disabled);
  // show_address_bar is the default for popups opened under
  // WebviewPopupWindowPolicy::Allow; address_bar_hidden_url_patterns are
  // regexes checked against each popup's target URL, and override
  // show_address_bar to false for any popup that matches one.
  void SetPopupWindowPolicy(
      WebviewPopupWindowPolicy policy, bool show_address_bar,
      std::vector<std::string> address_bar_hidden_url_patterns);
  // Cancels (before it ever renders) any navigation whose URL either
  // exactly matches an entry in exact_urls, or starts with an entry in
  // url_prefixes (each of which must end in '/', so a subtree block like
  // ".../admin/" can never accidentally match an unrelated URL that merely
  // starts with the same characters, e.g. ".../administrator"). WebView2's
  // NavigationStarting event doesn't support deferral, so this decision has
  // to be made synchronously and natively rather than round-tripped to Dart
  // per navigation.
  void SetNavigationBlocklist(std::vector<std::string> exact_urls,
                              std::vector<std::string> url_prefixes);
  void SetNewWindowDelegateEnabled(bool enabled) {
    new_window_delegate_enabled_ = enabled;
  }
  void SetInterceptedAcceleratorKeys(
      std::vector<WebviewAcceleratorKey> accelerator_keys);
  bool RequestFocus();
  bool SetUserAgent(const std::string& user_agent);
  bool OpenDevTools();
  bool SetBackgroundColor(int32_t color);
  bool SetZoomFactor(double factor);
  // Toggles Chromium's own built-in FPS counter overlay (the same one shown
  // by DevTools' Rendering pane), via the DevTools Protocol. Useful for
  // eyeballing whether the page itself is rendering smoothly -- windowed
  // hosting gives this plugin no visibility into WebView2's own frame
  // production the way the former capture pipeline did, so this asks
  // Chromium to show the number itself rather than trying to measure it.
  bool SetShowFpsOverlay(bool show);
  bool Suspend();
  bool Resume();

  bool SetVirtualHostNameMapping(const std::string& hostName,
                                 const std::string& path,
                                 WebviewHostResourceAccessKind accessKind);
  bool ClearVirtualHostNameMapping(const std::string& hostName);

  void UpdateDownloadProgress(ICoreWebView2DownloadOperation* download);

  void OnUrlChanged(UrlChangedCallback callback) {
    url_changed_callback_ = std::move(callback);
  }

  void OnLoadError(OnLoadErrorCallback callback) {
    on_load_error_callback_ = std::move(callback);
  }

  void OnLoadingStateChanged(LoadingStateChangedCallback callback) {
    loading_state_changed_callback_ = std::move(callback);
  }

  void OnDownloadEvent(DownloadEventCallback callback) {
    download_event_callback_ = std::move(callback);
  }

  void OnHistoryChanged(HistoryChangedCallback callback) {
    history_changed_callback_ = std::move(callback);
  }

  void OnDocumentTitleChanged(DocumentTitleChangedCallback callback) {
    document_title_changed_callback_ = std::move(callback);
  }

  void OnFocusChanged(FocusChangedCallback callback) {
    focus_changed_callback_ = std::move(callback);
  }

  void OnWebMessageReceived(WebMessageReceivedCallback callback) {
    web_message_received_callback_ = std::move(callback);
  }

  void OnPermissionRequested(PermissionRequestedCallback callback) {
    permission_requested_callback_ = std::move(callback);
  }

  void OnNavigationBlocked(NavigationBlockedCallback callback) {
    navigation_blocked_callback_ = std::move(callback);
  }

  void OnNewWindowRequested(NewWindowRequestedCallback callback) {
    new_window_requested_callback_ = std::move(callback);
  }

  void OnDevtoolsProtocolEvent(DevtoolsProtocolEventCallback callback) {
    devtools_protocol_event_callback_ = std::move(callback);
  }

  void OnContainsFullScreenElementChanged(
      ContainsFullScreenElementChangedCallback callback) {
    contains_fullscreen_element_changed_callback_ = std::move(callback);
  }

  void OnAcceleratorKeyPressed(AcceleratorKeyPressedCallback callback) {
    accelerator_key_pressed_callback_ = std::move(callback);
  }

 private:
  HWND hwnd_;
  bool owns_window_;
  bool is_valid_ = false;
  bool is_closed_ = false;
  float scale_factor_ = 1.0;
  wil::com_ptr<ICoreWebView2Controller3> webview_controller_;
  wil::com_ptr<ICoreWebView2> webview_;
  wil::com_ptr<ICoreWebView2DevToolsProtocolEventReceiver>
      devtools_protocol_event_receiver_;
  wil::com_ptr<ICoreWebView2Settings2> settings2_;
  WebviewPopupWindowPolicy popup_window_policy_ =
      WebviewPopupWindowPolicy::Allow;
  bool popup_window_show_address_bar_ = true;
  bool new_window_delegate_enabled_ = false;
  std::vector<std::string> popup_window_address_bar_hidden_url_patterns_;
  std::vector<std::string> navigation_blocklist_exact_urls_;
  std::vector<std::string> navigation_blocklist_url_prefixes_;
  std::vector<WebviewAcceleratorKey> intercepted_accelerator_keys_;

  WebviewHost* host_;
  EventRegistrations event_registrations_{};

  UrlChangedCallback url_changed_callback_;
  LoadingStateChangedCallback loading_state_changed_callback_;
  DownloadEventCallback download_event_callback_;
  OnLoadErrorCallback on_load_error_callback_;
  HistoryChangedCallback history_changed_callback_;
  DocumentTitleChangedCallback document_title_changed_callback_;
  FocusChangedCallback focus_changed_callback_;
  WebMessageReceivedCallback web_message_received_callback_;
  PermissionRequestedCallback permission_requested_callback_;
  NavigationBlockedCallback navigation_blocked_callback_;
  NewWindowRequestedCallback new_window_requested_callback_;
  DevtoolsProtocolEventCallback devtools_protocol_event_callback_;
  ContainsFullScreenElementChangedCallback
      contains_fullscreen_element_changed_callback_;
  AcceleratorKeyPressedCallback accelerator_key_pressed_callback_;

  Webview(wil::com_ptr<ICoreWebView2Controller> controller, WebviewHost* host,
         HWND hwnd, bool owns_window);

  void RegisterEventHandlers();
  void UnregisterEventHandlers();
  void ClearCallbacks();
  void EnableSecurityUpdates();
};
