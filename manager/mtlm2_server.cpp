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

method OpenSession(clientName: string) -> (sessionId: string)
method ClaimSession(clientName: string, leaseIds: []string) -> (sessionId: string, claimed: []string, missing: []string)
method CloseSession() -> ()
method AcquireCpu(preferred: ?int, numaNode: ?int, allowFallback: ?bool) -> (leaseId: string, cpuId: int)
method ReleaseLease(leaseId: string) -> ()
method GetStatus() -> (shmMode: string, shmTamperWarnings: int, quarantinedCount: int, sessions: []object, leases: []object)
method RevokeLease(leaseId: string) -> ()
method RevokeSession(sessionId: string) -> ()
method KillQuarantinedOwner(leaseId: string) -> ()

error ResourceBusy (message: string)
error AccessDenied (message: string)
error InvalidParameter (parameter: string, message: string)
error LeaseNotFound (message: string)
error LeaseNotOwned (message: string)
error SessionNotFound (message: string)
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

  static const std::set<std::string> kTopKeys = {"allowed_cpus", "allowed_interfaces",
                                                 "allowed_udp_ports", "default_quota",
                                                 "uid_quotas"};
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
    static const std::set<std::string> kQKeys = {"max_cpus", "max_queues", "max_flows"};
    if (!check_allowed_keys(qobj, kQKeys, nullptr)) return false;
    auto read_opt_u32 = [&](const char* k, uint32_t* dst) -> bool {
      json_object* v = nullptr;
      if (!json_object_object_get_ex(qobj, k, &v) || !v) return true;
      int64_t n = 0;
      if (!parse_strict_i64(v, &n) || n < 0 || n > 65535) return false;
      *dst = static_cast<uint32_t>(n);
      return true;
    };
    return read_opt_u32("max_cpus", &qout->max_cpus) &&
           read_opt_u32("max_queues", &qout->max_queues) &&
           read_opt_u32("max_flows", &qout->max_flows);
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

  *out = std::move(cfg);
  return true;
}

bool PolicyConfig::is_cpu_allowed(uint32_t cpu_id) const {
  if (allow_all_single_host) return true;
  return allowed_cpus.find(cpu_id) != allowed_cpus.end();
}

uint32_t PolicyConfig::max_cpus_for_uid(uid_t uid) const {
  if (allow_all_single_host) return 65535u;
  auto it = uid_quotas.find(uid);
  if (it != uid_quotas.end()) return it->second.max_cpus;
  return default_quota.max_cpus;
}

MtlManagerServer::MtlManagerServer(ServerConfig cfg)
    : cfg_(std::move(cfg)), peer_extractor_(cfg_.proc_root), journal_(cfg_.state_dir) {
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
      /* Owner dead or PID reused -> FREE and clean legacy shm slot if owned by dead PID.
       */
      shm_.release_cpu(e.cpu_id, e.pid);
    }
  }
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

    int n = ::epoll_wait(epoll_fd_.get(), events, 64, wait_ms);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
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
      } else if (kind == kTagClientListen) {
        handle_accept(client_listener_);
      } else if (kind == kTagAdminListen) {
        handle_accept(admin_listener_);
      } else if (kind == kTagLeasePidFd) {
        handle_pidfd_event(static_cast<int>(payload));
      } else if (kind == kTagConnPidFd) {
        handle_conn_pidfd_event(static_cast<int>(payload));
      } else if (kind == kTagConn) {
        handle_conn_event(payload, events[i].events);
      }
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

void MtlManagerServer::reap_dead_pidfds() {
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
      /* Normal stop preserves live leases, journal, and tenant-PID shm slots (R4-1). */
      reap_dead_pidfds();
      std::string jerr;
      (void)persist_journal(&jerr);
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

  if (!conn->session_id.empty()) {
    bool changed = false;
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
  if (frame.fd_count > 0u) {
    enqueue_error(conn, "org.varlink.service.InvalidParameter",
                  "unexpected file descriptors", "fds");
    return;
  }

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
  } else if (m == MT_VARLINK_MANAGER_INTERFACE ".AcquireCpu") {
    rpc_acquire_cpu(conn, params);
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

void MtlManagerServer::rpc_open_session(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"clientName"};
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

  conn.session_id = next_id("ses");
  conn.client_name = PeerCredExtractor::sanitize_label(raw_name);
  json_object* rep = json_object_new_object();
  json_add_str(rep, "sessionId", conn.session_id);
  enqueue_reply(conn, rep);
}

void MtlManagerServer::rpc_claim_session(Connection& conn, struct json_object* params) {
  static const std::set<std::string> kKeys = {"clientName", "leaseIds"};
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
  for (size_t i = 0; i < n; ++i) {
    std::string lid;
    if (!parse_strict_str(json_object_array_get_idx(jids, i), &lid) || lid.empty()) {
      enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                    "leaseIds must contain non-empty strings", "leaseIds");
      return;
    }
    if (!seen.insert(lid).second) continue;
    auto it = leases_.find(lid);
    if (it != leases_.end() && it->second.state == LeaseState::Recovering &&
        it->second.pid == conn.peer.principal.pid &&
        it->second.start_time_ticks == conn.peer.principal.start_time_ticks &&
        it->second.uid == conn.peer.principal.uid) {
      claimed_ids.push_back(std::move(lid));
    } else {
      /* SEC-AC-08: non-existent, non-recovering, or another process's lease IDs are
       * reported in 'missing' without touching the owner's lease. */
      missing_ids.push_back(std::move(lid));
    }
  }

  conn.session_id = next_id("ses");
  conn.client_name = PeerCredExtractor::sanitize_label(raw_name);

  json_object* claimed = json_object_new_array();
  for (const std::string& lid : claimed_ids) {
    CpuLease& l = leases_.at(lid);
    l.state = LeaseState::Allocated;
    l.session_id = conn.session_id;
    l.client_name = conn.client_name;
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

uint32_t MtlManagerServer::count_uid_cpu_leases(uid_t uid) const {
  uint32_t cnt = 0;
  for (const auto& [lid, lease] : leases_) {
    if (lease.uid == uid) ++cnt;
  }
  return cnt;
}

bool MtlManagerServer::is_cpu_allocated(uint32_t cpu_id) const {
  for (const auto& [lid, lease] : leases_) {
    if (lease.cpu_id == cpu_id) return true;
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

  auto is_online = [&](uint32_t c) {
    return std::binary_search(online_cpus_.begin(), online_cpus_.end(), c);
  };
  auto matches_numa = [&](uint32_t c) {
    if (!numa_node.has_value()) return true;
    auto it = cpu_numa_node_.find(c);
    return it != cpu_numa_node_.end() && it->second == *numa_node;
  };

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
      if (!cfg_.policy.is_cpu_allowed(c) || !matches_numa(c) || is_cpu_allocated(c) ||
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
  auto it = leases_.find(lid);
  if (it == leases_.end() || it->second.state != LeaseState::Allocated ||
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

  json_object* rep = json_object_new_object();
  json_add_str(rep, "shmMode", shm_.mode_string());
  json_object_object_add(
      rep, "shmTamperWarnings",
      json_object_new_int64(static_cast<int64_t>(shm_tamper_warnings_)));

  json_object* sessions_arr = json_object_new_array();
  for (const auto& [cid, c] : connections_) {
    if (c->session_id.empty()) continue;
    json_object* so = json_object_new_object();
    json_add_str(so, "sessionId", c->session_id);
    json_add_str(so, "clientName", c->client_name);
    json_object_object_add(so, "pid", json_object_new_int64(c->peer.principal.pid));
    json_object_object_add(so, "uid", json_object_new_int64(c->peer.principal.uid));
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
  json_object_object_add(rep, "quarantinedCount", json_object_new_int64(qcnt));
  json_object_object_add(rep, "leases", leases_arr);

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

  auto it = leases_.find(lid);
  if (it == leases_.end()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotFound",
                  "lease not found: " + lid);
    return;
  }

  CpuLease& l = it->second;
  if (!l.pidfd.valid() || !PeerCredExtractor::is_pidfd_alive(l.pidfd.get())) {
    if (l.pidfd.valid()) {
      del_epoll(l.pidfd.get());
      pidfd_to_lease_.erase(l.pidfd.get());
    }
    shm_.release_cpu(l.cpu_id, l.pid);
    leases_.erase(it);
  } else {
    /* Live owner -> QUARANTINED; CPU and shm slot remain held until pidfd exits. */
    l.state = LeaseState::Quarantined;
    l.session_id.clear();
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

  std::vector<std::string> matching_leases;
  for (const auto& [lid, l] : leases_) {
    if (l.session_id == sid) matching_leases.push_back(lid);
  }
  if (target_cid == 0 && matching_leases.empty()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound",
                  "session not found: " + sid);
    return;
  }

  for (const std::string& lid : matching_leases) {
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

  auto it = leases_.find(lid);
  if (it == leases_.end()) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotFound",
                  "lease not found: " + lid);
    return;
  }
  if (it->second.state != LeaseState::Quarantined) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter",
                  "lease is not in QUARANTINED state", "leaseId");
    return;
  }

  std::string kerr;
  if (!PeerCredExtractor::send_sigkill_pidfd(it->second.pidfd.get(), &kerr)) {
    enqueue_error(conn, MT_VARLINK_MANAGER_INTERFACE ".InternalError", kerr);
    return;
  }
  /* Do NOT free CPU yet; epoll pidfd readiness frees it when the process actually exits.
   */
  enqueue_reply(conn, json_object_new_object());
}

}  // namespace mtlm2
