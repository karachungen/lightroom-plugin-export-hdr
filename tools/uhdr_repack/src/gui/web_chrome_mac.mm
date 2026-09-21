#include "gui/web_chrome.h"

#include <QMetaObject>
#include <QResizeEvent>
#include <QUrl>
#include <QWidget>

#import <WebKit/WebKit.h>

@interface UhdrScriptHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) uhdr_repack::WebChrome* bridge;
@end

@implementation UhdrScriptHandler
- (void)userContentController:(WKUserContentController*)userContentController
      didReceiveScriptMessage:(WKScriptMessage*)message {
  Q_UNUSED(userContentController);
  if (!self.bridge || ![message.name isEqualToString:@"uhdrNative"]) {
    return;
  }
  const QString json = QString::fromNSString([message.body description]);
  QMetaObject::invokeMethod(self.bridge, "deliverMessage", Qt::QueuedConnection,
                            Q_ARG(QString, json));
}
@end

namespace uhdr_repack {

class WebChrome::Impl {
 public:
  QWidget* container = nullptr;
  WKWebView* web_view = nil;
  UhdrScriptHandler* handler = nil;
};

WebChrome::WebChrome(QObject* parent) : QObject(parent), impl_(new Impl) {}

WebChrome::~WebChrome() {
  if (impl_->web_view) {
    [impl_->web_view.configuration.userContentController removeScriptMessageHandlerForName:@"uhdrNative"];
    impl_->web_view = nil;
  }
  impl_->handler = nil;
  delete impl_;
  impl_ = nullptr;
}

bool WebChrome::attachTo(QWidget* container, QString* error) {
  if (!container) {
    if (error) *error = QStringLiteral("null container");
    return false;
  }
  impl_->container = container;
  impl_->handler = [UhdrScriptHandler new];
  impl_->handler.bridge = this;

  WKUserContentController* controller = [WKUserContentController new];
  [controller addScriptMessageHandler:impl_->handler name:@"uhdrNative"];

  WKWebViewConfiguration* config = [WKWebViewConfiguration new];
  config.userContentController = controller;

  NSString* bootstrap = @"window.uhdrNative = { post: function(msg) { "
                        @"window.webkit.messageHandlers.uhdrNative.postMessage(msg); } };";
  WKUserScript* script =
      [[WKUserScript alloc] initWithSource:bootstrap
                             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                          forMainFrameOnly:YES];
  [controller addUserScript:script];

  NSView* parent_view = reinterpret_cast<NSView*>(container->winId());
  impl_->web_view = [[WKWebView alloc] initWithFrame:parent_view.bounds configuration:config];
  impl_->web_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [parent_view addSubview:impl_->web_view];
  return true;
}

void WebChrome::loadApp(const QString& web_root) {
  if (!impl_->web_view) return;
  const QUrl url = QUrl::fromLocalFile(web_root + QStringLiteral("/index.html"));
  NSURL* nsurl = [NSURL fileURLWithPath:url.toLocalFile().toNSString()];
  [impl_->web_view loadFileURL:nsurl
       allowingReadAccessToURL:[NSURL fileURLWithPath:web_root.toNSString() isDirectory:YES]];
}

void WebChrome::postToPage(const QString& json) {
  if (!impl_->web_view) return;
  [impl_->web_view evaluateJavaScript:web_receive_script(json).toNSString() completionHandler:nil];
}

void WebChrome::evaluateJavaScript(const QString& script) {
  if (!impl_->web_view) return;
  [impl_->web_view evaluateJavaScript:script.toNSString() completionHandler:nil];
}

}  // namespace uhdr_repack
