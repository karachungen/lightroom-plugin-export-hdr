#include "gui/web_chrome.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace uhdr_repack {

namespace {

bool copyResourceTree(const QString& prefix, const QString& dest_root) {
  QDir dest(dest_root);
  if (!dest.exists() && !dest.mkpath(".")) {
    return false;
  }

  const auto entries = {
      std::pair{QStringLiteral(":/web/index.html"), QStringLiteral("index.html")},
      std::pair{QStringLiteral(":/web/app.css"), QStringLiteral("app.css")},
      std::pair{QStringLiteral(":/web/app.js"), QStringLiteral("app.js")},
      std::pair{QStringLiteral(":/web/fonts/fraunces.woff2"), QStringLiteral("fonts/fraunces.woff2")},
      std::pair{QStringLiteral(":/web/fonts/barlow-condensed.woff2"),
                QStringLiteral("fonts/barlow-condensed.woff2")},
  };

  for (const auto& [src, name] : entries) {
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
      return false;
    }
    const QString out_path = dest.filePath(name);
    QDir().mkpath(QFileInfo(out_path).absolutePath());
    QFile out(out_path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      return false;
    }
    if (out.write(in.readAll()) < 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

QString web_receive_script(const QString& json) {
  QString escaped = json;
  escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
  escaped.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
  escaped.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
  return QStringLiteral("if (window.uhdrReceive) window.uhdrReceive('") + escaped +
         QStringLiteral("');");
}

QString extract_web_assets() {
  static QString cached;
  if (!cached.isEmpty()) {
    return cached;
  }
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/uhdr_web");
  QDir().mkpath(base);
  if (!copyResourceTree(QStringLiteral(":/web"), base)) {
    return {};
  }
  cached = base;
  return cached;
}

}  // namespace uhdr_repack
