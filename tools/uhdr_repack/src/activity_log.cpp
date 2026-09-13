#include "activity_log.h"

#include "path_io.h"
#include "session.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace uhdr_repack {

namespace {

std::mutex g_mu;
std::string g_work_path;
std::string g_dest_path;
ActivityLogHook g_hook;

std::string iso_utc_now() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

void append_path(const std::string& path, const std::string& line) {
  if (path.empty()) return;
  std::error_code ec;
  fs::create_directories(path_from_utf8(path).parent_path(), ec);
  std::ofstream out(path_from_utf8(path), std::ios::out | std::ios::app);
  if (!out) return;
  out << line << "\n";
}

}  // namespace

void activity_log_configure(const std::string& work_dir, const std::string& dest_log_path) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_work_path.clear();
  g_dest_path.clear();
  if (!work_dir.empty()) {
    g_work_path = (path_from_utf8(work_dir) / "uhdr_preview.log").u8string();
  }
  g_dest_path = dest_log_path;
}

void activity_log_configure_session(const PreviewSession& session) {
  std::string dest = session.log_path;
  if (dest.empty() && !session.dest_dir.empty()) {
    dest = (path_from_utf8(session.dest_dir) / "uhdr_preview.log").u8string();
  }
  activity_log_configure(session.work_dir, dest);
}

void activity_log_set_hook(ActivityLogHook hook) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_hook = std::move(hook);
}

std::string activity_log_work_path() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_work_path;
}

std::string activity_log_dest_path() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_dest_path;
}

std::string activity_log_primary_path() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_dest_path.empty()) return g_dest_path;
  return g_work_path;
}

void activity_log_append(const std::string& photo_id, const std::string& phase,
                         const std::string& detail, int elapsed_ms) {
  std::ostringstream line;
  line << iso_utc_now();
  if (!photo_id.empty()) line << " " << photo_id;
  if (!phase.empty()) line << " " << phase;
  if (elapsed_ms >= 0) line << " +" << elapsed_ms << "ms";
  if (!detail.empty()) line << " " << detail;
  const std::string text = line.str();

  ActivityLogHook hook;
  std::string work;
  std::string dest;
  std::string primary;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    work = g_work_path;
    dest = g_dest_path;
    primary = dest.empty() ? work : dest;
    hook = g_hook;
  }
  append_path(work, text);
  if (!dest.empty() && dest != work) append_path(dest, text);
  if (hook) hook(text, primary);
}

}  // namespace uhdr_repack
