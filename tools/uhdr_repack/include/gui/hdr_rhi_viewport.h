#pragma once

#include <QImage>
#include <QString>
#include <QWindow>

#include <memory>
#include <vector>

namespace uhdr_repack {

enum class PreviewMode {
  kSdr = 0,
  kGainMap = 1,
  kLiveHdr = 2,
  kFinalHdr = 3,
};

struct HdrViewportStatus {
  bool initialized = false;
  bool hdr_active = false;
  QString backend;
  QString swapchain_format;
  QString luminance_behavior;
  float max_color_component = 1.0f;
  float sdr_white_level = 80.0f;
  QString error;
};

/**
 * Native QRhi swapchain viewport. The image surface is a QWindow so HDR values
 * reach Metal/DXGI directly instead of being flattened by QWidget composition.
 */
class HdrRhiViewport final : public QWindow {
  Q_OBJECT

 public:
  explicit HdrRhiViewport(QWindow* parent = nullptr);
  ~HdrRhiViewport() override;

  void setSdrImage(const QImage& image);
  void setGainMap(const std::vector<float>& gain, int width, int height);
  void setFinalHdr(const std::vector<uint16_t>& rgba_half, int width, int height,
                   int color_gamut = 0);
  void clearFinalHdr();
  void setPreviewMode(PreviewMode mode);
  void setContentBoost(float min_boost, float max_boost);
  void setTargetDisplayPeak(float nits);
  void setGainVisualizationRange(float min_gain, float max_gain);
  void setZoom(float zoom);
  void fitToView();
  void showActualPixels();

  PreviewMode previewMode() const;
  HdrViewportStatus status() const;

  /** CPU SDR fallback used by headless tests and non-HDR screenshot capture. */
  QImage renderSdrFallback() const;

  void releaseSwapChain();

 signals:
  void statusChanged(const uhdr_repack::HdrViewportStatus& status);

 protected:
  void exposeEvent(QExposeEvent* event) override;
  bool event(QEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

 private:
  class Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace uhdr_repack

Q_DECLARE_METATYPE(uhdr_repack::HdrViewportStatus)
