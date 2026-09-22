import 'dart:async';
import 'dart:convert';
import 'dart:ui';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'enums.dart';

class HistoryChanged {
  final bool canGoBack;
  final bool canGoForward;
  const HistoryChanged(this.canGoBack, this.canGoForward);
}

class WebviewDownloadEvent {
  final WebviewDownloadEventKind kind;
  final String url;
  final String resultFilePath;
  final int bytesReceived;
  final int totalBytesToReceive;
  const WebviewDownloadEvent(
    this.kind,
    this.url,
    this.resultFilePath,
    this.bytesReceived,
    this.totalBytesToReceive,
  );
}

class NavigationBlockedEvent {
  final String url;
  final bool isUserInitiated;
  final bool isRedirected;
  const NavigationBlockedEvent(
    this.url,
    this.isUserInitiated,
    this.isRedirected,
  );
}

/// A virtual-key and exact modifier combination to intercept in WebView2.
class WebviewAcceleratorKey {
  const WebviewAcceleratorKey({
    required this.virtualKey,
    this.control = false,
    this.shift = false,
    this.alt = false,
  });

  final int virtualKey;
  final bool control;
  final bool shift;
  final bool alt;

  Map<String, dynamic> toJson() => <String, dynamic>{
        'virtualKey': virtualKey,
        'control': control,
        'shift': shift,
        'alt': alt,
      };
}

/// An intercepted WebView2 accelerator keyboard event.
class WebviewAcceleratorKeyEvent {
  const WebviewAcceleratorKeyEvent({
    required this.virtualKey,
    required this.kind,
    required this.control,
    required this.shift,
    required this.alt,
    required this.wasKeyDown,
    required this.isKeyReleased,
  });

  final int virtualKey;
  final WebviewAcceleratorKeyEventKind kind;
  final bool control;
  final bool shift;
  final bool alt;

  /// Whether this physical key was already down before this event.
  final bool wasKeyDown;

  /// Whether this event represents a key release.
  final bool isKeyReleased;
}

typedef PermissionRequestedDelegate
    = FutureOr<WebviewPermissionDecision> Function(
        String url, WebviewPermissionKind permissionKind, bool isUserInitiated);

/// Keyboard modifiers active when a WebView request was raised.
class WebviewKeyModifiers {
  const WebviewKeyModifiers({
    required this.ctrl,
    required this.shift,
    required this.alt,
  });

  final bool ctrl;
  final bool shift;
  final bool alt;
}

/// Information about a request to open a new browsing window.
///
/// WebView2 raises these requests for links with `target="_blank"`, calls to
/// `window.open()`, and browser gestures such as Ctrl-clicking a link.
class WebviewNewWindowRequest {
  const WebviewNewWindowRequest({
    required this.url,
    required this.isUserInitiated,
    this.modifiers = const WebviewKeyModifiers(
      ctrl: false,
      shift: false,
      alt: false,
    ),
  });

  final Uri url;
  final bool isUserInitiated;
  final WebviewKeyModifiers modifiers;
}

/// Decides whether WebView2 may handle a new-window request.
typedef NewWindowRequestedDelegate = FutureOr<WebviewNavigationDecision>
    Function(WebviewNewWindowRequest request);

typedef ScriptID = String;

const String _pluginChannelPrefix = 'io.jns.webview.win';
const MethodChannel _pluginChannel = MethodChannel(_pluginChannelPrefix);

WebviewAcceleratorKeyEventKind _acceleratorKeyEventKindFromChannel(
    dynamic value) {
  switch (value) {
    case 'keyDown':
      return WebviewAcceleratorKeyEventKind.keyDown;
    case 'keyUp':
      return WebviewAcceleratorKeyEventKind.keyUp;
    case 'systemKeyDown':
      return WebviewAcceleratorKeyEventKind.systemKeyDown;
    case 'systemKeyUp':
      return WebviewAcceleratorKeyEventKind.systemKeyUp;
    default:
      throw ArgumentError.value(value, 'kind', 'Unknown accelerator key kind');
  }
}

class WebviewValue {
  const WebviewValue({
    required this.isInitialized,
  });

  final bool isInitialized;

  WebviewValue copyWith({
    bool? isInitialized,
  }) {
    return WebviewValue(
      isInitialized: isInitialized ?? this.isInitialized,
    );
  }

  WebviewValue.uninitialized()
      : this(
          isInitialized: false,
        );
}

/// Controls a WebView and provides streams for various change events.
class WebviewController extends ValueNotifier<WebviewValue> {
  /// Explicitly initializes the underlying WebView environment
  /// using  an optional [browserExePath], an optional [userDataPath]
  /// and optional Chromium command line arguments [additionalArguments].
  ///
  /// The environment is shared between all WebviewController instances and
  /// can be initialized only once. Initialization must take place before any
  /// WebviewController is created/initialized.
  ///
  /// Throws [PlatformException] if the environment was initialized before.
  static Future<void> initializeEnvironment(
      {String? userDataPath,
      String? browserExePath,
      String? additionalArguments}) async {
    return _pluginChannel
        .invokeMethod('initializeEnvironment', <String, dynamic>{
      'userDataPath': userDataPath,
      'browserExePath': browserExePath,
      'additionalArguments': additionalArguments
    });
  }

  /// Get the browser version info including channel name if it is not the
  /// WebView2 Runtime.
  /// Returns [null] if the webview2 runtime is not installed.
  static Future<String?> getWebViewVersion() async {
    return _pluginChannel.invokeMethod<String>('getWebViewVersion');
  }

  late Completer<void> _creatingCompleter;
  int _webviewId = 0;
  bool _isDisposed = false;

  Future<void> get ready => _creatingCompleter.future;

  PermissionRequestedDelegate? _permissionRequested;
  NewWindowRequestedDelegate? _newWindowRequested;

  late MethodChannel _methodChannel;
  late EventChannel _eventChannel;
  StreamSubscription? _eventStreamSubscription;

  final StreamController<String> _urlStreamController =
      StreamController<String>();

  /// A stream reflecting the current URL.
  Stream<String> get url => _urlStreamController.stream;

  final StreamController<LoadingState> _loadingStateStreamController =
      StreamController<LoadingState>.broadcast();

  final StreamController<WebviewDownloadEvent> _downloadEventStreamController =
      StreamController<WebviewDownloadEvent>.broadcast();

  final StreamController<NavigationBlockedEvent>
      _navigationBlockedStreamController =
      StreamController<NavigationBlockedEvent>.broadcast();

  final StreamController<WebErrorStatus> _onLoadErrorStreamController =
      StreamController<WebErrorStatus>();

  /// A stream reflecting the current loading state.
  Stream<LoadingState> get loadingState => _loadingStateStreamController.stream;

  Stream<WebviewDownloadEvent> get onDownloadEvent =>
      _downloadEventStreamController.stream;

  /// A stream of navigations that were cancelled before they loaded because
  /// their URL matched a prefix passed to [setNavigationBlocklist].
  Stream<NavigationBlockedEvent> get onNavigationBlocked =>
      _navigationBlockedStreamController.stream;

  /// A stream reflecting the navigation error when navigation completed with an error.
  Stream<WebErrorStatus> get onLoadError => _onLoadErrorStreamController.stream;

  final StreamController<HistoryChanged> _historyChangedStreamController =
      StreamController<HistoryChanged>();

  /// A stream reflecting the current history state.
  Stream<HistoryChanged> get historyChanged =>
      _historyChangedStreamController.stream;

  final StreamController<String> _securityStateChangedStreamController =
      StreamController<String>();

  /// A stream reflecting the current security state.
  Stream<String> get securityStateChanged =>
      _securityStateChangedStreamController.stream;

  final StreamController<String> _titleStreamController =
      StreamController<String>();

  /// A stream reflecting the current document title.
  Stream<String> get title => _titleStreamController.stream;

  final StreamController<WebviewAcceleratorKeyEvent>
      _acceleratorKeyPressedStreamController =
      StreamController<WebviewAcceleratorKeyEvent>.broadcast();

  /// Emits events for registered accelerator keys.
  Stream<WebviewAcceleratorKeyEvent> get acceleratorKeyPressed =>
      _acceleratorKeyPressedStreamController.stream;

  final StreamController<dynamic> _webMessageStreamController =
      StreamController<dynamic>();

  Stream<dynamic> get webMessage => _webMessageStreamController.stream;

  final StreamController<bool>
      _containsFullScreenElementChangedStreamController =
      StreamController<bool>.broadcast();

  /// A stream reflecting whether the document currently contains full-screen elements.
  Stream<bool> get containsFullScreenElementChanged =>
      _containsFullScreenElementChangedStreamController.stream;

  WebviewController() : super(WebviewValue.uninitialized());

  /// Initializes the underlying platform view.
  Future<void> initialize() async {
    if (_isDisposed) {
      return Future<void>.value();
    }
    _creatingCompleter = Completer<void>();
    try {
      final reply =
          await _pluginChannel.invokeMapMethod<String, dynamic>('initialize');

      _webviewId = reply!['webviewId'];
      _methodChannel = MethodChannel('$_pluginChannelPrefix/$_webviewId');
      _eventChannel = EventChannel('$_pluginChannelPrefix/$_webviewId/events');
      _eventStreamSubscription =
          _eventChannel.receiveBroadcastStream().listen((event) {
        final map = event as Map<dynamic, dynamic>;
        switch (map['type']) {
          case 'urlChanged':
            _urlStreamController.add(map['value']);
            break;
          case 'onLoadError':
            final value = WebErrorStatus.values[map['value']];
            _onLoadErrorStreamController.add(value);
            break;
          case 'loadingStateChanged':
            final value = LoadingState.values[map['value']];
            _loadingStateStreamController.add(value);
            break;
          case 'downloadEvent':
            final value = WebviewDownloadEvent(
              WebviewDownloadEventKind.values[map['value']['kind']],
              map['value']['url'],
              map['value']['resultFilePath'],
              map['value']['bytesReceived'],
              map['value']['totalBytesToReceive'],
            );
            _downloadEventStreamController.add(value);
            break;
          case 'historyChanged':
            final value = HistoryChanged(
                map['value']['canGoBack'], map['value']['canGoForward']);
            _historyChangedStreamController.add(value);
            break;
          case 'securityStateChanged':
            _securityStateChangedStreamController.add(map['value']);
            break;
          case 'titleChanged':
            _titleStreamController.add(map['value']);
            break;
          case 'webMessageReceived':
            try {
              final message = json.decode(map['value']);
              _webMessageStreamController.add(message);
            } catch (ex) {
              _webMessageStreamController.addError(ex);
            }
            break;
          case 'containsFullScreenElementChanged':
            _containsFullScreenElementChangedStreamController.add(map['value']);
            break;
          case 'navigationBlocked':
            final value = NavigationBlockedEvent(
              map['value']['url'],
              map['value']['isUserInitiated'],
              map['value']['isRedirected'],
            );
            _navigationBlockedStreamController.add(value);
            break;
          case 'acceleratorKeyPressed':
            final event = map['value'] as Map<dynamic, dynamic>;
            _acceleratorKeyPressedStreamController.add(
              WebviewAcceleratorKeyEvent(
                virtualKey: event['virtualKey'],
                kind: _acceleratorKeyEventKindFromChannel(event['kind']),
                control: event['control'],
                shift: event['shift'],
                alt: event['alt'],
                wasKeyDown: event['wasKeyDown'],
                isKeyReleased: event['isKeyReleased'],
              ),
            );
            break;
        }
      });

      _methodChannel.setMethodCallHandler((call) {
        if (call.method == 'permissionRequested') {
          return _onPermissionRequested(
              call.arguments as Map<dynamic, dynamic>);
        }
        if (call.method == 'newWindowRequested') {
          return _onNewWindowRequested(call.arguments as Map<dynamic, dynamic>);
        }

        throw MissingPluginException('Unknown method ${call.method}');
      });

      value = value.copyWith(isInitialized: true);
      _creatingCompleter.complete();
    } on PlatformException catch (e) {
      _creatingCompleter.completeError(e);
    }

    return _creatingCompleter.future;
  }

  /// Sets the delegate used to decide whether WebView2 may handle requests to
  /// open a new window.
  ///
  /// New-window requests include links with `target="_blank"`, calls to
  /// `window.open()`, and browser gestures such as Ctrl-clicking a link. This
  /// does not intercept ordinary current-window navigations.
  ///
  /// Return [WebviewNavigationDecision.allow] to apply the popup behavior set
  /// by [setPopupWindowPolicy], or [WebviewNavigationDecision.reject] after
  /// handling the URL in Flutter. Passing `null` restores the normal popup
  /// policy without invoking Dart.
  Future<void> setNewWindowDelegate(
      NewWindowRequestedDelegate? delegate) async {
    if (_isDisposed) {
      return;
    }
    if (!value.isInitialized) {
      throw StateError(
          'WebviewController must be initialized before setting a new-window delegate.');
    }
    _newWindowRequested = delegate;
    return _methodChannel.invokeMethod(
      'setNewWindowDelegateEnabled',
      delegate != null,
    );
  }

  /// Replaces the accelerator combinations intercepted by this controller.
  ///
  /// Registered combinations are consumed synchronously by the native plugin;
  /// unregistered combinations continue to WebView2 normally.
  Future<void> setInterceptedAcceleratorKeys(
      List<WebviewAcceleratorKey> keys) async {
    if (_isDisposed) {
      return;
    }
    if (!value.isInitialized) {
      throw StateError(
          'WebviewController must be initialized before registering accelerator keys.');
    }
    return _methodChannel.invokeMethod(
      'setInterceptedAcceleratorKeys',
      keys.map((key) => key.toJson()).toList(),
    );
  }

  Future<bool?> _onPermissionRequested(Map<dynamic, dynamic> args) async {
    if (_permissionRequested == null) {
      return null;
    }

    final url = args['url'] as String?;
    final permissionKindIndex = args['permissionKind'] as int?;
    final isUserInitiated = args['isUserInitiated'] as bool?;

    if (url != null && permissionKindIndex != null && isUserInitiated != null) {
      final permissionKind = WebviewPermissionKind.values[permissionKindIndex];
      final decision =
          await _permissionRequested!(url, permissionKind, isUserInitiated);

      switch (decision) {
        case WebviewPermissionDecision.allow:
          return true;
        case WebviewPermissionDecision.deny:
          return false;
        default:
          return null;
      }
    }

    return null;
  }

  Future<int?> _onNewWindowRequested(Map<dynamic, dynamic> args) async {
    final delegate = _newWindowRequested;
    if (delegate == null) {
      return null;
    }

    final url = args['url'] as String?;
    final isUserInitiated = args['isUserInitiated'] as bool?;
    final modifiers = args['modifiers'] as Map<dynamic, dynamic>?;
    if (url == null || isUserInitiated == null || modifiers == null) {
      return null;
    }

    final ctrl = modifiers['ctrl'] as bool?;
    final shift = modifiers['shift'] as bool?;
    final alt = modifiers['alt'] as bool?;
    if (ctrl == null || shift == null || alt == null) {
      return null;
    }

    final uri = Uri.tryParse(url);
    if (uri == null) {
      return null;
    }

    try {
      final decision = await Future<WebviewNavigationDecision>.value(
        delegate(WebviewNewWindowRequest(
          url: uri,
          isUserInitiated: isUserInitiated,
          modifiers: WebviewKeyModifiers(
            ctrl: ctrl,
            shift: shift,
            alt: alt,
          ),
        )),
      ).timeout(const Duration(seconds: 5));
      return decision.index;
    } catch (_) {
      // Preserve existing popup behavior if the delegate throws or times out.
      return WebviewNavigationDecision.allow.index;
    }
  }

  @override
  Future<void> dispose() async {
    await _creatingCompleter.future;
    if (!_isDisposed) {
      _isDisposed = true;
      await _eventStreamSubscription?.cancel();
      await _pluginChannel.invokeMethod('dispose', _webviewId);
      _closeStreams();
    }
    super.dispose();
  }

  void _closeStreams() {
    // A single-subscription StreamController's close future does not complete
    // until its done event is consumed. Some optional controller streams may
    // never have a listener, so awaiting close would make dispose hang.
    _urlStreamController.close();
    _loadingStateStreamController.close();
    _downloadEventStreamController.close();
    _navigationBlockedStreamController.close();
    _onLoadErrorStreamController.close();
    _historyChangedStreamController.close();
    _securityStateChangedStreamController.close();
    _titleStreamController.close();
    _acceleratorKeyPressedStreamController.close();
    _webMessageStreamController.close();
    _containsFullScreenElementChangedStreamController.close();
  }

  /// Requests keyboard focus for this WebView.
  ///
  /// Call this after displaying a newly active WebView so WebView2 receives
  /// keyboard input and registered accelerator keys.
  Future<void> requestFocus() async {
    if (_isDisposed) {
      return;
    }
    if (!value.isInitialized) {
      throw StateError(
          'WebviewController must be initialized before requesting focus.');
    }
    return _methodChannel.invokeMethod('requestFocus');
  }

  /// Loads the given [url].
  Future<void> loadUrl(String url) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('loadUrl', url);
  }

  /// Loads a document from the given string.
  Future<void> loadStringContent(String content) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('loadStringContent', content);
  }

  /// Reloads the current document.
  Future<void> reload() async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('reload');
  }

  /// Stops all navigations and pending resource fetches.
  Future<void> stop() async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('stop');
  }

  /// Navigates the WebView to the previous page in the navigation history.
  Future<void> goBack() async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('goBack');
  }

  /// Navigates the WebView to the next page in the navigation history.
  Future<void> goForward() async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('goForward');
  }

  /// Adds the provided JavaScript [script] to a list of scripts that should be run after the global
  /// object has been created, but before the HTML document has been parsed and before any
  /// other script included by the HTML document is run.
  ///
  /// Returns a [ScriptID] on success which can be used for [removeScriptToExecuteOnDocumentCreated].
  ///
  /// see https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2?view=webview2-1.0.1264.42#addscripttoexecuteondocumentcreated
  Future<ScriptID?> addScriptToExecuteOnDocumentCreated(String script) async {
    if (_isDisposed) {
      return null;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod<String?>(
        'addScriptToExecuteOnDocumentCreated', script);
  }

  /// Removes the script identified by [scriptId] from the list of registered scripts.
  ///
  /// see https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2?view=webview2-1.0.1264.42#removescripttoexecuteondocumentcreated
  Future<void> removeScriptToExecuteOnDocumentCreated(ScriptID scriptId) async {
    if (_isDisposed) {
      return null;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod(
        'removeScriptToExecuteOnDocumentCreated', scriptId);
  }

  /// Runs the JavaScript [script] in the current top-level document rendered in
  /// the WebView and returns its result.
  ///
  /// see https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2?view=webview2-1.0.1264.42#executescript
  Future<dynamic> executeScript(String script) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);

    final data = await _methodChannel.invokeMethod('executeScript', script);
    if (data == null) return null;
    return jsonDecode(data as String);
  }

  /// Posts the given JSON-formatted message to the current document.
  Future<void> postWebMessage(String message) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('postWebMessage', message);
  }

  /// Sets the user agent value.
  Future<void> setUserAgent(String userAgent) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('setUserAgent', userAgent);
  }

  /// Clears browser cookies.
  Future<void> clearCookies() async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('clearCookies');
  }

  /// Sets a single browser cookie via WebView2's cookie manager
  /// (ICoreWebView2CookieManager -- not the DevTools Protocol, which
  /// Microsoft doesn't guarantee stable across WebView2 releases).
  /// [expires] is seconds since epoch (UTC); omit for a session cookie.
  /// Returns whether the browser accepted it.
  Future<bool> setCookie({
    required String name,
    required String cookieValue,
    required String domain,
    String path = '/',
    bool secure = true,
    bool httpOnly = true,
    double? expires,
  }) async {
    if (_isDisposed) {
      return false;
    }
    assert(value.isInitialized);
    try {
      await _methodChannel.invokeMethod('setCookie', <String, dynamic>{
        'name': name,
        'value': cookieValue,
        'domain': domain,
        'path': path,
        'secure': secure,
        'httpOnly': httpOnly,
        if (expires != null) 'expires': expires,
      });
      return true;
    } on PlatformException {
      return false;
    }
  }

  /// Returns cookies visible to [url] (or every cookie if omitted), each a
  /// map with name/value/domain/path/httpOnly/secure/session/expires.
  Future<List<Map<String, dynamic>>> getCookies([String? url]) async {
    if (_isDisposed) {
      return [];
    }
    assert(value.isInitialized);
    try {
      final result =
          await _methodChannel.invokeMethod<List<dynamic>>('getCookies', url);
      if (result == null) return [];
      return result
          .cast<Map<dynamic, dynamic>>()
          .map((m) => m.cast<String, dynamic>())
          .toList();
    } on PlatformException {
      return [];
    }
  }

  /// Clears browser cache.
  Future<void> clearCache() async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('clearCache');
  }

  /// Toggles ignoring cache for each request. If true, cache will not be used.
  Future<void> setCacheDisabled(bool disabled) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('setCacheDisabled', disabled);
  }

  /// Opens the Browser DevTools in a separate window
  Future<void> openDevTools() async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('openDevTools');
  }

  /// Sets the background color to the provided [color].
  ///
  /// Due to a limitation of the underlying WebView implementation,
  /// semi-transparent values are not supported.
  /// Any non-zero alpha value will be considered as opaque (0xff).
  Future<void> setBackgroundColor(Color color) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod(
        'setBackgroundColor', color.value.toSigned(32));
  }

  /// Sets the zoom factor.
  Future<void> setZoomFactor(double zoomFactor) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('setZoomFactor', zoomFactor);
  }

  /// Sets the [WebviewPopupWindowPolicy].
  ///
  /// Under [WebviewPopupWindowPolicy.allow], popups are hosted in a native
  /// window this app owns (its own icon, taskbar entry, and focus), rather
  /// than WebView2's own default popup window.
  ///
  /// [showAddressBar] controls whether that window shows a read-only
  /// address bar with the popup's current URL; it defaults to `true`, since
  /// most callers expect the browser-chrome look WebView2's own default
  /// popup used to give them for free.
  ///
  /// [addressBarHiddenUrlPatterns] are regexes checked against each popup's
  /// target URL; any match hides the address bar for that popup regardless
  /// of [showAddressBar], so a single call can show it by default while
  /// suppressing it for specific flows (e.g. an OAuth domain) or vice versa.
  /// Matching happens natively and synchronously when the popup is
  /// requested, so it adds no round trip to Dart.
  Future<void> setPopupWindowPolicy(
    WebviewPopupWindowPolicy popupPolicy, {
    bool showAddressBar = true,
    List<String> addressBarHiddenUrlPatterns = const [],
  }) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('setPopupWindowPolicy', {
      'policy': popupPolicy.index,
      'showAddressBar': showAddressBar,
      'addressBarHiddenUrlPatterns': addressBarHiddenUrlPatterns,
    });
  }

  /// Prevents matching navigations from ever loading or rendering.
  ///
  /// [exactUrls] are blocked only on an exact match. [urlPrefixes] block an
  /// entire subtree and must each end in `/`, so the match is unambiguous:
  /// e.g. `'https://example.com/admin/'` blocks everything under `/admin/`
  /// without also matching an unrelated path like `/administrator`. This
  /// distinction matters because plain substring-prefix matching would
  /// otherwise make an exact-match URL like `'https://example.com/login'`
  /// also match `/login/email/auth` or `/login/sms/verify` -- use
  /// [exactUrls] for that case instead.
  ///
  /// Matches are cancelled by WebView2 before they start, since its
  /// `NavigationStarting` event doesn't support deferring the decision
  /// (unlike e.g. permission requests), so the check happens natively and
  /// synchronously rather than round-tripping to Dart for each navigation.
  /// Listen to [onNavigationBlocked] to react afterwards, e.g. to show a
  /// native replacement screen.
  Future<void> setNavigationBlocklist({
    List<String> exactUrls = const [],
    List<String> urlPrefixes = const [],
  }) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('setNavigationBlocklist', {
      'exactUrls': exactUrls,
      'urlPrefixes': urlPrefixes,
    });
  }

  /// Suspends the web view.
  Future<void> suspend() async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('suspend');
  }

  /// Resumes the web view.
  Future<void> resume() async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('resume');
  }

  /// Adds a Virtual Host Name Mapping.
  ///
  /// Please refer to
  /// [Microsofts](https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_3#setvirtualhostnametofoldermapping)
  /// documentation for more details.
  Future<void> addVirtualHostNameMapping(String hostName, String folderPath,
      WebviewHostResourceAccessKind accessKind) async {
    if (_isDisposed) {
      return;
    }

    return _methodChannel.invokeMethod(
        'setVirtualHostNameMapping', [hostName, folderPath, accessKind.index]);
  }

  /// Removes a Virtual Host Name Mapping.
  ///
  /// Please refer to
  /// [Microsofts](https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_3#clearvirtualhostnametofoldermapping)
  /// documentation for more details.
  Future<void> removeVirtualHostNameMapping(String hostName) async {
    if (_isDisposed) {
      return;
    }
    return _methodChannel.invokeMethod('clearVirtualHostNameMapping', hostName);
  }

  /// Toggles native rendering for this webview without closing it.
  ///
  /// Unlike the former texture-backed rendering, an unpainted native webview
  /// isn't hidden automatically just because its Flutter widget isn't
  /// currently shown (e.g. behind an [IndexedStack] for tab switching) --
  /// call this explicitly to keep only the active tab's webview visible
  /// while others stay alive in the background.
  Future<void> setVisible(bool visible) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('setVisible', visible);
  }

  /// Moves this webview to a different native top-level window, identified
  /// by [windowHandle] -- the raw HWND value (e.g. as obtained from
  /// Flutter's `RegularWindowController.getWindowHandle().address`, or
  /// platform channel code that reinterpret-casts a native HWND through
  /// `intptr_t` to an int) of the window this webview should now render
  /// into. After calling this, report fresh bounds relative to the new
  /// window's client area (e.g. by re-laying-out the [Webview] widget in
  /// its new location) before the webview is shown there.
  ///
  /// Needed because this plugin hosts WebView2 as a native windowed control
  /// parented to one specific top-level window; unlike the former
  /// texture-backed rendering, moving the Dart-side widget to a different
  /// window's view does not move the underlying native content on its own
  /// -- e.g. when an app with its own multi-window support (such as
  /// Flutter's experimental multi-view desktop windowing) lets a user drag
  /// a tab from one native window to another.
  ///
  /// Returns whether the reparent succeeded.
  Future<bool> setParentWindow(int windowHandle) async {
    if (_isDisposed) {
      return false;
    }
    assert(value.isInitialized);
    final succeeded = await _methodChannel.invokeMethod<bool>(
        'setParentWindow', windowHandle);
    return succeeded ?? false;
  }

  /// Toggles Chromium's own built-in FPS counter overlay -- the same one
  /// shown by DevTools' Rendering pane -- via the DevTools Protocol.
  ///
  /// Windowed hosting gives this plugin no visibility into WebView2's own
  /// frame production (unlike the former offscreen capture pipeline, which
  /// could count captured/rendered frames itself), so this asks Chromium to
  /// show its own real frame rate directly on the page instead. Intended for
  /// debugging, e.g. via [Webview.showFpsOverlay].
  Future<void> setShowFpsOverlay(bool show) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('setShowFpsOverlay', show);
  }

  /// Sets the bounds of this webview, relative to the top-left of the
  /// hosting Flutter window's client area.
  Future<void> _setBounds(Offset position, Size size, double scaleFactor) async {
    if (_isDisposed) {
      return;
    }
    assert(value.isInitialized);
    return _methodChannel.invokeMethod('setSize', [
      position.dx,
      position.dy,
      size.width,
      size.height,
      scaleFactor,
    ]);
  }
}

class Webview extends StatefulWidget {
  final WebviewController controller;
  final PermissionRequestedDelegate? permissionRequested;
  final double? width;
  final double? height;

  /// An optional scale factor. Defaults to [FlutterView.devicePixelRatio] for
  /// rendering in native resolution.
  /// Setting this to 1.0 will disable high-DPI support.
  /// This should only be needed to mimic old behavior before high-DPI support
  /// was available.
  final double? scaleFactor;

  /// Whether to show Chromium's own built-in FPS counter overlay on the
  /// page, for eyeballing rendering smoothness. Intended for debugging, e.g.
  /// `showFpsOverlay: kDebugMode`. See [WebviewController.setShowFpsOverlay].
  final bool showFpsOverlay;

  /// Whether this webview should be shown as soon as it's ready. Set this
  /// to `false` for a tab that's opening in the background (e.g. a
  /// Ctrl-click), or it will briefly force itself to the front the moment
  /// it loads, ahead of anything you do afterwards to hide it again. This
  /// only affects that one first reveal -- call
  /// [WebviewController.setVisible] as normal for everything after.
  final bool visible;

  const Webview(this.controller,
      {this.width,
      this.height,
      this.permissionRequested,
      this.scaleFactor,
      this.showFpsOverlay = false,
      this.visible = true});

  @override
  _WebviewState createState() => _WebviewState();
}

class _WebviewState extends State<Webview> {
  final GlobalKey _key = GlobalKey();

  WebviewController get _controller => widget.controller;

  // Guards a one-time automatic setVisible(true) the first time real bounds
  // are reported, so a plain (non-tabbed) Webview just works once mounted.
  // An app switching between multiple webviews (e.g. tabs kept alive behind
  // an IndexedStack) should call controller.setVisible(false) itself for
  // whichever ones aren't currently shown -- unlike the former
  // texture-backed rendering, a native webview isn't hidden automatically
  // just because its Flutter widget isn't currently painted.
  bool _madeVisible = false;

  @override
  void initState() {
    super.initState();

    // TODO: Refactor callback and event handling and
    // remove this line
    _controller._permissionRequested = widget.permissionRequested;

    // Report initial bounds once laid out.
    WidgetsBinding.instance.addPostFrameCallback((_) => _reportBounds());
  }

  @override
  Widget build(BuildContext context) {
    return (widget.height != null && widget.width != null)
        ? SizedBox(
            key: _key,
            width: widget.width,
            height: widget.height,
            child: _buildInner())
        : SizedBox.expand(key: _key, child: _buildInner());
  }

  Widget _buildInner() {
    // WebView2 renders as a native window, not through Flutter's own paint
    // pipeline -- this widget exists purely to reserve, and report, the
    // screen rectangle it should occupy (see _reportBounds).
    return NotificationListener<SizeChangedLayoutNotification>(
        onNotification: (notification) {
          _reportBounds();
          return true;
        },
        child: SizeChangedLayoutNotifier(child: SizedBox.expand()));
  }

  void _reportBounds() async {
    final box = _key.currentContext?.findRenderObject() as RenderBox?;
    if (box != null) {
      await _controller.ready;
      final position = box.localToGlobal(Offset.zero);
      unawaited(_controller._setBounds(
          position, box.size, widget.scaleFactor ?? window.devicePixelRatio));
      if (!_madeVisible) {
        _madeVisible = true;
        unawaited(_controller.setVisible(widget.visible));
        if (widget.showFpsOverlay) {
          unawaited(_controller.setShowFpsOverlay(true));
        }
      }
    }
  }
}
