#pragma once

#include "gui/hdr_rhi_viewport.h"
#include "session.h"

#include <QMainWindow>
#include <QRect>
#include <QShowEvent>
#include <QVector>
#include <memory>

namespace uhdr_repack {

class GuiBridge;
class PreviewDocument;
class WebChrome;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(PreviewSession session, QWidget* parent = nullptr);
  ~MainWindow() override;

  bool userApproved() const { return approved_; }

 protected:
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private slots:
  void onApplyAll();
  void encodeApprovedQueue();
  void onCancel();
  void onChooseDest();
  void onPreviewOverlayChanged(const QRect& preview_rect, const QRect& hdr_rect, bool visible);
  void onSliceGuidesChanged(const QVector<QRect>& rects, bool interactive, int axis_x, int slack_px,
                            float crop_offset);

 private:
  void applyOverlayGeometry();

  std::unique_ptr<PreviewDocument> document_;
  HdrRhiViewport* viewport_ = nullptr;
  WebChrome* chrome_ = nullptr;
  GuiBridge* bridge_ = nullptr;
  bool approved_ = false;
#if defined(Q_OS_WIN)
  bool windows_notice_shown_ = false;
#endif
  QRect overlay_rect_;
  QRect hdr_hole_rect_;
  bool overlay_visible_ = false;

  class Ui;
  std::unique_ptr<Ui> ui_;
};

}  // namespace uhdr_repack
