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
decision, so the check happens natively and synchronously against a list of
URL prefixes rather than through an awaited per-navigation Dart callback.

```dart
await controller.setNavigationBlocklist(['https://example.com/login']);

controller.onNavigationBlocked.listen((event) {
  // React to the blocked navigation, e.g. show a native login screen
  // instead of the page WebView2 just cancelled.
});
```

## Limitations
This plugin provides seamless composition of web-based contents with other Flutter widgets by rendering off-screen.

Unfortunately, [Microsoft Edge WebView2](https://docs.microsoft.com/en-us/microsoft-edge/webview2/) doesn't currently have an explicit API for offscreen rendering.
In order to still be able to obtain a pixel buffer upon rendering a new frame, this plugin currently relies on the `Windows.Graphics.Capture` API provided by Windows 10.
The downside is that older Windows versions aren't currently supported.

Older Windows versions might still be targeted by using `BitBlt` for the time being.

See:
- https://github.com/MicrosoftEdge/WebView2Feedback/issues/20
- https://github.com/MicrosoftEdge/WebView2Feedback/issues/526
- https://github.com/MicrosoftEdge/WebView2Feedback/issues/547
