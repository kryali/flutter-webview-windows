#pragma once

#include <windows.graphics.capture.h>
#include <wrl.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

#include "graphics_context.h"

typedef struct {
  size_t width;
  size_t height;
} Size;

// Cumulative frame counters, used to derive FPS for debugging.
typedef struct {
  // Frames delivered by Windows Graphics Capture (i.e. produced by WebView2).
  uint64_t captured;
  // Frames copied into the Flutter texture (after any FPS limit is applied).
  uint64_t rendered;
} FrameCounts;

class TextureBridge {
 public:
  typedef std::function<void()> FrameAvailableCallback;
  typedef std::function<void(Size size)> SurfaceSizeChangedCallback;
  typedef std::chrono::duration<double, std::milli> FrameDuration;

  TextureBridge(GraphicsContext* graphics_context,
                ABI::Windows::UI::Composition::IVisual* visual);
  virtual ~TextureBridge();

  bool Start();
  void Stop();

  void SetOnFrameAvailable(FrameAvailableCallback callback) {
    frame_available_ = std::move(callback);
  }

  void SetOnSurfaceSizeChanged(SurfaceSizeChangedCallback callback) {
    surface_size_changed_ = std::move(callback);
  }

  void NotifySurfaceSizeChanged();
  void SetFpsLimit(std::optional<int> max_fps);
  FrameCounts GetFrameCounts();

 protected:
  bool is_running_ = false;

  const GraphicsContext* graphics_context_;
  std::mutex mutex_;
  std::optional<FrameDuration> frame_duration_ = std::nullopt;

  FrameAvailableCallback frame_available_;
  SurfaceSizeChangedCallback surface_size_changed_;
  std::atomic<bool> needs_update_ = false;
  winrt::com_ptr<ID3D11Texture2D> last_frame_;
  // Set whenever last_frame_ is updated with a new frame, cleared once a
  // subclass has copied it out. Lets GetSurfaceDescriptor skip re-copying
  // last_frame_ when the engine pulls the surface more than once between
  // captured frames.
  bool frame_dirty_ = false;
  std::optional<std::chrono::high_resolution_clock::time_point>
      last_frame_timestamp_;
  // Guarded by mutex_.
  FrameCounts frame_counts_ = {0, 0};

  winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>
      capture_item_;
  winrt::com_ptr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool>
      frame_pool_;
  winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession>
      capture_session_;

  EventRegistrationToken on_closed_token_ = {};
  EventRegistrationToken on_frame_arrived_token_ = {};

  virtual void StopInternal();
  void OnFrameArrived();
  bool ShouldDropFrame();

  // corresponds to DXGI_FORMAT_B8G8R8A8_UNORM
  static constexpr auto kPixelFormat = ABI::Windows::Graphics::DirectX::
      DirectXPixelFormat::DirectXPixelFormat_B8G8R8A8UIntNormalized;
};
