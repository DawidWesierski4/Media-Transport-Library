/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <grp.h>
#include <signal.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "mtlm2_server.hpp"

namespace {

bool parse_octal_mode(const std::string& text, mode_t* out) {
  if (text.empty() || text.size() > 4 || !out) return false;
  for (char c : text) {
    if (c < '0' || c > '7') return false;
  }
  char* end = nullptr;
  unsigned long v = std::strtoul(text.c_str(), &end, 8);
  if (!end || *end != '\0' || v > 0777u) return false;
  *out = static_cast<mode_t>(v);
  return true;
}

bool resolve_group(const std::string& spec, gid_t* out) {
  if (spec.empty() || !out) return false;
  if (struct group* gr = ::getgrnam(spec.c_str())) {
    *out = gr->gr_gid;
    return true;
  }
  for (char c : spec) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  char* end = nullptr;
  unsigned long long v = std::strtoull(spec.c_str(), &end, 10);
  if (!end || *end != '\0' ||
      v > static_cast<unsigned long long>(std::numeric_limits<gid_t>::max()))
    return false;
  *out = static_cast<gid_t>(v);
  return true;
}

void print_usage(const char* prog) {
  std::cout << "Usage: " << prog << " [OPTIONS]\n"
            << "  --socket <path>           Client Varlink socket (default: "
            << MT_VARLINK_DEFAULT_SOCK_PATH << ")\n"
            << "  --admin-socket <path>     Admin Varlink socket, 0600 (default: "
            << MT_VARLINK_DEFAULT_ADMIN_SOCK_PATH << ")\n"
            << "  --socket-mode <octal>     Client socket mode (0666 zero-arg, 0660 with "
               "--policy-file)\n"
            << "  --socket-group <grp|gid>  Client socket group ownership\n"
            << "  --state-dir <path>        State directory for journal.json (default: "
               "/run/imtl/state)\n"
            << "  --host-lock <path>        Exclusive host lock (default: "
               "/run/imtl/.host.lock with default paths)\n"
            << "  --v1-socket <path>        Legacy v1 socket probe path (default: "
               "/var/run/imtl/mtl_manager.sock)\n"
            << "  --open-session-timeout-ms <ms>  Pre-session idle connection timeout "
               "(default: 5000)\n"
            << "  --policy-file <path>      Flat JSON allowlist policy file\n"
            << "  --fail-on-legacy-shm      Refuse startup if live legacy shm clients "
               "exist\n"
            << "  --shm-lock-path <path>    Override legacy shm lock path (for tests)\n"
            << "  --shm-lock <path>         Alias for --shm-lock-path\n"
            << "  --shm-ftok-path <path>    Override legacy shm ftok path (for tests)\n"
            << "  --teardown-on-exit        Release all leases and shm slots on normal "
               "stop (for tests)\n"
            << "  -h, --help                Show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
  /* Block termination signals BEFORE opening listeners or epoll (ISSUE-90). */
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGQUIT);
  sigaddset(&mask, SIGPIPE);
  if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
    std::cerr << "MtlManager: pthread_sigmask failed\n";
    return 1;
  }

  mtlm2::ServerConfig cfg;
  std::string policy_file;
  bool mode_explicit = false;
  bool custom_paths = false;
  bool host_lock_explicit = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need_val = [&](const char* opt) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "MtlManager: missing value for " << opt << "\n";
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--socket") {
      cfg.client_socket_path = need_val("--socket");
      custom_paths = true;
    } else if (arg == "--admin-socket") {
      cfg.admin_socket_path = need_val("--admin-socket");
      custom_paths = true;
    } else if (arg == "--socket-mode") {
      std::string mstr = need_val("--socket-mode");
      if (!parse_octal_mode(mstr, &cfg.client_socket_mode)) {
        std::cerr << "MtlManager: invalid --socket-mode '" << mstr << "'\n";
        return 2;
      }
      mode_explicit = true;
    } else if (arg == "--socket-group") {
      std::string gstr = need_val("--socket-group");
      gid_t gid = 0;
      if (!resolve_group(gstr, &gid)) {
        std::cerr << "MtlManager: invalid --socket-group '" << gstr << "'\n";
        return 2;
      }
      cfg.client_socket_gid = gid;
    } else if (arg == "--state-dir") {
      cfg.state_dir = need_val("--state-dir");
      custom_paths = true;
    } else if (arg == "--host-lock") {
      cfg.host_lock_path = need_val("--host-lock");
      host_lock_explicit = true;
    } else if (arg == "--v1-socket") {
      cfg.v1_socket_path = need_val("--v1-socket");
    } else if (arg == "--open-session-timeout-ms") {
      std::string tstr = need_val("--open-session-timeout-ms");
      char* end = nullptr;
      errno = 0;
      long long ms = std::strtoll(tstr.c_str(), &end, 10);
      if (errno != 0 || !end || *end != '\0' || ms < 0 || ms > 3600000LL) {
        std::cerr << "MtlManager: invalid --open-session-timeout-ms '" << tstr << "'\n";
        return 2;
      }
      cfg.open_session_timeout_ms = static_cast<int64_t>(ms);
    } else if (arg == "--policy-file") {
      policy_file = need_val("--policy-file");
    } else if (arg == "--fail-on-legacy-shm") {
      cfg.shm.fail_on_legacy_shm = true;
    } else if (arg == "--shm-lock-path" || arg == "--shm-lock") {
      cfg.shm.lock_path = need_val(arg.c_str());
    } else if (arg == "--shm-ftok-path") {
      cfg.shm.ftok_path = need_val("--shm-ftok-path");
    } else if (arg == "--teardown-on-exit") {
      cfg.teardown_on_exit = true;
    } else {
      std::cerr << "MtlManager: unknown option '" << arg << "'\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  if (!host_lock_explicit && !custom_paths) {
    cfg.host_lock_path = "/run/imtl/.host.lock";
  }

  if (policy_file.empty()) {
    /* Zero-argument / single-host default mode (R4-5). */
    cfg.policy = mtlm2::PolicyConfig::make_zero_arg_single_host();
    cfg.allow_world_client_socket = true;
    if (!mode_explicit) cfg.client_socket_mode = 0666;
  } else {
    std::string perr;
    if (!mtlm2::PolicyConfig::load_file(policy_file, &cfg.policy, &perr)) {
      std::cerr << "MtlManager: policy load failed: " << perr << "\n";
      return 1;
    }
    cfg.allow_world_client_socket = false;
    if (!mode_explicit) cfg.client_socket_mode = 0660;
  }

  mtlm2::MtlManagerServer server(std::move(cfg));
  std::string err;
  if (!server.start(&err)) {
    std::cerr << "MtlManager: startup failed: " << err << "\n";
    return 1;
  }

  return server.run();
}
