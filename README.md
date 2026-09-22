# webview_windows

[![CI](https://github.com/jnschulze/flutter-webview-windows/actions/workflows/ci.yml/badge.svg)](https://github.com/jnschulze/flutter-webview-windows/actions/workflows/ci.yml)
[![Pub](https://img.shields.io/pub/v/webview_windows.svg)](https://pub.dartlang.org/packages/webview_windows)

A [Flutter](https://flutter.dev/) WebView plugin for Windows built on [Microsoft Edge WebView2](https://docs.microsoft.com/en-us/microsoft-edge/webview2/).


### Target platform requirements
- [WebView2 Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/)  
  Before initializing the webview, call `getWebViewVersion()` to check whether the required **WebView2 Runtime** is installed or not on the current system. If `getWebViewVersion()` returns null, guide your user to install **WebView2 Runtime** from this [page](https://developer.microsoft.com/en-us/microsoft-edge/webview2/).
- Windows 10 1809+

### Development platform requirements
- Visual Studio 2019 or higher
- Windows 11 SDK (10.0.22000.194 or higher)
- (recommended) nuget.exe in your $PATH *(The makefile attempts to download nuget if it's not installed, however, this fallback might not work in China)*

## Demo
![image](https://user-images.githubusercontent.com/720469/116823636-d8b9fe00-ab85-11eb-9f91-b7bc819615ed.png)

https://user-images.githubusercontent.com/720469/116716747-66f08180-a9d8-11eb-86ca-63ad5c24f07b.mp4



## Preventing navigations

`WebviewController.setNavigationBlocklist` lets you cancel navigations to
matching URLs *before* they load, so the page never renders and is never
briefly clickable. This is the WebView2 equivalent of `webview_flutter`'s
`NavigationDelegate.onNavigationRequest`, with one difference: WebView2's
underlying `NavigationStarting` event doesn't support deferring its
decision, so the check happens natively and synchronously rather than
through an awaited per-navigation Dart callback.

Pass `exactUrls` for URLs that should be blocked only on an exact match, and
`urlPrefixes` (each ending in `/`) for whole subtrees -- keeping the two
separate avoids a plain prefix match on `.../login` also catching an
unrelated page like `.../login/email/auth`.

```dart
await controller.setNavigationBlocklist(
  exactUrls: ['https://example.com/login'],
  urlPrefixes: ['https://example.com/admin/'],
);

controller.onNavigationBlocked.listen((event) {
  // React to the blocked navigation, e.g. show a native login screen
  // instead of the page WebView2 just cancelled.
});
```

## Handling new windows

Use `setNewWindowDelegate` to decide how to handle links with
`target="_blank"`, calls to `window.open()`, and browser gestures such as
Ctrl-clicking a link. WebView2 defers the request while the delegate runs.
Returning `allow` applies the controller's configured popup policy; returning
`reject` suppresses the popup, allowing the app to open the URL externally or
in its own tab first.

For example, an app can turn only Ctrl-clicked links into its own tabs while
leaving `target="_blank"` and `window.open()` to the configured popup policy:

```dart
await controller.setNewWindowDelegate((request) async {
  if (request.modifiers.ctrl) {
    await tabManager.openTab(request.url);
    return WebviewNavigationDecision.reject;
  }

  return WebviewNavigationDecision.allow;
});
```

Passing `null` removes the delegate:

```dart
await controller.setNewWindowDelegate(null);
```

This callback handles new-window requests only. Ordinary current-window
navigations must use `setNavigationBlocklist`, because WebView2 does not allow
its `NavigationStarting` event to be deferred while Dart returns a decision.

## Limitations
This plugin hosts WebView2 as a native windowed control (`ICoreWebView2Controller`) parented to the Flutter app's own top-level window, rather than compositing its content into the Flutter widget tree as a `Texture`.

The practical effect: a `Webview` widget reserves and reports a screen rectangle (its position and size within the Flutter window), and WebView2 renders directly into that rectangle as its own native window content -- not through Flutter's own paint pipeline. This means nothing Flutter draws can visually overlap the webview's rectangle (no clipping, no rounded corners, no animated transitions or overlays across that boundary -- the classic "airspace" limitation of embedding a native window inside a compositor-driven UI framework). Multiple webviews (e.g. browser tabs) can share the same parent window and coexist simultaneously; use `WebviewController.setVisible` to show/hide the ones not currently active, since an invisible Flutter widget doesn't hide the underlying native rendering automatically the way it would for a `Texture`.

This trades away the flexibility of the former offscreen/texture-based approach (which used the `Windows.Graphics.Capture` API to capture a composition surface into a shared GPU texture) for meaningfully lower overhead: no capture step, no extra GPU copy per frame, and no synthetic input forwarding -- WebView2 receives real Windows input messages directly.
