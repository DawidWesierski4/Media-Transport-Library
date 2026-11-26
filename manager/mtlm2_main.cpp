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
            << "  --xdp-prog <path>         Override mtl.xdp.o object path\n"
            << "  --heartbeat-policy <warn_only|revoke>  Heartbeat expiry policy "
               "(default: warn_only)\n"
            << "  --heartbeat-eviction-action <none|revoke>  Heartbeat eviction action "
               "(default: none)\n"
            << "  --heartbeat-interval-ms <ms>  Default heartbeat interval in ms "
               "(default: 5000)\n"
            << "  --heartbeat-max-missed <n>    Default missed beats threshold "
               "(default: 6)\n"
            << "  --heartbeat-min-timeout-ms <ms>  Minimum allowed heartbeat timeout "
               "(default: 5000)\n"
            << "  --heartbeat-stall-grace-ms <ms>  Grace extension after event loop "
               "stall (default: 5000)\n"
            << "  --cgroup-mode <disabled|verify_only>  Cgroup v2 verification mode "
               "(default: disabled)\n"
            << "  --cgroup-mount <path>     Read-only cgroup v2 mount path (default: "
               "/sys/fs/cgroup)\n"
            << "  --cgroup-pattern <glob>   Expected peer cgroup v2 glob pattern\n"
            << "  --test-mock-netdev        Use in-memory mock netdev/XDP backend (for "
               "tests)\n"
            << "  --test-attach-xdp-delay-ms <ms>  Simulated attach_xdp delay in mock "
               "netdev (for tests)\n"
            << "  --test-proc-root <path>   Override /proc root path (for tests)\n"
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

  std::optional<mtlm2::HeartbeatExpiryPolicy> cli_hb_policy;
  std::optional<mtlm2::HeartbeatEvictionAction> cli_hb_eviction;
  std::optional<int64_t> cli_hb_interval_ms;
  std::optional<int64_t> cli_hb_max_missed;
  std::optional<int64_t> cli_hb_min_timeout_ms;
  std::optional<int64_t> cli_hb_stall_grace_ms;
  std::optional<mtlm2::CgroupVerifyMode> cli_cg_mode;
  std::optional<std::string> cli_cg_mount;
  std::optional<std::string> cli_cg_pattern;

  auto parse_i64_range = [](const std::string& text, int64_t lo, int64_t hi,
                            int64_t* out) -> bool {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || v < lo || v > hi) return false;
    *out = static_cast<int64_t>(v);
    return true;
  };

  for (int i = 1; i < argc; ++i) {
    std::string raw_arg = argv[i];
    std::string arg = raw_arg;
    std::optional<std::string> eq_val;
    if (raw_arg.rfind("--", 0) == 0) {
      size_t eq = raw_arg.find('=');
      if (eq != std::string::npos) {
        arg = raw_arg.substr(0, eq);
        eq_val = raw_arg.substr(eq + 1);
      }
    }
    auto need_val = [&](const char* opt) -> std::string {
      if (eq_val.has_value()) return *eq_val;
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
    } else if (arg == "--xdp-prog") {
      cfg.xdp_prog_path = need_val("--xdp-prog");
    } else if (arg == "--heartbeat-policy") {
      std::string pstr = need_val("--heartbeat-policy");
      mtlm2::HeartbeatExpiryPolicy pol{};
      if (!mtlm2::parse_heartbeat_expiry_policy(pstr, &pol)) {
        std::cerr << "MtlManager: invalid --heartbeat-policy '" << pstr << "'\n";
        return 2;
      }
      cli_hb_policy = pol;
      if (!cli_hb_eviction.has_value()) {
        cli_hb_eviction = (pol == mtlm2::HeartbeatExpiryPolicy::Revoke)
                              ? mtlm2::HeartbeatEvictionAction::Revoke
                              : mtlm2::HeartbeatEvictionAction::None;
      }
    } else if (arg == "--heartbeat-eviction-action") {
      std::string estr = need_val("--heartbeat-eviction-action");
      mtlm2::HeartbeatEvictionAction act{};
      if (!mtlm2::parse_heartbeat_eviction_action(estr, &act)) {
        std::cerr << "MtlManager: invalid --heartbeat-eviction-action '" << estr << "'\n";
        return 2;
      }
      cli_hb_eviction = act;
    } else if (arg == "--heartbeat-interval-ms") {
      std::string vstr = need_val("--heartbeat-interval-ms");
      int64_t v = 0;
      if (!parse_i64_range(vstr, 10, 3600000, &v)) {
        std::cerr << "MtlManager: invalid --heartbeat-interval-ms '" << vstr << "'\n";
        return 2;
      }
      cli_hb_interval_ms = v;
    } else if (arg == "--heartbeat-max-missed") {
      std::string vstr = need_val("--heartbeat-max-missed");
      int64_t v = 0;
      if (!parse_i64_range(vstr, 1, 1000, &v)) {
        std::cerr << "MtlManager: invalid --heartbeat-max-missed '" << vstr << "'\n";
        return 2;
      }
      cli_hb_max_missed = v;
    } else if (arg == "--heartbeat-min-timeout-ms") {
      std::string vstr = need_val("--heartbeat-min-timeout-ms");
      int64_t v = 0;
      if (!parse_i64_range(vstr, 10, 3600000, &v)) {
        std::cerr << "MtlManager: invalid --heartbeat-min-timeout-ms '" << vstr << "'\n";
        return 2;
      }
      cli_hb_min_timeout_ms = v;
    } else if (arg == "--heartbeat-stall-grace-ms") {
      std::string vstr = need_val("--heartbeat-stall-grace-ms");
      int64_t v = 0;
      if (!parse_i64_range(vstr, 10, 3600000, &v)) {
        std::cerr << "MtlManager: invalid --heartbeat-stall-grace-ms '" << vstr << "'\n";
        return 2;
      }
      cli_hb_stall_grace_ms = v;
    } else if (arg == "--cgroup-mode") {
      std::string mstr = need_val("--cgroup-mode");
      mtlm2::CgroupVerifyMode mode{};
      if (!mtlm2::parse_cgroup_verify_mode(mstr, &mode)) {
        std::cerr << "MtlManager: invalid --cgroup-mode '" << mstr << "'\n";
        return 2;
      }
      cli_cg_mode = mode;
    } else if (arg == "--cgroup-mount") {
      std::string mp = need_val("--cgroup-mount");
      if (mp.empty()) {
        std::cerr << "MtlManager: --cgroup-mount must not be empty\n";
        return 2;
      }
      cli_cg_mount = std::move(mp);
    } else if (arg == "--cgroup-pattern") {
      std::string raw_pat = need_val("--cgroup-pattern");
      std::string norm_pat;
      std::string perr;
      if (!mtlm2::normalize_and_validate_cgroup_pattern(raw_pat, &norm_pat, &perr)) {
        std::cerr << "MtlManager: invalid --cgroup-pattern '" << raw_pat << "': " << perr
                  << "\n";
        return 2;
      }
      cli_cg_pattern = std::move(norm_pat);
    } else if (arg == "--test-mock-netdev") {
      cfg.use_mock_netdev = true;
    } else if (arg == "--test-attach-xdp-delay-ms") {
      std::string vstr = need_val("--test-attach-xdp-delay-ms");
      int64_t v = 0;
      if (!parse_i64_range(vstr, 0, 60000, &v)) {
        std::cerr << "MtlManager: invalid --test-attach-xdp-delay-ms '" << vstr << "'\n";
        return 2;
      }
      cfg.mock_attach_xdp_delay_ms = v;
    } else if (arg == "--test-proc-root") {
      cfg.proc_root = need_val("--test-proc-root");
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
    if (cfg.policy.has_heartbeat) cfg.heartbeat = cfg.policy.heartbeat;
    if (cfg.policy.has_cgroup_verify) cfg.cgroup_verify = cfg.policy.cgroup_verify;
  }

  if (cli_hb_policy.has_value()) cfg.heartbeat.expiry_policy = *cli_hb_policy;
  if (cli_hb_eviction.has_value()) cfg.heartbeat.eviction_action = *cli_hb_eviction;
  if (cli_hb_interval_ms.has_value())
    cfg.heartbeat.default_interval_ms = *cli_hb_interval_ms;
  if (cli_hb_max_missed.has_value())
    cfg.heartbeat.default_missed_beats = *cli_hb_max_missed;
  if (cli_hb_min_timeout_ms.has_value())
    cfg.heartbeat.min_timeout_ms = *cli_hb_min_timeout_ms;
  if (cli_hb_stall_grace_ms.has_value())
    cfg.heartbeat.stall_grace_ms = *cli_hb_stall_grace_ms;
  if (cli_cg_mode.has_value()) cfg.cgroup_verify.mode = *cli_cg_mode;
  if (cli_cg_mount.has_value()) cfg.cgroup_verify.mount_path = *cli_cg_mount;
  if (cli_cg_pattern.has_value())
    cfg.cgroup_verify.expected_cgroup_pattern = *cli_cg_pattern;

  mtlm2::MtlManagerServer server(std::move(cfg));
  std::string err;
  if (!server.start(&err)) {
    std::cerr << "MtlManager: startup failed: " << err << "\n";
    return 1;
  }

  return server.run();
}
