#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include "activity_log.h"
#include "gui/web_chrome.h"

#include <QDir>
#include <QMetaObject>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace uhdr_repack {

namespace {

std::string hr_hex(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

void log_webview(const std::string& detail) {
  activity_log_append("", "webview", detail);
}

std::wstring folderPathForVirtualHost(const QString& web_root) {
  QString folder = QDir(web_root).absolutePath();
  if (!folder.endsWith(QLatin1Char('\\')) && !folder.endsWith(QLatin1Char('/'))) {
    folder += QDir::separator();
  }
  return folder.toStdWString();
}

RECT clientBounds(QWidget* container) {
  RECT bounds{};
  if (!container) {
    return bounds;
  }
  const HWND hwnd = reinterpret_cast<HWND>(container->winId());
  if (hwnd && GetClientRect(hwnd, &bounds) && bounds.right > 0 && bounds.bottom > 0) {
    return bounds;
  }
  const QRect rect = container->rect();
  const qreal dpr = container->devicePixelRatioF();
  bounds.left = 0;
  bounds.top = 0;
  bounds.right = static_cast<LONG>(std::lround(rect.width() * dpr));
  bounds.bottom = static_cast<LONG>(std::lround(rect.height() * dpr));
  return bounds;
}

}  // namespace

class WebChrome::Impl {
 public:
  QWidget* container = nullptr;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
  Microsoft::WRL::ComPtr<ICoreWebView2> webview;
  EventRegistrationToken message_token{};
  EventRegistrationToken navigation_token{};
  WebChrome* owner = nullptr;
  QString pending_web_root;

  void reportFailure(const QString& error) {
    log_webview("fail " + error.toStdString());
    if (!owner) return;
    QMetaObject::invokeMethod(
        owner, [owner = owner, error]() { emit owner->attachFailed(error); },
        Qt::QueuedConnection);
  }

  void syncBounds() {
    if (!controller || !container) return;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller3> controller3;
    if (SUCCEEDED(controller.As(&controller3))) {
      controller3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
    }
    const RECT bounds = clientBounds(container);
    controller->put_Bounds(bounds);
    controller->put_IsVisible(TRUE);
  }

  void navigateToApp() {
    if (!webview || pending_web_root.isEmpty()) return;

    const QString host = QStringLiteral("uhdr.app");
    const std::wstring folder = folderPathForVirtualHost(pending_web_root);
    bool mapped = false;
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(webview.As(&webview3))) {
      const HRESULT hr = webview3->SetVirtualHostNameToFolderMapping(
          host.toStdWString().c_str(), folder.c_str(),
          COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
      mapped = SUCCEEDED(hr);
      log_webview(std::string("virtual host ") + (mapped ? "ok" : "fail ") + hr_hex(hr) + " " +
                  QDir(pending_web_root).absolutePath().toStdString());
    } else {
      log_webview("virtual host unavailable (no ICoreWebView2_3)");
    }

    if (mapped) {
      const std::wstring url = L"https://" + host.toStdWString() + L"/index.html";
      log_webview("navigate https://uhdr.app/index.html");
      const HRESULT hr = webview->Navigate(url.c_str());
      if (FAILED(hr)) {
        reportFailure(QStringLiteral("WebView2 Navigate failed (%1).")
                          .arg(QString::fromStdString(hr_hex(hr))));
      }
      return;
    }

    const QUrl url =
        QUrl::fromLocalFile(QDir(pending_web_root).filePath(QStringLiteral("index.html")));
    const QString encoded = url.toString(QUrl::FullyEncoded);
    log_webview("navigate " + encoded.toStdString());
    const HRESULT hr = webview->Navigate(encoded.toStdWString().c_str());
    if (FAILED(hr)) {
      reportFailure(QStringLiteral("WebView2 file Navigate failed (%1).")
                        .arg(QString::fromStdString(hr_hex(hr))));
    }
  }
};

WebChrome::WebChrome(QObject* parent) : QObject(parent), impl_(new Impl) {
  impl_->owner = this;
}

WebChrome::~WebChrome() {
  if (impl_->webview) {
    impl_->webview->remove_WebMessageReceived(impl_->message_token);
    impl_->webview->remove_NavigationCompleted(impl_->navigation_token);
  }
  impl_->controller = nullptr;
  impl_->webview = nullptr;
  delete impl_;
  impl_ = nullptr;
}

bool WebChrome::attachTo(QWidget* container, QString* error) {
  if (!container) {
    if (error) *error = QStringLiteral("null container");
    log_webview("fail null container");
    return false;
  }
  impl_->container = container;
  container->setAttribute(Qt::WA_NativeWindow);
  container->setAttribute(Qt::WA_DontCreateNativeAncestors);
  container->setAttribute(Qt::WA_NoSystemBackground);
  container->setAutoFillBackground(false);

  const HWND hwnd = reinterpret_cast<HWND>(container->winId());
  const QRect rect = container->rect();
  log_webview("attach hwnd size=" + std::to_string(rect.width()) + "x" +
              std::to_string(rect.height()));

  const HRESULT create_hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
              impl_->reportFailure(
                  QStringLiteral(
                      "WebView2 environment failed (%1). Install the Microsoft Edge WebView2 Runtime.")
                      .arg(QString::fromStdString(hr_hex(result))));
              return result;
            }
            log_webview("env ok");
            env->CreateCoreWebView2Controller(
                reinterpret_cast<HWND>(impl_->container->winId()),
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result) || !controller) {
                        impl_->reportFailure(
                            QStringLiteral(
                                "WebView2 controller failed (%1). Install the Microsoft Edge WebView2 Runtime.")
                                .arg(QString::fromStdString(hr_hex(result))));
                        return result;
                      }
                      impl_->controller = controller;
                      impl_->controller->get_CoreWebView2(&impl_->webview);
                      if (!impl_->webview) {
                        impl_->reportFailure(QStringLiteral("WebView2 core object is missing."));
                        return E_FAIL;
                      }
                      impl_->syncBounds();
                      const RECT bounds = clientBounds(impl_->container);
                      log_webview("controller ok bounds=" + std::to_string(bounds.right) + "x" +
                                  std::to_string(bounds.bottom));

                      const wchar_t* bootstrap =
                          L"window.uhdrNative = { post: function(msg) { "
                          L"chrome.webview.postMessage(msg); } };";
                      impl_->webview->AddScriptToExecuteOnDocumentCreated(bootstrap, nullptr);

                      impl_->webview->add_WebMessageReceived(
                          Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [this](ICoreWebView2* sender,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                Q_UNUSED(sender);
                                LPWSTR message = nullptr;
                                args->TryGetWebMessageAsString(&message);
                                if (message) {
                                  emit impl_->owner->messageReceived(
                                      QString::fromWCharArray(message));
                                  CoTaskMemFree(message);
                                }
                                return S_OK;
                              })
                              .Get(),
                          &impl_->message_token);

                      impl_->webview->add_NavigationCompleted(
                          Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [this](ICoreWebView2* sender,
                                     ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                BOOL ok = FALSE;
                                args->get_IsSuccess(&ok);
                                COREWEBVIEW2_WEB_ERROR_STATUS status =
                                    COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                args->get_WebErrorStatus(&status);
                                LPWSTR uri = nullptr;
                                if (sender) sender->get_Source(&uri);
                                const std::string uri_utf8 =
                                    uri ? QString::fromWCharArray(uri).toStdString() : std::string();
                                if (uri) CoTaskMemFree(uri);
                                if (ok) {
                                  log_webview("navigation ok " + uri_utf8);
                                } else {
                                  log_webview("navigation fail status=" +
                                              std::to_string(static_cast<int>(status)) + " " +
                                              uri_utf8);
                                }
                                return S_OK;
                              })
                              .Get(),
                          &impl_->navigation_token);

                      impl_->navigateToApp();
                      impl_->syncBounds();
                      emit loadFinished();
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
  if (FAILED(create_hr)) {
    const QString msg =
        QStringLiteral(
            "WebView2 CreateCoreWebView2Environment failed (%1). Install the Microsoft Edge WebView2 Runtime.")
            .arg(QString::fromStdString(hr_hex(create_hr)));
    if (error) *error = msg;
    log_webview("fail " + msg.toStdString());
    return false;
  }
  log_webview("env create started");
  return true;
}

void WebChrome::loadApp(const QString& web_root) {
  impl_->pending_web_root = web_root;
  log_webview("web root " + QDir(web_root).absolutePath().toStdString());
  impl_->navigateToApp();
}

void WebChrome::syncBounds() {
  impl_->syncBounds();
}

void WebChrome::postToPage(const QString& json) {
  if (!impl_->webview) return;
  impl_->webview->ExecuteScript(web_receive_script(json).toStdWString().c_str(), nullptr);
}

void WebChrome::evaluateJavaScript(const QString& script) {
  if (!impl_->webview) return;
  impl_->webview->ExecuteScript(script.toStdWString().c_str(), nullptr);
}

}  // namespace uhdr_repack
