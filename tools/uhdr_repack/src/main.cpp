#include "cli.h"
#include "session.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef UHDR_ENABLE_GUI
#include "gui_app.h"
#endif

#ifdef _WIN32
#include <objbase.h>
#include <windows.h>
#endif

namespace uhdr_repack {

#ifdef _WIN32
namespace {

struct ComInit {
  HRESULT hr;
  ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComInit() {
    if (SUCCEEDED(hr)) {
      CoUninitialize();
    }
  }
};

std::string wide_to_utf8(const wchar_t* wide) {
  if (!wide || wide[0] == L'\0') {
    return std::string();
  }
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return std::string();
  }
  std::string utf8(static_cast<size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), needed, nullptr, nullptr);
  return utf8;
}

}  // namespace
#endif

static int run(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }

  if (std::strcmp(argv[1], "--inspect") == 0) {
    return cli_inspect_main(argc, argv);
  }

  if (std::strcmp(argv[1], "--dump-gainmap") == 0) {
    return cli_dump_gainmap_main(argc, argv);
  }

  if (std::strcmp(argv[1], "--self-test") == 0) {
#ifndef UHDR_ENABLE_GUI
    std::cerr << "uhdr_repack was built without GUI support (--self-test unavailable)\n";
    return 1;
#else
    std::string session_path = "test/ui/preview_session.json";
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
        session_path = argv[++i];
      }
    }
    return gui_self_test_main(session_path);
#endif
  }

  bool edit_mode = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--edit") == 0) {
      edit_mode = true;
      break;
    }
  }

  if (edit_mode) {
#ifndef UHDR_ENABLE_GUI
    std::cerr << "uhdr_repack was built without GUI support (--edit unavailable)\n";
    return 1;
#else
    PreviewSession session;
    std::string err;
    if (!parse_edit_cli_args(argc, argv, &session, &err)) {
      std::cerr << err << "\n";
      print_usage();
      return 1;
    }
    return gui_edit_main(std::move(session));
#endif
  }

  return cli_encode_main(argc, argv);
}

}  // namespace uhdr_repack

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {
  uhdr_repack::ComInit com;
  if (com.hr != S_OK && com.hr != S_FALSE && FAILED(com.hr)) {
    std::cerr << "CoInitializeEx failed\n";
    return 1;
  }

  std::vector<std::string> utf8_args;
  std::vector<char*> argv_ptrs;
  utf8_args.reserve(static_cast<size_t>(argc));
  argv_ptrs.reserve(static_cast<size_t>(argc) + 1);
  for (int i = 0; i < argc; ++i) {
    utf8_args.push_back(uhdr_repack::wide_to_utf8(wargv[i]));
    argv_ptrs.push_back(utf8_args.back().data());
  }
  argv_ptrs.push_back(nullptr);
  return uhdr_repack::run(argc, argv_ptrs.data());
}
#else
int main(int argc, char** argv) {
  return uhdr_repack::run(argc, argv);
}
#endif
