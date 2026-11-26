/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mtlm2_server.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace mtlm2 {

namespace {

constexpr int kSdListenFdsStart = 3;
constexpr int kListenBacklog = 128;
constexpr int kMaxLockRetries = 5;
constexpr size_t kMaxAcceptPerTick = 8u;
constexpr size_t kMaxErrorMsgBytes = 256u;
constexpr size_t kMaxPolicyBytes = 1024u * 1024u;
constexpr size_t kMaxClaimLeaseIds = 1024u;

constexpr uint64_t kTagMask = 0xFFULL << 56;
constexpr uint64_t kTagClientListen = 1ULL << 56;
constexpr uint64_t kTagAdminListen = 2ULL << 56;
constexpr uint64_t kTagSignalFd = 3ULL << 56;
constexpr uint64_t kTagWakeFd = 4ULL << 56;
constexpr uint64_t kTagConn = 5ULL << 56;
constexpr uint64_t kTagLeasePidFd = 6ULL << 56;
constexpr uint64_t kTagConnPidFd = 7ULL << 56;
constexpr uint64_t kTagQueuePidFd = 8ULL << 56;
constexpr uint64_t kTagHeartbeatTimerFd = 9ULL << 56;

const char* kVarlinkServiceDesc = R"(interface org.varlink.service

method GetInfo() -> (
  vendor: string,
  product: string,
  version: string,
  url: string,
  interfaces: []string
)

method GetInterfaceDescription(interface: string) -> (description: string)

error InterfaceNotFound (interface: string)
error MethodNotFound (method: string)
error MethodNotImplemented (method: string)
error InvalidParameter (parameter: string)
error PermissionDenied ()
)";

const char* kMtlManagerDesc = R"(interface org.openvisualcloud.mtl.Manager

method OpenSession(clientName: string, heartbeatIntervalMs: ?int, heartbeatMaxMissed: ?int) -> (sessionId: string, heartbeatIntervalMs: int, heartbeatMaxMissed: int)
method ClaimSession(clientName: string, leaseIds: []string, heartbeatIntervalMs: ?int, heartbeatMaxMissed: ?int) -> (sessionId: string, claimed: []string, missing: []string, heartbeatIntervalMs: int, heartbeatMaxMissed: int)
method CloseSession() -> ()
method Ping(sequence: int) -> (sequence: int, serverMonotonicUsec: int)
method Heartbeat(sequence: int, clientState: ?string) -> (sequence: int, serverMonotonicUsec: int, status: string, nextDeadlineMs: int)
method AcquireCpu(preferred: ?int, numaNode: ?int, allowFallback: ?bool) -> (leaseId: string, cpuId: int)
method AcquireQueue(ifName: string, preferredQueue: ?int) -> (leaseId: string, ifName: string, ifindex: int, queueId: int)
method AttachXsk(queueLease: string) -> (leaseId: string)
method AddFlow(queueLease: string, protocol: ?string, srcIp: ?string, dstIp: string, srcPort: ?int, dstPort: int) -> (leaseId: string, ruleLocation: int)
method AddUdpFilter(queueLease: string, destinationPort: int) -> (leaseId: string)
method ReleaseLease(leaseId: string) -> ()
method GetStatus() -> (shmMode: string, shmTamperWarnings: int, heartbeatPolicy: string, heartbeatEvictionAction: string, heartbeatWarnings: int, heartbeatRevocations: int, managerStallEvents: int, cgroupVerifyMode: string, quarantinedCount: int, sessions: []object, leases: []object, queues: []object, flows: []object, udpFilters: []object, xskAttachments: []object, xdpAttachments: []object, backendInterfaces: []object)
method RevokeLease(leaseId: string) -> ()
method RevokeSession(sessionId: string) -> ()
method KillQuarantinedOwner(leaseId: string) -> ()

error ResourceBusy (message: string)
error AccessDenied (message: string)
error InvalidParameter (parameter: string, message: string)
error UnsupportedFlowType (message: string)
error LeaseNotFound (message: string)
error LeaseNotOwned (message: string)
error SessionNotFound (message: string)
error RateLimited (message: string)
error CgroupVerificationFailed (message: string)
error CpuOutsideEffectiveSet (message: string)
error NumaNodeNotAllowed (message: string)
error InternalError (message: string)
)";

struct JsonDeleter {
  void operator()(json_object* p) const noexcept {
    if (p) json_object_put(p);
  }
};
using JsonPtr = std::unique_ptr<json_object, JsonDeleter>;

std::string json_to_compact(json_object* obj) {
  if (!obj) return "{}";
  const char* s = json_object_to_json_string_ext(
      obj, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
  return s ? std::string(s) : std::string("{}");
}

void json_add_str(json_object* obj, const char* key, const std::string& val) {
  json_object_object_add(
      obj, key, json_object_new_string_len(val.data(), static_cast<int>(val.size())));
}

std::string truncate_err(std::string_view msg) {
  if (msg.size() <= kMaxErrorMsgBytes) return std::string(msg);
  return std::string(msg.substr(0, kMaxErrorMsgBytes));
}

bool parse_strict_i64(json_object* val, int64_t* out) {
  if (!val || !json_object_is_type(val, json_type_int)) return false;
  errno = 0;
  int64_t v = json_object_get_int64(val);
  /* Reject int64 overflow (ISSUE-47). */
  if (errno == ERANGE || (v == std::numeric_limits<int64_t>::max() &&
                          json_object_get_uint64(val) >
                              static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
    return false;
  *out = v;
  return true;
}

bool parse_strict_str(json_object* val, std::string* out) {
  if (!val || !json_object_is_type(val, json_type_string)) return false;
  const char* s = json_object_get_string(val);
  int len = json_object_get_string_len(val);
  if (!s || len < 0 || std::memchr(s, '\0', static_cast<size_t>(len)) != nullptr)
    return false;
  out->assign(s, static_cast<size_t>(len));
  return true;
}

bool check_allowed_keys(json_object* params, const std::set<std::string>& allowed,
                        std::string* bad_key) {
  if (!params) return true;
  json_object_object_foreach(params, key, val) {
    (void)val;
    if (allowed.find(key) == allowed.end()) {
      if (bad_key) *bad_key = key;
      return false;
    }
  }
  return true;
}

bool parse_cpu_range_string(std::string_view text, std::set<uint32_t>* out) {
  size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                 text[pos] == '\n' || text[pos] == '\r'))
      ++pos;
    if (pos >= text.size()) break;
    size_t comma = text.find(',', pos);
    std::string_view tok = (comma == std::string_view::npos)
                               ? text.substr(pos)
                               : text.substr(pos, comma - pos);
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t' ||
                            tok.back() == '\n' || tok.back() == '\r'))
      tok.remove_suffix(1);
    if (tok.empty()) return false;

    size_t dash = tok.find('-');
    if (dash == std::string_view::npos) {
      uint32_t cpu = 0;
      auto r = std::from_chars(tok.data(), tok.data() + tok.size(), cpu, 10);
      if (r.ec != std::errc{} || r.ptr != tok.data() + tok.size() || cpu > 65535u)
        return false;
      out->insert(cpu);
    } else {
      uint32_t lo = 0, hi = 0;
      auto r1 = std::from_chars(tok.data(), tok.data() + dash, lo, 10);
      auto r2 = std::from_chars(tok.data() + dash + 1, tok.data() + tok.size(), hi, 10);
      if (r1.ec != std::errc{} || r1.ptr != tok.data() + dash || r2.ec != std::errc{} ||
          r2.ptr != tok.data() + tok.size() || lo > hi || hi > 65535u)
        return false;
      for (uint32_t c = lo; c <= hi; ++c) out->insert(c);
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  return true;
}

bool parse_positive_env_i64(const char* text, int64_t* out) {
  if (!text || *text == '\0' || !out) return false;
  if (*text == '-' || *text == '+' || std::isspace(static_cast<unsigned char>(*text)))
    return false;
  errno = 0;
  char* end = nullptr;
  long long v = std::strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || v < 0) return false;
  *out = static_cast<int64_t>(v);
  return true;
}

std::string normalize_dir(const std::filesystem::path& p) {
  std::string s = p.lexically_normal().string();
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

bool ensure_parent_dir(const std::string& sock_path, mode_t dir_mode,
                       UniqueFd* out_dir_fd, std::string* out_base,
                       std::string* err_out) {
  if (sock_path.empty() || sock_path.back() == '/') {
    if (err_out) *err_out = "socket path must name a file";
    return false;
  }
  std::filesystem::path full(sock_path);
  std::string base = full.filename().string();
  if (base.empty() || base == "." || base == "..") {
    if (err_out) *err_out = "invalid socket filename";
    return false;
  }
  std::filesystem::path parent = full.parent_path();
  std::string parent_norm = parent.empty() ? std::string(".") : normalize_dir(parent);

  UniqueFd existing(
      ::open(parent_norm.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (existing.valid()) {
    *out_dir_fd = std::move(existing);
    *out_base = std::move(base);
    return true;
  }
  if (errno == ELOOP) {
    if (err_out) *err_out = "socket parent directory is a symlink";
    return false;
  }
  if (errno != ENOENT) {
    if (err_out)
      *err_out = "open socket parent failed: " + std::string(std::strerror(errno));
    return false;
  }

  std::vector<std::string> missing;
  std::filesystem::path cur(parent_norm);
  UniqueFd cur_fd;
  for (;;) {
    std::string comp = cur.filename().string();
    std::filesystem::path next = cur.parent_path();
    if (comp.empty() || comp == "." || comp == ".." || next == cur) {
      if (err_out) *err_out = "cannot resolve ancestor for socket directory";
      return false;
    }
    missing.push_back(std::move(comp));
    cur = next;
    std::string anc = cur.empty() ? std::string(".") : normalize_dir(cur);
    std::string open_anc = (anc == "/var/run") ? std::string("/run") : anc;
    cur_fd.reset(
        ::open(open_anc.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (cur_fd.valid()) break;
    if (errno == ELOOP) {
      if (err_out) *err_out = "socket ancestor directory is a symlink";
      return false;
    }
    if (errno != ENOENT) {
      if (err_out)
        *err_out = "open socket ancestor failed: " + std::string(std::strerror(errno));
      return false;
    }
  }

  std::reverse(missing.begin(), missing.end());
  for (const std::string& comp : missing) {
    bool created = false;
    if (::mkdirat(cur_fd.get(), comp.c_str(), dir_mode) == 0) {
      created = true;
    } else if (errno != EEXIST) {
      if (err_out) *err_out = "mkdirat failed: " + std::string(std::strerror(errno));
      return false;
    }
    UniqueFd next_fd(::openat(cur_fd.get(), comp.c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!next_fd.valid()) {
      if (err_out)
        *err_out = "openat component failed: " + std::string(std::strerror(errno));
      return false;
    }
    if (created) {
      (void)::fchmod(next_fd.get(), dir_mode);
    }
    cur_fd = std::move(next_fd);
  }
  *out_dir_fd = std::move(cur_fd);
  *out_base = std::move(base);
  return true;
}

bool acquire_verified_socket_lock(int parent_dir_fd, const std::string& lock_name,
                                  const std::string& sock_path, UniqueFd* out_fd,
                                  std::string* err_out) {
  const uid_t my_euid = ::geteuid();
  for (int attempt = 0; attempt < kMaxLockRetries; ++attempt) {
    UniqueFd lfd(::openat(parent_dir_fd, lock_name.c_str(),
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0640));
    if (!lfd.valid()) {
      if (err_out)
        *err_out =
            "failed to open socket lock for '" + sock_path + "': " + std::strerror(errno);
      return false;
    }
    struct stat lst{};
    if (::fstat(lfd.get(), &lst) != 0 || !S_ISREG(lst.st_mode) ||
        (lst.st_uid != my_euid && lst.st_uid != 0) || (lst.st_mode & 0022u) != 0) {
      if (err_out) *err_out = "socket lock file failed security verification";
      return false;
    }
    if (::flock(lfd.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        if (err_out)
          *err_out = "socket '" + sock_path + "' is locked by a live MtlManager instance";
        return false;
      }
      if (err_out)
        *err_out = "flock failed on socket lock: " + std::string(std::strerror(errno));
      return false;
    }
    struct stat pst{};
    if (::fstatat(parent_dir_fd, lock_name.c_str(), &pst, AT_SYMLINK_NOFOLLOW) == 0 &&
        pst.st_dev == lst.st_dev && pst.st_ino == lst.st_ino) {
      *out_fd = std::move(lfd);
      return true;
    }
  }
  if (err_out) *err_out = "socket lock changed concurrently";
  return false;
}

int lookup_cpu_numa_node(uint32_t cpu_id) {
  std::string dir_path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id);
  DIR* d = ::opendir(dir_path.c_str());
  if (!d) return 0;
  int found = 0;
  while (struct dirent* ent = ::readdir(d)) {
    if (std::strncmp(ent->d_name, "node", 4) == 0 &&
        std::isdigit(static_cast<unsigned char>(ent->d_name[4]))) {
      found = std::atoi(ent->d_name + 4);
      break;
    }
  }
  ::closedir(d);
  return found;
}

}  // namespace

const char* lease_state_to_string(LeaseState state) noexcept {
  switch (state) {
    case LeaseState::Allocated:
      return "ALLOCATED";
    case LeaseState::Recovering:
      return "RECOVERING";
    case LeaseState::Quarantined:
      return "QUARANTINED";
  }
  return "UNKNOWN";
}

PolicyConfig PolicyConfig::make_zero_arg_single_host() {
  PolicyConfig p;
  p.allow_all_single_host = true;
  p.allowed_udp_ports.emplace_back(1u, 65535u);
  p.default_quota.max_cpus = 65535u;
  p.default_quota.max_queues = 65535u;
  p.default_quota.max_flows = 65535u;
  return p;
}

bool PolicyConfig::load_file(const std::string& path, PolicyConfig* out,
                             std::string* err_out) {
  if (!out || path.empty()) {
    if (err_out) *err_out = "empty policy path";
    return false;
  }
  UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!fd.valid()) {
    if (err_out)
      *err_out = "failed to open policy file '" + path + "': " + std::strerror(errno);
    return false;
  }
  struct stat st{};
  if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
    if (err_out) *err_out = "policy path is not a regular file";
    return false;
  }
  if ((st.st_mode & 0022u) != 0) {
    if (err_out) *err_out = "policy file must not be group- or world-writable";
    return false;
  }

  std::string content;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd.get(), buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      if (err_out) *err_out = "failed to read policy file";
      return false;
    }
    if (n == 0) break;
    if (content.size() + static_cast<size_t>(n) > kMaxPolicyBytes) {
      if (err_out) *err_out = "policy file exceeds size limit";
      return false;
    }
    content.append(buf, static_cast<size_t>(n));
  }

  json_object* raw_root = nullptr;
  if (mt_varlink_parse_json_object(content.data(), content.size(), &raw_root) != 0 ||
      !raw_root) {
    if (err_out) *err_out = "invalid JSON in policy file";
    return false;
  }
  JsonPtr root(raw_root);

  static const std::set<std::string> kTopKeys = {
      "allowed_cpus", "allowed_interfaces", "allowed_udp_ports", "default_quota",
      "uid_quotas",   "heartbeat",          "cgroup_verify"};
  std::string bad_key;
  if (!check_allowed_keys(root.get(), kTopKeys, &bad_key)) {
    if (err_out) *err_out = "unknown key in policy file: " + bad_key;
    return false;
  }

  PolicyConfig cfg;
  cfg.allow_all_single_host = false;

  json_object* cpus = nullptr;
  if (json_object_object_get_ex(root.get(), "allowed_cpus", &cpus) && cpus) {
    if (json_object_is_type(cpus, json_type_string)) {
      std::string range_str;
      if (!parse_strict_str(cpus, &range_str) ||
          !parse_cpu_range_string(range_str, &cfg.allowed_cpus)) {
        if (err_out) *err_out = "invalid allowed_cpus range string";
        return false;
      }
    } else if (json_object_is_type(cpus, json_type_array)) {
      size_t n = json_object_array_length(cpus);
      for (size_t i = 0; i < n; ++i) {
        int64_t v = 0;
        if (!parse_strict_i64(json_object_array_get_idx(cpus, i), &v) || v < 0 ||
            v > 65535) {
          if (err_out) *err_out = "invalid CPU ID in allowed_cpus";
          return false;
        }
        cfg.allowed_cpus.insert(static_cast<uint32_t>(v));
      }
    } else {
      if (err_out) *err_out = "allowed_cpus must be array or range string";
      return false;
    }
  }

  json_object* ifaces = nullptr;
  if (json_object_object_get_ex(root.get(), "allowed_interfaces", &ifaces) && ifaces) {
    if (!json_object_is_type(ifaces, json_type_array)) {
      if (err_out) *err_out = "allowed_interfaces must be an array";
      return false;
    }
    size_t n = json_object_array_length(ifaces);
    for (size_t i = 0; i < n; ++i) {
      std::string name;
      if (!parse_strict_str(json_object_array_get_idx(ifaces, i), &name) ||
          name.empty()) {
        if (err_out) *err_out = "invalid interface name in allowed_interfaces";
        return false;
      }
      cfg.allowed_interfaces.insert(std::move(name));
    }
  }

  json_object* ports = nullptr;
  if (json_object_object_get_ex(root.get(), "allowed_udp_ports", &ports) && ports) {
    if (!json_object_is_type(ports, json_type_array)) {
      if (err_out) *err_out = "allowed_udp_ports must be an array";
      return false;
    }
    size_t n = json_object_array_length(ports);
    for (size_t i = 0; i < n; ++i) {
      json_object* pair = json_object_array_get_idx(ports, i);
      if (!pair || !json_object_is_type(pair, json_type_array) ||
          json_object_array_length(pair) != 2) {
        if (err_out) *err_out = "allowed_udp_ports entries must be [lo, hi]";
        return false;
      }
      int64_t lo = 0, hi = 0;
      if (!parse_strict_i64(json_object_array_get_idx(pair, 0), &lo) ||
          !parse_strict_i64(json_object_array_get_idx(pair, 1), &hi) || lo < 1 ||
          hi > 65535 || lo > hi) {
        if (err_out) *err_out = "invalid port range in allowed_udp_ports";
        return false;
      }
      cfg.allowed_udp_ports.emplace_back(static_cast<uint16_t>(lo),
                                         static_cast<uint16_t>(hi));
    }
  }

  auto parse_quota = [&](json_object* qobj, UidQuota* qout) -> bool {
    if (!qobj || !json_object_is_type(qobj, json_type_object)) return false;
    static const std::set<std::string> kQKeys = {"max_cpus", "max_queues", "max_flows",
                                                 "expected_cgroup_pattern"};
    if (!check_allowed_keys(qobj, kQKeys, nullptr)) return false;
    auto read_opt_u32 = [&](const char* k, uint32_t* dst) -> bool {
      json_object* v = nullptr;
      if (!json_object_object_get_ex(qobj, k, &v) || !v) return true;
      int64_t n = 0;
      if (!parse_strict_i64(v, &n) || n < 0 || n > 65535) return false;
      *dst = static_cast<uint32_t>(n);
      return true;
    };
    if (!read_opt_u32("max_cpus", &qout->max_cpus) ||
        !read_opt_u32("max_queues", &qout->max_queues) ||
        !read_opt_u32("max_flows", &qout->max_flows)) {
      return false;
    }
    json_object* jpat = nullptr;
    if (json_object_object_get_ex(qobj, "expected_cgroup_pattern", &jpat) && jpat) {
      std::string raw_pat;
      if (!parse_strict_str(jpat, &raw_pat)) return false;
      if (!raw_pat.empty()) {
        std::string norm_pat;
        std::string perr;
        if (!normalize_and_validate_cgroup_pattern(raw_pat, &norm_pat, &perr)) {
          if (err_out) *err_out = "invalid expected_cgroup_pattern: " + perr;
          return false;
        }
        qout->expected_cgroup_pattern = std::move(norm_pat);
      }
    }
    return true;
  };

  json_object* dq = nullptr;
  if (json_object_object_get_ex(root.get(), "default_quota", &dq) && dq) {
    if (!parse_quota(dq, &cfg.default_quota)) {
      if (err_out) *err_out = "invalid default_quota in policy file";
      return false;
    }
  }

  json_object* uqs = nullptr;
  if (json_object_object_get_ex(root.get(), "uid_quotas", &uqs) && uqs) {
    if (!json_object_is_type(uqs, json_type_object)) {
      if (err_out) *err_out = "uid_quotas must be an object";
      return false;
    }
    json_object_object_foreach(uqs, ukey, uval) {
      uint64_t uid_num = 0;
      auto r = std::from_chars(ukey, ukey + std::strlen(ukey), uid_num, 10);
      if (r.ec != std::errc{} || r.ptr != ukey + std::strlen(ukey) ||
          uid_num > static_cast<uint64_t>(std::numeric_limits<uid_t>::max())) {
        if (err_out) *err_out = "invalid UID key in uid_quotas";
        return false;
      }
      UidQuota q{};
      if (!parse_quota(uval, &q)) {
        if (err_out) *err_out = "invalid quota object for UID " + std::string(ukey);
        return false;
      }
      cfg.uid_quotas[static_cast<uid_t>(uid_num)] = q;
    }
  }

  json_object* jhb = nullptr;
  if (json_object_object_get_ex(root.get(), "heartbeat", &jhb) && jhb) {
    if (!json_object_is_type(jhb, json_type_object)) {
      if (err_out) *err_out = "heartbeat must be an object";
      return false;
    }
    static const std::set<std::string> kHbKeys = {
        "expiry_policy",        "eviction_action", "default_interval_ms",
        "default_missed_beats", "min_timeout_ms",  "max_timeout_ms",
        "stall_grace_ms",
    };
    std::string hb_bad;
    if (!check_allowed_keys(jhb, kHbKeys, &hb_bad)) {
      if (err_out) *err_out = "unknown key in policy heartbeat: " + hb_bad;
      return false;
    }
    cfg.has_heartbeat = true;

    json_object* v = nullptr;
    if (json_object_object_get_ex(jhb, "expiry_policy", &v) && v) {
      std::string s;
      if (!parse_strict_str(v, &s) ||
          !parse_heartbeat_expiry_policy(s, &cfg.heartbeat.expiry_policy)) {
        if (err_out) *err_out = "invalid heartbeat.expiry_policy";
        return false;
      }
    }
    if (json_object_object_get_ex(jhb, "eviction_action", &v) && v) {
      std::string s;
      if (!parse_strict_str(v, &s) ||
          !parse_heartbeat_eviction_action(s, &cfg.heartbeat.eviction_action)) {
        if (err_out) *err_out = "invalid heartbeat.eviction_action";
        return false;
      }
    }
    auto read_hb_i64 = [&](const char* key, int64_t lo, int64_t hi,
                           int64_t* dst) -> bool {
      json_object* jv = nullptr;
      if (!json_object_object_get_ex(jhb, key, &jv) || !jv) return true;
      int64_t num = 0;
      if (!parse_strict_i64(jv, &num) || num < lo || num > hi) {
        if (err_out) *err_out = std::string("invalid heartbeat.") + key;
        return false;
      }
      *dst = num;
      return true;
    };
    if (!read_hb_i64("default_interval_ms", 10, 3600000,
                     &cfg.heartbeat.default_interval_ms) ||
        !read_hb_i64("default_missed_beats", 1, 1000,
                     &cfg.heartbeat.default_missed_beats) ||
        !read_hb_i64("min_timeout_ms", 10, 3600000, &cfg.heartbeat.min_timeout_ms) ||
        !read_hb_i64("max_timeout_ms", 10, 86400000, &cfg.heartbeat.max_timeout_ms) ||
        !read_hb_i64("stall_grace_ms", 10, 3600000, &cfg.heartbeat.stall_grace_ms)) {
      return false;
    }
    if (cfg.heartbeat.min_timeout_ms > cfg.heartbeat.max_timeout_ms) {
      if (err_out) *err_out = "heartbeat.min_timeout_ms exceeds max_timeout_ms";
      return false;
    }
  }

  json_object* jcg = nullptr;
  if (json_object_object_get_ex(root.get(), "cgroup_verify", &jcg) && jcg) {
    if (!json_object_is_type(jcg, json_type_object)) {
      if (err_out) *err_out = "cgroup_verify must be an object";
      return false;
    }
    static const std::set<std::string> kCgKeys = {"mode", "mount_path",
                                                  "expected_cgroup_pattern"};
    std::string cg_bad;
    if (!check_allowed_keys(jcg, kCgKeys, &cg_bad)) {
      if (err_out) *err_out = "unknown key in policy cgroup_verify: " + cg_bad;
      return false;
    }
    cfg.has_cgroup_verify = true;

    json_object* v = nullptr;
    if (json_object_object_get_ex(jcg, "mode", &v) && v) {
      std::string s;
      if (!parse_strict_str(v, &s) ||
          !parse_cgroup_verify_mode(s, &cfg.cgroup_verify.mode)) {
        if (err_out) *err_out = "invalid cgroup_verify.mode";
        return false;
      }
    }
    if (json_object_object_get_ex(jcg, "mount_path", &v) && v) {
      std::string s;
      if (!parse_strict_str(v, &s) || s.empty()) {
        if (err_out) *err_out = "invalid cgroup_verify.mount_path";
        return false;
      }
      cfg.cgroup_verify.mount_path = std::move(s);
    }
    if (json_object_object_get_ex(jcg, "expected_cgroup_pattern", &v) && v) {
      std::string raw_pat;
      if (!parse_strict_str(v, &raw_pat)) {
        if (err_out) *err_out = "invalid cgroup_verify.expected_cgroup_pattern";
        return false;
      }
      if (!raw_pat.empty()) {
        std::string norm_pat;
        std::string perr;
        if (!normalize_and_validate_cgroup_pattern(raw_pat, &norm_pat, &perr)) {
          if (err_out)
            *err_out = "invalid cgroup_verify.expected_cgroup_pattern: " + perr;
          return false;
        }
        cfg.cgroup_verify.expected_cgroup_pattern = std::move(norm_pat);
      }
    }
  }

  *out = std::move(cfg);
  return true;
}

bool PolicyConfig::is_cpu_allowed(uint32_t cpu_id) const {
  if (allow_all_single_host) return true;
  return allowed_cpus.find(cpu_id) != allowed_cpus.end();
}

bool PolicyConfig::is_interface_allowed(const std::string& if_name) const {
  if (allow_all_single_host) return true;
  return allowed_interfaces.find(if_name) != allowed_interfaces.end();
}

bool PolicyConfig::is_udp_port_allowed(uint16_t port) const {
  if (port == 0) return false;
  if (allow_all_single_host) return true;
  for (const auto& [lo, hi] : allowed_udp_ports) {
    if (port >= lo && port <= hi) return true;
  }
  return false;
}

uint32_t PolicyConfig::max_cpus_for_uid(uid_t uid) const {
  if (allow_all_single_host) return 65535u;
  auto it = uid_quotas.find(uid);
  if (it != uid_quotas.end()) return it->second.max_cpus;
  return default_quota.max_cpus;
}

uint32_t PolicyConfig::max_queues_for_uid(uid_t uid) const {
  if (allow_all_single_host) return 65535u;
  auto it = uid_quotas.find(uid);
  if (it != uid_quotas.end()) return it->second.max_queues;
  return default_quota.max_queues;
}

uint32_t PolicyConfig::max_flows_for_uid(uid_t uid) const {
  if (allow_all_single_host) return 65535u;
  auto it = uid_quotas.find(uid);
  if (it != uid_quotas.end()) return it->second.max_flows;
  return default_quota.max_flows;
}

std::string PolicyConfig::expected_cgroup_pattern_for_uid(uid_t uid) const {
  auto it = uid_quotas.find(uid);
  if (it != uid_quotas.end() && !it->second.expected_cgroup_pattern.empty()) {
    return it->second.expected_cgroup_pattern;
  }
  if (!default_quota.expected_cgroup_pattern.empty()) {
    return default_quota.expected_cgroup_pattern;
  }
  return cgroup_verify.expected_cgroup_pattern;
}

std::string MtlManagerServer::resolve_expected_cgroup_pattern(uid_t uid) const {
  auto it = cfg_.policy.uid_quotas.find(uid);
  if (it != cfg_.policy.uid_quotas.end() && !it->second.expected_cgroup_pattern.empty()) {
    return it->second.expected_cgroup_pattern;
  }
  if (!cfg_.policy.default_quota.expected_cgroup_pattern.empty()) {
    return cfg_.policy.default_quota.expected_cgroup_pattern;
  }
  return cfg_.cgroup_verify.expected_cgroup_pattern;
}

MtlManagerServer::MtlManagerServer(ServerConfig cfg,
                                   std::unique_ptr<NetdevBackend> netdev)
    : cfg_(std::move(cfg)),
      peer_extractor_(cfg_.proc_root),
      cgroup_verifier_(cfg_.cgroup_verify, peer_extractor_),
      journal_(cfg_.state_dir),
      netdev_(std::move(netdev)) {
  if (!netdev_) {
    if (cfg_.use_mock_netdev) {
      auto mock = std::make_unique<MockNetdevBackend>();
      mock->attach_xdp_delay_ms = cfg_.mock_attach_xdp_delay_ms;
      netdev_ = std::move(mock);
    } else {
      netdev_ = std::make_unique<LinuxNetdevBackend>(cfg_.xdp_prog_path);
    }
  }
}

MtlManagerServer::~MtlManagerServer() {
  shutdown();
}

bool MtlManagerServer::discover_online_cpus(std::string* err_out) {
  online_cpus_.clear();
  cpu_numa_node_.clear();

  if (!cfg_.override_online_cpus.empty()) {
    online_cpus_ = cfg_.override_online_cpus;
    std::sort(online_cpus_.begin(), online_cpus_.end());
    online_cpus_.erase(std::unique(online_cpus_.begin(), online_cpus_.end()),
                       online_cpus_.end());
    for (uint32_t c : online_cpus_) cpu_numa_node_[c] = 0;
    return true;
  }

  std::ifstream f("/sys/devices/system/cpu/online");
  std::string line;
  std::set<uint32_t> cpus;
  if (f && std::getline(f, line) && parse_cpu_range_string(line, &cpus) &&
      !cpus.empty()) {
    online_cpus_.assign(cpus.begin(), cpus.end());
  } else {
    long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 1;
    for (long i = 0; i < n; ++i) online_cpus_.push_back(static_cast<uint32_t>(i));
  }
  if (online_cpus_.empty()) {
    if (err_out) *err_out = "no online CPUs discovered";
    return false;
  }
  for (uint32_t c : online_cpus_) cpu_numa_node_[c] = lookup_cpu_numa_node(c);
  return true;
}

bool MtlManagerServer::open_listener(const std::string& path, mode_t mode,
                                     std::optional<gid_t> gid, bool is_admin,
                                     bool allow_world, Listener* out,
                                     std::string* err_out) {
  if (is_admin && (mode & 0077u) != 0) {
    if (err_out) *err_out = "admin socket must be 0600";
    return false;
  }
  if (!allow_world && (mode & 0007u) != 0) {
    if (err_out)
      *err_out = "world-accessible socket mode forbidden outside zero-arg mode";
    return false;
  }

  /* Support systemd socket activation for client listener. */
  if (!is_admin) {
    const char* lpid = std::getenv("LISTEN_PID");
    const char* lfds = std::getenv("LISTEN_FDS");
    int64_t env_pid = -1, env_fds = -1;
    if (parse_positive_env_i64(lpid, &env_pid) &&
        parse_positive_env_i64(lfds, &env_fds) &&
        env_pid == static_cast<int64_t>(::getpid()) && env_fds >= 1) {
      int fd = kSdListenFdsStart;
      struct stat st{};
      int domain = 0, stype = 0, acc = 0;
      socklen_t len = sizeof(int);
      if (::fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode) ||
          ::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) != 0 ||
          domain != AF_UNIX || ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &stype, &len) != 0 ||
          stype != SOCK_STREAM ||
          ::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &acc, &len) != 0 || acc == 0) {
        if (err_out) *err_out = "invalid inherited systemd LISTEN_FD";
        return false;
      }
      (void)set_cloexec(fd);
      (void)set_nonblock(fd);
      ::unsetenv("LISTEN_PID");
      ::unsetenv("LISTEN_FDS");
      out->fd.reset(fd);
      out->path = path;
      out->is_admin = false;
      out->owns_socket_file = false;
      return true;
    }
  }

  struct sockaddr_un addr{};
  if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
    if (err_out) *err_out = "socket path empty or exceeds sun_path limit";
    return false;
  }

  UniqueFd parent_dir_fd;
  std::string basename;
  if (!ensure_parent_dir(path, is_admin ? 0750 : 0755, &parent_dir_fd, &basename,
                         err_out))
    return false;

  UniqueFd lock_fd;
  if (!acquire_verified_socket_lock(parent_dir_fd.get(), basename + ".lock", path,
                                    &lock_fd, err_out))
    return false;

  /* Probe existing entry; never unlink a live listener. */
  struct stat st{};
  if (::fstatat(parent_dir_fd.get(), basename.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISSOCK(st.st_mode)) {
      if (err_out) *err_out = "existing path '" + path + "' is not a socket";
      return false;
    }
    UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!probe.valid()) {
      if (err_out) *err_out = "failed to create probe socket";
      return false;
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    int rc =
        ::connect(probe.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0 ||
        (rc < 0 && (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK))) {
      if (err_out) *err_out = "live daemon already listening on '" + path + "'";
      return false;
    }
    if (errno != ECONNREFUSED && errno != ENOENT) {
      if (err_out)
        *err_out = "probe connect failed on '" + path + "': " + std::strerror(errno);
      return false;
    }
    struct stat cur{};
    if (::fstatat(parent_dir_fd.get(), basename.c_str(), &cur, AT_SYMLINK_NOFOLLOW) ==
        0) {
      if (!S_ISSOCK(cur.st_mode) || cur.st_dev != st.st_dev || cur.st_ino != st.st_ino) {
        if (err_out) *err_out = "socket changed during stale cleanup";
        return false;
      }
      if (::unlinkat(parent_dir_fd.get(), basename.c_str(), 0) != 0 && errno != ENOENT) {
        if (err_out) *err_out = "failed to unlink stale socket '" + path + "'";
        return false;
      }
    }
  } else if (errno != ENOENT) {
    if (err_out) *err_out = "fstatat failed on '" + path + "'";
    return false;
  }

  UniqueFd listen_fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!listen_fd.valid()) {
    if (err_out) *err_out = "socket() failed";
    return false;
  }
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  const mode_t bind_umask = static_cast<mode_t>((~mode) & 0777u);
  const mode_t prev_umask = ::umask(bind_umask);
  int brc =
      ::bind(listen_fd.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  int berr = errno;
  ::umask(prev_umask);
  if (brc != 0) {
    if (err_out) *err_out = "bind failed on '" + path + "': " + std::strerror(berr);
    return false;
  }

  struct stat bst{};
  if (::fstatat(parent_dir_fd.get(), basename.c_str(), &bst, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISSOCK(bst.st_mode) || bst.st_uid != ::geteuid()) {
    if (err_out) *err_out = "bound socket verification failed on '" + path + "'";
    return false;
  }

  auto cleanup_bound = [&]() {
    struct stat cur{};
    if (::fstatat(parent_dir_fd.get(), basename.c_str(), &cur, AT_SYMLINK_NOFOLLOW) ==
            0 &&
        S_ISSOCK(cur.st_mode) && cur.st_dev == bst.st_dev && cur.st_ino == bst.st_ino) {
      (void)::unlinkat(parent_dir_fd.get(), basename.c_str(), 0);
    }
  };

  if (gid.has_value()) {
    if (::fchownat(parent_dir_fd.get(), basename.c_str(), static_cast<uid_t>(-1), *gid,
                   AT_SYMLINK_NOFOLLOW) != 0) {
      int err = errno;
      cleanup_bound();
      if (err_out) *err_out = "fchownat failed on '" + path + "': " + std::strerror(err);
      return false;
    }
  }
  if ((bst.st_mode & 0777u) != (mode & 0777u)) {
    if (::fchmodat(parent_dir_fd.get(), basename.c_str(), mode, 0) != 0) {
      int err = errno;
      cleanup_bound();
      if (err_out) *err_out = "fchmodat failed on '" + path + "': " + std::strerror(err);
      return false;
    }
  }
  if (::listen(listen_fd.get(), kListenBacklog) != 0) {
    int err = errno;
    cleanup_bound();
    if (err_out) *err_out = "listen failed on '" + path + "': " + std::strerror(err);
    return false;
  }

  out->fd = std::move(listen_fd);
  out->lock_fd = std::move(lock_fd);
  out->parent_dir_fd = std::move(parent_dir_fd);
  out->path = path;
  out->basename = std::move(basename);
  out->socket_dev = bst.st_dev;
  out->socket_ino = bst.st_ino;
  out->is_admin = is_admin;
  out->owns_socket_file = true;
  return true;
}

void MtlManagerServer::close_listener(Listener* ls) {
  if (!ls) return;
  if (ls->fd.valid()) {
    del_epoll(ls->fd.get());
    ls->fd.reset();
  }
  if (ls->owns_socket_file && ls->parent_dir_fd.valid() && !ls->basename.empty()) {
    struct stat cur{};
    if (::fstatat(ls->parent_dir_fd.get(), ls->basename.c_str(), &cur,
                  AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISSOCK(cur.st_mode) && cur.st_dev == ls->socket_dev &&
        cur.st_ino == ls->socket_ino) {
      (void)::unlinkat(ls->parent_dir_fd.get(), ls->basename.c_str(), 0);
    }
  }
  /* NEVER unlink <socket>.lock (ISSUE-01). */
  ls->lock_fd.reset();
  ls->parent_dir_fd.reset();
  ls->owns_socket_file = false;
}

bool MtlManagerServer::add_epoll(int fd, uint32_t events, uint64_t tag) {
  struct epoll_event ev{};
  ev.events = events;
  ev.data.u64 = tag;
  return ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool MtlManagerServer::mod_epoll(int fd, uint32_t events, uint64_t tag) {
  struct epoll_event ev{};
  ev.events = events;
  ev.data.u64 = tag;
  return ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev) == 0;
}

void MtlManagerServer::del_epoll(int fd) {
  if (epoll_fd_.valid() && fd >= 0) {
    (void)::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
  }
}

void MtlManagerServer::watch_lease_pidfd(CpuLease& lease) {
  if (!lease.pidfd.valid()) return;
  int pfd = lease.pidfd.get();
  pidfd_to_lease_[pfd] = lease.lease_id;
  uint64_t tag = kTagLeasePidFd | static_cast<uint64_t>(static_cast<uint32_t>(pfd));
  (void)add_epoll(pfd, EPOLLIN, tag);
}

void MtlManagerServer::watch_queue_pidfd(QueueLease& lease) {
  if (!lease.pidfd.valid()) return;
  int pfd = lease.pidfd.get();
  pidfd_to_queue_lease_[pfd] = lease.lease_id;
  uint64_t tag = kTagQueuePidFd | static_cast<uint64_t>(static_cast<uint32_t>(pfd));
  (void)add_epoll(pfd, EPOLLIN, tag);
}

bool MtlManagerServer::persist_journal(std::string* err_out) {
  JournalState st;
  st.schema_version = JournalState::kSupportedSchemaVersion;
  st.manager_id = "mtlm2d";

  st.cpu_leases.reserve(leases_.size());
  for (const auto& [lid, lease] : leases_) {
    JournalCpuLease e;
    e.lease_id = lease.lease_id;
    e.cpu_id = lease.cpu_id;
    e.pid = lease.pid;
    e.start_time_ticks = lease.start_time_ticks;
    e.uid = lease.uid;
    e.client_name = lease.client_name;
    e.quarantined = (lease.state == LeaseState::Quarantined);
    st.cpu_leases.push_back(std::move(e));
  }

  st.queues.reserve(queue_leases_.size());
  for (const auto& [lid, q] : queue_leases_) {
    JournalQueueLease e;
    e.lease_id = q.lease_id;
    e.if_name = q.if_name;
    e.ifindex = q.ifindex;
    e.queue_id = q.queue_id;
    e.pid = q.pid;
    e.start_time_ticks = q.start_time_ticks;
    e.uid = q.uid;
    e.client_name = q.client_name;
    e.quarantined = (q.state == LeaseState::Quarantined);
    st.queues.push_back(std::move(e));
  }

  st.flows.reserve(flow_leases_.size());
  for (const auto& [lid, f] : flow_leases_) {
    JournalFlowLease e;
    e.lease_id = f.lease_id;
    e.queue_lease_id = f.queue_lease_id;
    e.if_name = f.if_name;
    e.ifindex = f.ifindex;
    e.queue_id = f.queue_id;
    e.rule_location = f.rule_location;
    e.protocol = f.spec.protocol;
    e.src_ip = (f.spec.src_ip_be != 0) ? format_ipv4(f.spec.src_ip_be) : std::string();
    e.dst_ip = format_ipv4(f.spec.dst_ip_be);
    e.src_port = f.spec.src_port;
    e.dst_port = f.spec.dst_port;
    e.pid = f.pid;
    e.start_time_ticks = f.start_time_ticks;
    e.uid = f.uid;
    e.client_name = f.client_name;
    st.flows.push_back(std::move(e));
  }

  st.udp_filters.reserve(udp_filter_leases_.size());
  for (const auto& [lid, u] : udp_filter_leases_) {
    JournalUdpFilterLease e;
    e.lease_id = u.lease_id;
    e.queue_lease_id = u.queue_lease_id;
    e.if_name = u.if_name;
    e.ifindex = u.ifindex;
    e.queue_id = u.queue_id;
    e.dst_port = u.dst_port;
    e.pid = u.pid;
    e.start_time_ticks = u.start_time_ticks;
    e.uid = u.uid;
    e.client_name = u.client_name;
    st.udp_filters.push_back(std::move(e));
  }

  st.xsk_attachments.reserve(xsk_leases_.size());
  for (const auto& [lid, x] : xsk_leases_) {
    JournalXskAttachment e;
    e.lease_id = x.lease_id;
    e.queue_lease_id = x.queue_lease_id;
    e.if_name = x.if_name;
    e.ifindex = x.ifindex;
    e.queue_id = x.queue_id;
    e.pid = x.pid;
    e.start_time_ticks = x.start_time_ticks;
    e.uid = x.uid;
    e.client_name = x.client_name;
    st.xsk_attachments.push_back(std::move(e));
  }

  st.xdp_attachments.reserve(xdp_attachments_.size());
  for (const auto& [if_name, xa] : xdp_attachments_) {
    if (xa.mode == XdpAttachMode::None) continue;
    JournalXdpAttachment e;
    e.if_name = xa.if_name;
    e.ifindex = xa.ifindex;
    e.mode = xdp_attach_mode_to_string(xa.mode);
    st.xdp_attachments.push_back(std::move(e));
  }

  if (!journal_.save(st, err_out)) {
    std::cerr << "mtlm2d: journal save failed: " << (err_out ? *err_out : "unknown")
              << "\n";
    return false;
  }
  return true;
}

bool MtlManagerServer::reconcile_journal_startup(std::string* err_out) {
  JournalState loaded;
  bool not_found = false;
  bool quarantined = false;
  std::string lerr;
  if (!journal_.load(&loaded, &not_found, &quarantined, &lerr)) {
    if (not_found) {
      return persist_journal(err_out);
    }
    if (quarantined) {
      std::cerr << "mtlm2d: quarantined corrupt journal: " << lerr << "\n";
      return persist_journal(err_out);
    }
    if (err_out) *err_out = lerr;
    return false;
  }

  /* Restore XDP attachment handles first so dead-owner BPF map entries can be cleaned. */
  for (const auto& xa : loaded.xdp_attachments) {
    XdpAttachMode mode = XdpAttachMode::None;
    if (!parse_xdp_attach_mode(xa.mode, &mode) || mode == XdpAttachMode::None) continue;
    xdp_attachments_[xa.if_name] = XdpAttachment{xa.if_name, xa.ifindex, mode};
    netdev_->restore_xdp_state(xa.if_name, xa.ifindex, mode);
  }

  for (const auto& e : loaded.cpu_leases) {
    UniqueFd pfd;
    if (PeerCredExtractor::open_verified_pidfd(cfg_.proc_root, e.pid, e.start_time_ticks,
                                               &pfd)) {
      CpuLease lease;
      lease.lease_id = e.lease_id;
      lease.cpu_id = e.cpu_id;
      lease.state = e.quarantined ? LeaseState::Quarantined : LeaseState::Recovering;
      lease.pid = e.pid;
      lease.start_time_ticks = e.start_time_ticks;
      lease.uid = e.uid;
      lease.client_name = e.client_name;
      lease.pidfd = std::move(pfd);
      auto [it, inserted] = leases_.emplace(lease.lease_id, std::move(lease));
      if (inserted) watch_lease_pidfd(it->second);
    } else {
      shm_.release_cpu(e.cpu_id, e.pid);
    }
  }

  for (const auto& e : loaded.queues) {
    UniqueFd pfd;
    if (PeerCredExtractor::open_verified_pidfd(cfg_.proc_root, e.pid, e.start_time_ticks,
                                               &pfd)) {
      QueueLease ql;
      ql.lease_id = e.lease_id;
      ql.if_name = e.if_name;
      ql.ifindex = e.ifindex;
      ql.queue_id = e.queue_id;
      ql.state = e.quarantined ? LeaseState::Quarantined : LeaseState::Recovering;
      ql.pid = e.pid;
      ql.start_time_ticks = e.start_time_ticks;
      ql.uid = e.uid;
      ql.client_name = e.client_name;
      ql.pidfd = std::move(pfd);
      auto [it, inserted] = queue_leases_.emplace(ql.lease_id, std::move(ql));
      if (inserted) watch_queue_pidfd(it->second);
    }
  }

  auto parent_queue_live_and_unquarantined = [&](const std::string& qlid, pid_t pid,
                                                 uint64_t ticks) {
    auto it = queue_leases_.find(qlid);
    if (it == queue_leases_.end()) return false;
    return it->second.state != LeaseState::Quarantined && it->second.pid == pid &&
           it->second.start_time_ticks == ticks;
  };

  for (const auto& e : loaded.xsk_attachments) {
    if (parent_queue_live_and_unquarantined(e.queue_lease_id, e.pid,
                                            e.start_time_ticks)) {
      XskLease xl;
      xl.lease_id = e.lease_id;
      xl.queue_lease_id = e.queue_lease_id;
      xl.if_name = e.if_name;
      xl.ifindex = e.ifindex;
      xl.queue_id = e.queue_id;
      xl.state = LeaseState::Recovering;
      xl.pid = e.pid;
      xl.start_time_ticks = e.start_time_ticks;
      xl.uid = e.uid;
      xl.client_name = e.client_name;
      xsk_leases_.emplace(xl.lease_id, std::move(xl));
      netdev_->restore_xsk_state(e.if_name, e.queue_id);
    } else {
      std::string berr;
      (void)netdev_->delete_xsk_map(e.if_name, e.ifindex, e.queue_id, &berr);
    }
  }

  for (const auto& e : loaded.flows) {
    FlowSpec spec;
    spec.protocol = e.protocol;
    if (!e.src_ip.empty()) (void)parse_strict_ipv4(e.src_ip, &spec.src_ip_be);
    if (!e.dst_ip.empty()) (void)parse_strict_ipv4(e.dst_ip, &spec.dst_ip_be);
    spec.src_port = e.src_port;
    spec.dst_port = e.dst_port;

    if (parent_queue_live_and_unquarantined(e.queue_lease_id, e.pid,
                                            e.start_time_ticks)) {
      FlowLease fl;
      fl.lease_id = e.lease_id;
      fl.queue_lease_id = e.queue_lease_id;
      fl.if_name = e.if_name;
      fl.ifindex = e.ifindex;
      fl.queue_id = e.queue_id;
      fl.rule_location = e.rule_location;
      fl.spec = spec;
      fl.state = LeaseState::Recovering;
      fl.pid = e.pid;
      fl.start_time_ticks = e.start_time_ticks;
      fl.uid = e.uid;
      fl.client_name = e.client_name;
      flow_leases_.emplace(fl.lease_id, std::move(fl));
      netdev_->restore_flow_state(e.if_name, e.queue_id, e.rule_location, spec);
    } else {
      /* Delete only MTL-owned rule locations recorded in the journal (RES-Q-02). */
      std::string berr;
      (void)netdev_->delete_flow(e.if_name, e.ifindex, e.rule_location, &berr);
    }
  }

  for (const auto& e : loaded.udp_filters) {
    if (parent_queue_live_and_unquarantined(e.queue_lease_id, e.pid,
                                            e.start_time_ticks)) {
      UdpFilterLease ul;
      ul.lease_id = e.lease_id;
      ul.queue_lease_id = e.queue_lease_id;
      ul.if_name = e.if_name;
      ul.ifindex = e.ifindex;
      ul.queue_id = e.queue_id;
      ul.dst_port = e.dst_port;
      ul.state = LeaseState::Recovering;
      ul.pid = e.pid;
      ul.start_time_ticks = e.start_time_ticks;
      ul.uid = e.uid;
      ul.client_name = e.client_name;
      udp_filter_leases_.emplace(ul.lease_id, std::move(ul));
      netdev_->restore_udp_filter_state(e.if_name, e.queue_id, e.dst_port);
    } else {
      std::string berr;
      (void)netdev_->delete_udp_filter(e.if_name, e.ifindex, e.queue_id, e.dst_port,
                                       &berr);
    }
  }

  /* Detach XDP on any interface that no longer has live queue owners. */
  std::vector<std::string> stale_xdp;
  for (const auto& [if_name, xa] : xdp_attachments_) {
    (void)xa;
    if (!has_active_queue_on_iface(if_name)) stale_xdp.push_back(if_name);
  }
  for (const std::string& if_name : stale_xdp) maybe_detach_xdp_if_unused(if_name);

  return persist_journal(err_out);
}

bool MtlManagerServer::acquire_host_lock(std::string* err_out) {
  if (cfg_.host_lock_path.empty()) return true;
  UniqueFd parent_dir_fd;
  std::string basename;
  if (!ensure_parent_dir(cfg_.host_lock_path, 0750, &parent_dir_fd, &basename, err_out))
    return false;

  const uid_t my_euid = ::geteuid();
  for (int attempt = 0; attempt < kMaxLockRetries; ++attempt) {
    UniqueFd lfd(::openat(parent_dir_fd.get(), basename.c_str(),
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (!lfd.valid()) {
      if (err_out)
        *err_out = "failed to open host lock '" + cfg_.host_lock_path +
                   "': " + std::strerror(errno);
      return false;
    }
    struct stat lst{};
    if (::fstat(lfd.get(), &lst) != 0 || !S_ISREG(lst.st_mode) ||
        (lst.st_uid != my_euid && lst.st_uid != 0) || (lst.st_mode & 0022u) != 0) {
      if (err_out)
        *err_out = "host lock '" + cfg_.host_lock_path + "' failed security verification";
      return false;
    }
    if (::flock(lfd.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        if (err_out)
          *err_out = "host lock '" + cfg_.host_lock_path +
                     "' is already held by another live MtlManager instance";
        return false;
      }
      if (err_out)
        *err_out = "flock failed on host lock '" + cfg_.host_lock_path +
                   "': " + std::strerror(errno);
      return false;
    }
    struct stat pst{};
    if (::fstatat(parent_dir_fd.get(), basename.c_str(), &pst, AT_SYMLINK_NOFOLLOW) ==
            0 &&
        pst.st_dev == lst.st_dev && pst.st_ino == lst.st_ino) {
      host_lock_fd_ = std::move(lfd);
      return true;
    }
  }
  if (err_out) *err_out = "host lock changed concurrently";
  return false;
}

bool MtlManagerServer::check_v1_manager_absent(std::string* err_out) const {
  if (cfg_.v1_socket_path.empty()) return true;
  struct sockaddr_un addr{};
  if (cfg_.v1_socket_path.size() >= sizeof(addr.sun_path)) return true;

  UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!probe.valid()) return true;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, cfg_.v1_socket_path.c_str(), cfg_.v1_socket_path.size() + 1);
  int rc =
      ::connect(probe.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  if (rc == 0 ||
      (rc < 0 && (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK))) {
    if (err_out)
      *err_out = "v1 MtlManager is running at '" + cfg_.v1_socket_path +
                 "'; stop v1 before starting MtlManager 2";
    return false;
  }
  return true;
}

bool MtlManagerServer::start(std::string* err_out) {
  if (started_) return true;

  if (!discover_online_cpus(err_out)) return false;

  if (cfg_.cgroup_verify.mode == CgroupVerifyMode::VerifyOnly) {
    if (!cfg_.cgroup_verify.expected_cgroup_pattern.empty()) {
      std::string norm_pat;
      std::string perr;
      if (!normalize_and_validate_cgroup_pattern(
              cfg_.cgroup_verify.expected_cgroup_pattern, &norm_pat, &perr)) {
        if (err_out) *err_out = "invalid cgroup pattern: " + perr;
        return false;
      }
      cfg_.cgroup_verify.expected_cgroup_pattern = std::move(norm_pat);
    }
    std::string cg_warn;
    if (!cgroup_verifier_.probe_mount(&cg_warn)) {
      std::cerr << "MtlManager: warning: " << cg_warn << "\n";
    }
  }

  /* D3 (MM-05): refuse startup if v1 MtlManager is listening. */
  if (!check_v1_manager_absent(err_out)) return false;

  /* D1 (MM-03): acquire exclusive lifetime host lock before state/listeners. */
  if (!acquire_host_lock(err_out)) return false;

  /* Acquire exclusive lifetime state-dir lock BEFORE journal load (ISSUE-05). */
  if (!journal_.acquire_state_dir_lock(&state_dir_lock_fd_, err_out)) {
    host_lock_fd_.reset();
    return false;
  }

  /* Initialize legacy SysV shm bridge (Case A default coexist, Case B conflict refuse).
   */
  if (!shm_.init(cfg_.shm, err_out)) {
    state_dir_lock_fd_.reset();
    host_lock_fd_.reset();
    return false;
  }

  /* Reserve 1 FD for graceful EMFILE accept draining (SEC-DOS-04). */
  emfile_reserve_fd_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));

  epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd_.valid()) {
    if (err_out) *err_out = "epoll_create1 failed";
    shutdown();
    return false;
  }

  wake_fd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!wake_fd_.valid() || !add_epoll(wake_fd_.get(), EPOLLIN, kTagWakeFd)) {
    if (err_out) *err_out = "eventfd setup failed";
    shutdown();
    return false;
  }

  /* Single CLOCK_MONOTONIC timerfd embedded directly into epoll_fd_ (R2-5). */
  if (!hb_timers_.init(err_out) ||
      !add_epoll(hb_timers_.fd(), EPOLLIN, kTagHeartbeatTimerFd)) {
    if (err_out && err_out->empty()) *err_out = "heartbeat timerfd epoll setup failed";
    shutdown();
    return false;
  }

  if (cfg_.use_signalfd) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);
    signal_fd_.reset(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!signal_fd_.valid() || !add_epoll(signal_fd_.get(), EPOLLIN, kTagSignalFd)) {
      if (err_out) *err_out = "signalfd setup failed";
      shutdown();
      return false;
    }
  }

  /* Reconcile journal before opening sockets so live leases are protected immediately. */
  if (!reconcile_journal_startup(err_out)) {
    shutdown();
    return false;
  }

  if (!open_listener(cfg_.client_socket_path, cfg_.client_socket_mode,
                     cfg_.client_socket_gid, /*is_admin=*/false,
                     cfg_.allow_world_client_socket, &client_listener_, err_out)) {
    shutdown();
    return false;
  }
  if (!add_epoll(client_listener_.fd.get(), EPOLLIN, kTagClientListen)) {
    if (err_out) *err_out = "epoll add failed for client listener";
    shutdown();
    return false;
  }

  if (!cfg_.admin_socket_path.empty()) {
    if (!open_listener(cfg_.admin_socket_path, 0600, std::nullopt, /*is_admin=*/true,
                       /*allow_world=*/false, &admin_listener_, err_out)) {
      shutdown();
      return false;
    }
    if (!add_epoll(admin_listener_.fd.get(), EPOLLIN, kTagAdminListen)) {
      if (err_out) *err_out = "epoll add failed for admin listener";
      shutdown();
      return false;
    }
  }

  started_ = true;
  stop_requested_.store(false, std::memory_order_release);
  return true;
}

void MtlManagerServer::request_stop() {
  stop_requested_.store(true, std::memory_order_release);
  if (wake_fd_.valid()) {
    uint64_t one = 1;
    ssize_t n = ::write(wake_fd_.get(), &one, sizeof(one));
    (void)n;
  }
}

int MtlManagerServer::run() {
  if (!started_) return 1;

  struct epoll_event events[64];
  int64_t last_shm_audit_ns = mt_varlink_mono_ns();
  while (!stop_requested_.load(std::memory_order_acquire)) {
    int wait_ms = 500;
    if (cfg_.open_session_timeout_ms > 0) {
      int64_t now_ns = mt_varlink_mono_ns();
      int64_t timeout_ns = cfg_.open_session_timeout_ms * 1000000LL;
      for (const auto& [cid, c] : connections_) {
        if (!c || c->is_admin || !c->session_id.empty()) continue;
        int64_t rem_ns = (c->connected_mono_ns + timeout_ns) - now_ns;
        int rem_ms =
            (rem_ns <= 0) ? 1 : static_cast<int>((rem_ns + 999999LL) / 1000000LL);
        if (rem_ms < wait_ms) wait_ms = rem_ms;
      }
    }
    if (!hb_timers_.empty()) {
      int64_t stall_probe_ms = std::max<int64_t>(10, hb_timers_.stall_threshold_ms() / 2);
      if (stall_probe_ms < wait_ms) wait_ms = static_cast<int>(stall_probe_ms);
    }

    const int64_t before_wait_ns = mt_varlink_mono_ns();
    int n = ::epoll_wait(epoll_fd_.get(), events, 64, wait_ms);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    const int64_t after_wait_ns = mt_varlink_mono_ns();
    bool stalled_this_tick = false;
    if (!hb_timers_.empty()) {
      const int64_t threshold_ns = hb_timers_.stall_threshold_ms() * 1000000LL;
      const int64_t wait_budget_ns =
          static_cast<int64_t>(wait_ms) * 1000000LL + threshold_ns;
      const int64_t armed_ns = hb_timers_.armed_deadline_ns();
      const bool wait_stalled = (after_wait_ns - before_wait_ns) >= wait_budget_ns;
      const bool timer_overrun =
          (armed_ns > 0 && (after_wait_ns - armed_ns) >= threshold_ns);
      if (wait_stalled || timer_overrun) {
        handle_manager_stall(after_wait_ns);
        stalled_this_tick = true;
      }
    }

    bool hb_timer_fired = false;
    for (int i = 0; i < n; ++i) {
      const uint64_t tag = events[i].data.u64;
      const uint64_t kind = tag & kTagMask;
      const uint64_t payload = tag & ~kTagMask;

      if (kind == kTagSignalFd) {
        struct signalfd_siginfo si{};
        while (::read(signal_fd_.get(), &si, sizeof(si)) ==
               static_cast<ssize_t>(sizeof(si))) {
          if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT ||
              si.ssi_signo == SIGQUIT) {
            stop_requested_.store(true, std::memory_order_release);
          }
        }
      } else if (kind == kTagWakeFd) {
        uint64_t v = 0;
        while (::read(wake_fd_.get(), &v, sizeof(v)) > 0) {
        }
      } else if (kind == kTagHeartbeatTimerFd) {
        hb_timer_fired = true;
      } else if (kind == kTagClientListen) {
        handle_accept(client_listener_);
      } else if (kind == kTagAdminListen) {
        handle_accept(admin_listener_);
      } else if (kind == kTagLeasePidFd) {
        handle_pidfd_event(static_cast<int>(payload));
      } else if (kind == kTagQueuePidFd) {
        handle_queue_pidfd_event(static_cast<int>(payload));
      } else if (kind == kTagConnPidFd) {
        handle_conn_pidfd_event(static_cast<int>(payload));
      } else if (kind == kTagConn) {
        handle_conn_event(payload, events[i].events);
      }
    }

    /* Dispatch-duration stall guard runs BEFORE heartbeat timer expiration (HB-02). */
    const int64_t dispatch_end_ns = mt_varlink_mono_ns();
    if (!stalled_this_tick && !hb_timers_.empty()) {
      const int64_t threshold_ns = hb_timers_.stall_threshold_ms() * 1000000LL;
      if ((dispatch_end_ns - after_wait_ns) >= threshold_ns) {
        handle_manager_stall(dispatch_end_ns);
        stalled_this_tick = true;
      }
    }

    if (hb_timer_fired) {
      (void)hb_timers_.drain_timerfd();
      process_heartbeat_timers(dispatch_end_ns);
    }

    sweep_idle_pre_session_conns();
    int64_t now_ns = mt_varlink_mono_ns();
    if (now_ns - last_shm_audit_ns >= 1000000000LL) {
      last_shm_audit_ns = now_ns;
      audit_shm_integrity();
    }
  }

  shutdown();
  return 0;
}

void MtlManagerServer::sweep_idle_pre_session_conns() {
  if (cfg_.open_session_timeout_ms <= 0) return;
  const int64_t now_ns = mt_varlink_mono_ns();
  const int64_t timeout_ns = cfg_.open_session_timeout_ms * 1000000LL;
  std::vector<uint64_t> expired;
  for (const auto& [cid, c] : connections_) {
    if (!c || c->is_admin || !c->session_id.empty()) continue;
    if ((now_ns - c->connected_mono_ns) >= timeout_ns) expired.push_back(cid);
  }
  for (uint64_t cid : expired) destroy_conn(cid);
}

void MtlManagerServer::audit_shm_integrity() {
  if (shm_.mode() != ShmMode::CaseACoexist || leases_.empty()) return;
  std::vector<std::pair<uint32_t, pid_t>> owned;
  owned.reserve(leases_.size());
  for (const auto& [lid, l] : leases_) {
    owned.emplace_back(l.cpu_id, l.pid);
  }
  std::vector<uint32_t> tampered = shm_.audit_owned_leases(owned);
  for (uint32_t cpu_id : tampered) {
    ++shm_tamper_warnings_;
    std::cerr << "mtlm2d: WARNING: legacy shm slot for CPU " << cpu_id
              << " was modified externally; keeping authoritative v2 lease\n";
  }
}

void MtlManagerServer::strip_queue_children(const std::string& queue_lease_id) {
  /* Strict reverse order for data-steering children: XSK -> Flow -> UdpFilter. */
  std::vector<std::string> xsk_ids;
  for (const auto& [lid, x] : xsk_leases_) {
    if (x.queue_lease_id == queue_lease_id) xsk_ids.push_back(lid);
  }
  for (const std::string& lid : xsk_ids) {
    auto it = xsk_leases_.find(lid);
    if (it == xsk_leases_.end()) continue;
    std::string berr;
    (void)netdev_->delete_xsk_map(it->second.if_name, it->second.ifindex,
                                  it->second.queue_id, &berr);
    xsk_leases_.erase(it);
  }

  std::vector<std::string> flow_ids;
  for (const auto& [lid, f] : flow_leases_) {
    if (f.queue_lease_id == queue_lease_id) flow_ids.push_back(lid);
  }
  for (const std::string& lid : flow_ids) {
    auto it = flow_leases_.find(lid);
    if (it == flow_leases_.end()) continue;
    std::string berr;
    (void)netdev_->delete_flow(it->second.if_name, it->second.ifindex,
                               it->second.rule_location, &berr);
    flow_leases_.erase(it);
  }

  std::vector<std::string> udp_ids;
  for (const auto& [lid, u] : udp_filter_leases_) {
    if (u.queue_lease_id == queue_lease_id) udp_ids.push_back(lid);
  }
  for (const std::string& lid : udp_ids) {
    auto it = udp_filter_leases_.find(lid);
    if (it == udp_filter_leases_.end()) continue;
    std::string berr;
    (void)netdev_->delete_udp_filter(it->second.if_name, it->second.ifindex,
                                     it->second.queue_id, it->second.dst_port, &berr);
    udp_filter_leases_.erase(it);
  }
}

void MtlManagerServer::maybe_detach_xdp_if_unused(const std::string& if_name) {
  if (has_active_queue_on_iface(if_name)) return;
  auto xit = xdp_attachments_.find(if_name);
  if (xit == xdp_attachments_.end()) return;
  std::string berr;
  (void)netdev_->detach_xdp(xit->second.if_name, xit->second.ifindex, xit->second.mode,
                            &berr);
  xdp_attachments_.erase(xit);
}

void MtlManagerServer::release_queue_cascade(const std::string& queue_lease_id) {
  auto qit = queue_leases_.find(queue_lease_id);
  if (qit == queue_leases_.end()) return;
  std::string if_name = qit->second.if_name;

  strip_queue_children(queue_lease_id);

  if (qit->second.pidfd.valid()) {
    del_epoll(qit->second.pidfd.get());
    pidfd_to_queue_lease_.erase(qit->second.pidfd.get());
  }
  queue_leases_.erase(qit);
  maybe_detach_xdp_if_unused(if_name);
}

void MtlManagerServer::reap_dead_pidfds() {
  std::vector<std::string> dead_qids;
  for (const auto& [lid, ql] : queue_leases_) {
    if (!ql.pidfd.valid() || !PeerCredExtractor::is_pidfd_alive(ql.pidfd.get())) {
      dead_qids.push_back(lid);
    }
  }
  for (const std::string& lid : dead_qids) {
    release_queue_cascade(lid);
  }

  std::vector<std::string> dead_ids;
  for (const auto& [lid, lease] : leases_) {
    if (!lease.pidfd.valid() || !PeerCredExtractor::is_pidfd_alive(lease.pidfd.get())) {
      dead_ids.push_back(lid);
    }
  }
  for (const std::string& lid : dead_ids) {
    auto it = leases_.find(lid);
    if (it == leases_.end()) continue;
    if (it->second.pidfd.valid()) {
      del_epoll(it->second.pidfd.get());
      pidfd_to_lease_.erase(it->second.pidfd.get());
    }
    shm_.release_cpu(it->second.cpu_id, it->second.pid);
    leases_.erase(it);
  }
}

void MtlManagerServer::shutdown() {
  if (!started_ && !epoll_fd_.valid() && !state_dir_lock_fd_.valid() &&
      !host_lock_fd_.valid())
    return;

  /* Close active client sockets first without revoking live leases (R4-1). */
  for (auto& [cid, conn] : connections_) {
    if (conn) {
      if (conn->fd.valid()) del_epoll(conn->fd.get());
      if (conn->peer.pidfd_owner.valid()) del_epoll(conn->peer.pidfd_owner.get());
    }
  }
  conn_pidfd_to_id_.clear();
  connections_.clear();

  close_listener(&client_listener_);
  close_listener(&admin_listener_);

  if (started_) {
    if (cfg_.teardown_on_exit) {
      std::vector<std::string> all_qids;
      all_qids.reserve(queue_leases_.size());
      for (const auto& [lid, ql] : queue_leases_) all_qids.push_back(lid);
      for (const std::string& lid : all_qids) release_queue_cascade(lid);
      pidfd_to_queue_lease_.clear();
      queue_leases_.clear();
      xsk_leases_.clear();
      flow_leases_.clear();
      udp_filter_leases_.clear();
      std::vector<std::string> rem_xdp;
      for (const auto& [ifn, xa] : xdp_attachments_) rem_xdp.push_back(ifn);
      for (const std::string& ifn : rem_xdp) maybe_detach_xdp_if_unused(ifn);

      for (auto& [lid, lease] : leases_) {
        if (lease.pidfd.valid()) del_epoll(lease.pidfd.get());
        shm_.release_cpu(lease.cpu_id, lease.pid);
      }
      pidfd_to_lease_.clear();
      leases_.clear();
      std::string jerr;
      (void)persist_journal(&jerr);
      shm_.shutdown(/*preserve_live_entries=*/false);
    } else {
      /* Normal stop preserves live leases, AF_XDP kernel objects, journal, and
       * tenant-PID shm slots (R4-1, LIFE-07). */
      reap_dead_pidfds();
      std::string jerr;
      (void)persist_journal(&jerr);
      for (auto& [lid, ql] : queue_leases_) {
        if (ql.pidfd.valid()) del_epoll(ql.pidfd.get());
      }
      pidfd_to_queue_lease_.clear();
      queue_leases_.clear();
      xsk_leases_.clear();
      flow_leases_.clear();
      udp_filter_leases_.clear();
      xdp_attachments_.clear();
      for (auto& [lid, lease] : leases_) {
        if (lease.pidfd.valid()) del_epoll(lease.pidfd.get());
      }
      pidfd_to_lease_.clear();
      leases_.clear();
      shm_.shutdown(/*preserve_live_entries=*/true);
    }
  } else {
    shm_.shutdown(/*preserve_live_entries=*/true);
  }

  if (hb_timers_.valid()) {
    del_epoll(hb_timers_.fd());
    hb_timers_.close();
  }
  if (signal_fd_.valid()) {
    del_epoll(signal_fd_.get());
    signal_fd_.reset();
  }
  if (wake_fd_.valid()) {
    del_epoll(wake_fd_.get());
    wake_fd_.reset();
  }
  epoll_fd_.reset();
  emfile_reserve_fd_.reset();
  state_dir_lock_fd_.reset();
  host_lock_fd_.reset();
  started_ = false;
}

void MtlManagerServer::handle_accept(const Listener& ls) {
  for (size_t accepted = 0; accepted < kMaxAcceptPerTick; ++accepted) {
    int cfd = ::accept4(ls.fd.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      if ((errno == EMFILE || errno == ENFILE) && emfile_reserve_fd_.valid()) {
        /* SEC-DOS-04: release reserve FD, drain and close one pending connection, then
         * re-acquire reserve FD to avoid level-triggered epoll EMFILE spin. */
        emfile_reserve_fd_.reset();
        int drain_fd =
            ::accept4(ls.fd.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (drain_fd >= 0) ::close(drain_fd);
        emfile_reserve_fd_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
      }
      break;
    }
    UniqueFd client_fd(cfd);

    /* Enforce total and per-UID connection caps BEFORE heavy PeerCred extraction
     * (ISSUE-29). */
    if (connections_.size() >= cfg_.max_connections) continue;

    struct ucred quick_cred{};
    socklen_t qlen = sizeof(quick_cred);
    if (::getsockopt(client_fd.get(), SOL_SOCKET, SO_PEERCRED, &quick_cred, &qlen) != 0 ||
        qlen != sizeof(quick_cred) || quick_cred.pid <= 0)
      continue;

    size_t uid_conns = 0;
    for (const auto& [id, existing] : connections_) {
      if (existing->peer.principal.uid == quick_cred.uid) ++uid_conns;
    }
    if (uid_conns >= cfg_.max_conns_per_uid) continue;

    PeerIdentity identity;
    std::string perr;
    if (!peer_extractor_.extract(client_fd.get(), &identity, &perr)) continue;

    auto conn = std::make_unique<Connection>();
    conn->id = next_conn_id_++;
    conn->fd = std::move(client_fd);
    conn->is_admin = ls.is_admin;
    conn->peer = std::move(identity);
    conn->connected_mono_ns = mt_varlink_mono_ns();
    conn->epoll_events = EPOLLIN;

    uint64_t tag = kTagConn | conn->id;
    if (!add_epoll(conn->fd.get(), EPOLLIN, tag)) continue;
    /* SEC-AC-06: watch connection owner's pidfd so parent exit closes inherited FD
     * immediately even if a forked child retains the socket descriptor. */
    if (conn->peer.pidfd_owner.valid()) {
      int pfd = conn->peer.pidfd_owner.get();
      uint64_t ptag = kTagConnPidFd | static_cast<uint64_t>(static_cast<uint32_t>(pfd));
      if (add_epoll(pfd, EPOLLIN, ptag)) {
        conn_pidfd_to_id_[pfd] = conn->id;
      }
    }
    connections_[conn->id] = std::move(conn);
  }
}

void MtlManagerServer::update_conn_epoll(Connection& conn) {
  uint32_t want = 0;
  /* Disarm EPOLLIN once conn->closing is set (ISSUE-30). */
  if (!conn.closing) want |= EPOLLIN;
  if (mt_varlink_writer_has_pending(&conn.writer)) want |= EPOLLOUT;
  if (want != conn.epoll_events) {
    conn.epoll_events = want;
    (void)mod_epoll(conn.fd.get(), want, kTagConn | conn.id);
  }
}

void MtlManagerServer::begin_conn_close(Connection& conn) {
  conn.closing = true;
  update_conn_epoll(conn);
}

void MtlManagerServer::destroy_conn(uint64_t conn_id) {
  auto it = connections_.find(conn_id);
  if (it == connections_.end()) return;
  auto conn = std::move(it->second);
  connections_.erase(it);

  if (conn->fd.valid()) del_epoll(conn->fd.get());
  if (conn->peer.pidfd_owner.valid()) {
    del_epoll(conn->peer.pidfd_owner.get());
    conn_pidfd_to_id_.erase(conn->peer.pidfd_owner.get());
  }
  stop_session_heartbeat(*conn);

  if (!conn->session_id.empty()) {
    bool changed = false;

    /* Handle queue leases first: reverse-cascade dead owners, strip steering & quarantine
     * live owners. */
    std::vector<std::string> dead_qids;
    std::vector<std::string> live_qids;
    for (const auto& [lid, ql] : queue_leases_) {
      if (ql.session_id != conn->session_id || ql.state != LeaseState::Allocated)
        continue;
      if (ql.pidfd.valid() && PeerCredExtractor::is_pidfd_alive(ql.pidfd.get()))
        live_qids.push_back(lid);
      else
        dead_qids.push_back(lid);
    }
    for (const std::string& lid : live_qids) {
      strip_queue_children(lid);
      auto qit = queue_leases_.find(lid);
      if (qit != queue_leases_.end()) {
        qit->second.state = LeaseState::Quarantined;
        qit->second.session_id.clear();
        changed = true;
      }
    }
    for (const std::string& lid : dead_qids) {
      release_queue_cascade(lid);
      changed = true;
    }

    std::vector<std::string> to_free;
    for (auto& [lid, lease] : leases_) {
      if (lease.session_id != conn->session_id) continue;
      if (lease.state == LeaseState::Allocated) {
        if (lease.pidfd.valid() && PeerCredExtractor::is_pidfd_alive(lease.pidfd.get())) {
          /* Process disconnected without CloseSession while still alive: quarantine CPU
           * until pidfd signals exit to prevent double-booking. */
          lease.state = LeaseState::Quarantined;
          lease.session_id.clear();
          changed = true;
        } else {
          to_free.push_back(lid);
        }
      }
    }
    for (const std::string& lid : to_free) {
      auto lit = leases_.find(lid);
      if (lit == leases_.end()) continue;
      if (lit->second.pidfd.valid()) {
        del_epoll(lit->second.pidfd.get());
        pidfd_to_lease_.erase(lit->second.pidfd.get());
      }
      shm_.release_cpu(lit->second.cpu_id, lit->second.pid);
      leases_.erase(lit);
      changed = true;
    }
    if (changed) {
      std::string jerr;
      (void)persist_journal(&jerr);
    }
  }
}

void MtlManagerServer::handle_pidfd_event(int pidfd) {
  auto it = pidfd_to_lease_.find(pidfd);
  if (it == pidfd_to_lease_.end()) {
    del_epoll(pidfd);
    return;
  }
  const std::string lid = it->second;
  pidfd_to_lease_.erase(it);
  del_epoll(pidfd);

  auto lit = leases_.find(lid);
  if (lit == leases_.end()) return;
  shm_.release_cpu(lit->second.cpu_id, lit->second.pid);
  leases_.erase(lit);
  std::string jerr;
  (void)persist_journal(&jerr);
}

void MtlManagerServer::handle_queue_pidfd_event(int pidfd) {
  auto it = pidfd_to_queue_lease_.find(pidfd);
  if (it == pidfd_to_queue_lease_.end()) {
    del_epoll(pidfd);
    return;
  }
  const std::string qlid = it->second;
  pidfd_to_queue_lease_.erase(it);
  del_epoll(pidfd);

  release_queue_cascade(qlid);
  std::string jerr;
  (void)persist_journal(&jerr);
}

void MtlManagerServer::handle_conn_pidfd_event(int pidfd) {
  auto it = conn_pidfd_to_id_.find(pidfd);
  if (it == conn_pidfd_to_id_.end()) {
    del_epoll(pidfd);
    return;
  }
  uint64_t cid = it->second;
  conn_pidfd_to_id_.erase(it);
  del_epoll(pidfd);
  destroy_conn(cid);
}

void MtlManagerServer::handle_conn_event(uint64_t conn_id, uint32_t events) {
  auto it = connections_.find(conn_id);
  if (it == connections_.end()) return;
  Connection& conn = *it->second;

  if ((events & EPOLLERR) != 0) {
    destroy_conn(conn_id);
    return;
  }

  if ((events & EPOLLOUT) != 0 && mt_varlink_writer_has_pending(&conn.writer)) {
    int frc = mt_varlink_writer_flush(&conn.writer, conn.fd.get());
    if (frc < 0 && frc != -EAGAIN) {
      destroy_conn(conn_id);
      return;
    }
    if (conn.closing && !mt_varlink_writer_has_pending(&conn.writer)) {
      destroy_conn(conn_id);
      return;
    }
  }

  if (!conn.closing && (events & (EPOLLIN | EPOLLHUP)) != 0) {
    /* Process one recvmsg chunk per epoll tick per connection for fair multi-tenant
     * scheduling under pipelined 64 KiB flood (SEC-DOS-05). */
    int rrc = mt_varlink_reader_recv(&conn.reader, conn.fd.get());
    if (rrc == 0) {
      destroy_conn(conn_id);
      return;
    }
    if (rrc < 0) {
      if (rrc != -EAGAIN) {
        if (rrc == -EMSGSIZE) {
          enqueue_error(conn, "org.varlink.service.InvalidParameter",
                        "frame exceeds 64 KiB limit", "frame");
        } else {
          enqueue_error(conn, "org.varlink.service.InvalidParameter",
                        "protocol framing error", "frame");
        }
        begin_conn_close(conn);
      }
    } else {
      while (!conn.closing) {
        mt_varlink_frame_t frame;
        mt_varlink_frame_init(&frame);
        int prc = mt_varlink_reader_pop(&conn.reader, &frame);
        if (prc == 0) break;
        if (prc < 0) {
          mt_varlink_frame_clear(&frame);
          enqueue_error(conn, "org.varlink.service.InvalidParameter",
                        "invalid frame delimiter or SCM_RIGHTS", "frame");
          begin_conn_close(conn);
          break;
        }
        dispatch_frame(conn, frame);
        mt_varlink_frame_clear(&frame);
      }
    }
  }

  /* SEC-DOS-03: drop immediately when outbound queue cap was exceeded on a slow reader.
   */
  if (conn.drop_immediately) {
    destroy_conn(conn_id);
    return;
  }

  /* Opportunistically flush queued replies right away. */
  if (mt_varlink_writer_has_pending(&conn.writer)) {
    int frc = mt_varlink_writer_flush(&conn.writer, conn.fd.get());
    if (frc < 0 && frc != -EAGAIN) {
      destroy_conn(conn_id);
      return;
    }
  }

  if (conn.closing && !mt_varlink_writer_has_pending(&conn.writer)) {
    destroy_conn(conn_id);
    return;
  }
  update_conn_epoll(conn);
}

void MtlManagerServer::enqueue_reply(Connection& conn, struct json_object* params_owned,
                                     bool allow_large) {
  JsonPtr root(json_object_new_object());
  json_object_object_add(root.get(), "parameters",
                         params_owned ? params_owned : json_object_new_object());
  std::string payload = json_to_compact(root.get());
  int rc = allow_large ? mt_varlink_writer_enqueue_large(&conn.writer, payload.data(),
                                                         payload.size())
                       : mt_varlink_writer_enqueue(&conn.writer, payload.data(),
                                                   payload.size(), nullptr, 0);
  if (rc == -ENOBUFS) {
    conn.closing = true;
    conn.drop_immediately = true;
  }
}

void MtlManagerServer::enqueue_error(Connection& conn, const std::string& error_name,
                                     const std::string& message,
                                     const std::string& param) {
  JsonPtr root(json_object_new_object());
  json_add_str(root.get(), "error", error_name);
  json_object* p = json_object_new_object();
  if (!param.empty()) json_add_str(p, "parameter", truncate_err(param));
  if (!message.empty()) json_add_str(p, "message", truncate_err(message));
  json_object_object_add(root.get(), "parameters", p);
  std::string payload = json_to_compact(root.get());
  int rc =
      mt_varlink_writer_enqueue(&conn.writer, payload.data(), payload.size(), nullptr, 0);
  if (rc == -ENOBUFS) {
    conn.closing = true;
    conn.drop_immediately = true;
  }
}

void MtlManagerServer::dispatch_frame(Connection& conn, const mt_varlink_frame_t& frame) {
  json_object* raw_obj = nullptr;
  int prc = mt_varlink_parse_json_object(frame.json_text, frame.json_len, &raw_obj);
  if (prc != 0 || !raw_obj) {
    enqueue_error(conn, "org.varlink.service.InvalidParameter", "malformed JSON object",
                  "frame");
    begin_conn_close(conn);
    return;
  }
  JsonPtr req(raw_obj);

  const char* method = nullptr;
  json_object* params = nullptr;
  bool more = false, oneway = false, upgrade = false;
  if (mt_varlink_validate_request_envelope(req.get(), &method, &params, &more, &oneway,
                                           &upgrade) != 0 ||
      !method) {
    enqueue_error(conn, "org.varlink.service.InvalidParameter",
                  "invalid Varlink envelope", "method");
    return;
  }
  if (more || oneway || upgrade) {
    enqueue_error(conn, "org.varlink.service.InvalidParameter",
                  "more/oneway/upgrade flags unsupported", "method");
    return;
  }

  std::string m(method);
  /* ISSUE-20 / ISSUE-82: run heartbeat flood check BEFORE FD, session, or parameter
   * validation so rapid invalid/pre-session/FD-carrying Heartbeats are rate-limited. */
  if (!conn.is_admin && m == MT_VARLINK_MANAGER_INTERFACE ".Heartbeat") {
    rpc_heartbeat(conn, params, frame.fd_count);
    return;
  }

  const bool allows_fd =
      (!conn.is_admin && m == MT_VARLINK_MANAGER_INTERFACE ".AttachXsk");
  if (!allows_fd && frame.fd_count > 0u) {
    enqueue_error(conn, "org.varlink.service.InvalidParameter",
                  "unexpected file descriptors", "fds");
    return;
  }

  if (m == "org.varlink.service.GetInfo") {
    rpc_get_info(conn, params);
  } else if (m == "org.varlink.service.GetInterfaceDescription") {
    rpc_get_interface_desc(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".GetStatus") {
    if (!conn.is_admin) {
      enqueue_error(conn, "org.varlink.service.PermissionDenied",
                    "admin RPCs are restricted to admin socket");
      return;
    }
    rpc_get_status(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".RevokeLease") {
    if (!conn.is_admin) {
      enqueue_error(conn, "org.varlink.service.PermissionDenied",
                    "admin RPCs are restricted to admin socket");
      return;
    }
    rpc_revoke_lease(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".RevokeSession") {
    if (!conn.is_admin) {
      enqueue_error(conn, "org.varlink.service.PermissionDenied",
                    "admin RPCs are restricted to admin socket");
      return;
    }
    rpc_revoke_session(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".KillQuarantinedOwner") {
    if (!conn.is_admin) {
      enqueue_error(conn, "org.varlink.service.PermissionDenied",
                    "admin RPCs are restricted to admin socket");
      return;
    }
    rpc_kill_quarantined_owner(conn, params);
  } else if (conn.is_admin) {
    enqueue_error(conn, "org.varlink.service.MethodNotFound",
                  "client RPC not available on admin socket", m);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".OpenSession") {
    rpc_open_session(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".ClaimSession") {
    rpc_claim_session(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".CloseSession") {
    rpc_close_session(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".Ping") {
    rpc_ping(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".AcquireCpu") {
    rpc_acquire_cpu(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".AcquireQueue") {
    rpc_acquire_queue(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".AttachXsk") {
    rpc_attach_xsk(conn, params, frame.fds, frame.fd_count);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".AddFlow") {
    rpc_add_flow(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".AddUdpFilter") {
    rpc_add_udp_filter(conn, params);
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease") {
    rpc_release_lease(conn, params);
  } else {
    enqueue_error(conn, "org.varlink.service.MethodNotFound", "unknown method", m);
  }
}

void MtlManagerServer::rpc_get_info(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kEmpty{};
  std::string bad;
  if (!check_allowed_keys(params, kEmpty, &bad)) {
    enqueue_error(conn, "org.varlink.service.InvalidParameter", "unexpected parameter",
                  bad);
    return;
  }
  json_object* rep = json_object_new_object();
  json_add_str(rep, "vendor", "OpenVisualCloud");
  json_add_str(rep, "product", "MtlManager 2");
  json_add_str(rep, "version", "2.0.0");
  json_add_str(rep, "url", "https://github.com/OpenVisualCloud/Media-Transport-Library");
  json_object* ifaces = json_object_new_array();
  json_object_array_add(ifaces, json_object_new_string(MT_VARLINK_SERVICE_INTERFACE));
  json_object_array_add(ifaces, json_object_new_string(MT_VARLINK_MANAGER_INTERFACE));
  json_object_object_add(rep, "interfaces", ifaces);
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_get_interface_desc(Connection& conn,
                                              struct json_object* params) {
  static const std::set<std::string> kKeys = {"interface"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, "org.varlink.service.InvalidParameter", "unexpected parameter",
                  bad);
    return;
  }
  json_object* jif = nullptr;
  std::string name;
  if (!params || !json_object_object_get_ex(params, "interface", &jif) ||
      !parse_strict_str(jif, &name)) {
    enqueue_error(conn, "org.varlink.service.InvalidParameter",
                  "interface must be a string", "interface");
    return;
  }
  json_object* rep = json_object_new_object();
  if (name == MT_VARLINK_SERVICE_INTERFACE) {
    json_add_str(rep, "description", kVarlinkServiceDesc);
    enqueue_reply(conn, rep);
  } else if (name == MT_VARLINK_MANAGER_INTERFACE) {
    json_add_str(rep, "description", kMtlManagerDesc);
    enqueue_reply(conn, rep);
  } else {
    json_object_put(rep);
    enqueue_error(conn, "org.varlink.service.InterfaceNotFound", "unknown interface",
                  name);
  }
}

std::string MtlManagerServer::next_id(const char* prefix) {
  uint64_t rnd = 0;
  if (::getrandom(&rnd, sizeof(rnd), 0) != static_cast<ssize_t>(sizeof(rnd)))
    rnd = static_cast<uint64_t>(::getpid()) ^ (next_id_seq_ * 0x9E3779B97F4A7C15ULL);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s-%012llx-%04llx", prefix,
                static_cast<unsigned long long>(rnd & 0xFFFFFFFFFFFFULL),
                static_cast<unsigned long long>((next_id_seq_++) & 0xFFFFULL));
  return std::string(buf);
}

void MtlManagerServer::start_session_heartbeat(Connection& conn,
                                               std::optional<int64_t> req_interval_ms,
                                               std::optional<int64_t> req_max_missed) {
  int64_t eff_interval_ms = cfg_.heartbeat.default_interval_ms;
  int64_t eff_max_missed = cfg_.heartbeat.default_missed_beats;
  cfg_.heartbeat.resolve_session_params(req_interval_ms, req_max_missed, &eff_interval_ms,
                                        &eff_max_missed);

  const int64_t now_ns = mt_varlink_mono_ns();
  conn.hb = HeartbeatSessionState{};
  conn.hb.active = true;
  conn.hb.interval_ms = eff_interval_ms;
  conn.hb.max_missed = eff_max_missed;
  conn.hb.last_beat_ns = now_ns;
  conn.hb.next_deadline_ns = now_ns + eff_interval_ms * 1000000LL;
  conn.hb.missed_beats = 0;
  conn.hb.health = HeartbeatHealth::Healthy;
  conn.hb.warned_episode = false;

  hb_timers_.schedule(conn.id, conn.hb.next_deadline_ns, conn.hb.total_timeout_ms());
  hb_timers_.arm_timerfd(now_ns);
}

void MtlManagerServer::stop_session_heartbeat(Connection& conn) {
  if (!conn.hb.active) return;
  conn.hb.active = false;
  hb_timers_.remove(conn.id);
  hb_timers_.arm_timerfd(mt_varlink_mono_ns());
}

void MtlManagerServer::handle_manager_stall(int64_t now_ns) {
  ++manager_stall_events_;
  const int64_t grace_ms = std::max<int64_t>(1, cfg_.heartbeat.stall_grace_ms);
  const int64_t grace_ns = grace_ms * 1000000LL;
  std::cerr << "mtlm2d: WARNING: event loop stall detected; extending active heartbeat "
               "deadlines by "
            << grace_ms << " ms (stall_events=" << manager_stall_events_ << ")\n";

  for (auto& [cid, c] : connections_) {
    if (!c || c->session_id.empty() || !c->hb.active) continue;
    int64_t extended_ns = std::max(c->hb.next_deadline_ns, now_ns + grace_ns);
    c->hb.next_deadline_ns = extended_ns;
    hb_timers_.schedule(cid, extended_ns, c->hb.total_timeout_ms());
  }
  hb_timers_.arm_timerfd(now_ns);
}

void MtlManagerServer::revoke_session_leases_quarantine(const std::string& sid) {
  if (sid.empty()) return;

  std::vector<std::string> matching_queues;
  for (const auto& [lid, q] : queue_leases_) {
    if (q.session_id == sid) matching_queues.push_back(lid);
  }
  for (const std::string& qlid : matching_queues) {
    auto qit = queue_leases_.find(qlid);
    if (qit == queue_leases_.end()) continue;
    QueueLease& ql = qit->second;
    if (!ql.pidfd.valid() || !PeerCredExtractor::is_pidfd_alive(ql.pidfd.get())) {
      release_queue_cascade(qlid);
    } else {
      /* Finding 2 / HB-03: strip steering children, hold live queue in QUARANTINED. */
      strip_queue_children(qlid);
      ql.state = LeaseState::Quarantined;
      ql.session_id.clear();
    }
  }

  std::vector<std::string> matching_cpus;
  for (const auto& [lid, l] : leases_) {
    if (l.session_id == sid) matching_cpus.push_back(lid);
  }
  for (const std::string& lid : matching_cpus) {
    auto it = leases_.find(lid);
    if (it == leases_.end()) continue;
    CpuLease& l = it->second;
    if (!l.pidfd.valid() || !PeerCredExtractor::is_pidfd_alive(l.pidfd.get())) {
      if (l.pidfd.valid()) {
        del_epoll(l.pidfd.get());
        pidfd_to_lease_.erase(l.pidfd.get());
      }
      shm_.release_cpu(l.cpu_id, l.pid);
      leases_.erase(it);
    } else {
      /* Finding 2 / HB-03: hold live CPU in QUARANTINED until pidfd signals exit. */
      l.state = LeaseState::Quarantined;
      l.session_id.clear();
    }
  }

  std::string jerr;
  (void)persist_journal(&jerr);
}

void MtlManagerServer::process_heartbeat_timers(int64_t now_ns) {
  std::vector<uint64_t> due = hb_timers_.pop_due(now_ns);
  std::vector<uint64_t> to_revoke;

  for (uint64_t cid : due) {
    auto it = connections_.find(cid);
    if (it == connections_.end() || !it->second) continue;
    Connection& conn = *it->second;
    if (!conn.hb.active || conn.session_id.empty()) continue;

    if (now_ns < conn.hb.next_deadline_ns) {
      hb_timers_.schedule(cid, conn.hb.next_deadline_ns, conn.hb.total_timeout_ms());
      continue;
    }

    const int64_t interval_ns = std::max<int64_t>(1, conn.hb.interval_ms) * 1000000LL;
    const int64_t overdue_ns = now_ns - conn.hb.next_deadline_ns;
    const int64_t newly_missed = 1 + (overdue_ns / interval_ns);
    conn.hb.missed_beats += newly_missed;
    conn.hb.next_deadline_ns += newly_missed * interval_ns;

    if (conn.hb.missed_beats < conn.hb.max_missed) {
      conn.hb.health = HeartbeatHealth::Degraded;
      hb_timers_.schedule(cid, conn.hb.next_deadline_ns, conn.hb.total_timeout_ms());
    } else if (cfg_.heartbeat.should_revoke_on_expiry()) {
      to_revoke.push_back(cid);
    } else {
      /* Safe default (Finding 3, HB-01): warn only, preserve process and leases. */
      conn.hb.health = HeartbeatHealth::Warned;
      if (!conn.hb.warned_episode) {
        conn.hb.warned_episode = true;
        ++heartbeat_warnings_;
        std::cerr
            << "mtlm2d: WARNING: heartbeat missed threshold reached (policy=warn_only"
            << " sessionId=" << conn.session_id << " pid=" << conn.peer.principal.pid
            << " clientName=" << conn.client_name
            << " missedBeats=" << conn.hb.missed_beats << ")\n";
      }
      hb_timers_.schedule(cid, conn.hb.next_deadline_ns, conn.hb.total_timeout_ms());
    }
  }

  for (uint64_t cid : to_revoke) {
    auto it = connections_.find(cid);
    if (it == connections_.end() || !it->second) continue;
    Connection& conn = *it->second;
    ++heartbeat_revocations_;
    std::cerr << "mtlm2d: WARNING: opt-in heartbeat revocation triggered (policy=revoke"
              << " sessionId=" << conn.session_id << " pid=" << conn.peer.principal.pid
              << " clientName=" << conn.client_name
              << " missedBeats=" << conn.hb.missed_beats << ")\n";
    const std::string sid = conn.session_id;
    stop_session_heartbeat(conn);
    revoke_session_leases_quarantine(sid);
    conn.session_id.clear();
    destroy_conn(cid);
  }

  hb_timers_.arm_timerfd(mt_varlink_mono_ns());
}

void MtlManagerServer::rpc_open_session(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"clientName", "heartbeatIntervalMs",
                                              "heartbeatMaxMissed"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  if (!conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "session already open on this connection", "session");
    return;
  }
  json_object* jname = nullptr;
  std::string raw_name;
  if (!params || !json_object_object_get_ex(params, "clientName", &jname) ||
      !parse_strict_str(jname, &raw_name) || raw_name.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "clientName must be a non-empty string", "clientName");
    return;
  }

  std::optional<int64_t> req_interval;
  json_object* jint = nullptr;
  if (json_object_object_get_ex(params, "heartbeatIntervalMs", &jint) && jint &&
      !json_object_is_type(jint, json_type_null)) {
    int64_t v = 0;
    if (!parse_strict_i64(jint, &v) || v <= 0 || v > 3600000) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "heartbeatIntervalMs must be a positive integer",
                    "heartbeatIntervalMs");
      return;
    }
    req_interval = v;
  }

  std::optional<int64_t> req_missed;
  json_object* jmis = nullptr;
  if (json_object_object_get_ex(params, "heartbeatMaxMissed", &jmis) && jmis &&
      !json_object_is_type(jmis, json_type_null)) {
    int64_t v = 0;
    if (!parse_strict_i64(jmis, &v) || v <= 0 || v > 1000) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "heartbeatMaxMissed must be an integer in 1..1000",
                    "heartbeatMaxMissed");
      return;
    }
    req_missed = v;
  }

  if (cfg_.cgroup_verify.mode == CgroupVerifyMode::VerifyOnly) {
    CgroupEffectiveSnapshot cg_snap;
    std::string cg_err;
    if (!cgroup_verifier_.verify_peer(
            conn.peer.principal.pid, conn.peer.principal.pidfd,
            resolve_expected_cgroup_pattern(conn.peer.principal.uid), &cg_snap,
            &cg_err)) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed",
                    cg_err);
      return;
    }
    conn.cgroup_path = std::move(cg_snap.cgroup_path);
    conn.effective_cpus = std::move(cg_snap.effective_cpus_str);
    conn.effective_mems = std::move(cg_snap.effective_mems_str);
  }

  conn.session_id = next_id("ses");
  conn.client_name = PeerCredExtractor::sanitize_label(raw_name);
  start_session_heartbeat(conn, req_interval, req_missed);

  json_object* rep = json_object_new_object();
  json_add_str(rep, "sessionId", conn.session_id);
  json_object_object_add(rep, "heartbeatIntervalMs",
                         json_object_new_int64(conn.hb.interval_ms));
  json_object_object_add(rep, "heartbeatMaxMissed",
                         json_object_new_int64(conn.hb.max_missed));
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_claim_session(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {
      "clientName", "leaseIds", "heartbeatIntervalMs", "heartbeatMaxMissed"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  if (!conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "session already open on this connection", "session");
    return;
  }
  json_object* jname = nullptr;
  json_object* jids = nullptr;
  std::string raw_name;
  if (!params || !json_object_object_get_ex(params, "clientName", &jname) ||
      !parse_strict_str(jname, &raw_name) || raw_name.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "clientName must be a non-empty string", "clientName");
    return;
  }
  if (!json_object_object_get_ex(params, "leaseIds", &jids) || !jids ||
      !json_object_is_type(jids, json_type_array)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "leaseIds must be an array of strings", "leaseIds");
    return;
  }

  std::optional<int64_t> req_interval;
  json_object* jint = nullptr;
  if (json_object_object_get_ex(params, "heartbeatIntervalMs", &jint) && jint &&
      !json_object_is_type(jint, json_type_null)) {
    int64_t v = 0;
    if (!parse_strict_i64(jint, &v) || v <= 0 || v > 3600000) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "heartbeatIntervalMs must be a positive integer",
                    "heartbeatIntervalMs");
      return;
    }
    req_interval = v;
  }

  std::optional<int64_t> req_missed;
  json_object* jmis = nullptr;
  if (json_object_object_get_ex(params, "heartbeatMaxMissed", &jmis) && jmis &&
      !json_object_is_type(jmis, json_type_null)) {
    int64_t v = 0;
    if (!parse_strict_i64(jmis, &v) || v <= 0 || v > 1000) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "heartbeatMaxMissed must be an integer in 1..1000",
                    "heartbeatMaxMissed");
      return;
    }
    req_missed = v;
  }

  const size_t n = json_object_array_length(jids);
  if (n > kMaxClaimLeaseIds) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "too many leaseIds", "leaseIds");
    return;
  }

  std::vector<std::string> claimed_ids;
  std::vector<std::string> missing_ids;
  claimed_ids.reserve(n);
  missing_ids.reserve(n);
  std::set<std::string> seen;

  auto matches_caller = [&](LeaseState st, pid_t pid, uint64_t ticks, uid_t uid) {
    return st == LeaseState::Recovering && pid == conn.peer.principal.pid &&
           ticks == conn.peer.principal.start_time_ticks &&
           uid == conn.peer.principal.uid;
  };

  for (size_t i = 0; i < n; ++i) {
    std::string lid;
    if (!parse_strict_str(json_object_array_get_idx(jids, i), &lid) || lid.empty()) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "leaseIds must contain non-empty strings", "leaseIds");
      return;
    }
    if (!seen.insert(lid).second) continue;

    bool ok = false;
    if (auto it = leases_.find(lid); it != leases_.end()) {
      ok = matches_caller(it->second.state, it->second.pid, it->second.start_time_ticks,
                          it->second.uid);
    } else if (auto qit = queue_leases_.find(lid); qit != queue_leases_.end()) {
      ok = matches_caller(qit->second.state, qit->second.pid,
                          qit->second.start_time_ticks, qit->second.uid);
    } else if (auto xit = xsk_leases_.find(lid); xit != xsk_leases_.end()) {
      ok = matches_caller(xit->second.state, xit->second.pid,
                          xit->second.start_time_ticks, xit->second.uid);
    } else if (auto fit = flow_leases_.find(lid); fit != flow_leases_.end()) {
      ok = matches_caller(fit->second.state, fit->second.pid,
                          fit->second.start_time_ticks, fit->second.uid);
    } else if (auto uit = udp_filter_leases_.find(lid); uit != udp_filter_leases_.end()) {
      ok = matches_caller(uit->second.state, uit->second.pid,
                          uit->second.start_time_ticks, uit->second.uid);
    }

    if (ok) {
      claimed_ids.push_back(std::move(lid));
    } else {
      /* SEC-AC-08: non-existent, non-recovering, or another process's lease IDs are
       * reported in 'missing' without touching the owner's lease. */
      missing_ids.push_back(std::move(lid));
    }
  }

  if (cfg_.cgroup_verify.mode == CgroupVerifyMode::VerifyOnly) {
    CgroupEffectiveSnapshot cg_snap;
    std::string cg_err;
    if (!cgroup_verifier_.verify_peer(
            conn.peer.principal.pid, conn.peer.principal.pidfd,
            resolve_expected_cgroup_pattern(conn.peer.principal.uid), &cg_snap,
            &cg_err)) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed",
                    cg_err);
      return;
    }
    conn.cgroup_path = std::move(cg_snap.cgroup_path);
    conn.effective_cpus = std::move(cg_snap.effective_cpus_str);
    conn.effective_mems = std::move(cg_snap.effective_mems_str);
  }

  conn.session_id = next_id("ses");
  conn.client_name = PeerCredExtractor::sanitize_label(raw_name);
  start_session_heartbeat(conn, req_interval, req_missed);

  json_object* claimed = json_object_new_array();
  for (const std::string& lid : claimed_ids) {
    if (auto it = leases_.find(lid); it != leases_.end()) {
      it->second.state = LeaseState::Allocated;
      it->second.session_id = conn.session_id;
      it->second.client_name = conn.client_name;
    } else if (auto qit = queue_leases_.find(lid); qit != queue_leases_.end()) {
      qit->second.state = LeaseState::Allocated;
      qit->second.session_id = conn.session_id;
      qit->second.client_name = conn.client_name;
    } else if (auto xit = xsk_leases_.find(lid); xit != xsk_leases_.end()) {
      xit->second.state = LeaseState::Allocated;
      xit->second.session_id = conn.session_id;
      xit->second.client_name = conn.client_name;
    } else if (auto fit = flow_leases_.find(lid); fit != flow_leases_.end()) {
      fit->second.state = LeaseState::Allocated;
      fit->second.session_id = conn.session_id;
      fit->second.client_name = conn.client_name;
    } else if (auto uit = udp_filter_leases_.find(lid); uit != udp_filter_leases_.end()) {
      uit->second.state = LeaseState::Allocated;
      uit->second.session_id = conn.session_id;
      uit->second.client_name = conn.client_name;
    }
    json_object_array_add(claimed, json_object_new_string(lid.c_str()));
  }
  json_object* missing = json_object_new_array();
  for (const std::string& lid : missing_ids) {
    json_object_array_add(missing, json_object_new_string(lid.c_str()));
  }

  if (!claimed_ids.empty()) {
    std::string jerr;
    (void)persist_journal(&jerr);
  }

  json_object* rep = json_object_new_object();
  json_add_str(rep, "sessionId", conn.session_id);
  json_object_object_add(rep, "claimed", claimed);
  json_object_object_add(rep, "missing", missing);
  json_object_object_add(rep, "heartbeatIntervalMs",
                         json_object_new_int64(conn.hb.interval_ms));
  json_object_object_add(rep, "heartbeatMaxMissed",
                         json_object_new_int64(conn.hb.max_missed));
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_close_session(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kEmpty{};
  std::string bad;
  if (!check_allowed_keys(params, kEmpty, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  if (conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "no active session on connection");
    return;
  }

  stop_session_heartbeat(conn);

  /* Strict reverse cascade: XSK -> Flow -> UdpFilter -> Queue -> XDP -> Cpu. */
  std::vector<std::string> q_release;
  for (const auto& [lid, q] : queue_leases_) {
    if (q.session_id == conn.session_id && q.state == LeaseState::Allocated)
      q_release.push_back(lid);
  }
  for (const std::string& qlid : q_release) {
    release_queue_cascade(qlid);
  }

  std::vector<std::string> to_release;
  for (const auto& [lid, lease] : leases_) {
    if (lease.session_id == conn.session_id && lease.state == LeaseState::Allocated)
      to_release.push_back(lid);
  }
  for (const std::string& lid : to_release) {
    auto it = leases_.find(lid);
    if (it == leases_.end()) continue;
    if (it->second.pidfd.valid()) {
      del_epoll(it->second.pidfd.get());
      pidfd_to_lease_.erase(it->second.pidfd.get());
    }
    shm_.release_cpu(it->second.cpu_id, it->second.pid);
    leases_.erase(it);
  }
  conn.session_id.clear();

  std::string jerr;
  (void)persist_journal(&jerr);
  enqueue_reply(conn, json_object_new_object());
}

void MtlManagerServer::rpc_ping(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"sequence"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  json_object* jseq = nullptr;
  int64_t seq = 0;
  if (!params || !json_object_object_get_ex(params, "sequence", &jseq) ||
      !parse_strict_i64(jseq, &seq) || seq < 0) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "sequence must be a non-negative integer", "sequence");
    return;
  }

  const int64_t mono_us = mt_varlink_mono_ns() / 1000LL;
  json_object* rep = json_object_new_object();
  json_object_object_add(rep, "sequence", json_object_new_int64(seq));
  json_object_object_add(rep, "serverMonotonicUsec", json_object_new_int64(mono_us));
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_heartbeat(Connection& conn, struct json_object* params,
                                     size_t fd_count) {
  const int64_t now_ns = mt_varlink_mono_ns();
  const int64_t ref_interval_ms =
      conn.hb.active ? conn.hb.interval_ms : cfg_.heartbeat.default_interval_ms;

  /* ISSUE-20 / ISSUE-82: flood token-bucket and 20-strike check MUST run BEFORE
   * FD, session, sequence, or clientState validation. */
  const HeartbeatFloodTracker::Decision dec =
      conn.hb_flood.check_and_consume(now_ns, ref_interval_ms);
  if (dec == HeartbeatFloodTracker::Decision::RevokeAndDisconnect) {
    ++heartbeat_revocations_;
    std::cerr << "mtlm2d: WARNING: heartbeat flood 20-strike limit exceeded; "
                 "disconnecting peer (pid="
              << conn.peer.principal.pid
              << " sessionId=" << (conn.session_id.empty() ? "none" : conn.session_id)
              << ")\n";
    if (!conn.session_id.empty()) {
      const std::string sid = conn.session_id;
      stop_session_heartbeat(conn);
      revoke_session_leases_quarantine(sid);
      conn.session_id.clear();
    }
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".RateLimited",
                  "heartbeat flood strike limit exceeded; disconnecting");
    begin_conn_close(conn);
    return;
  }
  if (dec == HeartbeatFloodTracker::Decision::RateLimited) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".RateLimited",
                  "heartbeat rate limit exceeded");
    return;
  }

  if (fd_count > 0u) {
    enqueue_error(conn, "org.varlink.service.InvalidParameter",
                  "unexpected file descriptors", "fds");
    return;
  }
  if (conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "open a session before Heartbeat");
    return;
  }

  static const std::set<std::string> kKeys = {"sequence", "clientState"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }

  json_object* jseq = nullptr;
  int64_t seq = 0;
  if (!params || !json_object_object_get_ex(params, "sequence", &jseq) ||
      !parse_strict_i64(jseq, &seq) || seq < 0) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "sequence must be a non-negative integer", "sequence");
    return;
  }
  if (conn.hb.has_sequence && seq <= conn.hb.last_sequence) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "sequence must be strictly monotonically increasing", "sequence");
    return;
  }

  std::string client_state;
  json_object* jst = nullptr;
  if (json_object_object_get_ex(params, "clientState", &jst) && jst &&
      !json_object_is_type(jst, json_type_null)) {
    if (!parse_strict_str(jst, &client_state) || client_state.size() > 64u) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "clientState must be a string of at most 64 bytes", "clientState");
      return;
    }
    client_state = PeerCredExtractor::sanitize_label(client_state);
  }

  const bool was_degraded_or_warned =
      (conn.hb.health != HeartbeatHealth::Healthy || conn.hb.missed_beats > 0);

  conn.hb_flood.reset_strikes();
  conn.hb.has_sequence = true;
  conn.hb.last_sequence = seq;
  conn.hb.last_beat_ns = now_ns;
  conn.hb.next_deadline_ns = now_ns + conn.hb.interval_ms * 1000000LL;
  conn.hb.missed_beats = 0;
  conn.hb.warned_episode = false;
  conn.hb.client_state = client_state;

  const char* reply_status = "ok";
  if (was_degraded_or_warned) {
    conn.hb.health = HeartbeatHealth::Healthy;
    reply_status = "recovered";
  } else if (client_state == "warned" || client_state == "degraded") {
    conn.hb.health = HeartbeatHealth::Warned;
    reply_status = "warned";
  } else {
    conn.hb.health = HeartbeatHealth::Healthy;
    reply_status = "ok";
  }

  hb_timers_.schedule(conn.id, conn.hb.next_deadline_ns, conn.hb.total_timeout_ms());
  hb_timers_.arm_timerfd(now_ns);

  json_object* rep = json_object_new_object();
  json_object_object_add(rep, "sequence", json_object_new_int64(seq));
  json_object_object_add(rep, "serverMonotonicUsec",
                         json_object_new_int64(now_ns / 1000LL));
  json_add_str(rep, "status", reply_status);
  json_object_object_add(rep, "nextDeadlineMs",
                         json_object_new_int64(conn.hb.total_timeout_ms()));
  enqueue_reply(conn, rep);
}

uint32_t MtlManagerServer::count_uid_cpu_leases(uid_t uid) const {
  uint32_t cnt = 0;
  for (const auto& [lid, lease] : leases_) {
    if (lease.uid == uid) ++cnt;
  }
  return cnt;
}

uint32_t MtlManagerServer::count_uid_queue_leases(uid_t uid) const {
  uint32_t cnt = 0;
  for (const auto& [lid, ql] : queue_leases_) {
    if (ql.uid == uid) ++cnt;
  }
  return cnt;
}

uint32_t MtlManagerServer::count_uid_flow_leases(uid_t uid) const {
  uint32_t cnt = 0;
  for (const auto& [lid, fl] : flow_leases_) {
    if (fl.uid == uid) ++cnt;
  }
  return cnt;
}

bool MtlManagerServer::is_cpu_allocated(uint32_t cpu_id) const {
  for (const auto& [lid, lease] : leases_) {
    if (lease.cpu_id == cpu_id) return true;
  }
  return false;
}

bool MtlManagerServer::is_queue_allocated(const std::string& if_name,
                                          uint32_t queue_id) const {
  for (const auto& [lid, ql] : queue_leases_) {
    if (ql.if_name == if_name && ql.queue_id == queue_id) return true;
  }
  return false;
}

bool MtlManagerServer::has_active_queue_on_iface(const std::string& if_name) const {
  for (const auto& [lid, ql] : queue_leases_) {
    if (ql.if_name == if_name) return true;
  }
  return false;
}

bool MtlManagerServer::has_duplicate_flow(const std::string& if_name,
                                          const FlowSpec& spec) const {
  for (const auto& [lid, fl] : flow_leases_) {
    if (fl.if_name == if_name && fl.spec == spec) return true;
  }
  return false;
}

bool MtlManagerServer::has_xsk_on_queue_lease(const std::string& queue_lease_id) const {
  for (const auto& [lid, xl] : xsk_leases_) {
    if (xl.queue_lease_id == queue_lease_id) return true;
  }
  return false;
}

bool MtlManagerServer::has_udp_filter(const std::string& if_name, uint32_t queue_id,
                                      uint16_t dst_port) const {
  for (const auto& [lid, ul] : udp_filter_leases_) {
    if (ul.if_name == if_name && ul.queue_id == queue_id && ul.dst_port == dst_port)
      return true;
  }
  return false;
}

void MtlManagerServer::rpc_acquire_cpu(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"preferred", "numaNode", "allowFallback"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  if (conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "open a session before AcquireCpu");
    return;
  }

  std::optional<uint32_t> preferred;
  std::optional<int> numa_node;
  bool allow_fallback = true;

  if (params) {
    json_object* jpref = nullptr;
    if (json_object_object_get_ex(params, "preferred", &jpref) && jpref &&
        !json_object_is_type(jpref, json_type_null)) {
      int64_t v = 0;
      if (!parse_strict_i64(jpref, &v) || v < 0 || v > 65535) {
        enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                      "preferred must be an integer CPU ID in 0..65535", "preferred");
        return;
      }
      preferred = static_cast<uint32_t>(v);
    }

    json_object* jnuma = nullptr;
    if (json_object_object_get_ex(params, "numaNode", &jnuma) && jnuma &&
        !json_object_is_type(jnuma, json_type_null)) {
      int64_t v = 0;
      if (!parse_strict_i64(jnuma, &v) || v < 0 || v > 1024) {
        enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                      "numaNode must be a valid non-negative integer", "numaNode");
        return;
      }
      numa_node = static_cast<int>(v);
    }

    json_object* jfb = nullptr;
    if (json_object_object_get_ex(params, "allowFallback", &jfb) && jfb &&
        !json_object_is_type(jfb, json_type_null)) {
      if (!json_object_is_type(jfb, json_type_boolean)) {
        enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                      "allowFallback must be a boolean", "allowFallback");
        return;
      }
      allow_fallback = json_object_get_boolean(jfb) != 0;
    }
  }

  /* Cross-session per-UID quota aggregation (ISSUE-18). */
  const uint32_t uid_limit = cfg_.policy.max_cpus_for_uid(conn.peer.principal.uid);
  if (count_uid_cpu_leases(conn.peer.principal.uid) >= uid_limit) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied",
                  "per-UID max_cpus quota exceeded");
    return;
  }

  /* Live cgroup v2 effective cpuset/mems refresh on every AcquireCpu (ISSUE-59, CG-01).
   */
  std::optional<CgroupEffectiveSnapshot> cg_snap;
  if (cfg_.cgroup_verify.mode == CgroupVerifyMode::VerifyOnly) {
    CgroupEffectiveSnapshot live;
    std::string cg_err;
    if (!cgroup_verifier_.verify_peer(
            conn.peer.principal.pid, conn.peer.principal.pidfd,
            resolve_expected_cgroup_pattern(conn.peer.principal.uid), &live, &cg_err)) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed",
                    cg_err);
      return;
    }
    conn.cgroup_path = live.cgroup_path;
    conn.effective_cpus = live.effective_cpus_str;
    conn.effective_mems = live.effective_mems_str;
    cg_snap = std::move(live);
  }

  auto is_online = [&](uint32_t c) {
    return std::binary_search(online_cpus_.begin(), online_cpus_.end(), c);
  };
  auto cpu_node_of = [&](uint32_t c) -> int {
    auto it = cpu_numa_node_.find(c);
    return (it != cpu_numa_node_.end()) ? it->second : 0;
  };
  auto matches_numa = [&](uint32_t c) {
    if (!numa_node.has_value()) return true;
    auto it = cpu_numa_node_.find(c);
    return it != cpu_numa_node_.end() && it->second == *numa_node;
  };
  auto in_effective_cpus = [&](uint32_t c) {
    if (!cg_snap.has_value()) return true;
    return cg_snap->effective_cpus.find(c) != cg_snap->effective_cpus.end();
  };
  auto in_effective_mems = [&](uint32_t c) {
    if (!cg_snap.has_value()) return true;
    int node = cpu_node_of(c);
    if (node < 0) return false;
    return cg_snap->effective_mems.find(static_cast<uint32_t>(node)) !=
           cg_snap->effective_mems.end();
  };

  if (cg_snap.has_value() && numa_node.has_value() &&
      cg_snap->effective_mems.find(static_cast<uint32_t>(*numa_node)) ==
          cg_snap->effective_mems.end()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".NumaNodeNotAllowed",
                  "requested NUMA node " + std::to_string(*numa_node) +
                      " is outside live cpuset.mems.effective (" +
                      cg_snap->effective_mems_str + ")");
    return;
  }

  std::set<uint32_t> shm_foreign_busy;
  if (shm_.mode() == ShmMode::CaseACoexist) {
    std::string shm_snap_err;
    if (!shm_.snapshot_foreign_busy(&shm_foreign_busy, &shm_snap_err)) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError", shm_snap_err);
      return;
    }
  }
  auto is_shm_foreign_busy = [&](uint32_t c) {
    return shm_foreign_busy.find(c) != shm_foreign_busy.end();
  };

  std::optional<uint32_t> chosen;
  if (preferred.has_value()) {
    uint32_t c = *preferred;
    if (!cfg_.policy.is_cpu_allowed(c)) {
      if (!allow_fallback) {
        enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied",
                      "requested CPU is not permitted by policy");
        return;
      }
    } else if (!in_effective_cpus(c)) {
      if (!allow_fallback) {
        enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".CpuOutsideEffectiveSet",
                      "requested CPU " + std::to_string(c) +
                          " is outside live cpuset.cpus.effective (" +
                          cg_snap->effective_cpus_str + ")");
        return;
      }
    } else if (!in_effective_mems(c)) {
      if (!allow_fallback) {
        enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".NumaNodeNotAllowed",
                      "NUMA node " + std::to_string(cpu_node_of(c)) + " of CPU " +
                          std::to_string(c) + " is outside live cpuset.mems.effective (" +
                          cg_snap->effective_mems_str + ")");
        return;
      }
    } else if (is_online(c) && matches_numa(c) && !is_cpu_allocated(c) &&
               !is_shm_foreign_busy(c)) {
      chosen = c;
    } else if (!allow_fallback) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy",
                    "requested CPU " + std::to_string(c) + " is unavailable");
      return;
    }
  }

  if (!chosen.has_value()) {
    for (uint32_t c : online_cpus_) {
      if (!cfg_.policy.is_cpu_allowed(c) || !in_effective_cpus(c) ||
          !in_effective_mems(c) || !matches_numa(c) || is_cpu_allocated(c) ||
          is_shm_foreign_busy(c))
        continue;
      chosen = c;
      break;
    }
  }

  if (!chosen.has_value()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy",
                  "no eligible free CPU available");
    return;
  }

  /* Duplicate peer pidfd for lease lifetime ownership. */
  UniqueFd lease_pidfd;
  if (conn.peer.pidfd_owner.valid()) {
    int dup_fd = ::fcntl(conn.peer.pidfd_owner.get(), F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError",
                    "failed to duplicate peer pidfd");
      return;
    }
    lease_pidfd.reset(dup_fd);
  }

  bool shm_busy = false;
  std::string shm_err;
  if (!shm_.try_acquire_cpu(*chosen, conn.peer.principal.pid, conn.peer.principal.uid,
                            conn.client_name, &shm_busy, &shm_err)) {
    enqueue_error(conn,
                  shm_busy ? (MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy")
                           : (MT_VARLINK_MANAGER_INTERFACE ".InternalError"),
                  shm_err);
    return;
  }

  CpuLease lease;
  lease.lease_id = next_id("cpu");
  lease.cpu_id = *chosen;
  lease.state = LeaseState::Allocated;
  lease.session_id = conn.session_id;
  lease.pid = conn.peer.principal.pid;
  lease.start_time_ticks = conn.peer.principal.start_time_ticks;
  lease.uid = conn.peer.principal.uid;
  lease.client_name = conn.client_name;
  lease.pidfd = std::move(lease_pidfd);

  const std::string lid = lease.lease_id;
  auto [it, inserted] = leases_.emplace(lid, std::move(lease));
  std::string jerr;
  if (!inserted || !persist_journal(&jerr)) {
    /* Roll back lease + shm on journal save failure (ISSUE-07/32). */
    shm_.release_cpu(*chosen, conn.peer.principal.pid);
    if (inserted) leases_.erase(it);
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError",
                  "failed to persist CPU lease to journal: " + jerr);
    return;
  }

  watch_lease_pidfd(it->second);

  json_object* rep = json_object_new_object();
  json_add_str(rep, "leaseId", lid);
  json_object_object_add(rep, "cpuId",
                         json_object_new_int64(static_cast<int64_t>(*chosen)));
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_acquire_queue(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"ifName", "preferredQueue"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  if (conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "open a session before AcquireQueue");
    return;
  }

  json_object* jif = nullptr;
  std::string if_name;
  if (!params || !json_object_object_get_ex(params, "ifName", &jif) ||
      !parse_strict_str(jif, &if_name) || if_name.empty() || if_name.size() >= 16u) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "ifName must be a valid interface name", "ifName");
    return;
  }
  for (char c : if_name) {
    if (c == '/' || std::isspace(static_cast<unsigned char>(c)) ||
        static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) > 0x7Eu) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "ifName contains illegal characters", "ifName");
      return;
    }
  }

  std::optional<uint32_t> preferred_q;
  json_object* jpq = nullptr;
  if (json_object_object_get_ex(params, "preferredQueue", &jpq) && jpq &&
      !json_object_is_type(jpq, json_type_null)) {
    int64_t v = 0;
    if (!parse_strict_i64(jpq, &v) || v < 0 || v > 65535) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "preferredQueue must be an integer in 0..65535", "preferredQueue");
      return;
    }
    if (v == 0) {
      /* RES-Q-01: queue 0 is permanently protected. */
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "queue 0 is permanently protected", "preferredQueue");
      return;
    }
    preferred_q = static_cast<uint32_t>(v);
  }

  if (!cfg_.policy.is_interface_allowed(if_name)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied",
                  "interface '" + if_name + "' is not permitted by policy");
    return;
  }

  /* Cross-session per-UID max_queues quota aggregation (ISSUE-18). */
  const uint32_t q_limit = cfg_.policy.max_queues_for_uid(conn.peer.principal.uid);
  if (count_uid_queue_leases(conn.peer.principal.uid) >= q_limit) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied",
                  "per-UID max_queues quota exceeded");
    return;
  }

  InterfaceInfo info;
  std::string qerr;
  if (!netdev_->query_interface(if_name, &info, &qerr)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter", qerr, "ifName");
    return;
  }
  if (info.max_queues <= 1u) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy",
                  "interface '" + if_name + "' has no allocatable queues above queue 0");
    return;
  }

  std::optional<uint32_t> chosen_q;
  if (preferred_q.has_value()) {
    if (*preferred_q >= info.max_queues) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "preferredQueue exceeds interface queue count", "preferredQueue");
      return;
    }
    if (is_queue_allocated(if_name, *preferred_q)) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy",
                    "requested queue " + std::to_string(*preferred_q) + " is busy");
      return;
    }
    chosen_q = *preferred_q;
  } else {
    /* RES-Q-01: never allocate queue 0. */
    for (uint32_t q = 1; q < info.max_queues; ++q) {
      if (!is_queue_allocated(if_name, q)) {
        chosen_q = q;
        break;
      }
    }
    if (!chosen_q.has_value()) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy",
                    "no free RX queues available on '" + if_name + "'");
      return;
    }
  }

  UniqueFd lease_pidfd;
  if (conn.peer.pidfd_owner.valid()) {
    int dup_fd = ::fcntl(conn.peer.pidfd_owner.get(), F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError",
                    "failed to duplicate peer pidfd");
      return;
    }
    lease_pidfd.reset(dup_fd);
  }

  bool newly_attached_xdp = false;
  XdpAttachMode xdp_mode = XdpAttachMode::None;
  if (xdp_attachments_.find(if_name) == xdp_attachments_.end()) {
    std::string xerr;
    if (!netdev_->attach_xdp(if_name, info.ifindex, &xdp_mode, &xerr)) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError", xerr);
      return;
    }
    xdp_attachments_[if_name] = XdpAttachment{if_name, info.ifindex, xdp_mode};
    newly_attached_xdp = true;
  }

  QueueLease ql;
  ql.lease_id = next_id("que");
  ql.if_name = if_name;
  ql.ifindex = info.ifindex;
  ql.queue_id = *chosen_q;
  ql.state = LeaseState::Allocated;
  ql.session_id = conn.session_id;
  ql.pid = conn.peer.principal.pid;
  ql.start_time_ticks = conn.peer.principal.start_time_ticks;
  ql.uid = conn.peer.principal.uid;
  ql.client_name = conn.client_name;
  ql.pidfd = std::move(lease_pidfd);

  const std::string lid = ql.lease_id;
  auto [it, inserted] = queue_leases_.emplace(lid, std::move(ql));
  std::string jerr;
  if (!inserted || !persist_journal(&jerr)) {
    /* ISSUE-32 / RES-Q-07: roll back queue and lazy XDP attach on journal failure. */
    if (inserted) queue_leases_.erase(it);
    if (newly_attached_xdp) {
      std::string derr;
      (void)netdev_->detach_xdp(if_name, info.ifindex, xdp_mode, &derr);
      xdp_attachments_.erase(if_name);
    }
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError",
                  "failed to persist queue lease to journal: " + jerr);
    return;
  }

  watch_queue_pidfd(it->second);

  json_object* rep = json_object_new_object();
  json_add_str(rep, "leaseId", lid);
  json_add_str(rep, "ifName", if_name);
  json_object_object_add(rep, "ifindex",
                         json_object_new_int64(static_cast<int64_t>(info.ifindex)));
  json_object_object_add(rep, "queueId",
                         json_object_new_int64(static_cast<int64_t>(*chosen_q)));
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_attach_xsk(Connection& conn, struct json_object* params,
                                      const int* fds, size_t fd_count) {
  static const std::set<std::string> kKeys = {"queueLease"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  if (conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "open a session before AttachXsk");
    return;
  }
  if (!fds || fd_count != 1u || fds[0] < 0) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "AttachXsk requires exactly 1 file descriptor via SCM_RIGHTS", "fds");
    return;
  }

  json_object* jql = nullptr;
  std::string qlid;
  if (!params || !json_object_object_get_ex(params, "queueLease", &jql) ||
      !parse_strict_str(jql, &qlid) || qlid.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "queueLease must be a non-empty string", "queueLease");
    return;
  }

  auto qit = queue_leases_.find(qlid);
  if (qit == queue_leases_.end() || qit->second.state != LeaseState::Allocated ||
      qit->second.session_id != conn.session_id) {
    /* RES-Q-04: identical LeaseNotOwned for nonexistent or unowned queue lease. */
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned",
                  "queue lease is not owned by active session");
    return;
  }

  if (has_xsk_on_queue_lease(qlid)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy",
                  "queue lease already has an attached XSK socket");
    return;
  }

  const QueueLease& ql = qit->second;
  const int xsk_fd = fds[0];
  std::string verr;
  if (!netdev_->validate_xsk_fd(xsk_fd, ql.ifindex, ql.queue_id, &verr)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter", verr, "fds");
    return;
  }

  std::string berr;
  if (!netdev_->update_xsk_map(ql.if_name, ql.ifindex, ql.queue_id, xsk_fd, &berr)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError", berr);
    return;
  }

  XskLease xl;
  xl.lease_id = next_id("xsk");
  xl.queue_lease_id = ql.lease_id;
  xl.if_name = ql.if_name;
  xl.ifindex = ql.ifindex;
  xl.queue_id = ql.queue_id;
  xl.state = LeaseState::Allocated;
  xl.session_id = conn.session_id;
  xl.pid = conn.peer.principal.pid;
  xl.start_time_ticks = conn.peer.principal.start_time_ticks;
  xl.uid = conn.peer.principal.uid;
  xl.client_name = conn.client_name;

  const std::string lid = xl.lease_id;
  auto [it, inserted] = xsk_leases_.emplace(lid, std::move(xl));
  std::string jerr;
  if (!inserted || !persist_journal(&jerr)) {
    /* ISSUE-32 / RES-Q-07: roll back xsks_map on journal save failure. */
    if (inserted) xsk_leases_.erase(it);
    std::string derr;
    (void)netdev_->delete_xsk_map(ql.if_name, ql.ifindex, ql.queue_id, &derr);
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError",
                  "failed to persist XSK attachment to journal: " + jerr);
    return;
  }

  json_object* rep = json_object_new_object();
  json_add_str(rep, "leaseId", lid);
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_add_flow(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"queueLease", "protocol", "srcIp",
                                              "dstIp",      "srcPort",  "dstPort"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  if (conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "open a session before AddFlow");
    return;
  }

  json_object* jql = nullptr;
  std::string qlid;
  if (!params || !json_object_object_get_ex(params, "queueLease", &jql) ||
      !parse_strict_str(jql, &qlid) || qlid.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "queueLease must be a non-empty string", "queueLease");
    return;
  }

  auto qit = queue_leases_.find(qlid);
  if (qit == queue_leases_.end() || qit->second.state != LeaseState::Allocated ||
      qit->second.session_id != conn.session_id) {
    /* RES-Q-04: zero existence oracle. */
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned",
                  "queue lease is not owned by active session");
    return;
  }

  std::string protocol = "udp4";
  json_object* jproto = nullptr;
  if (json_object_object_get_ex(params, "protocol", &jproto) && jproto &&
      !json_object_is_type(jproto, json_type_null)) {
    if (!parse_strict_str(jproto, &protocol) || protocol.empty()) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "protocol must be a non-empty string", "protocol");
      return;
    }
    if (protocol != "udp4") {
      /* RES-Q-06: only udp4 flows are supported; reject udp6 with UnsupportedFlowType. */
      enqueue_error(
          conn, MT_VARLINK_MANAGER_INTERFACE ".UnsupportedFlowType",
          "unsupported flow protocol '" + protocol + "'; only udp4 is supported");
      return;
    }
  }

  FlowSpec spec;
  spec.protocol = "udp4";

  json_object* jsrc = nullptr;
  if (json_object_object_get_ex(params, "srcIp", &jsrc) && jsrc &&
      !json_object_is_type(jsrc, json_type_null)) {
    std::string src_str;
    if (!parse_strict_str(jsrc, &src_str) || src_str.empty() ||
        !parse_strict_ipv4(src_str, &spec.src_ip_be)) {
      /* ISSUE-16 / ISSUE-88: reject embedded NUL, empty srcIp "", and malformed IPv4. */
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "srcIp must be a valid non-empty IPv4 address", "srcIp");
      return;
    }
  }

  json_object* jdst = nullptr;
  if (json_object_object_get_ex(params, "dstIp", &jdst) && jdst &&
      !json_object_is_type(jdst, json_type_null)) {
    std::string dst_str;
    if (!parse_strict_str(jdst, &dst_str) || dst_str.empty() ||
        !parse_strict_ipv4(dst_str, &spec.dst_ip_be)) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "dstIp must be a valid non-empty IPv4 address", "dstIp");
      return;
    }
  }

  json_object* jsp = nullptr;
  if (json_object_object_get_ex(params, "srcPort", &jsp) && jsp &&
      !json_object_is_type(jsp, json_type_null)) {
    int64_t v = 0;
    if (!parse_strict_i64(jsp, &v) || v < 0 || v > 65535) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "srcPort must be an integer in 0..65535", "srcPort");
      return;
    }
    spec.src_port = static_cast<uint16_t>(v);
  }

  json_object* jdp = nullptr;
  int64_t dp_val = 0;
  if (!json_object_object_get_ex(params, "dstPort", &jdp) ||
      !parse_strict_i64(jdp, &dp_val) || dp_val < 1 || dp_val > 65535) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "dstPort must be an integer in 1..65535", "dstPort");
    return;
  }
  spec.dst_port = static_cast<uint16_t>(dp_val);

  if (!cfg_.policy.is_udp_port_allowed(spec.dst_port)) {
    enqueue_error(
        conn, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied",
        "UDP port " + std::to_string(spec.dst_port) + " is not permitted by policy");
    return;
  }

  /* Cross-session per-UID max_flows quota aggregation (ISSUE-18). */
  const uint32_t f_limit = cfg_.policy.max_flows_for_uid(conn.peer.principal.uid);
  if (count_uid_flow_leases(conn.peer.principal.uid) >= f_limit) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied",
                  "per-UID max_flows quota exceeded");
    return;
  }

  const QueueLease& ql = qit->second;
  /* ISSUE-52 / RES-Q-03: reject duplicate canonical FlowSpec on same interface. */
  if (has_duplicate_flow(ql.if_name, spec)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy",
                  "flow tuple is already active on interface '" + ql.if_name + "'");
    return;
  }

  uint32_t rule_loc = 0;
  std::string berr;
  if (!netdev_->add_flow(ql.if_name, ql.ifindex, ql.queue_id, spec, &rule_loc, &berr)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError", berr);
    return;
  }

  FlowLease fl;
  fl.lease_id = next_id("flw");
  fl.queue_lease_id = ql.lease_id;
  fl.if_name = ql.if_name;
  fl.ifindex = ql.ifindex;
  fl.queue_id = ql.queue_id;
  fl.rule_location = rule_loc;
  fl.spec = spec;
  fl.state = LeaseState::Allocated;
  fl.session_id = conn.session_id;
  fl.pid = conn.peer.principal.pid;
  fl.start_time_ticks = conn.peer.principal.start_time_ticks;
  fl.uid = conn.peer.principal.uid;
  fl.client_name = conn.client_name;

  const std::string lid = fl.lease_id;
  auto [it, inserted] = flow_leases_.emplace(lid, std::move(fl));
  std::string jerr;
  if (!inserted || !persist_journal(&jerr)) {
    /* ISSUE-32 / RES-Q-07: roll back NIC flow rule on journal save failure. */
    if (inserted) flow_leases_.erase(it);
    std::string derr;
    (void)netdev_->delete_flow(ql.if_name, ql.ifindex, rule_loc, &derr);
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError",
                  "failed to persist flow lease to journal: " + jerr);
    return;
  }

  json_object* rep = json_object_new_object();
  json_add_str(rep, "leaseId", lid);
  json_object_object_add(rep, "ruleLocation",
                         json_object_new_int64(static_cast<int64_t>(rule_loc)));
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_add_udp_filter(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"queueLease", "destinationPort"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  if (conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "open a session before AddUdpFilter");
    return;
  }

  json_object* jql = nullptr;
  std::string qlid;
  if (!params || !json_object_object_get_ex(params, "queueLease", &jql) ||
      !parse_strict_str(jql, &qlid) || qlid.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "queueLease must be a non-empty string", "queueLease");
    return;
  }

  auto qit = queue_leases_.find(qlid);
  if (qit == queue_leases_.end() || qit->second.state != LeaseState::Allocated ||
      qit->second.session_id != conn.session_id) {
    /* RES-Q-04: zero existence oracle. */
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned",
                  "queue lease is not owned by active session");
    return;
  }

  json_object* jdp = nullptr;
  int64_t dp_val = 0;
  if (!json_object_object_get_ex(params, "destinationPort", &jdp) ||
      !parse_strict_i64(jdp, &dp_val) || dp_val < 1 || dp_val > 65535) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "destinationPort must be an integer in 1..65535", "destinationPort");
    return;
  }
  const uint16_t dst_port = static_cast<uint16_t>(dp_val);

  if (!cfg_.policy.is_udp_port_allowed(dst_port)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied",
                  "UDP port " + std::to_string(dst_port) + " is not permitted by policy");
    return;
  }

  const QueueLease& ql = qit->second;
  if (has_udp_filter(ql.if_name, ql.queue_id, dst_port)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy",
                  "UDP filter already installed on this queue and port");
    return;
  }

  std::string berr;
  if (!netdev_->add_udp_filter(ql.if_name, ql.ifindex, ql.queue_id, dst_port, &berr)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError", berr);
    return;
  }

  UdpFilterLease ul;
  ul.lease_id = next_id("udp");
  ul.queue_lease_id = ql.lease_id;
  ul.if_name = ql.if_name;
  ul.ifindex = ql.ifindex;
  ul.queue_id = ql.queue_id;
  ul.dst_port = dst_port;
  ul.state = LeaseState::Allocated;
  ul.session_id = conn.session_id;
  ul.pid = conn.peer.principal.pid;
  ul.start_time_ticks = conn.peer.principal.start_time_ticks;
  ul.uid = conn.peer.principal.uid;
  ul.client_name = conn.client_name;

  const std::string lid = ul.lease_id;
  auto [it, inserted] = udp_filter_leases_.emplace(lid, std::move(ul));
  std::string jerr;
  if (!inserted || !persist_journal(&jerr)) {
    /* ISSUE-32 / RES-Q-07: roll back udp4_dp_filter BPF map on journal save failure. */
    if (inserted) udp_filter_leases_.erase(it);
    std::string derr;
    (void)netdev_->delete_udp_filter(ql.if_name, ql.ifindex, ql.queue_id, dst_port,
                                     &derr);
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError",
                  "failed to persist UDP filter lease to journal: " + jerr);
    return;
  }

  json_object* rep = json_object_new_object();
  json_add_str(rep, "leaseId", lid);
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_release_lease(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"leaseId"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  if (conn.session_id.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "no active session on connection");
    return;
  }
  json_object* jlid = nullptr;
  std::string lid;
  if (!params || !json_object_object_get_ex(params, "leaseId", &jlid) ||
      !parse_strict_str(jlid, &lid) || lid.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "leaseId must be a non-empty string", "leaseId");
    return;
  }

  /* SEC-AC-09: return identical LeaseNotOwned for nonexistent or unowned lease IDs so
   * there is zero existence oracle. */
  if (auto xit = xsk_leases_.find(lid); xit != xsk_leases_.end()) {
    if (xit->second.state != LeaseState::Allocated ||
        xit->second.session_id != conn.session_id) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned",
                    "lease is not owned by active session");
      return;
    }
    std::string berr;
    (void)netdev_->delete_xsk_map(xit->second.if_name, xit->second.ifindex,
                                  xit->second.queue_id, &berr);
    xsk_leases_.erase(xit);
  } else if (auto fit = flow_leases_.find(lid); fit != flow_leases_.end()) {
    if (fit->second.state != LeaseState::Allocated ||
        fit->second.session_id != conn.session_id) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned",
                    "lease is not owned by active session");
      return;
    }
    std::string berr;
    (void)netdev_->delete_flow(fit->second.if_name, fit->second.ifindex,
                               fit->second.rule_location, &berr);
    flow_leases_.erase(fit);
  } else if (auto uit = udp_filter_leases_.find(lid); uit != udp_filter_leases_.end()) {
    if (uit->second.state != LeaseState::Allocated ||
        uit->second.session_id != conn.session_id) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned",
                    "lease is not owned by active session");
      return;
    }
    std::string berr;
    (void)netdev_->delete_udp_filter(uit->second.if_name, uit->second.ifindex,
                                     uit->second.queue_id, uit->second.dst_port, &berr);
    udp_filter_leases_.erase(uit);
  } else if (auto qit = queue_leases_.find(lid); qit != queue_leases_.end()) {
    if (qit->second.state != LeaseState::Allocated ||
        qit->second.session_id != conn.session_id) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned",
                    "lease is not owned by active session");
      return;
    }
    release_queue_cascade(lid);
  } else if (auto it = leases_.find(lid); it != leases_.end()) {
    if (it->second.state != LeaseState::Allocated ||
        it->second.session_id != conn.session_id) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned",
                    "lease is not owned by active session");
      return;
    }
    if (it->second.pidfd.valid()) {
      del_epoll(it->second.pidfd.get());
      pidfd_to_lease_.erase(it->second.pidfd.get());
    }
    shm_.release_cpu(it->second.cpu_id, it->second.pid);
    leases_.erase(it);
  } else {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned",
                  "lease is not owned by active session");
    return;
  }

  std::string jerr;
  (void)persist_journal(&jerr);
  enqueue_reply(conn, json_object_new_object());
}

void MtlManagerServer::rpc_get_status(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kEmpty{};
  std::string bad;
  if (!check_allowed_keys(params, kEmpty, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }

  audit_shm_integrity();

  const int64_t now_ns = mt_varlink_mono_ns();

  json_object* rep = json_object_new_object();
  json_add_str(rep, "shmMode", shm_.mode_string());
  json_object_object_add(
      rep, "shmTamperWarnings",
      json_object_new_int64(static_cast<int64_t>(shm_tamper_warnings_)));
  json_add_str(rep, "heartbeatPolicy",
               heartbeat_expiry_policy_to_string(cfg_.heartbeat.expiry_policy));
  json_add_str(rep, "heartbeatEvictionAction",
               heartbeat_eviction_action_to_string(cfg_.heartbeat.eviction_action));
  json_object_object_add(
      rep, "heartbeatWarnings",
      json_object_new_int64(static_cast<int64_t>(heartbeat_warnings_)));
  json_object_object_add(
      rep, "heartbeatRevocations",
      json_object_new_int64(static_cast<int64_t>(heartbeat_revocations_)));
  json_object_object_add(
      rep, "managerStallEvents",
      json_object_new_int64(static_cast<int64_t>(manager_stall_events_)));
  json_add_str(rep, "cgroupVerifyMode",
               cgroup_verify_mode_to_string(cfg_.cgroup_verify.mode));

  json_object* sessions_arr = json_object_new_array();
  for (const auto& [cid, c] : connections_) {
    if (c->session_id.empty()) continue;
    if (cfg_.cgroup_verify.mode == CgroupVerifyMode::VerifyOnly) {
      CgroupEffectiveSnapshot live;
      if (cgroup_verifier_.read_peer_effective(c->peer.principal.pid,
                                               c->peer.principal.pidfd, &live, nullptr)) {
        c->cgroup_path = std::move(live.cgroup_path);
        c->effective_cpus = std::move(live.effective_cpus_str);
        c->effective_mems = std::move(live.effective_mems_str);
      } else {
        c->effective_cpus.clear();
        c->effective_mems.clear();
      }
    }
    json_object* so = json_object_new_object();
    json_add_str(so, "sessionId", c->session_id);
    json_add_str(so, "clientName", c->client_name);
    json_object_object_add(so, "pid", json_object_new_int64(c->peer.principal.pid));
    json_object_object_add(so, "uid", json_object_new_int64(c->peer.principal.uid));
    json_add_str(so, "heartbeatState", heartbeat_health_to_string(c->hb.health));
    json_object_object_add(so, "missedBeats", json_object_new_int64(c->hb.missed_beats));
    const int64_t age_ms =
        (now_ns >= c->hb.last_beat_ns) ? ((now_ns - c->hb.last_beat_ns) / 1000000LL) : 0;
    json_object_object_add(so, "lastBeatAgeMs", json_object_new_int64(age_ms));
    json_object_object_add(so, "heartbeatIntervalMs",
                           json_object_new_int64(c->hb.interval_ms));
    json_object_object_add(so, "heartbeatMaxMissed",
                           json_object_new_int64(c->hb.max_missed));
    if (!c->hb.client_state.empty()) {
      json_add_str(so, "clientState", c->hb.client_state);
    }
    json_add_str(so, "cgroupPath", c->cgroup_path);
    json_add_str(so, "effectiveCpus", c->effective_cpus);
    json_add_str(so, "effectiveMems", c->effective_mems);
    json_object_array_add(sessions_arr, so);
  }
  json_object_object_add(rep, "sessions", sessions_arr);

  int64_t qcnt = 0;
  json_object* leases_arr = json_object_new_array();
  for (const auto& [lid, l] : leases_) {
    if (l.state == LeaseState::Quarantined) ++qcnt;
    json_object* lo = json_object_new_object();
    json_add_str(lo, "leaseId", l.lease_id);
    json_object_object_add(lo, "cpuId", json_object_new_int64(l.cpu_id));
    json_add_str(lo, "state", lease_state_to_string(l.state));
    json_add_str(lo, "sessionId", l.session_id);
    json_add_str(lo, "clientName", l.client_name);
    json_object_object_add(lo, "pid", json_object_new_int64(l.pid));
    json_object_object_add(lo, "uid", json_object_new_int64(l.uid));
    json_object_array_add(leases_arr, lo);
  }
  json_object_object_add(rep, "leases", leases_arr);

  json_object* queues_arr = json_object_new_array();
  for (const auto& [lid, q] : queue_leases_) {
    if (q.state == LeaseState::Quarantined) ++qcnt;
    json_object* qo = json_object_new_object();
    json_add_str(qo, "leaseId", q.lease_id);
    json_add_str(qo, "ifName", q.if_name);
    json_object_object_add(qo, "ifindex",
                           json_object_new_int64(static_cast<int64_t>(q.ifindex)));
    json_object_object_add(qo, "queueId",
                           json_object_new_int64(static_cast<int64_t>(q.queue_id)));
    json_add_str(qo, "state", lease_state_to_string(q.state));
    json_add_str(qo, "sessionId", q.session_id);
    json_add_str(qo, "clientName", q.client_name);
    json_object_object_add(qo, "pid", json_object_new_int64(q.pid));
    json_object_object_add(qo, "uid", json_object_new_int64(q.uid));
    json_object_array_add(queues_arr, qo);
  }
  json_object_object_add(rep, "queues", queues_arr);
  json_object_object_add(rep, "quarantinedCount", json_object_new_int64(qcnt));

  json_object* flows_arr = json_object_new_array();
  for (const auto& [lid, f] : flow_leases_) {
    json_object* fo = json_object_new_object();
    json_add_str(fo, "leaseId", f.lease_id);
    json_add_str(fo, "queueLeaseId", f.queue_lease_id);
    json_add_str(fo, "ifName", f.if_name);
    json_object_object_add(fo, "queueId",
                           json_object_new_int64(static_cast<int64_t>(f.queue_id)));
    json_object_object_add(fo, "ruleLocation",
                           json_object_new_int64(static_cast<int64_t>(f.rule_location)));
    json_add_str(fo, "protocol", f.spec.protocol);
    json_add_str(fo, "srcIp",
                 (f.spec.src_ip_be != 0) ? format_ipv4(f.spec.src_ip_be) : std::string());
    json_add_str(fo, "dstIp", format_ipv4(f.spec.dst_ip_be));
    json_object_object_add(fo, "srcPort", json_object_new_int64(f.spec.src_port));
    json_object_object_add(fo, "dstPort", json_object_new_int64(f.spec.dst_port));
    json_add_str(fo, "state", lease_state_to_string(f.state));
    json_add_str(fo, "sessionId", f.session_id);
    json_object_object_add(fo, "pid", json_object_new_int64(f.pid));
    json_object_object_add(fo, "uid", json_object_new_int64(f.uid));
    json_object_array_add(flows_arr, fo);
  }
  json_object_object_add(rep, "flows", flows_arr);

  json_object* udps_arr = json_object_new_array();
  for (const auto& [lid, u] : udp_filter_leases_) {
    json_object* uo = json_object_new_object();
    json_add_str(uo, "leaseId", u.lease_id);
    json_add_str(uo, "queueLeaseId", u.queue_lease_id);
    json_add_str(uo, "ifName", u.if_name);
    json_object_object_add(uo, "queueId",
                           json_object_new_int64(static_cast<int64_t>(u.queue_id)));
    json_object_object_add(uo, "destinationPort", json_object_new_int64(u.dst_port));
    json_add_str(uo, "state", lease_state_to_string(u.state));
    json_add_str(uo, "sessionId", u.session_id);
    json_object_object_add(uo, "pid", json_object_new_int64(u.pid));
    json_object_object_add(uo, "uid", json_object_new_int64(u.uid));
    json_object_array_add(udps_arr, uo);
  }
  json_object_object_add(rep, "udpFilters", udps_arr);

  json_object* xsks_arr = json_object_new_array();
  for (const auto& [lid, x] : xsk_leases_) {
    json_object* xo = json_object_new_object();
    json_add_str(xo, "leaseId", x.lease_id);
    json_add_str(xo, "queueLeaseId", x.queue_lease_id);
    json_add_str(xo, "ifName", x.if_name);
    json_object_object_add(xo, "queueId",
                           json_object_new_int64(static_cast<int64_t>(x.queue_id)));
    json_add_str(xo, "state", lease_state_to_string(x.state));
    json_add_str(xo, "sessionId", x.session_id);
    json_object_object_add(xo, "pid", json_object_new_int64(x.pid));
    json_object_object_add(xo, "uid", json_object_new_int64(x.uid));
    json_object_array_add(xsks_arr, xo);
  }
  json_object_object_add(rep, "xskAttachments", xsks_arr);

  json_object* xdps_arr = json_object_new_array();
  for (const auto& [ifn, xa] : xdp_attachments_) {
    json_object* xo = json_object_new_object();
    json_add_str(xo, "ifName", xa.if_name);
    json_object_object_add(xo, "ifindex",
                           json_object_new_int64(static_cast<int64_t>(xa.ifindex)));
    json_add_str(xo, "mode", xdp_attach_mode_to_string(xa.mode));
    json_object_array_add(xdps_arr, xo);
  }
  json_object_object_add(rep, "xdpAttachments", xdps_arr);

  json_object* backend_arr = json_object_new_array();
  for (const auto& snap : netdev_->snapshot_state()) {
    json_object* bo = json_object_new_object();
    json_add_str(bo, "ifName", snap.if_name);
    json_object_object_add(bo, "ifindex",
                           json_object_new_int64(static_cast<int64_t>(snap.ifindex)));
    json_object_object_add(bo, "maxQueues",
                           json_object_new_int64(static_cast<int64_t>(snap.max_queues)));
    json_add_str(bo, "xdpMode", xdp_attach_mode_to_string(snap.xdp_mode));

    json_object* bx = json_object_new_array();
    for (uint32_t qid : snap.xsk_queues)
      json_object_array_add(bx, json_object_new_int64(static_cast<int64_t>(qid)));
    json_object_object_add(bo, "xskQueues", bx);

    json_object* bu = json_object_new_array();
    for (const auto& [qid, dp] : snap.udp_filters) {
      json_object* uo = json_object_new_object();
      json_object_object_add(uo, "queueId",
                             json_object_new_int64(static_cast<int64_t>(qid)));
      json_object_object_add(uo, "dstPort", json_object_new_int64(dp));
      json_object_array_add(bu, uo);
    }
    json_object_object_add(bo, "udpFilters", bu);

    json_object* bf = json_object_new_array();
    for (const auto& r : snap.flow_rules) {
      json_object* ro = json_object_new_object();
      json_object_object_add(ro, "location",
                             json_object_new_int64(static_cast<int64_t>(r.location)));
      json_object_object_add(ro, "queueId",
                             json_object_new_int64(static_cast<int64_t>(r.queue_id)));
      json_object_object_add(ro, "mtlOwned",
                             json_object_new_boolean(r.mtl_owned ? 1 : 0));
      json_object_object_add(ro, "dstPort", json_object_new_int64(r.spec.dst_port));
      json_object_array_add(bf, ro);
    }
    json_object_object_add(bo, "flowRules", bf);

    json_object_array_add(backend_arr, bo);
  }
  json_object_object_add(rep, "backendInterfaces", backend_arr);

  /* Support up to 4 MiB GetStatus replies without disconnecting caller (ISSUE-31). */
  enqueue_reply(conn, rep, /*allow_large=*/true);
}

void MtlManagerServer::rpc_revoke_lease(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"leaseId"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  json_object* jlid = nullptr;
  std::string lid;
  if (!params || !json_object_object_get_ex(params, "leaseId", &jlid) ||
      !parse_strict_str(jlid, &lid) || lid.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "leaseId must be a non-empty string", "leaseId");
    return;
  }

  if (auto it = leases_.find(lid); it != leases_.end()) {
    CpuLease& l = it->second;
    if (!l.pidfd.valid() || !PeerCredExtractor::is_pidfd_alive(l.pidfd.get())) {
      if (l.pidfd.valid()) {
        del_epoll(l.pidfd.get());
        pidfd_to_lease_.erase(l.pidfd.get());
      }
      shm_.release_cpu(l.cpu_id, l.pid);
      leases_.erase(it);
    } else {
      l.state = LeaseState::Quarantined;
      l.session_id.clear();
    }
  } else if (auto qit = queue_leases_.find(lid); qit != queue_leases_.end()) {
    QueueLease& ql = qit->second;
    if (!ql.pidfd.valid() || !PeerCredExtractor::is_pidfd_alive(ql.pidfd.get())) {
      release_queue_cascade(lid);
    } else {
      strip_queue_children(lid);
      ql.state = LeaseState::Quarantined;
      ql.session_id.clear();
    }
  } else if (auto xit = xsk_leases_.find(lid); xit != xsk_leases_.end()) {
    std::string berr;
    (void)netdev_->delete_xsk_map(xit->second.if_name, xit->second.ifindex,
                                  xit->second.queue_id, &berr);
    xsk_leases_.erase(xit);
  } else if (auto fit = flow_leases_.find(lid); fit != flow_leases_.end()) {
    std::string berr;
    (void)netdev_->delete_flow(fit->second.if_name, fit->second.ifindex,
                               fit->second.rule_location, &berr);
    flow_leases_.erase(fit);
  } else if (auto uit = udp_filter_leases_.find(lid); uit != udp_filter_leases_.end()) {
    std::string berr;
    (void)netdev_->delete_udp_filter(uit->second.if_name, uit->second.ifindex,
                                     uit->second.queue_id, uit->second.dst_port, &berr);
    udp_filter_leases_.erase(uit);
  } else {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotFound",
                  "lease not found: " + lid);
    return;
  }

  std::string jerr;
  (void)persist_journal(&jerr);
  enqueue_reply(conn, json_object_new_object());
}

void MtlManagerServer::rpc_revoke_session(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"sessionId"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  json_object* jsid = nullptr;
  std::string sid;
  if (!params || !json_object_object_get_ex(params, "sessionId", &jsid) ||
      !parse_strict_str(jsid, &sid) || sid.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "sessionId must be a non-empty string", "sessionId");
    return;
  }

  uint64_t target_cid = 0;
  for (const auto& [cid, c] : connections_) {
    if (c && c->session_id == sid) {
      target_cid = cid;
      break;
    }
  }

  std::vector<std::string> matching_cpus;
  for (const auto& [lid, l] : leases_) {
    if (l.session_id == sid) matching_cpus.push_back(lid);
  }
  std::vector<std::string> matching_queues;
  for (const auto& [lid, q] : queue_leases_) {
    if (q.session_id == sid) matching_queues.push_back(lid);
  }
  if (target_cid == 0 && matching_cpus.empty() && matching_queues.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "session not found: " + sid);
    return;
  }

  for (const std::string& qlid : matching_queues) {
    auto qit = queue_leases_.find(qlid);
    if (qit == queue_leases_.end()) continue;
    QueueLease& ql = qit->second;
    if (!ql.pidfd.valid() || !PeerCredExtractor::is_pidfd_alive(ql.pidfd.get())) {
      release_queue_cascade(qlid);
    } else {
      /* Hold queue in QUARANTINED while live, immediately removing XSK/Flow/UdpFilter. */
      strip_queue_children(qlid);
      ql.state = LeaseState::Quarantined;
      ql.session_id.clear();
    }
  }

  for (const std::string& lid : matching_cpus) {
    auto it = leases_.find(lid);
    if (it == leases_.end()) continue;
    CpuLease& l = it->second;
    if (!l.pidfd.valid() || !PeerCredExtractor::is_pidfd_alive(l.pidfd.get())) {
      if (l.pidfd.valid()) {
        del_epoll(l.pidfd.get());
        pidfd_to_lease_.erase(l.pidfd.get());
      }
      shm_.release_cpu(l.cpu_id, l.pid);
      leases_.erase(it);
    } else {
      l.state = LeaseState::Quarantined;
      l.session_id.clear();
    }
  }

  if (target_cid != 0 && target_cid != conn.id) {
    auto cit = connections_.find(target_cid);
    if (cit != connections_.end() && cit->second) {
      cit->second->session_id.clear();
      destroy_conn(target_cid);
    }
  }

  std::string jerr;
  (void)persist_journal(&jerr);
  enqueue_reply(conn, json_object_new_object());
}

void MtlManagerServer::rpc_kill_quarantined_owner(Connection& conn,
                                                  struct json_object* params) {
  static const std::set<std::string> kKeys = {"leaseId"};
  std::string bad;
  if (!check_allowed_keys(params, kKeys, &bad)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "unexpected parameter", bad);
    return;
  }
  json_object* jlid = nullptr;
  std::string lid;
  if (!params || !json_object_object_get_ex(params, "leaseId", &jlid) ||
      !parse_strict_str(jlid, &lid) || lid.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "leaseId must be a non-empty string", "leaseId");
    return;
  }

  int target_pidfd = -1;
  LeaseState target_state = LeaseState::Allocated;
  if (auto it = leases_.find(lid); it != leases_.end()) {
    target_state = it->second.state;
    target_pidfd = it->second.pidfd.get();
  } else if (auto qit = queue_leases_.find(lid); qit != queue_leases_.end()) {
    target_state = qit->second.state;
    target_pidfd = qit->second.pidfd.get();
  } else {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotFound",
                  "lease not found: " + lid);
    return;
  }

  if (target_state != LeaseState::Quarantined) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "lease is not in QUARANTINED state", "leaseId");
    return;
  }

  std::string kerr;
  if (!PeerCredExtractor::send_sigkill_pidfd(target_pidfd, &kerr)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError", kerr);
    return;
  }
  /* Do NOT free CPU/queue yet; epoll pidfd readiness frees it when the process exits. */
  enqueue_reply(conn, json_object_new_object());
}

}  // namespace mtlm2
