#include <QByteArray>
#include <QFile>
#include <QString>

#import <AppKit/AppKit.h>

namespace uhdr_repack {

void apply_macos_app_icon() {
  QFile file(QStringLiteral(":/resources/app.png"));
  if (!file.open(QIODevice::ReadOnly)) {
    return;
  }
  const QByteArray bytes = file.readAll();
  if (bytes.isEmpty()) {
    return;
  }
  NSData* data = [NSData dataWithBytes:bytes.constData()
                                length:static_cast<NSUInteger>(bytes.size())];
  NSImage* image = [[NSImage alloc] initWithData:data];
  if (image) {
    [NSApp setApplicationIconImage:image];
  }
}

}  // namespace uhdr_repack
