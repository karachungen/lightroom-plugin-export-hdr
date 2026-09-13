#include "gui_app.h"

#include "gui/main_window.h"

#include <QApplication>
#include <QIcon>
#include <QImageReader>
#include <QPalette>

namespace uhdr_repack {

#ifdef Q_OS_MACOS
void apply_macos_app_icon();
#endif

int gui_edit_main(PreviewSession session) {
  int argc = 0;
  char** argv = nullptr;
  QApplication app(argc, argv);
  QImageReader::setAllocationLimit(0);
  app.setApplicationName(QStringLiteral("Ultra HDR"));
  app.setWindowIcon(QIcon(QStringLiteral(":/resources/app.png")));
#ifdef Q_OS_MACOS
  apply_macos_app_icon();
#endif
  app.setStyle(QStringLiteral("Fusion"));
  app.setStyleSheet(QStringLiteral(R"(
    * { font-size: 13px; }
    QMainWindow, QWidget#workspace, QWidget#filmstripPanel { background: #111319; color: #e8eaf0; }
    QWidget#filmstripPanel { border-right: 1px solid #292d38; }
    QWidget#previewToolbar, QWidget#actionBar {
      background: #171a22; border-bottom: 1px solid #292d38;
    }
    QWidget#actionBar { border-top: 1px solid #292d38; border-bottom: 0; }
    QLabel#eyebrow { color: #858b9c; font-size: 11px; font-weight: 700; letter-spacing: 1px; }
    QLabel#imageTitle { font-size: 15px; font-weight: 600; }
    QLabel#secondaryText { color: #8f95a5; font-size: 12px; }
    QLabel#statusText { color: #aeb4c3; }
    QListWidget#filmstrip {
      background: transparent; border: 0; outline: 0; padding: 0;
    }
    QListWidget#filmstrip::item {
      background: #191c24; border: 1px solid transparent; border-radius: 7px;
      padding: 7px; margin: 2px 0;
    }
    QListWidget#filmstrip::item:selected {
      background: #242936; border-color: #667eea;
    }
    QToolButton#modeButton, QToolButton#compareButton {
      color: #aeb4c3; background: #20242e; border: 1px solid #303543;
      border-radius: 6px; padding: 6px 11px;
    }
    QToolButton#modeButton:checked { color: white; background: #4f63c8; border-color: #7488ed; }
    QToolButton#modeButton:hover, QToolButton#compareButton:hover { background: #2a2f3c; }
    QDockWidget { color: #e8eaf0; background: #151820; }
    QDockWidget::title { padding: 12px 14px; background: #191c24; font-weight: 600; }
    QGroupBox {
      border: 1px solid #2b303c; border-radius: 7px; margin-top: 10px;
      padding: 12px 8px 8px 8px; font-weight: 600; color: #cfd3de;
    }
    QGroupBox::title { subcontrol-origin: margin; left: 9px; padding: 0 4px; }
    QSlider::groove:horizontal { height: 4px; background: #343946; border-radius: 2px; }
    QSlider::sub-page:horizontal { background: #7185ea; border-radius: 2px; }
    QSlider::handle:horizontal {
      background: #e5e8f2; border: 1px solid #7d8495; width: 14px;
      margin: -5px 0; border-radius: 7px;
    }
    QPushButton, QToolButton {
      color: #e1e4ec; background: #252a35; border: 1px solid #363c49;
      border-radius: 6px; padding: 7px 12px;
    }
    QPushButton:hover, QToolButton:hover { background: #303643; }
    QPushButton#primaryButton { background: #5b6fd4; border-color: #7488ed; font-weight: 600; }
    QPushButton#primaryButton:hover { background: #687de2; }
    QLabel#displayStatus[hdrActive="true"] { color: #9ee6bf; }
    QLabel#displayStatus[hdrActive="false"] { color: #f1c17b; }
    QProgressBar { border: 0; background: #2a2f3a; height: 5px; border-radius: 2px; }
    QProgressBar::chunk { background: #7185ea; border-radius: 2px; }
  )"));

  MainWindow window(std::move(session));
  window.show();

  app.exec();
  return window.userApproved() ? 0 : 2;
}

}  // namespace uhdr_repack
