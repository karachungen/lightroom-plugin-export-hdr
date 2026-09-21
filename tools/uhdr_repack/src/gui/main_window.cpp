#include "gui/main_window.h"

#include "gui/gui_bridge.h"
#include "gui/preview_document.h"
#include "gui/web_chrome.h"
#include "session.h"

#include <QAbstractButton>
#include <QDesktopServices>
#include <QPushButton>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QIcon>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPoint>
#include <QResizeEvent>
#include <QShowEvent>
#include <QUrl>
#include <QVector>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

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

  ui_->web_container = new QWidget(central);
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

  QString attach_error;
  if (!chrome_->attachTo(ui_->web_container, &attach_error)) {
    QMessageBox::critical(this, tr("Web UI failed"), attach_error);
  }

  const QString web_root = extract_web_assets();
  if (web_root.isEmpty()) {
    QMessageBox::critical(this, tr("Web UI failed"), tr("Could not extract embedded web assets."));
  } else {
    chrome_->loadApp(web_root);
  }

  connect(chrome_, &WebChrome::messageReceived, bridge_, &GuiBridge::handleMessage);
  connect(document_.get(), &PreviewDocument::itemLoading, bridge_, &GuiBridge::onItemLoading);
  connect(document_.get(), &PreviewDocument::itemReady, bridge_, &GuiBridge::onItemReady);
  connect(document_.get(), &PreviewDocument::itemFailed, bridge_, &GuiBridge::onItemFailed);
  connect(document_.get(), &PreviewDocument::finalPreviewReady, bridge_,
          &GuiBridge::onFinalPreviewReady);
  connect(document_.get(), &PreviewDocument::finalPreviewFailed, bridge_,
          &GuiBridge::onFinalPreviewFailed);
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
}

MainWindow::~MainWindow() {
  if (viewport_) viewport_->releaseSwapChain();
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
#if defined(Q_OS_WIN)
  if (windows_notice_shown_) {
    return;
  }
  windows_notice_shown_ = true;
  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(tr("Ultra HDR"));
  box.setText(tr("Version 3 is only tested on macOS."));
  box.setInformativeText(
      tr("Windows may have issues. If something breaks, please open a GitHub issue."));
  QPushButton* issues = box.addButton(tr("Open issues"), QMessageBox::ActionRole);
  box.addButton(QMessageBox::Ok);
  box.exec();
  if (box.clickedButton() == static_cast<QAbstractButton*>(issues)) {
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("https://github.com/karachungen/lightroom-plugin-export-hdr/issues")));
  }
#endif
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (auto* central = centralWidget()) {
    ui_->web_container->setGeometry(central->rect());
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
    } else {
      ui_->viewport_container->setGeometry(hdr_hole_rect_);
      ui_->viewport_container->show();
      ui_->viewport_container->raise();
    }
  }
  if (ui_->slice_overlay) {
    if (overlay_rect_.width() >= 8 && overlay_rect_.height() >= 8 && ui_->slice_overlay->isVisible()) {
      ui_->slice_overlay->setGeometry(overlay_rect_);
      ui_->slice_overlay->raise();
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
  bridge_->setBusy(true, tr("HDR TIFF %1/%2…").arg(ordinal).arg(total));
  bridge_->ensureHdrTiffs({index}, [this, index, ordinal, total](bool ok, const QString& error) {
    if (encode_aborted_) {
      return;
    }
    if (!ok) {
      failEncode(tr("HDR render failed"),
                 error.isEmpty() ? tr("Lightroom could not render HDR TIFF.") : error);
      return;
    }

    bridge_->setBusy(true, tr("Encoding %1/%2…").arg(ordinal).arg(total));
    SessionItem item = document_->item(index);
    PreviewSession session_copy = document_->session();
    auto future = QtConcurrent::run([session_copy = std::move(session_copy),
                                     item = std::move(item)]() mutable {
      ItemEncodeResult ir;
      std::string error;
      const int code = encode_session_item(session_copy, item, &ir, &error);
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
  bridge_->setBusy(false, title);
  QMessageBox::critical(this, title, error);
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
