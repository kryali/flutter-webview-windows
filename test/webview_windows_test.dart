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
