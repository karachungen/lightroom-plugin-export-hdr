#include "gui/web_chrome.h"

#include <QDir>
#include <QUrl>
#include <QWidget>

#include <windows.h>

#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

namespace uhdr_repack {

class WebChrome::Impl {
 public:
  QWidget* container = nullptr;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
  Microsoft::WRL::ComPtr<ICoreWebView2> webview;
  EventRegistrationToken message_token{};
  WebChrome* owner = nullptr;
};

WebChrome::WebChrome(QObject* parent) : QObject(parent), impl_(new Impl) {
  impl_->owner = this;
}

WebChrome::~WebChrome() {
  if (impl_->webview) {
    impl_->webview->remove_WebMessageReceived(impl_->message_token);
  }
  impl_->controller = nullptr;
  impl_->webview = nullptr;
  delete impl_;
  impl_ = nullptr;
}

bool WebChrome::attachTo(QWidget* container, QString* error) {
  if (!container) {
    if (error) *error = QStringLiteral("null container");
    return false;
  }
  impl_->container = container;

  const HWND hwnd = reinterpret_cast<HWND>(container->winId());
  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this, hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) return result;
            env->CreateCoreWebView2Controller(
                hwnd,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result) || !controller) return result;
                      impl_->controller = controller;
                      impl_->controller->get_CoreWebView2(&impl_->webview);
                      RECT bounds{};
                      GetClientRect(reinterpret_cast<HWND>(impl_->container->winId()), &bounds);
                      impl_->controller->put_Bounds(bounds);

                      const wchar_t* bootstrap =
                          L"window.uhdrNative = { post: function(msg) { "
                          L"chrome.webview.postMessage(msg); } };";
                      impl_->webview->AddScriptToExecuteOnDocumentCreated(
                          bootstrap, nullptr);

                      impl_->webview->add_WebMessageReceived(
                          Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [this](ICoreWebView2* sender,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                Q_UNUSED(sender);
                                LPWSTR message = nullptr;
                                args->TryGetWebMessageAsString(&message);
                                if (message) {
                                  emit impl_->owner->messageReceived(QString::fromWCharArray(message));
                                  CoTaskMemFree(message);
                                }
                                return S_OK;
                              })
                              .Get(),
                          &impl_->message_token);
                      emit loadFinished();
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
  return true;
}

void WebChrome::loadApp(const QString& web_root) {
  if (!impl_->webview) return;
  const QString host = QStringLiteral("uhdr.app");
  const std::wstring folder = web_root.toStdWString();
  impl_->webview->SetVirtualHostNameToFolderMapping(
      host.toStdWString().c_str(), folder.c_str(),
      COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
  impl_->webview->Navigate((L"https://" + host.toStdWString() + L"/index.html").c_str());
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
