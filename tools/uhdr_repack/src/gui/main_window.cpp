#include "gui/main_window.h"

#include "activity_log.h"
#include "gui/gui_bridge.h"
#include "gui/preview_document.h"
#include "gui/web_chrome.h"
#include "session.h"

#include <QAbstractButton>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QIcon>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPoint>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QUrl>
#include <QVector>
#include <QWheelEvent>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace {

void showCriticalWithIssues(QWidget* parent, const QString& title, const QString& text) {
  QMessageBox box(parent);
  box.setIcon(QMessageBox::Critical);
  box.setWindowTitle(title);
  box.setText(text);
  QPushButton* issues = box.addButton(QObject::tr("Open issues"), QMessageBox::ActionRole);
  box.addButton(QMessageBox::Ok);
  box.exec();
  if (box.clickedButton() == static_cast<QAbstractButton*>(issues)) {
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("https://github.com/karachungen/lightroom-plugin-export-hdr/issues")));
  }
}

}  // namespace

namespace uhdr_repack {

class SliceGuideOverlay : public QWidget {
 public:
  explicit SliceGuideOverlay(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);
    setStyleSheet(QStringLiteral("background: transparent;"));
    setFocusPolicy(Qt::NoFocus);
    hide();
  }

  std::function<void()> onDragStarted;
  std::function<void(float)> onOffsetChanged;
  std::function<void(float)> onDragFinished;
  std::function<void(int)> onWheel;

  void setGuides(const QVector<QRect>& rects, bool interactive, bool axis_x, int slack_px,
                 float crop_offset) {
    rects_ = rects;
    interactive_ = interactive;
    axis_x_ = axis_x;
    slack_px_ = slack_px;
    if (!dragging_) crop_offset_ = crop_offset;
    setCursor(interactive_ && slack_px_ >= 2 ? Qt::OpenHandCursor : Qt::ArrowCursor);
    setVisible(!rects_.isEmpty());
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    QPen pen(QColor(227, 154, 82));
    pen.setWidth(2);
    painter.setPen(pen);
    painter.setFont(font());
    int index = 1;
    for (const QRect& crop : rects_) {
      painter.drawRect(crop.adjusted(1, 1, -1, -1));
      painter.drawText(crop.adjusted(8, 6, -8, -8), Qt::AlignTop | Qt::AlignLeft,
                       QString::number(index++));
    }
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (!interactive_ || slack_px_ < 2 || event->button() != Qt::LeftButton) return;
    dragging_ = true;
    press_pos_ = event->pos();
    press_offset_ = crop_offset_;
    grabMouse();
    setCursor(Qt::ClosedHandCursor);
    if (onDragStarted) onDragStarted();
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (!dragging_) return;
    const int delta =
        axis_x_ ? event->pos().x() - press_pos_.x() : event->pos().y() - press_pos_.y();
    const float next =
        std::clamp(press_offset_ + static_cast<float>(delta) / static_cast<float>(slack_px_), 0.0f,
                   1.0f);
    if (std::abs(next - crop_offset_) < 0.0001f) return;
    crop_offset_ = next;
    if (onOffsetChanged) onOffsetChanged(crop_offset_);
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (!dragging_ || event->button() != Qt::LeftButton) return;
    dragging_ = false;
    releaseMouse();
    setCursor(interactive_ && slack_px_ >= 2 ? Qt::OpenHandCursor : Qt::ArrowCursor);
    if (onDragFinished) onDragFinished(crop_offset_);
    event->accept();
  }

  void wheelEvent(QWheelEvent* event) override {
    if (onWheel) onWheel(event->angleDelta().y());
    event->accept();
  }

 private:
  QVector<QRect> rects_;
  bool interactive_ = false;
  bool axis_x_ = true;
  int slack_px_ = 0;
  float crop_offset_ = 0.5f;
  bool dragging_ = false;
  QPoint press_pos_;
  float press_offset_ = 0.5f;
};

class WebHost : public QWidget {
 public:
  explicit WebHost(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
  }

 protected:
  void paintEvent(QPaintEvent*) override {}
};

class MainWindow::Ui {
 public:
  QWidget* web_container = nullptr;
  QWidget* viewport_container = nullptr;
  SliceGuideOverlay* slice_overlay = nullptr;
};

MainWindow::MainWindow(PreviewSession session, QWidget* parent)
    : QMainWindow(parent),
      document_(std::make_unique<PreviewDocument>(std::move(session))),
      ui_(std::make_unique<Ui>()) {
  setWindowTitle(tr("Ultra HDR"));
  setWindowIcon(QIcon(QStringLiteral(":/resources/app.png")));
  resize(1600, 920);
  setMinimumSize(1100, 700);

  auto* central = new QWidget(this);
  central->setContentsMargins(0, 0, 0, 0);
  setCentralWidget(central);

  ui_->web_container = new WebHost(central);
  ui_->web_container->setGeometry(central->rect());

  viewport_ = new HdrRhiViewport();
  ui_->viewport_container = QWidget::createWindowContainer(viewport_, central);
  ui_->viewport_container->setMinimumSize(1, 1);
  ui_->viewport_container->hide();
  ui_->viewport_container->raise();

  ui_->slice_overlay = new SliceGuideOverlay(central);
  ui_->slice_overlay->raise();

  chrome_ = new WebChrome(this);
  bridge_ = new GuiBridge(document_.get(), viewport_, chrome_, this);

  connect(chrome_, &WebChrome::attachFailed, this, [this](const QString& error) {
    showCriticalWithIssues(this, tr("Web UI failed"), error);
  });
  connect(chrome_, &WebChrome::loadFinished, this, [this]() {
    if (chrome_) chrome_->syncBounds();
  });

  pending_web_root_ = extract_web_assets();
  if (pending_web_root_.isEmpty()) {
    showCriticalWithIssues(this, tr("Web UI failed"),
                           tr("Could not extract embedded web assets."));
  } else {
    chrome_->loadApp(pending_web_root_);
  }

  connect(chrome_, &WebChrome::messageReceived, bridge_, &GuiBridge::handleMessage);
  connect(document_.get(), &PreviewDocument::itemLoading, bridge_, &GuiBridge::onItemLoading);
  connect(document_.get(), &PreviewDocument::itemReady, bridge_, &GuiBridge::onItemReady);
  connect(document_.get(), &PreviewDocument::itemFailed, bridge_, &GuiBridge::onItemFailed);
  connect(document_.get(), &PreviewDocument::finalPreviewReady, bridge_,
          &GuiBridge::onFinalPreviewReady);
  connect(document_.get(), &PreviewDocument::finalPreviewFailed, bridge_,
          &GuiBridge::onFinalPreviewFailed);
  connect(document_.get(), &PreviewDocument::finalPreviewProgress, bridge_,
          &GuiBridge::onFinalPreviewProgress);
  connect(viewport_, &HdrRhiViewport::statusChanged, bridge_,
          &GuiBridge::onViewportStatusChanged);
  connect(bridge_, &GuiBridge::applyRequested, this, &MainWindow::onApplyAll);
  connect(bridge_, &GuiBridge::cancelRequested, this, &MainWindow::onCancel);
  connect(bridge_, &GuiBridge::chooseDestRequested, this, &MainWindow::onChooseDest);
  connect(bridge_, &GuiBridge::previewOverlayChanged, this, &MainWindow::onPreviewOverlayChanged);
  connect(bridge_, &GuiBridge::sliceGuidesChanged, this, &MainWindow::onSliceGuidesChanged);
  ui_->slice_overlay->onDragStarted = [this]() {
    if (bridge_) bridge_->onCropDragStarted();
  };
  ui_->slice_overlay->onOffsetChanged = [this](float offset) {
    if (bridge_) bridge_->onCropOffsetChanged(offset);
  };
  ui_->slice_overlay->onDragFinished = [this](float offset) {
    if (bridge_) bridge_->onCropDragFinished(offset);
  };
  ui_->slice_overlay->onWheel = [this](int delta_y) {
    if (bridge_ && bridge_->isEncoding()) return;
    if (viewport_ && delta_y != 0) viewport_->zoomBy(delta_y > 0 ? 1.15f : 1.0f / 1.15f);
  };
}

MainWindow::~MainWindow() {
  if (viewport_) viewport_->releaseSwapChain();
}

void MainWindow::ensureWebAttached() {
  if (web_attached_ || !chrome_ || !ui_->web_container) {
    return;
  }
  web_attached_ = true;
  QString attach_error;
  if (!chrome_->attachTo(ui_->web_container, &attach_error)) {
    showCriticalWithIssues(this, tr("Web UI failed"), attach_error);
    return;
  }
  if (!pending_web_root_.isEmpty()) {
    chrome_->loadApp(pending_web_root_);
  }
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  ensureWebAttached();
  if (chrome_) {
    chrome_->syncBounds();
  }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (auto* central = centralWidget()) {
    ui_->web_container->setGeometry(central->rect());
  }
  if (chrome_) {
    chrome_->syncBounds();
  }
  applyOverlayGeometry();
}

void MainWindow::onPreviewOverlayChanged(const QRect& preview_rect, const QRect& hdr_rect,
                                         bool visible) {
  overlay_rect_ = preview_rect;
  hdr_hole_rect_ = hdr_rect;
  overlay_visible_ = visible;
  applyOverlayGeometry();
}

void MainWindow::onSliceGuidesChanged(const QVector<QRect>& rects, bool interactive, int axis_x,
                                     int slack_px, float crop_offset) {
  if (ui_->slice_overlay) {
    ui_->slice_overlay->setGuides(rects, interactive, axis_x != 0, slack_px, crop_offset);
    applyOverlayGeometry();
  }
}

void MainWindow::applyOverlayGeometry() {
  if (ui_->viewport_container) {
    if (!overlay_visible_ || hdr_hole_rect_.width() < 8 || hdr_hole_rect_.height() < 8) {
      ui_->viewport_container->hide();
      ui_->viewport_container->setGeometry(QRect(0, 0, 1, 1));
      if (ui_->web_container) ui_->web_container->raise();
    } else {
      ui_->viewport_container->setGeometry(hdr_hole_rect_);
      ui_->viewport_container->show();
      ui_->viewport_container->raise();
    }
  }
  if (ui_->slice_overlay) {
    // Native crop guides sit on the QRhi HDR hole. Over WebView2, CompositionMode_Clear
    // punches a black hole through the page, so keep them off for the canvas simulation.
    if (overlay_visible_ && overlay_rect_.width() >= 8 && overlay_rect_.height() >= 8 &&
        ui_->slice_overlay->isVisible()) {
      ui_->slice_overlay->setGeometry(overlay_rect_);
      ui_->slice_overlay->raise();
    } else {
      ui_->slice_overlay->hide();
      if (ui_->web_container) ui_->web_container->raise();
    }
  }
}

void MainWindow::onApplyAll() {
  encode_aborted_ = false;
  std::string dest_error;
  if (!apply_session_dest_dir(&document_->mutableSession(), &dest_error)) {
    failEncode(tr("Encode failed"), QString::fromStdString(dest_error));
    return;
  }
  bridge_->sendDestDir();
  prepareItemGainMaps();

  encode_result_ = {};
  encode_result_.dest_dir = document_->session().dest_dir;
  encode_queue_.clear();
  encode_cursor_ = 0;
  for (int i = 0; i < document_->itemCount(); ++i) {
    if (document_->item(i).skipped) {
      ItemEncodeResult skipped;
      skipped.id = document_->item(i).id;
      skipped.skipped = true;
      skipped.out = document_->item(i).out;
      encode_result_.items.push_back(skipped);
      continue;
    }
    encode_queue_.push_back(i);
  }
  encodeNextQueuedItem();
}

void MainWindow::prepareItemGainMaps() {
  auto& session = document_->mutableSession();
  for (size_t i = 0; i < session.items.size(); ++i) {
    auto* editor = bridge_->editorForIndex(static_cast<int>(i));
    const bool override_map = document_->state(static_cast<int>(i)).gainmap_override ||
                              (editor && !editor->auto_gain_map.empty() &&
                               editor->gain_map != editor->auto_gain_map);
    if (override_map && editor) {
      document_->updateGainMap(static_cast<int>(i), editor->gain_map, editor->width,
                               editor->height, true);
      document_->saveGainMap(static_cast<int>(i), nullptr);
      session.items[i].gainmap_in = document_->item(static_cast<int>(i)).gainmap_in;
    } else {
      session.items[i].gainmap_in.clear();
    }
  }
}

void MainWindow::showEncodeProgress(bool busy, const QString& message) {
  if (ui_->slice_overlay) {
    ui_->slice_overlay->setAttribute(Qt::WA_TransparentForMouseEvents, busy);
  }
  if (bridge_) bridge_->setBusy(busy, message);
}

namespace {

QString photoStatusName(const SessionItem& item) {
  if (!item.label.empty()) return QString::fromStdString(item.label);
  const std::string& source = !item.sdr.empty() ? item.sdr : item.id;
  const auto slash = source.find_last_of("/\\");
  std::string leaf = slash == std::string::npos ? source : source.substr(slash + 1);
  const auto dot = leaf.find_last_of('.');
  if (dot != std::string::npos && dot > 0) leaf.resize(dot);
  return QString::fromStdString(leaf);
}

}  // namespace

void MainWindow::encodeNextQueuedItem() {
  if (encode_aborted_) {
    return;
  }
  if (encode_cursor_ >= encode_queue_.size()) {
    finishEncodeSuccess();
    return;
  }

  const int index = encode_queue_[encode_cursor_];
  const int ordinal = static_cast<int>(encode_cursor_) + 1;
  const int total = static_cast<int>(encode_queue_.size());
  const SessionItem queued = document_->item(index);
  const QString prefix =
      tr("Photo %1 of %2 · %3").arg(ordinal).arg(total).arg(photoStatusName(queued));
  showEncodeProgress(true, prefix + tr(" · Waiting for Lightroom HDR TIFF"));
  bridge_->ensureHdrTiffs({index}, [this, index, prefix](bool ok, const QString& error) {
    if (encode_aborted_) {
      return;
    }
    if (!ok) {
      failEncode(tr("HDR render failed"),
                 error.isEmpty() ? tr("Lightroom could not render HDR TIFF.") : error);
      return;
    }

    SessionItem item = document_->item(index);
    PreviewSession session_copy = document_->session();
    const bool full_frame = effective_slice_aspect(session_copy, item) == SliceAspect::kNone;
    const QString frame = full_frame ? tr(" · Full frame") : QString();
    showEncodeProgress(true, prefix + frame + tr(" · Preparing encode"));
    auto future = QtConcurrent::run([this, session_copy = std::move(session_copy),
                                     item = std::move(item), prefix, frame]() mutable {
      auto on_progress = [this, prefix, frame](const std::string& detail) {
        const QString line =
            prefix + frame + QStringLiteral(" · ") + QString::fromStdString(detail);
        QMetaObject::invokeMethod(
            this,
            [this, line] {
              if (encode_aborted_) return;
              showEncodeProgress(true, line);
            },
            Qt::QueuedConnection);
      };
      ItemEncodeResult ir;
      std::string error;
      const int code = encode_session_item(session_copy, item, &ir, &error, on_progress);
      return std::pair<int, ItemEncodeResult>{code, std::move(ir)};
    });

    auto* watcher = new QFutureWatcher<std::pair<int, ItemEncodeResult>>(this);
    connect(watcher, &QFutureWatcher<std::pair<int, ItemEncodeResult>>::finished, this,
            [this, watcher, index] {
              const auto [code, ir] = watcher->result();
              watcher->deleteLater();
              if (encode_aborted_) {
                return;
              }
              if (code != 0) {
                failEncode(tr("Encode failed"),
                           ir.error.empty() ? tr("The export queue could not be encoded.")
                                            : QString::fromStdString(ir.error));
                return;
              }
              discard_hdr_tiff_file(&document_->mutableSession().items[static_cast<size_t>(index)]);
              document_->setHdrTiff(index, {});
              encode_result_.items.push_back(ir);
              ++encode_cursor_;
              encodeNextQueuedItem();
            });
    watcher->setFuture(future);
  });
}

void MainWindow::failEncode(const QString& title, const QString& error) {
  encode_aborted_ = true;
  showEncodeProgress(false, title);
  showCriticalWithIssues(this, title, error);
}

void MainWindow::finishEncodeSuccess() {
  encode_result_.approved = true;
  std::string error;
  if (!document_->session().result_path.empty()) {
    write_result_file(document_->session().result_path, encode_result_, &error);
  }
  approved_ = true;
  bridge_->markApproved();
  close();
}

void MainWindow::onChooseDest() {
  QString start = QString::fromStdString(document_->session().dest_dir);
  if (start.isEmpty()) {
    start = QString::fromStdString(inferred_dest_dir(document_->session()));
  }
  const QString chosen =
      QFileDialog::getExistingDirectory(this, tr("Export folder"), start,
                                        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (!chosen.isEmpty()) {
    bridge_->setDestDir(chosen);
  }
}

void MainWindow::onCancel() {
  encode_aborted_ = true;
  approved_ = false;
  PreviewResult result;
  result.approved = false;
  result.dest_dir = document_->session().dest_dir;
  for (const auto& item : document_->session().items) {
    ItemEncodeResult encoded;
    encoded.id = item.id;
    encoded.skipped = true;
    result.items.push_back(encoded);
  }
  std::string error;
  if (!document_->session().result_path.empty()) {
    write_result_file(document_->session().result_path, result, &error);
  }
  close();
}

}  // namespace uhdr_repack
