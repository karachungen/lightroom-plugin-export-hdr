#pragma once

#include <QObject>
#include <QString>

class QWidget;

namespace uhdr_repack {

/** Extract embedded web assets to a folder suitable for WebView loading. */
QString extract_web_assets();

/** JS snippet that delivers JSON to window.uhdrReceive as a string (no QString::arg). */
QString web_receive_script(const QString& json);

class WebChrome : public QObject {
  Q_OBJECT

 public:
  explicit WebChrome(QObject* parent = nullptr);
  ~WebChrome() override;

  bool attachTo(QWidget* container, QString* error = nullptr);
  void loadApp(const QString& web_root);
  void postToPage(const QString& json);
  void evaluateJavaScript(const QString& script);

 signals:
  void messageReceived(const QString& json);
  void loadFinished();

 public slots:
  void deliverMessage(const QString& json) { emit messageReceived(json); }

 private:
  class Impl;
  Impl* impl_ = nullptr;
};

}  // namespace uhdr_repack
