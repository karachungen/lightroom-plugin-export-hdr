#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <functional>
#include <string>
#include <vector>

class QFileSystemWatcher;
class QTimer;

namespace uhdr_repack {

class PreviewDocument;

class HdrTiffClient : public QObject {
  Q_OBJECT

 public:
  using Finished = std::function<void(bool ok, QString error)>;

  explicit HdrTiffClient(PreviewDocument* document, QObject* parent = nullptr);

  bool bridgeEnabled() const;
  /** Queue missing TIFFs without waiting. No-op (no error) when Lightroom is not attached. */
  void requestMissing(const std::vector<int>& indices);
  void ensure(const std::vector<int>& indices, Finished finished);
  void setPriority(const std::string& id);

 private:
  struct Waiter {
    std::vector<int> indices;
    Finished finished;
    QElapsedTimer clock;
  };

  void startWatch();
  void stopWatch();
  void maybeStopWatch();
  void poll();
  bool applyDone(int index);
  bool itemHasTiff(int index) const;
  bool allReady(const std::vector<int>& indices) const;
  bool containsPending(int index) const;
  std::string pendingIds() const;
  QString queueIndices(const std::vector<int>& indices, bool require_bridge);
  void failWaitersIncluding(int index, const QString& message);
  void flushWaiters();

  PreviewDocument* document_ = nullptr;
  QFileSystemWatcher* watcher_ = nullptr;
  QTimer* poll_timer_ = nullptr;
  QElapsedTimer wait_clock_;
  qint64 last_wait_log_ms_ = 0;
  std::vector<int> pending_;
  std::vector<Waiter> waiters_;
};
}  // namespace uhdr_repack
