#include "gui/hdr_tiff_client.h"

#include "activity_log.h"
#include "gui/preview_document.h"
#include "hdr_bridge.h"
#include "path_io.h"

#include <QFileSystemWatcher>
#include <QTimer>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace uhdr_repack {

namespace {
constexpr int kPollMs = 200;
constexpr qint64 kTimeoutMs = 30LL * 60 * 1000;
constexpr qint64 kWaitLogMs = 2000;

std::string file_size_label(const std::string& path) {
  std::error_code ec;
  const auto bytes = fs::file_size(path_from_utf8(path), ec);
  if (ec) return path;
  return path + " " + std::to_string(bytes) + " bytes";
}

bool contains_index(const std::vector<int>& indices, int index) {
  return std::find(indices.begin(), indices.end(), index) != indices.end();
}
}  // namespace

HdrTiffClient::HdrTiffClient(PreviewDocument* document, QObject* parent)
    : QObject(parent), document_(document) {
  watcher_ = new QFileSystemWatcher(this);
  connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) { poll(); });
  connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString&) { poll(); });
  poll_timer_ = new QTimer(this);
  poll_timer_->setInterval(kPollMs);
  connect(poll_timer_, &QTimer::timeout, this, [this]() { poll(); });
}

bool HdrTiffClient::bridgeEnabled() const {
  if (!document_) return false;
  return hdr_bridge_enabled(document_->session().work_dir);
}

bool HdrTiffClient::itemHasTiff(int index) const {
  if (!document_ || index < 0 || index >= document_->itemCount()) return false;
  const auto& item = document_->item(index);
  std::error_code ec;
  return !item.hdr_tiff.empty() && fs::exists(path_from_utf8(item.hdr_tiff), ec);
}

bool HdrTiffClient::allReady(const std::vector<int>& indices) const {
  for (int index : indices) {
    if (!itemHasTiff(index)) return false;
  }
  return true;
}

bool HdrTiffClient::containsPending(int index) const {
  return contains_index(pending_, index);
}

void HdrTiffClient::setPriority(const std::string& id) {
  if (!document_ || id.empty() || !bridgeEnabled()) return;
  write_hdr_priority(document_->session().work_dir, id, nullptr);
}

void HdrTiffClient::requestMissing(const std::vector<int>& indices) {
  queueIndices(indices, false);
}

void HdrTiffClient::ensure(const std::vector<int>& indices, Finished finished) {
  const QString error = queueIndices(indices, true);
  if (!error.isEmpty()) {
    if (finished) finished(false, error);
    return;
  }
  if (allReady(indices)) {
    if (finished) finished(true, {});
    return;
  }
  if (!indices.empty() && document_ && indices[0] >= 0 && indices[0] < document_->itemCount()) {
    setPriority(document_->item(indices[0]).id);
  }
  Waiter waiter;
  waiter.indices = indices;
  waiter.finished = std::move(finished);
  waiter.clock.start();
  waiters_.push_back(std::move(waiter));
  startWatch();
}

QString HdrTiffClient::queueIndices(const std::vector<int>& indices, bool require_bridge) {
  if (!document_) {
    return require_bridge ? tr("Preview document is missing.") : QString();
  }

  const std::string work_dir = document_->session().work_dir;
  const bool bridge = hdr_bridge_enabled(work_dir);
  bool queued = false;

  for (int index : indices) {
    if (index < 0 || index >= document_->itemCount()) {
      return tr("Invalid photo index.");
    }
    const auto& item = document_->item(index);
    if (itemHasTiff(index)) {
      if (require_bridge) {
        activity_log_append(item.id, "wait_tiff", "already on disk " + file_size_label(item.hdr_tiff));
      }
      continue;
    }
    if (hdr_done_has_valid_tiff(work_dir, item.id)) {
      if (!applyDone(index)) {
        return tr("Could not apply rendered HDR TIFF.");
      }
      continue;
    }
    if (!bridge) {
      if (require_bridge) {
        activity_log_append(item.id, "wait_tiff", "fail Lightroom is not attached");
        return tr("HDR TIFF is not available (Lightroom is not attached).");
      }
      continue;
    }
    std::string error;
    if (!write_hdr_request(work_dir, item.id, &error)) {
      activity_log_append(item.id, "wait_tiff",
                          "fail request " + (error.empty() ? std::string("write") : error));
      return QString::fromStdString(error.empty() ? "Could not request HDR TIFF." : error);
    }
    activity_log_append(item.id, "wait_tiff", "requested Lightroom HDR TIFF");
    if (!containsPending(index)) pending_.push_back(index);
    queued = true;
  }

  if (queued) startWatch();
  return {};
}

void HdrTiffClient::startWatch() {
  if (!document_) return;
  const QString dir = QString::fromStdString(hdr_requests_dir(document_->session().work_dir));
  if (!watcher_->directories().contains(dir)) {
    watcher_->addPath(dir);
  }
  if (!wait_clock_.isValid()) {
    wait_clock_.start();
    last_wait_log_ms_ = 0;
  }
  if (!poll_timer_->isActive()) poll_timer_->start();
  poll();
}

void HdrTiffClient::stopWatch() {
  poll_timer_->stop();
  wait_clock_.invalidate();
  const auto dirs = watcher_->directories();
  if (!dirs.isEmpty()) watcher_->removePaths(dirs);
  const auto files = watcher_->files();
  if (!files.isEmpty()) watcher_->removePaths(files);
}

void HdrTiffClient::maybeStopWatch() {
  if (pending_.empty() && waiters_.empty()) stopWatch();
}

void HdrTiffClient::poll() {
  if (pending_.empty() && waiters_.empty()) return;
  if (wait_clock_.isValid()) {
    const qint64 elapsed = wait_clock_.elapsed();
    if (elapsed - last_wait_log_ms_ >= kWaitLogMs) {
      last_wait_log_ms_ = elapsed;
      if (!pending_.empty()) {
        activity_log_append(pendingIds(), "wait_tiff", "still waiting on Lightroom",
                            static_cast<int>(elapsed));
      }
    }
  }

  std::vector<int> still;
  still.reserve(pending_.size());
  for (int index : pending_) {
    if (itemHasTiff(index)) continue;
    const auto& item = document_->item(index);
    HdrRequestResult result;
    if (!read_hdr_done(document_->session().work_dir, item.id, &result, nullptr)) {
      still.push_back(index);
      continue;
    }
    if (!result.ok) {
      const QString message = result.error.empty()
                                  ? tr("Lightroom could not render HDR TIFF.")
                                  : QString::fromStdString(result.error);
      activity_log_append(item.id, "wait_tiff",
                          "fail " + (result.error.empty() ? std::string("Lightroom render") : result.error),
                          wait_clock_.isValid() ? static_cast<int>(wait_clock_.elapsed()) : -1);
      failWaitersIncluding(index, message);
      continue;
    }
    if (!applyDone(index)) {
      const QString message = result.error.empty() ? tr("Rendered HDR TIFF is missing.")
                                                   : QString::fromStdString(result.error);
      activity_log_append(item.id, "wait_tiff", "fail missing TIFF after done",
                          wait_clock_.isValid() ? static_cast<int>(wait_clock_.elapsed()) : -1);
      failWaitersIncluding(index, message);
      continue;
    }
  }
  pending_ = std::move(still);
  flushWaiters();
}

std::string HdrTiffClient::pendingIds() const {
  std::ostringstream out;
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (i) out << ",";
    out << document_->item(pending_[i]).id;
  }
  return out.str();
}

bool HdrTiffClient::applyDone(int index) {
  HdrRequestResult result;
  const auto& item = document_->item(index);
  if (!read_hdr_done(document_->session().work_dir, item.id, &result, nullptr) || !result.ok) {
    return false;
  }
  std::error_code ec;
  if (result.hdr_tiff.empty() || !fs::exists(path_from_utf8(result.hdr_tiff), ec)) {
    return false;
  }
  document_->setHdrTiff(index, result.hdr_tiff);
  activity_log_append(item.id, "wait_tiff", "ready " + file_size_label(result.hdr_tiff),
                      wait_clock_.isValid() ? static_cast<int>(wait_clock_.elapsed()) : -1);
  std::error_code rec;
  fs::remove(path_from_utf8(hdr_request_path(document_->session().work_dir, item.id)), rec);
  return true;
}

void HdrTiffClient::failWaitersIncluding(int index, const QString& message) {
  auto current = std::move(waiters_);
  waiters_.clear();
  for (auto& waiter : current) {
    if (contains_index(waiter.indices, index)) {
      activity_log_append(document_->item(index).id, "wait_tiff", "fail " + message.toStdString(),
                          waiter.clock.isValid() ? static_cast<int>(waiter.clock.elapsed()) : -1);
      auto cb = std::move(waiter.finished);
      if (cb) cb(false, message);
    } else {
      waiters_.push_back(std::move(waiter));
    }
  }
}

void HdrTiffClient::flushWaiters() {
  auto current = std::move(waiters_);
  waiters_.clear();
  for (auto& waiter : current) {
    if (waiter.clock.isValid() && waiter.clock.elapsed() >= kTimeoutMs) {
      activity_log_append(pendingIds(), "wait_tiff", "fail timed out",
                          static_cast<int>(waiter.clock.elapsed()));
      auto cb = std::move(waiter.finished);
      if (cb) cb(false, tr("Timed out waiting for Lightroom to render HDR TIFF."));
      continue;
    }
    if (allReady(waiter.indices)) {
      auto cb = std::move(waiter.finished);
      if (cb) cb(true, {});
      continue;
    }
    waiters_.push_back(std::move(waiter));
  }
  maybeStopWatch();
}

}  // namespace uhdr_repack
