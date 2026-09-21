#pragma once

#include "session.h"

#include <string>

namespace uhdr_repack {

/** Run Qt edit UI; returns process exit code. */
int gui_edit_main(PreviewSession session);

/** Headless Qt offscreen self-test for preview/gainmap pipeline. */
int gui_self_test_main(const std::string& session_path);

}  // namespace uhdr_repack
