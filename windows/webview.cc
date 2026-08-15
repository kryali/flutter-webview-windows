#include "webview.h"

#include <dwmapi.h>
#include <richedit.h>
#include <wrl.h>

#include <algorithm>
#include <format>
#include <iostream>
#include <regex>

#include "util/composition.desktop.interop.h"
#include "util/string_converter.h"
#include "webview_host.h"

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

using namespace Microsoft::WRL;

namespace {

inline void ConvertColor(COREWEBVIEW2_COLOR& webview_color, int32_t color) {
  webview_color.B = color & 0xFF;
  webview_color.G = (color >> 8) & 0xFF;
  webview_color.R = (color >> 16) & 0xFF;
  webview_color.A = (color >> 24) & 0xFF;
}

inline WebviewPermissionKind CW2PermissionKindToPermissionKind(
    COREWEBVIEW2_PERMISSION_KIND kind) {
  using k = COREWEBVIEW2_PERMISSION_KIND;
  switch (kind) {
    case k::COREWEBVIEW2_PERMISSION_KIND_MICROPHONE:
      return WebviewPermissionKind::Microphone;
    case k::COREWEBVIEW2_PERMISSION_KIND_CAMERA:
      return WebviewPermissionKind::Camera;
    case k::COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION:
      return WebviewPermissionKind::GeoLocation;
    case k::COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS:
      return WebviewPermissionKind::Notifications;
    case k::COREWEBVIEW2_PERMISSION_KIND_OTHER_SENSORS:
      return WebviewPermissionKind::OtherSensors;
    case k::COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ:
      return WebviewPermissionKind::ClipboardRead;
    default:
      return WebviewPermissionKind::Unknown;
  }
}

inline COREWEBVIEW2_PERMISSION_STATE WebViewPermissionStateToCW2PermissionState(
    WebviewPermissionState state) {
  using s = COREWEBVIEW2_PERMISSION_STATE;
  switch (state) {
    case WebviewPermissionState::Allow:
      return s::COREWEBVIEW2_PERMISSION_STATE_ALLOW;
    case WebviewPermissionState::Deny:
      return s::COREWEBVIEW2_PERMISSION_STATE_DENY;
    default:
      return s::COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
  }
}

// Mirrors WebView2's own default popup window, which follows the OS theme
// rather than a fixed light/dark look.
bool IsSystemDarkMode() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  LSTATUS status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return status == ERROR_SUCCESS && value == 0;
}

// Regex-matches value against each of patterns, ignoring (rather than
// failing on) any pattern that isn't valid regex syntax, since these come
// from app-supplied Dart strings that could be malformed.
bool MatchesAnyPattern(const std::string& value,
                       const std::vector<std::string>& patterns) {
  for (const auto& pattern : patterns) {
    try {
      if (std::regex_search(value, std::regex(pattern))) {
        return true;
      }
    } catch (const std::regex_error&) {
    }
  }
  return false;
}

constexpr wchar_t kPopupWindowClassName[] = L"FlutterWebviewPopupWindow";

constexpr int kAddressBarHeightDip = 31;
constexpr int kAddressBarMarginDip = 8;
constexpr int kAddressBarFontPointSize = 10;
// Segoe MDL2 Assets glyphs read visually larger than text at the same
// point size, so the lock glyph uses a smaller size to look proportionate.
constexpr int kAddressBarIconPointSize = 8;
// "Lock" glyph (U+E72E), Segoe MDL2 Assets -- matches the padlock
// Chromium's own omnibox shows for https:// URLs.
constexpr wchar_t kLockGlyph = L'\uE72E';
constexpr wchar_t kIconFontName[] = L"Segoe MDL2 Assets";

// Resource ID of IDI_APP_ICON in the Flutter Windows app template's
// Runner.rc/resource.h. This plugin builds as its own DLL (see
// webview_windows/CMakeLists.txt: add_library(... SHARED ...)), so it can't
// #include the host app's generated resource.h. GetModuleHandle(nullptr)
// still resolves to the hosting .exe regardless of which DLL calls it,
// so loading this resource ID from that module picks up the app's own
// icon rather than anything bundled with the plugin -- as long as the host
// app kept the template's default resource ID. Falls back to a system icon
// if that lookup fails (e.g. a customized Runner.rc using a different ID).
constexpr int kAppIconResourceId = 101;

// Hosts a WebView2 popup (from window.open()) in a native top-level window
// owned by the host app -- giving it the app's icon, taskbar grouping, and
// the ability to take foreground focus -- instead of falling back to
// WebView2's own opaque, unhandled-popup window. Deletes itself once its
// HWND is destroyed.
class PopupWindow {
 public:
  static void Create(ICoreWebView2Environment3* environment,
                      ICoreWebView2NewWindowRequestedEventArgs* args,
                      bool show_address_bar,
                      wil::com_ptr<ICoreWebView2Deferral> deferral) {
    auto* popup = new PopupWindow();
    popup->Initialize(environment, args, show_address_bar,
                      std::move(deferral));
  }

 private:
  HWND hwnd_ = nullptr;
  HWND address_bar_hwnd_ = nullptr;
  HBRUSH address_bar_border_brush_ = nullptr;
  bool show_address_bar_ = true;
  bool dark_mode_ = false;
  float scale_ = 1.0f;
  wchar_t ui_font_name_[LF_FACESIZE] = L"Segoe UI";
  wil::com_ptr<ICoreWebView2Controller> controller_;

  // Height, in pixels, of a single line of address-bar-sized text -- used
  // to center it (and the lock glyph) vertically within the bar, since
  // RichEdit top-aligns by default instead of centering the way a plain
  // single-line EDIT control would.
  int MeasureAddressBarTextHeightPx() {
    HDC hdc = GetDC(address_bar_hwnd_);
    int dpi = static_cast<int>(scale_ * 96.0f + 0.5f);
    HFONT font = CreateFontW(
        -MulDiv(kAddressBarFontPointSize, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        ui_font_name_);
    HFONT old_font = static_cast<HFONT>(SelectObject(hdc, font));
    TEXTMETRIC metrics;
    GetTextMetrics(hdc, &metrics);
    SelectObject(hdc, old_font);
    DeleteObject(font);
    ReleaseDC(address_bar_hwnd_, hdc);
    return metrics.tmHeight;
  }

  // Lays out the (optional) address bar strip and the WebView2 content
  // beneath it to fill the rest of the client area. Safe to call before
  // controller_ exists (e.g. from the initial WM_SIZE during creation).
  void LayoutChildren() {
    RECT client;
    GetClientRect(hwnd_, &client);
    int bar_height =
        show_address_bar_ ? static_cast<int>(kAddressBarHeightDip * scale_)
                          : 0;
    if (address_bar_hwnd_) {
      // A 1px inset top and bottom leaves room for WM_ERASEBKGND to paint
      // a border line on each edge of the bar (RichEdit has no border
      // style of its own that looks right here).
      int border = (std::max)(1, static_cast<int>(scale_ + 0.5f));
      int inner_height = (std::max)(0, bar_height - 2 * border);
      MoveWindow(address_bar_hwnd_, 0, border, client.right - client.left,
                inner_height, TRUE);

      int margin = static_cast<int>(kAddressBarMarginDip * scale_);
      int top_inset =
          (std::max)(0, (inner_height - MeasureAddressBarTextHeightPx()) / 2);
      RECT format_rect{margin, top_inset,
                       (client.right - client.left) - margin, inner_height};
      SendMessage(address_bar_hwnd_, EM_SETRECT, 0,
                 reinterpret_cast<LPARAM>(&format_rect));
    }
    if (controller_) {
      RECT bounds{0, bar_height, client.right - client.left,
                 client.bottom - client.top};
      controller_->put_Bounds(bounds);
    }
  }

  // Appends text to the end of the address bar with its own color/font run,
  // leaving any previously-appended runs untouched. Used to give the
  // domain, the rest of the URL, and the lock glyph each their own style,
  // mirroring Chromium's own omnibox convention of de-emphasizing
  // everything except the host.
  void AppendAddressBarRun(const std::wstring& text, COLORREF color,
                           const wchar_t* font_name, int point_size) {
    if (text.empty()) {
      return;
    }
    int start =
        static_cast<int>(SendMessage(address_bar_hwnd_, WM_GETTEXTLENGTH, 0, 0));
    SendMessage(address_bar_hwnd_, EM_SETSEL, start, start);
    SendMessage(address_bar_hwnd_, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>(text.c_str()));
    int end =
        static_cast<int>(SendMessage(address_bar_hwnd_, WM_GETTEXTLENGTH, 0, 0));
    SendMessage(address_bar_hwnd_, EM_SETSEL, start, end);

    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
    format.crTextColor = color;
    format.yHeight = point_size * 20;  // twips -- already DPI-independent.
    wcsncpy_s(format.szFaceName, font_name, _TRUNCATE);
    SendMessage(address_bar_hwnd_, EM_SETCHARFORMAT, SCF_SELECTION,
               reinterpret_cast<LPARAM>(&format));
  }

  // Replaces the address bar's content with url, broken into a padlock
  // glyph (https:// only), a dimmed scheme, a highlighted host, and a
  // dimmed remainder -- the same visual hierarchy Chromium's own omnibox
  // uses to make the part that actually identifies the site stand out.
  void UpdateAddressBarText(const std::wstring& url) {
    if (!address_bar_hwnd_) {
      return;
    }

    SetWindowTextW(address_bar_hwnd_, L"");

    const bool is_https = url.compare(0, 8, L"https://") == 0;
    const COLORREF dim_color =
        dark_mode_ ? RGB(0x9E, 0x9E, 0x9E) : RGB(0x6E, 0x6E, 0x6E);
    const COLORREF bright_color =
        dark_mode_ ? RGB(0xE8, 0xE8, 0xE8) : RGB(0x1A, 0x1A, 0x1A);

    if (is_https) {
      AppendAddressBarRun(std::wstring(1, kLockGlyph) + L"  ", dim_color,
                          kIconFontName, kAddressBarIconPointSize);
    }

    const size_t scheme_end = url.find(L"://");
    if (scheme_end == std::wstring::npos) {
      AppendAddressBarRun(url, bright_color, ui_font_name_,
                          kAddressBarFontPointSize);
    } else {
      const std::wstring scheme = url.substr(0, scheme_end + 3);
      const size_t host_end = url.find_first_of(L"/?#", scheme_end + 3);
      const std::wstring host =
          host_end == std::wstring::npos
              ? url.substr(scheme_end + 3)
              : url.substr(scheme_end + 3, host_end - (scheme_end + 3));
      const std::wstring rest =
          host_end == std::wstring::npos ? L"" : url.substr(host_end);

      AppendAddressBarRun(scheme, dim_color, ui_font_name_,
                          kAddressBarFontPointSize);
      AppendAddressBarRun(host, bright_color, ui_font_name_,
                          kAddressBarFontPointSize);
      AppendAddressBarRun(rest, dim_color, ui_font_name_,
                          kAddressBarFontPointSize);
    }

    SendMessage(address_bar_hwnd_, EM_SETSEL, 0, 0);
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    if (message == WM_NCCREATE) {
      auto* create_struct = reinterpret_cast<CREATESTRUCT*>(lparam);
      SetWindowLongPtr(
          hwnd, GWLP_USERDATA,
          reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
      return DefWindowProc(hwnd, message, wparam, lparam);
    }

    auto* self =
        reinterpret_cast<PopupWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (self) {
      switch (message) {
        case WM_SIZE:
          self->LayoutChildren();
          return 0;
        case WM_ERASEBKGND:
          // Paints a border line along the top and bottom of the address
          // bar strip -- RichEdit has no border style that looks right
          // here, so LayoutChildren() insets the control by 1px on each
          // edge and lets this show through in the gap.
          if (self->show_address_bar_ && self->address_bar_border_brush_) {
            RECT client;
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client,
                     self->address_bar_border_brush_);
            return 1;
          }
          break;
        case WM_DESTROY:
          if (self->controller_) {
            self->controller_->Close();
            self->controller_ = nullptr;
          }
          if (self->address_bar_border_brush_) {
            DeleteObject(self->address_bar_border_brush_);
          }
          self->hwnd_ = nullptr;
          delete self;
          return 0;
      }
    }

    return DefWindowProc(hwnd, message, wparam, lparam);
  }

  static void EnsureWindowClassRegistered() {
    static bool registered = false;
    if (registered) {
      return;
    }
    WNDCLASS window_class{};
    window_class.lpfnWndProc = WndProc;
    window_class.hInstance = GetModuleHandle(nullptr);
    window_class.lpszClassName = kPopupWindowClassName;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&window_class);
    registered = true;
  }

  // Registers the RICHEDIT50W window class (used for the address bar,
  // which needs per-run text coloring an EDIT control can't do).
  static void EnsureRichEditLoaded() {
    static bool loaded = false;
    if (loaded) {
      return;
    }
    LoadLibrary(L"Msftedit.dll");
    loaded = true;
  }

  void Initialize(ICoreWebView2Environment3* environment,
                  ICoreWebView2NewWindowRequestedEventArgs* args,
                  bool show_address_bar,
                  wil::com_ptr<ICoreWebView2Deferral> deferral) {
    EnsureWindowClassRegistered();
    show_address_bar_ = show_address_bar;
    dark_mode_ = IsSystemDarkMode();

    // ICoreWebView2WindowFeatures reports size in DIPs (like the CSS
    // pixels window.open() was called with), not physical pixels.
    //
    // Position (get_Left/get_Top) is deliberately NOT honored: those
    // coordinates are in the opener page's DIP space, and correctly
    // placing a physical-pixel window from them requires knowing which
    // monitor's DPI that space was anchored to -- information we don't
    // reliably have here. Guessing wrong can place the window entirely
    // outside every monitor's bounds (invisible, but still alive and in
    // the taskbar). CW_USEDEFAULT is guaranteed by Windows to land
    // on-screen, so position hints are ignored in favor of that.
    int width_dip = 1024;
    int height_dip = 768;

    wil::com_ptr<ICoreWebView2WindowFeatures> features;
    if (SUCCEEDED(args->get_WindowFeatures(features.put())) && features) {
      BOOL has_size = FALSE;
      if (SUCCEEDED(features->get_HasSize(&has_size)) && has_size) {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(features->get_Width(&w)) && w > 0) {
          width_dip = static_cast<int>(w);
        }
        if (SUCCEEDED(features->get_Height(&h)) && h > 0) {
          height_dip = static_cast<int>(h);
        }
      }
    }

    // Create at the raw (unscaled) DIP size first -- CreateWindowEx needs
    // a real HWND before GetDpiForWindow can tell us which monitor (and
    // therefore which DPI) it actually landed on. The window isn't shown
    // until later, so resizing it to the correctly-scaled physical size
    // right after creation causes no visible flicker.
    hwnd_ = CreateWindowEx(0, kPopupWindowClassName, L"", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, width_dip, height_dip,
                           nullptr, nullptr, GetModuleHandle(nullptr), this);

    if (!hwnd_) {
      deferral->Complete();
      delete this;
      return;
    }

    if (dark_mode_) {
      BOOL enabled = TRUE;
      DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled,
                            sizeof(enabled));
    }

    // Resize (position untouched) to the correctly scaled physical size,
    // now that GetDpiForWindow can tell us which monitor (and therefore
    // which DPI) the window actually landed on.
    UINT dpi = GetDpiForWindow(hwnd_);
    scale_ = dpi / 96.0f;
    if (scale_ != 1.0f) {
      int px_width = static_cast<int>(width_dip * scale_);
      int px_height = static_cast<int>(height_dip * scale_);
      SetWindowPos(hwnd_, nullptr, 0, 0, px_width, px_height,
                  SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    }

    HICON icon = LoadIcon(GetModuleHandle(nullptr),
                          MAKEINTRESOURCE(kAppIconResourceId));
    if (!icon) {
      icon = LoadIcon(nullptr, IDI_APPLICATION);
    }
    if (icon) {
      SendMessage(hwnd_, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(icon));
      SendMessage(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    }

    if (show_address_bar_) {
      EnsureRichEditLoaded();
      address_bar_border_brush_ = CreateSolidBrush(
          dark_mode_ ? RGB(0x45, 0x45, 0x45) : RGB(0xD0, 0xD0, 0xD0));

      // A plain CreateWindowEx("EDIT", ...) defaults to the ancient stock
      // Win32 UI font, not the Segoe UI used everywhere else in Windows
      // (including WebView2's own chrome) -- pull the real system message
      // font instead so this doesn't look out of place.
      NONCLIENTMETRICS metrics{sizeof(NONCLIENTMETRICS)};
      if (SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                               &metrics, 0)) {
        wcsncpy_s(ui_font_name_, metrics.lfMessageFont.lfFaceName, _TRUNCATE);
      }

      // A RichEdit control, not a plain EDIT: needs per-run text coloring
      // to de-emphasize the scheme/path the way Chromium's own omnibox
      // does. Still read-only -- an informational display of the current
      // URL, not a navigable omnibox (no back/forward/reload).
      address_bar_hwnd_ = CreateWindowEx(
          0, MSFTEDIT_CLASS, L"",
          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY, 0, 0, 0, 0,
          hwnd_, nullptr, GetModuleHandle(nullptr), nullptr);
      if (address_bar_hwnd_) {
        SendMessage(address_bar_hwnd_, EM_SETBKGNDCOLOR, 0,
                   dark_mode_ ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xF3, 0xF3, 0xF3));

        // Shows the requested URL immediately; the SourceChanged handler
        // registered below keeps it in sync with redirects/navigation
        // once the popup's own webview exists.
        wil::unique_cotaskmem_string wuri;
        if (SUCCEEDED(args->get_Uri(&wuri))) {
          UpdateAddressBarText(wuri.get());
        }
      }
      LayoutChildren();
    }

    HWND hwnd = hwnd_;
    environment->CreateCoreWebView2Controller(
        hwnd_,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, hwnd, args, deferral = std::move(deferral)](
                HRESULT result,
                ICoreWebView2Controller* controller) -> HRESULT {
              if (FAILED(result) || !controller) {
                DestroyWindow(hwnd);
                deferral->Complete();
                return S_OK;
              }

              controller_ = controller;
              LayoutChildren();

              wil::com_ptr<ICoreWebView2> new_webview;
              controller_->get_CoreWebView2(new_webview.put());
              if (new_webview) {
                args->put_NewWindow(new_webview.get());

                EventRegistrationToken title_token;
                new_webview->add_DocumentTitleChanged(
                    Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                        [this](ICoreWebView2* sender,
                              IUnknown* args) -> HRESULT {
                          LPWSTR wtitle;
                          if (hwnd_ &&
                              sender->get_DocumentTitle(&wtitle) == S_OK) {
                            SetWindowTextW(hwnd_, wtitle);
                          }
                          return S_OK;
                        })
                        .Get(),
                    &title_token);

                if (address_bar_hwnd_) {
                  EventRegistrationToken token;
                  new_webview->add_SourceChanged(
                      Callback<ICoreWebView2SourceChangedEventHandler>(
                          [this](ICoreWebView2* sender,
                                IUnknown* args) -> HRESULT {
                            LPWSTR wurl;
                            if (address_bar_hwnd_ &&
                                sender->get_Source(&wurl) == S_OK) {
                              UpdateAddressBarText(wurl);
                            }
                            return S_OK;
                          })
                          .Get(),
                      &token);
                }
              }
              args->put_Handled(TRUE);

              controller_->put_IsVisible(TRUE);
              ShowWindow(hwnd, SW_SHOW);
              // Best-effort: Windows' anti-focus-stealing heuristics are
              // most permissive close to the user gesture that triggered
              // window.open(). Because controller creation is async, this
              // foreground grab may still be silently ignored by the OS
              // even though every call here succeeds -- that's a real
              // caveat, not a bug in this code.
              SetForegroundWindow(hwnd);
              BringWindowToTop(hwnd);

              deferral->Complete();
              return S_OK;
            })
            .Get());
  }
};

}  // namespace

Webview::Webview(
    wil::com_ptr<ICoreWebView2CompositionController> composition_controller,
    WebviewHost* host, HWND hwnd, bool owns_window, bool offscreen_only)
    : composition_controller_(std::move(composition_controller)),
      host_(host),
      hwnd_(hwnd),
      owns_window_(owns_window) {
  webview_controller_ =
      composition_controller_.try_query<ICoreWebView2Controller3>();

  if (!webview_controller_ ||
      FAILED(webview_controller_->get_CoreWebView2(webview_.put()))) {
    return;
  }

  webview_controller_->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
  webview_controller_->put_ShouldDetectMonitorScaleChanges(FALSE);
  webview_controller_->put_RasterizationScale(1.0);

  wil::com_ptr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(webview_->get_Settings(settings.put()))) {
    settings2_ = settings.try_query<ICoreWebView2Settings2>();

    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
  }

  EnableSecurityUpdates();
  RegisterEventHandlers();

  is_valid_ = CreateSurface(host->compositor(), hwnd, offscreen_only);
}

Webview::~Webview() {
  if (owns_window_) {
    DestroyWindow(hwnd_);
  }
}

bool Webview::CreateSurface(
    winrt::com_ptr<ABI::Windows::UI::Composition::ICompositor> compositor,
    HWND hwnd, bool offscreen_only) {
  winrt::com_ptr<ABI::Windows::UI::Composition::IContainerVisual> root;
  if (FAILED(compositor->CreateContainerVisual(root.put()))) {
    return false;
  }

  surface_ = root.try_as<ABI::Windows::UI::Composition::IVisual>();
  assert(surface_);

  // initial size. doesn't matter as we resize the surface anyway.
  surface_->put_Size({1280, 720});
  surface_->put_IsVisible(true);

  // Create on-screen window for debugging purposes
  if (!offscreen_only) {
    window_target_ = util::TryCreateDesktopWindowTarget(compositor, hwnd);
    auto composition_target =
        window_target_
            .try_as<ABI::Windows::UI::Composition::ICompositionTarget>();
    if (composition_target) {
      composition_target->put_Root(surface_.get());
    }
  }

  winrt::com_ptr<ABI::Windows::UI::Composition::IVisual> webview_visual;
  compositor->CreateContainerVisual(
      reinterpret_cast<ABI::Windows::UI::Composition::IContainerVisual**>(
          webview_visual.put()));

  auto webview_visual2 =
      webview_visual.try_as<ABI::Windows::UI::Composition::IVisual2>();
  if (webview_visual2) {
    webview_visual2->put_RelativeSizeAdjustment({1.0f, 1.0f});
  }

  winrt::com_ptr<ABI::Windows::UI::Composition::IVisualCollection> children;
  root->get_Children(children.put());
  children->InsertAtTop(webview_visual.get());
  composition_controller_->put_RootVisualTarget(webview_visual2.get());

  webview_controller_->put_IsVisible(true);

  return true;
}

void Webview::EnableSecurityUpdates() {
  if (SUCCEEDED(webview_->CallDevToolsProtocolMethod(L"Security.enable", L"{}",
                                                     nullptr)) &&
      SUCCEEDED(webview_->GetDevToolsProtocolEventReceiver(
          L"Security.securityStateChanged",
          &devtools_protocol_event_receiver_))) {
    devtools_protocol_event_receiver_->add_DevToolsProtocolEventReceived(
        Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
            [this](ICoreWebView2* sender,
                   ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args)
                -> HRESULT {
              if (devtools_protocol_event_callback_) {
                wil::unique_cotaskmem_string json_args;
                if (args->get_ParameterObjectAsJson(&json_args) == S_OK) {
                  std::string json = util::Utf8FromUtf16(json_args.get());
                  devtools_protocol_event_callback_(json.c_str());
                }
              }

              return S_OK;
            })
            .Get(),
        &event_registrations_.devtools_protocol_event_token_);
  }
}

void Webview::RegisterEventHandlers() {
  if (!webview_) {
    return;
  }

  webview_->add_ContentLoading(
      Callback<ICoreWebView2ContentLoadingEventHandler>(
          [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            if (loading_state_changed_callback_) {
              loading_state_changed_callback_(WebviewLoadingState::Loading);
            }

            return S_OK;
          })
          .Get(),
      &event_registrations_.content_loading_token_);

  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this](ICoreWebView2* sender,
                 ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL is_success;
            args->get_IsSuccess(&is_success);
            if (!is_success && on_load_error_callback_) {
              COREWEBVIEW2_WEB_ERROR_STATUS web_error_status;
              args->get_WebErrorStatus(&web_error_status);
              on_load_error_callback_(web_error_status);
            }

            if (loading_state_changed_callback_) {
              loading_state_changed_callback_(
                  WebviewLoadingState::NavigationCompleted);
            }

            return S_OK;
          })
          .Get(),
      &event_registrations_.navigation_completed_token_);

  webview_->add_HistoryChanged(
      Callback<ICoreWebView2HistoryChangedEventHandler>(
          [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            if (history_changed_callback_) {
              BOOL can_go_back;
              BOOL can_go_forward;
              sender->get_CanGoBack(&can_go_back);
              sender->get_CanGoForward(&can_go_forward);
              history_changed_callback_({can_go_back, can_go_forward});
            }

            return S_OK;
          })
          .Get(),
      &event_registrations_.history_changed_token_);

  webview_->add_SourceChanged(
      Callback<ICoreWebView2SourceChangedEventHandler>(
          [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            LPWSTR wurl;
            if (url_changed_callback_ && webview_->get_Source(&wurl) == S_OK) {
              std::string url = util::Utf8FromUtf16(wurl);
              url_changed_callback_(url);
            }

            return S_OK;
          })
          .Get(),
      &event_registrations_.source_changed_token_);

  webview_->add_DocumentTitleChanged(
      Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
          [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            LPWSTR wtitle;
            if (document_title_changed_callback_ &&
                webview_->get_DocumentTitle(&wtitle) == S_OK) {
              std::string title = util::Utf8FromUtf16(wtitle);
              document_title_changed_callback_(title);
            }

            return S_OK;
          })
          .Get(),
      &event_registrations_.document_title_changed_token_);

  composition_controller_->add_CursorChanged(
      Callback<ICoreWebView2CursorChangedEventHandler>(
          [this](ICoreWebView2CompositionController* sender,
                 IUnknown* args) -> HRESULT {
            HCURSOR cursor;
            if (cursor_changed_callback_ &&
                sender->get_Cursor(&cursor) == S_OK) {
              cursor_changed_callback_(cursor);
            }
            return S_OK;
          })
          .Get(),
      &event_registrations_.cursor_changed_token_);

  webview_controller_->add_GotFocus(
      Callback<ICoreWebView2FocusChangedEventHandler>(
          [this](ICoreWebView2Controller* sender, IUnknown* args) -> HRESULT {
            if (focus_changed_callback_) {
              focus_changed_callback_(true);
            }
            return S_OK;
          })
          .Get(),
      &event_registrations_.got_focus_token_);

  webview_controller_->add_LostFocus(
      Callback<ICoreWebView2FocusChangedEventHandler>(
          [this](ICoreWebView2Controller* sender, IUnknown* args) -> HRESULT {
            if (focus_changed_callback_) {
              focus_changed_callback_(false);
            }
            return S_OK;
          })
          .Get(),
      &event_registrations_.lost_focus_token_);

  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2* sender,
                 ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            wil::unique_cotaskmem_string wmessage;
            if (web_message_received_callback_ &&
                args->get_WebMessageAsJson(&wmessage) == S_OK) {
              const std::string message = util::Utf8FromUtf16(wmessage.get());
              web_message_received_callback_(message);
            }

            return S_OK;
          })
          .Get(),
      &event_registrations_.web_message_received_token_);

  webview_->add_PermissionRequested(
      Callback<ICoreWebView2PermissionRequestedEventHandler>(
          [this](ICoreWebView2* sender,
                 ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            if (!permission_requested_callback_) {
              return S_OK;
            }

            wil::unique_cotaskmem_string wuri;
            COREWEBVIEW2_PERMISSION_KIND kind =
                COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
            BOOL is_user_initiated = false;

            if (args->get_Uri(&wuri) == S_OK &&
                args->get_PermissionKind(&kind) == S_OK &&
                args->get_IsUserInitiated(&is_user_initiated) == S_OK) {
              wil::com_ptr<ICoreWebView2Deferral> deferral;
              args->GetDeferral(deferral.put());

              const std::string uri = util::Utf8FromUtf16(wuri.get());
              permission_requested_callback_(
                  uri, CW2PermissionKindToPermissionKind(kind),
                  is_user_initiated == TRUE,
                  [deferral = std::move(deferral),
                   args = std::move(args)](WebviewPermissionState state) {
                    args->put_State(
                        WebViewPermissionStateToCW2PermissionState(state));
                    deferral->Complete();
                  });
            }

            return S_OK;
          })
          .Get(),
      &event_registrations_.permission_requested_token_);

  webview_->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [this](ICoreWebView2* sender,
                 ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            if (navigation_blocklist_exact_urls_.empty() &&
                navigation_blocklist_url_prefixes_.empty()) {
              return S_OK;
            }

            wil::unique_cotaskmem_string wuri;
            if (args->get_Uri(&wuri) != S_OK) {
              return S_OK;
            }
            const std::string uri = util::Utf8FromUtf16(wuri.get());

            const bool blocked =
                std::find(navigation_blocklist_exact_urls_.begin(),
                          navigation_blocklist_exact_urls_.end(),
                          uri) != navigation_blocklist_exact_urls_.end() ||
                std::any_of(navigation_blocklist_url_prefixes_.begin(),
                            navigation_blocklist_url_prefixes_.end(),
                            [&uri](const std::string& prefix) {
                              return uri.compare(0, prefix.size(), prefix) ==
                                     0;
                            });
            if (!blocked) {
              return S_OK;
            }

            args->put_Cancel(TRUE);

            if (navigation_blocked_callback_) {
              BOOL is_user_initiated = false;
              BOOL is_redirected = false;
              args->get_IsUserInitiated(&is_user_initiated);
              args->get_IsRedirected(&is_redirected);
              navigation_blocked_callback_(uri, is_user_initiated == TRUE,
                                           is_redirected == TRUE);
            }

            return S_OK;
          })
          .Get(),
      &event_registrations_.navigation_starting_token_);

  webview_->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [this](ICoreWebView2* sender,
                 ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            switch (popup_window_policy_) {
              case WebviewPopupWindowPolicy::Deny:
                args->put_Handled(TRUE);
                break;
              case WebviewPopupWindowPolicy::ShowInSameWindow:
                args->put_NewWindow(webview_.get());
                args->put_Handled(TRUE);
                break;
              case WebviewPopupWindowPolicy::Allow: {
                wil::com_ptr<ICoreWebView2Deferral> deferral;
                if (SUCCEEDED(args->GetDeferral(deferral.put())) &&
                    deferral) {
                  bool show_address_bar = popup_window_show_address_bar_;
                  if (show_address_bar &&
                      !popup_window_address_bar_hidden_url_patterns_
                           .empty()) {
                    wil::unique_cotaskmem_string wuri;
                    if (SUCCEEDED(args->get_Uri(&wuri))) {
                      const std::string uri = util::Utf8FromUtf16(wuri.get());
                      if (MatchesAnyPattern(
                              uri,
                              popup_window_address_bar_hidden_url_patterns_)) {
                        show_address_bar = false;
                      }
                    }
                  }
                  PopupWindow::Create(host_->environment(), args,
                                      show_address_bar, std::move(deferral));
                }
                break;
              }
            }

            return S_OK;
          })
          .Get(),
      &event_registrations_.new_windows_requested_token_);

  webview_->add_ContainsFullScreenElementChanged(
      Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
          [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            BOOL flag = FALSE;
            if (contains_fullscreen_element_changed_callback_ &&
                SUCCEEDED(sender->get_ContainsFullScreenElement(&flag))) {
              contains_fullscreen_element_changed_callback_(flag);
            }
            return S_OK;
          })
          .Get(),
      &event_registrations_.contains_fullscreen_element_changed_token_);

  auto webview24 = webview_.try_query<ICoreWebView2_4>();
  if (webview24) {
    webview24->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>(
            [this](ICoreWebView2* sender,
                   ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
              wil::com_ptr<ICoreWebView2Deferral> deferral;
              args->GetDeferral(&deferral);

              args->put_Handled(TRUE);

              wil::com_ptr<ICoreWebView2DownloadOperation> download;
              args->get_DownloadOperation(&download);

              INT64 totalBytesToReceive = 0;
              download->get_TotalBytesToReceive(&totalBytesToReceive);

              wil::unique_cotaskmem_string uri;
              download->get_Uri(&uri);

              wil::unique_cotaskmem_string mimeType;
              download->get_MimeType(&mimeType);

              wil::unique_cotaskmem_string contentDisposition;
              download->get_ContentDisposition(&contentDisposition);

              wil::unique_cotaskmem_string resultFilePath;
              args->get_ResultFilePath(&resultFilePath);

              args->put_ResultFilePath(resultFilePath.get());
              UpdateDownloadProgress(download.get());

              if (download_event_callback_) {
                download_event_callback_(
                    {WebviewDownloadEventKind::DownloadStarted,
                     util::Utf8FromUtf16(uri.get()),
                     util::Utf8FromUtf16(resultFilePath.get()), 0,
                     totalBytesToReceive});
              }

              return S_OK;
            })
            .Get(),
        &event_registrations_.download_starting_token_);
  }
}

void Webview::SetSurfaceSize(size_t width, size_t height, float scale_factor) {
  if (!IsValid()) {
    return;
  }

  if (surface_ && width > 0 && height > 0) {
    scale_factor_ = scale_factor;
    auto scaled_width = width * scale_factor;
    auto scaled_height = height * scale_factor;

    const LONG kBoundsOffset = -32000;
    RECT bounds;
    bounds.left = kBoundsOffset;
    bounds.top = kBoundsOffset;
    bounds.right = kBoundsOffset + static_cast<LONG>(scaled_width);
    bounds.bottom = kBoundsOffset + static_cast<LONG>(scaled_height);

    surface_->put_Size({scaled_width, scaled_height});
    webview_controller_->put_RasterizationScale(scale_factor);
    if (webview_controller_->put_Bounds(bounds) != S_OK) {
      std::cerr << "Setting webview bounds failed." << std::endl;
    }

    if (surface_size_changed_callback_) {
      surface_size_changed_callback_(width, height);
    }
  }
}

bool Webview::OpenDevTools() {
  if (!IsValid()) {
    return false;
  }
  webview_->OpenDevToolsWindow();
  return true;
}

bool Webview::ClearCookies() {
  if (!IsValid()) {
    return false;
  }
  return webview_->CallDevToolsProtocolMethod(L"Network.clearBrowserCookies",
                                              L"{}", nullptr) == S_OK;
}

namespace {
wil::com_ptr<ICoreWebView2CookieManager> GetCookieManager(
    const wil::com_ptr<ICoreWebView2>& webview) {
  auto webview2 = webview.try_query<ICoreWebView2_2>();
  if (!webview2) {
    return nullptr;
  }
  wil::com_ptr<ICoreWebView2CookieManager> cookie_manager;
  webview2->get_CookieManager(&cookie_manager);
  return cookie_manager;
}
}  // namespace

void Webview::SetCookie(const std::string& name, const std::string& value,
                        const std::string& domain, const std::string& path,
                        bool secure, bool http_only,
                        std::optional<double> expires,
                        SetCookieCallback callback) {
  if (!IsValid()) {
    callback(false);
    return;
  }

  auto cookie_manager = GetCookieManager(webview_);
  if (!cookie_manager) {
    callback(false);
    return;
  }

  wil::com_ptr<ICoreWebView2Cookie> cookie;
  if (FAILED(cookie_manager->CreateCookie(
          util::Utf16FromUtf8(name).c_str(), util::Utf16FromUtf8(value).c_str(),
          util::Utf16FromUtf8(domain).c_str(), util::Utf16FromUtf8(path).c_str(),
          &cookie)) ||
      !cookie) {
    callback(false);
    return;
  }

  cookie->put_IsHttpOnly(http_only);
  cookie->put_IsSecure(secure);
  if (expires.has_value()) {
    cookie->put_Expires(*expires);
  }

  callback(SUCCEEDED(cookie_manager->AddOrUpdateCookie(cookie.get())));
}

void Webview::GetCookies(const std::string& uri, GetCookiesCallback callback) {
  if (!IsValid()) {
    callback(false, {});
    return;
  }

  auto cookie_manager = GetCookieManager(webview_);
  if (!cookie_manager) {
    callback(false, {});
    return;
  }

  auto wuri = uri.empty() ? std::wstring() : util::Utf16FromUtf8(uri);
  if (FAILED(cookie_manager->GetCookies(
          uri.empty() ? nullptr : wuri.c_str(),
          Callback<ICoreWebView2GetCookiesCompletedHandler>(
              [callback](HRESULT result,
                        ICoreWebView2CookieList* list) -> HRESULT {
                if (FAILED(result) || !list) {
                  callback(false, {});
                  return S_OK;
                }

                UINT32 count = 0;
                list->get_Count(&count);

                std::vector<WebviewCookie> cookies;
                cookies.reserve(count);
                for (UINT32 i = 0; i < count; i++) {
                  wil::com_ptr<ICoreWebView2Cookie> cookie;
                  if (FAILED(list->GetValueAtIndex(i, &cookie)) || !cookie) {
                    continue;
                  }

                  wil::unique_cotaskmem_string name, value, domain, path;
                  cookie->get_Name(&name);
                  cookie->get_Value(&value);
                  cookie->get_Domain(&domain);
                  cookie->get_Path(&path);

                  BOOL http_only = FALSE, secure = FALSE, session = FALSE;
                  cookie->get_IsHttpOnly(&http_only);
                  cookie->get_IsSecure(&secure);
                  cookie->get_IsSession(&session);

                  double expires = 0;
                  cookie->get_Expires(&expires);

                  cookies.push_back(WebviewCookie{
                      util::Utf8FromUtf16(name.get()),
                      util::Utf8FromUtf16(value.get()),
                      util::Utf8FromUtf16(domain.get()),
                      util::Utf8FromUtf16(path.get()),
                      http_only == TRUE,
                      secure == TRUE,
                      session == TRUE,
                      expires,
                  });
                }

                callback(true, std::move(cookies));
                return S_OK;
              })
              .Get()))) {
    callback(false, {});
  }
}

bool Webview::ClearCache() {
  if (!IsValid()) {
    return false;
  }
  return webview_->CallDevToolsProtocolMethod(L"Network.clearBrowserCache",
                                              L"{}", nullptr) == S_OK;
}

bool Webview::SetCacheDisabled(bool disabled) {
  if (!IsValid()) {
    return false;
  }
  std::string json = std::format("{{\"disableCache\":{}}}", disabled);
  return webview_->CallDevToolsProtocolMethod(L"Network.setCacheDisabled",
                                              util::Utf16FromUtf8(json).c_str(),
                                              nullptr) == S_OK;
}

void Webview::SetPopupWindowPolicy(
    WebviewPopupWindowPolicy policy, bool show_address_bar,
    std::vector<std::string> address_bar_hidden_url_patterns) {
  popup_window_policy_ = policy;
  popup_window_show_address_bar_ = show_address_bar;
  popup_window_address_bar_hidden_url_patterns_ =
      std::move(address_bar_hidden_url_patterns);
}

void Webview::SetNavigationBlocklist(std::vector<std::string> exact_urls,
                                     std::vector<std::string> url_prefixes) {
  navigation_blocklist_exact_urls_ = std::move(exact_urls);
  navigation_blocklist_url_prefixes_ = std::move(url_prefixes);
}

bool Webview::SetUserAgent(const std::string& user_agent) {
  if (settings2_) {
    return settings2_->put_UserAgent(util::Utf16FromUtf8(user_agent).c_str()) ==
           S_OK;
  }
  return false;
}

bool Webview::SetBackgroundColor(int32_t color) {
  if (!IsValid()) {
    return false;
  }

  COREWEBVIEW2_COLOR webview_color;
  ConvertColor(webview_color, color);

  // Semi-transparent backgrounds are not supported.
  // Valid alpha values are 0 or 255.
  if (webview_color.A > 0) {
    webview_color.A = 0xFF;
  }

  return webview_controller_->put_DefaultBackgroundColor(webview_color) == S_OK;
}

bool Webview::SetZoomFactor(double factor) {
  if (!IsValid()) {
    return false;
  }
  return webview_controller_->put_ZoomFactor(factor) == S_OK;
}

void Webview::SetCursorPos(double x, double y) {
  if (!IsValid()) {
    return;
  }

  POINT point;
  point.x = static_cast<LONG>(x * scale_factor_);
  point.y = static_cast<LONG>(y * scale_factor_);
  last_cursor_pos_ = point;

  // https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2?view=webview2-1.0.774.44
  composition_controller_->SendMouseInput(
      COREWEBVIEW2_MOUSE_EVENT_KIND::COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE,
      virtual_keys_.state(), 0, point);
}

void Webview::SetPointerUpdate(int32_t pointer,
                               WebviewPointerEventKind eventKind, double x,
                               double y, double size, double pressure) {
  if (!IsValid()) {
    return;
  }

  COREWEBVIEW2_POINTER_EVENT_KIND event =
      COREWEBVIEW2_POINTER_EVENT_KIND_UPDATE;
  UINT32 pointerFlags = POINTER_FLAG_NONE;
  switch (eventKind) {
    case WebviewPointerEventKind::Activate:
      event = COREWEBVIEW2_POINTER_EVENT_KIND_ACTIVATE;
      break;
    case WebviewPointerEventKind::Down:
      event = COREWEBVIEW2_POINTER_EVENT_KIND_DOWN;
      pointerFlags =
          POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
      break;
    case WebviewPointerEventKind::Enter:
      event = COREWEBVIEW2_POINTER_EVENT_KIND_ENTER;
      break;
    case WebviewPointerEventKind::Leave:
      event = COREWEBVIEW2_POINTER_EVENT_KIND_LEAVE;
      break;
    case WebviewPointerEventKind::Up:
      event = COREWEBVIEW2_POINTER_EVENT_KIND_UP;
      pointerFlags = POINTER_FLAG_UP;
      break;
    case WebviewPointerEventKind::Update:
      event = COREWEBVIEW2_POINTER_EVENT_KIND_UPDATE;
      pointerFlags =
          POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
      break;
  }

  POINT point;
  point.x = static_cast<LONG>(x * scale_factor_);
  point.y = static_cast<LONG>(y * scale_factor_);

  RECT rect;
  rect.left = point.x - 2;
  rect.right = point.x + 2;
  rect.top = point.y - 2;
  rect.bottom = point.y + 2;

  host_->CreateWebViewPointerInfo(
      [this, pointer, event, pointerFlags, point, rect, pressure](
          wil::com_ptr<ICoreWebView2PointerInfo> pointerInfo,
          std::unique_ptr<WebviewCreationError> error) {
        if (pointerInfo) {
          ICoreWebView2PointerInfo* pInfo = pointerInfo.get();
          pInfo->put_PointerId(pointer);
          pInfo->put_PointerKind(PT_TOUCH);
          pInfo->put_PointerFlags(pointerFlags);
          pInfo->put_TouchFlags(TOUCH_FLAG_NONE);
          pInfo->put_TouchMask(TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE);
          pInfo->put_TouchPressure(
              std::clamp((UINT32)(pressure == 0.0 ? 1024 : 1024 * pressure),
                         (UINT32)0, (UINT32)1024));
          pInfo->put_PixelLocationRaw(point);
          pInfo->put_TouchContactRaw(rect);
          composition_controller_->SendPointerInput(event, pInfo);
        }
      });
}

void Webview::SetPointerButtonState(WebviewPointerButton button, bool is_down) {
  if (!IsValid()) {
    return;
  }

  COREWEBVIEW2_MOUSE_EVENT_KIND kind;
  switch (button) {
    case WebviewPointerButton::Primary:
      virtual_keys_.set_isLeftButtonDown(is_down);
      kind = is_down ? COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN
                     : COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
      break;
    case WebviewPointerButton::Secondary:
      virtual_keys_.set_isRightButtonDown(is_down);
      kind = is_down ? COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN
                     : COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
      break;
    case WebviewPointerButton::Tertiary:
      virtual_keys_.set_isMiddleButtonDown(is_down);
      kind = is_down ? COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN
                     : COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
      break;
    default:
      kind = static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(0);
  }

  composition_controller_->SendMouseInput(kind, virtual_keys_.state(), 0,
                                          last_cursor_pos_);
}

void Webview::SendScroll(double delta, bool horizontal) {
  // clang-format off
  //
  // TODO:
  // Using a fixed value here is certainly wrong. Flutter's calculation in flutter_window.cc
  // needs to be inverted to get back the "native" value. See
  // - https://github.com/flutter/engine/blob/82c1dfcf588c2669ca391134910d634ca31fddf1/shell/platform/windows/flutter_window.cc#L416-L426
  // Related:
  // - https://source.chromium.org/chromium/chromium/src/+/main:ui/events/blink/web_input_event_builders_win.cc
  // - https://github.com/flutter/flutter/issues/107248
  //
  // clang-format on
  constexpr auto kScrollMultiplier = 1.5;

  auto offset = static_cast<short>(delta * kScrollMultiplier);

  if (horizontal) {
    composition_controller_->SendMouseInput(
        COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL, virtual_keys_.state(),
        offset, last_cursor_pos_);
  } else {
    composition_controller_->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
                                            virtual_keys_.state(), offset,
                                            last_cursor_pos_);
  }
}

void Webview::SetScrollDelta(double delta_x, double delta_y) {
  if (!IsValid()) {
    return;
  }

  if (delta_x != 0.0) {
    SendScroll(delta_x, true);
  }
  if (delta_y != 0.0) {
    SendScroll(delta_y, false);
  }
}

void Webview::LoadUrl(const std::string& url) {
  if (IsValid()) {
    webview_->Navigate(util::Utf16FromUtf8(url).c_str());
  }
}

void Webview::LoadStringContent(const std::string& content) {
  if (IsValid()) {
    webview_->NavigateToString(util::Utf16FromUtf8(content).c_str());
  }
}

bool Webview::Stop() {
  if (!IsValid()) {
    return false;
  }
  return SUCCEEDED(webview_->CallDevToolsProtocolMethod(L"Page.stopLoading",
                                                        L"{}", nullptr));
}

bool Webview::Reload() {
  if (!IsValid()) {
    return false;
  }
  return SUCCEEDED(webview_->Reload());
}

bool Webview::GoBack() {
  if (!IsValid()) {
    return false;
  }
  return SUCCEEDED(webview_->GoBack());
}

bool Webview::GoForward() {
  if (!IsValid()) {
    return false;
  }
  return SUCCEEDED(webview_->GoForward());
}

void Webview::AddScriptToExecuteOnDocumentCreated(
    const std::string& script,
    AddScriptToExecuteOnDocumentCreatedCallback callback) {
  if (IsValid()) {
    if (SUCCEEDED(webview_->AddScriptToExecuteOnDocumentCreated(
            util::Utf16FromUtf8(script).c_str(),
            Callback<
                ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                [callback](HRESULT result, LPCWSTR wsid) -> HRESULT {
                  std::string sid = util::Utf8FromUtf16(wsid);
                  callback(SUCCEEDED(result), sid);
                  return S_OK;
                })
                .Get()))) {
      return;
    }
  }

  callback(false, std::string());
}

void Webview::RemoveScriptToExecuteOnDocumentCreated(
    const std::string& script_id) {
  if (IsValid()) {
    webview_->RemoveScriptToExecuteOnDocumentCreated(
        util::Utf16FromUtf8(script_id).c_str());
  }
}

void Webview::ExecuteScript(const std::string& script,
                            ScriptExecutedCallback callback) {
  if (IsValid()) {
    if (SUCCEEDED(webview_->ExecuteScript(
            util::Utf16FromUtf8(script).c_str(),
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [callback](HRESULT result, LPCWSTR json_result_object) {
                  callback(SUCCEEDED(result),
                           util::Utf8FromUtf16(json_result_object));
                  return S_OK;
                })
                .Get()))) {
      return;
    }
  }

  callback(false, std::string());
}

bool Webview::PostWebMessage(const std::string& json) {
  if (!IsValid()) {
    return false;
  }
  return webview_->PostWebMessageAsJson(util::Utf16FromUtf8(json).c_str()) ==
         S_OK;
}

bool Webview::Suspend() {
  if (!IsValid()) {
    return false;
  }

  wil::com_ptr<ICoreWebView2_3> webview;
  webview = webview_.query<ICoreWebView2_3>();
  if (!webview) {
    return false;
  }

  webview_controller_->put_IsVisible(false);
  return webview->TrySuspend(
             Callback<ICoreWebView2TrySuspendCompletedHandler>(
                 [](HRESULT error_code, BOOL is_successful) -> HRESULT {
                   return S_OK;
                 })
                 .Get()) == S_OK;
}

bool Webview::Resume() {
  if (!IsValid()) {
    return false;
  }

  wil::com_ptr<ICoreWebView2_3> webview;
  webview = webview_.query<ICoreWebView2_3>();
  if (!webview) {
    return false;
  }
  return webview->Resume() == S_OK &&
         webview_controller_->put_IsVisible(true) == S_OK;
}

bool Webview::SetVirtualHostNameMapping(
    const std::string& hostName, const std::string& path,
    WebviewHostResourceAccessKind accessKind) {
  if (!IsValid()) {
    return false;
  }

  wil::com_ptr<ICoreWebView2_3> webview;
  webview = webview_.query<ICoreWebView2_3>();
  if (!webview) {
    return false;
  }

  COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND accessKindIntValue =
      COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY;
  switch (accessKind) {
    case WebviewHostResourceAccessKind::Allow:
      accessKindIntValue = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW;
      break;
    case WebviewHostResourceAccessKind::DenyCors:
      accessKindIntValue = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS;
      break;
    case WebviewHostResourceAccessKind::Deny:
      accessKindIntValue = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY;
      break;
  }

  return webview->SetVirtualHostNameToFolderMapping(
      util::Utf16FromUtf8(hostName).c_str(), util::Utf16FromUtf8(path).c_str(),
      accessKindIntValue);
}

bool Webview::ClearVirtualHostNameMapping(const std::string& hostName) {
  if (!IsValid()) {
    return false;
  }

  wil::com_ptr<ICoreWebView2_3> webview;
  webview = webview_.query<ICoreWebView2_3>();
  if (!webview) {
    return false;
  }

  return webview->ClearVirtualHostNameToFolderMapping(
      util::Utf16FromUtf8(hostName).c_str());
}

void Webview::UpdateDownloadProgress(ICoreWebView2DownloadOperation* download) {
  download->add_BytesReceivedChanged(
      Callback<ICoreWebView2BytesReceivedChangedEventHandler>(
          [this](ICoreWebView2DownloadOperation* download,
                 IUnknown* args) -> HRESULT {
            if (download_event_callback_) {
              INT64 recvd = 0;
              download->get_BytesReceived(&recvd);
              INT64 total = 0;
              download->get_TotalBytesToReceive(&total);

              wil::unique_cotaskmem_string uri;
              download->get_Uri(&uri);
              wil::unique_cotaskmem_string resultFilePath;
              download->get_ResultFilePath(&resultFilePath);
              download_event_callback_(
                  {WebviewDownloadEventKind::DownloadProgress,
                   util::Utf8FromUtf16(uri.get()),
                   util::Utf8FromUtf16(resultFilePath.get()), recvd, total});
            }
            return S_OK;
          })
          .Get(),
      &event_registrations_.download_bytes_received_token_);

  download->add_StateChanged(
      Callback<ICoreWebView2StateChangedEventHandler>(
          [this](ICoreWebView2DownloadOperation* download,
                 IUnknown* args) -> HRESULT {
            COREWEBVIEW2_DOWNLOAD_STATE state;
            download->get_State(&state);
            switch (state) {
              case COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS:
                break;
              case COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED:
                // Here developer can take different actions based on
                // `download->InterruptReason`. For example, show an error
                // message to the end user.
                break;
              case COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED:
                if (download_event_callback_) {
                  wil::unique_cotaskmem_string uri;
                  download->get_Uri(&uri);
                  wil::unique_cotaskmem_string resultFilePath;
                  download->get_ResultFilePath(&resultFilePath);
                  INT64 recvd = 0;
                  download->get_BytesReceived(&recvd);
                  INT64 total = 0;
                  download->get_TotalBytesToReceive(&total);
                  download_event_callback_(
                      {WebviewDownloadEventKind::DownloadCompleted,
                       util::Utf8FromUtf16(uri.get()),
                       util::Utf8FromUtf16(resultFilePath.get()), recvd,
                       total});
                }
                break;
            }
            return S_OK;
          })
          .Get(),
      &event_registrations_.download_state_changed_token_);
}