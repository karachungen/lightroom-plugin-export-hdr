#include "gui/main_window.h"

#include "gui/gui_bridge.h"
#include "gui/preview_document.h"
#include "gui/web_chrome.h"
#include "session.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QIcon>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QUrl>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>
#include <vector>

namespace uhdr_repack {

class SliceGuideOverlay : public QWidget {
 public:
  explicit SliceGuideOverlay(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
  }

  void setRects(const QVector<QRect>& rects) {
    rects_ = rects;
    setVisible(!rects_.isEmpty());
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), QColor(0, 0, 0, 120));
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    for (const QRect& crop : rects_) {
      painter.fillRect(crop, Qt::transparent);
    }
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

 private:
  QVector<QRect> rects_;
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
  auto* issues = box.addButton(tr("Open issues"), QMessageBox::ActionRole);
  box.addButton(QMessageBox::Ok);
  box.exec();
  if (box.clickedButton() == issues) {
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

void MainWindow::onPreviewOverlayChanged(const QRect& rect, bool visible) {
  overlay_rect_ = rect;
  overlay_visible_ = visible;
  applyOverlayGeometry();
}

void MainWindow::onSliceGuidesChanged(const QVector<QRect>& rects) {
  if (ui_->slice_overlay) {
    ui_->slice_overlay->setRects(rects);
    applyOverlayGeometry();
  }
}

void MainWindow::applyOverlayGeometry() {
  if (ui_->viewport_container) {
    if (!overlay_visible_ || overlay_rect_.width() < 8 || overlay_rect_.height() < 8) {
      ui_->viewport_container->hide();
    } else {
      ui_->viewport_container->setGeometry(overlay_rect_);
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
  bridge_->setBusy(true, tr("Preparing HDR…"));

  std::vector<int> needed;
  for (int i = 0; i < document_->itemCount(); ++i) {
    if (document_->item(i).skipped) continue;
    needed.push_back(i);
  }
  bridge_->ensureHdrTiffs(needed, [this](bool ok, const QString& error) {
    if (!ok) {
      bridge_->setBusy(false, tr("HDR render failed"));
      QMessageBox::critical(this, tr("HDR render failed"),
                            error.isEmpty() ? tr("Lightroom could not render HDR TIFF.") : error);
      return;
    }
    encodeApprovedQueue();
  });
}

void MainWindow::encodeApprovedQueue() {
  bridge_->setBusy(true, tr("Encoding export queue…"));

  PreviewSession session_copy = document_->session();
  for (size_t i = 0; i < session_copy.items.size(); ++i) {
    auto* editor = bridge_->editorForIndex(static_cast<int>(i));
    const bool override_map = document_->state(static_cast<int>(i)).gainmap_override ||
                              (editor && !editor->auto_gain_map.empty() &&
                               editor->gain_map != editor->auto_gain_map);
    if (override_map && editor) {
      document_->updateGainMap(static_cast<int>(i), editor->gain_map, editor->width,
                               editor->height, true);
      document_->saveGainMap(static_cast<int>(i), nullptr);
      session_copy.items[i].gainmap_in = document_->item(static_cast<int>(i)).gainmap_in;
    } else {
      session_copy.items[i].gainmap_in.clear();
    }
  }

  auto future = QtConcurrent::run([session_copy = std::move(session_copy)]() mutable {
    PreviewResult result;
    std::string error;
    const int code = apply_session_batch(&session_copy, &result, &error);
    return std::pair<int, QString>{code, QString::fromStdString(error)};
  });

  auto* watcher = new QFutureWatcher<std::pair<int, QString>>(this);
  connect(watcher, &QFutureWatcher<std::pair<int, QString>>::finished, this, [this, watcher] {
    const auto [code, error] = watcher->result();
    watcher->deleteLater();
    if (code != 0) {
      bridge_->setBusy(false, tr("Encode failed"));
      QMessageBox::critical(this, tr("Encode failed"),
                            error.isEmpty() ? tr("The export queue could not be encoded.") : error);
      return;
    }
    approved_ = true;
    bridge_->markApproved();
    close();
  });
  watcher->setFuture(future);
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
