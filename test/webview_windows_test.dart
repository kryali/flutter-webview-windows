import 'package:flutter_test/flutter_test.dart';
import 'package:webview_windows/webview_windows.dart';

void main() {
  test('WebviewAcceleratorKey serializes exact modifier state', () {
    const key = WebviewAcceleratorKey(
      virtualKey: 0x54,
      control: true,
      shift: true,
    );

    expect(key.toJson(), <String, dynamic>{
      'virtualKey': 0x54,
      'control': true,
      'shift': true,
      'alt': false,
    });
  });

  test('WebviewNavigationDecision uses stable channel indexes', () {
    expect(WebviewNavigationDecision.allow.index, 0);
    expect(WebviewNavigationDecision.reject.index, 1);
  });

  test('WebviewNewWindowRequest retains request information', () {
    final request = WebviewNewWindowRequest(
      url: Uri.parse('https://example.com/new'),
      isUserInitiated: true,
      modifiers: const WebviewKeyModifiers(
        ctrl: true,
        shift: false,
        alt: false,
      ),
    );

    expect(request.url, Uri.parse('https://example.com/new'));
    expect(request.isUserInitiated, isTrue);
    expect(request.modifiers.ctrl, isTrue);
    expect(request.modifiers.shift, isFalse);
    expect(request.modifiers.alt, isFalse);
  });

  test('WebviewAcceleratorKeyEvent retains physical key status', () {
    const event = WebviewAcceleratorKeyEvent(
      virtualKey: 0x09,
      kind: WebviewAcceleratorKeyEventKind.keyDown,
      control: true,
      shift: false,
      alt: false,
      wasKeyDown: false,
      isKeyReleased: false,
    );

    expect(event.virtualKey, 0x09);
    expect(event.kind, WebviewAcceleratorKeyEventKind.keyDown);
    expect(event.wasKeyDown, isFalse);
    expect(event.isKeyReleased, isFalse);
  });
}
