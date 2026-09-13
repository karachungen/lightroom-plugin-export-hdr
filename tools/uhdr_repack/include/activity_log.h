#pragma once

#include <functional>
#include <string>

namespace uhdr_repack {

struct PreviewSession;

using ActivityLogHook = std::function<void(const std::string& line, const std::string& log_path)>;

void activity_log_configure(const std::string& work_dir, const std::string& dest_log_path);
void activity_log_configure_session(const PreviewSession& session);
void activity_log_set_hook(ActivityLogHook hook);

std::string activity_log_work_path();
std::string activity_log_dest_path();
std::string activity_log_primary_path();

/** One line: timestamp, photo id, phase, optional elapsed, detail. */
void activity_log_append(const std::string& photo_id, const std::string& phase,
                         const std::string& detail, int elapsed_ms = -1);

}  // namespace uhdr_repack
