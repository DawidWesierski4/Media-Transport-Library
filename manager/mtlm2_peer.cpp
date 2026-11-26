/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mtlm2_peer.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

#ifndef PIDFD_NONBLOCK
#define PIDFD_NONBLOCK O_NONBLOCK
#endif

namespace mtlm2 {

namespace {

constexpr size_t kMaxProcFileBytes = 1024u * 1024u;
constexpr size_t kMaxSupplementaryGroups = 65536u;

std::string join_proc_path(const std::string& root, const std::string& suffix) {
  if (root.empty()) return "/" + suffix;
  if (root.back() == '/') return root + suffix;
  return root + "/" + suffix;
}

bool read_proc_file(const std::string& path, std::string* out, std::string* err_out) {
  int raw_fd = -1;
  do {
    raw_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  } while (raw_fd < 0 && errno == EINTR);

  if (raw_fd < 0) {
    if (err_out) *err_out = "failed to open " + path + ": " + std::strerror(errno);
    return false;
  }

  UniqueFd fd(raw_fd);
  std::string content;
  char buf[4096];

  while (content.size() < kMaxProcFileBytes) {
    const size_t rem = kMaxProcFileBytes - content.size();
    const size_t chunk = rem < sizeof(buf) ? rem : sizeof(buf);
    ssize_t n = ::read(fd.get(), buf, chunk);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (err_out) *err_out = "failed to read " + path;
      return false;
    }
    if (n == 0) break;
    content.append(buf, static_cast<size_t>(n));
  }

  if (content.size() == kMaxProcFileBytes) {
    char probe = 0;
    ssize_t extra = 0;
    do {
      extra = ::read(fd.get(), &probe, 1);
    } while (extra < 0 && errno == EINTR);
    if (extra != 0) {
      if (err_out) *err_out = "proc file exceeds size limit: " + path;
      return false;
    }
  }

  *out = std::move(content);
  return true;
}

bool parse_u64_token(std::string_view tok, uint64_t* out) {
  if (tok.empty()) return false;
  uint64_t val = 0;
  auto res = std::from_chars(tok.data(), tok.data() + tok.size(), val, 10);
  if (res.ec != std::errc{} || res.ptr != tok.data() + tok.size()) return false;
  *out = val;
  return true;
}

std::string_view trim_ws(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> split_ws(std::string_view line) {
  std::vector<std::string_view> toks;
  size_t pos = 0;
  while (pos < line.size()) {
    while (pos < line.size() &&
           (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r'))
      ++pos;
    if (pos >= line.size()) break;
    size_t start = pos;
    while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t' &&
           line[pos] != '\r')
      ++pos;
    toks.push_back(line.substr(start, pos - start));
  }
  return toks;
}

struct ProcStatusInfo {
  std::optional<pid_t> tgid;
  std::optional<uid_t> ruid;
  std::optional<uid_t> euid;
  bool has_groups_line = false;
  std::vector<gid_t> supplementary_gids;
};

bool parse_proc_status(std::string_view text, ProcStatusInfo* out) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string_view line =
        (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
    pos = (nl == std::string_view::npos) ? text.size() : (nl + 1);

    if (line.rfind("Tgid:", 0) == 0) {
      uint64_t parsed = 0;
      if (!parse_u64_token(trim_ws(line.substr(5)), &parsed) ||
          parsed > static_cast<uint64_t>(std::numeric_limits<pid_t>::max()))
        return false;
      out->tgid = static_cast<pid_t>(parsed);
    } else if (line.rfind("Uid:", 0) == 0) {
      auto toks = split_ws(line.substr(4));
      if (toks.size() < 2) return false;
      uint64_t r = 0, e = 0;
      if (!parse_u64_token(toks[0], &r) || !parse_u64_token(toks[1], &e) ||
          r > static_cast<uint64_t>(std::numeric_limits<uid_t>::max()) ||
          e > static_cast<uint64_t>(std::numeric_limits<uid_t>::max()))
        return false;
      out->ruid = static_cast<uid_t>(r);
      out->euid = static_cast<uid_t>(e);
    } else if (line.rfind("Groups:", 0) == 0) {
      out->has_groups_line = true;
      auto toks = split_ws(line.substr(7));
      if (toks.size() > kMaxSupplementaryGroups) return false;
      out->supplementary_gids.clear();
      out->supplementary_gids.reserve(toks.size());
      for (std::string_view tok : toks) {
        uint64_t g = 0;
        if (!parse_u64_token(tok, &g) ||
            g > static_cast<uint64_t>(std::numeric_limits<gid_t>::max()))
          return false;
        out->supplementary_gids.push_back(static_cast<gid_t>(g));
      }
    }
  }
  return true;
}

bool parse_proc_stat_start_time(std::string_view text, uint64_t* out_ticks) {
  /* /proc/<pid>/stat format: pid (comm) state ppid ... field22=starttime.
   * Scan past the LAST ')' to handle spaces or parentheses in comm safely. */
  size_t rparen = text.rfind(')');
  if (rparen == std::string_view::npos || rparen + 2 >= text.size()) return false;
  auto toks = split_ws(text.substr(rparen + 1));
  /* toks[0] is field 3 (state); field 22 (starttime) is index 19. */
  if (toks.size() <= 19) return false;
  return parse_u64_token(toks[19], out_ticks);
}

bool parse_unified_cgroup_v2(std::string_view text, std::string* out_path,
                             std::string* err_out) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string_view line =
        (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
    pos = (nl == std::string_view::npos) ? text.size() : (nl + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.rfind("0::", 0) == 0) {
      std::string_view path = line.substr(3);
      if (path.empty() || path.front() != '/' ||
          path.find('\0') != std::string_view::npos) {
        if (err_out) *err_out = "invalid unified cgroup v2 path";
        return false;
      }
      constexpr std::string_view kDeletedSuffix = " (deleted)";
      if (path.size() >= kDeletedSuffix.size() &&
          path.substr(path.size() - kDeletedSuffix.size()) == kDeletedSuffix) {
        if (err_out) *err_out = "peer cgroup path is deleted";
        return false;
      }
      size_t seg_pos = 0;
      while (seg_pos < path.size()) {
        while (seg_pos < path.size() && path[seg_pos] == '/') ++seg_pos;
        if (seg_pos >= path.size()) break;
        size_t seg_end = path.find('/', seg_pos);
        std::string_view comp = (seg_end == std::string_view::npos)
                                    ? path.substr(seg_pos)
                                    : path.substr(seg_pos, seg_end - seg_pos);
        if (comp == "..") {
          if (err_out) *err_out = "invalid cgroup v2 path traversal component";
          return false;
        }
        if (seg_end == std::string_view::npos) break;
        seg_pos = seg_end + 1;
      }
      *out_path = std::string(path);
      return true;
    }
  }
  if (err_out) *err_out = "unified cgroup v2 entry not found";
  return false;
}

}  // namespace

bool set_cloexec(int fd) {
  int flags = ::fcntl(fd, F_GETFD);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool set_nonblock(int fd) {
  int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

PeerCredExtractor::PeerCredExtractor(std::string proc_root)
    : proc_root_(std::move(proc_root)) {
}

bool PeerCredExtractor::is_pidfd_alive(int pidfd) {
  if (pidfd < 0) return false;
  struct pollfd pfd{};
  pfd.fd = pidfd;
  pfd.events = POLLIN;
  int rc = 0;
  do {
    rc = ::poll(&pfd, 1, 0);
  } while (rc < 0 && errno == EINTR);
  return rc == 0;
}

bool PeerCredExtractor::read_proc_start_time(const std::string& proc_root, pid_t pid,
                                             uint64_t* out_ticks) {
  if (pid <= 0 || !out_ticks) return false;
  std::string raw;
  if (!read_proc_file(join_proc_path(proc_root, std::to_string(pid) + "/stat"), &raw,
                      nullptr))
    return false;
  return parse_proc_stat_start_time(raw, out_ticks);
}

bool PeerCredExtractor::open_verified_pidfd(const std::string& proc_root, pid_t pid,
                                            uint64_t expected_start_ticks,
                                            UniqueFd* out_pidfd) {
  if (pid <= 0 || !out_pidfd) return false;
  out_pidfd->reset();
#ifdef SYS_pidfd_open
  long raw_fd = ::syscall(SYS_pidfd_open, pid, static_cast<unsigned int>(PIDFD_NONBLOCK));
  if (raw_fd < 0) return false;
  UniqueFd fd(static_cast<int>(raw_fd));
  (void)set_cloexec(fd.get());
  (void)set_nonblock(fd.get());

  if (!is_pidfd_alive(fd.get())) return false;
  uint64_t actual_ticks = 0;
  if (!read_proc_start_time(proc_root, pid, &actual_ticks)) return false;
  if (actual_ticks != expected_start_ticks) return false;
  if (!is_pidfd_alive(fd.get())) return false;

  *out_pidfd = std::move(fd);
  return true;
#else
  (void)proc_root;
  (void)expected_start_ticks;
  return false;
#endif
}

bool PeerCredExtractor::send_sigkill_pidfd(int pidfd, std::string* err_out) {
  if (pidfd < 0) {
    if (err_out) *err_out = "invalid pidfd";
    return false;
  }
#ifdef SYS_pidfd_send_signal
  if (::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0u) != 0) {
    if (errno == ESRCH) return true;
    if (err_out)
      *err_out = std::string("pidfd_send_signal failed: ") + std::strerror(errno);
    return false;
  }
  return true;
#else
  if (err_out) *err_out = "SYS_pidfd_send_signal unsupported";
  return false;
#endif
}

std::string PeerCredExtractor::sanitize_label(std::string_view raw, size_t max_len) {
  std::string out;
  out.reserve(raw.size() < max_len ? raw.size() : max_len);
  for (unsigned char ch : raw) {
    if (ch >= 0x20u && ch <= 0x7Eu) {
      out.push_back(static_cast<char>(ch));
      if (out.size() >= max_len) break;
    }
  }
  return out;
}

bool PeerCredExtractor::read_cgroup_v2_path(pid_t pid, int pidfd, std::string* out_path,
                                            std::string* err_out) const {
  if (pid <= 0 || !out_path) {
    if (err_out) *err_out = "invalid peer pid";
    return false;
  }
  if (pidfd >= 0 && !is_pidfd_alive(pidfd)) {
    if (err_out) *err_out = "peer exited before cgroup read";
    return false;
  }
  std::string raw;
  if (!read_proc_file(join_proc_path(proc_root_, std::to_string(pid) + "/cgroup"), &raw,
                      err_out))
    return false;
  if (pidfd >= 0 && !is_pidfd_alive(pidfd)) {
    if (err_out) *err_out = "peer exited during cgroup read";
    return false;
  }
  return parse_unified_cgroup_v2(raw, out_path, err_out);
}

bool PeerCredExtractor::extract(int sock_fd, PeerIdentity* out,
                                std::string* err_out) const {
  if (sock_fd < 0 || !out) {
    if (err_out) *err_out = "invalid peer socket descriptor";
    return false;
  }

  struct ucred cred{};
  socklen_t cred_len = sizeof(cred);
  if (::getsockopt(sock_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
      cred_len != sizeof(cred)) {
    if (err_out) *err_out = "SO_PEERCRED extraction failed";
    return false;
  }
  if (cred.pid <= 0) {
    if (err_out) *err_out = "peer pid is not valid in host namespace";
    return false;
  }

  UniqueFd pidfd_owner;
  bool pidfd_supported = false;
  bool used_fallback = false;

  int raw_pidfd = -1;
  socklen_t pidfd_len = sizeof(raw_pidfd);
  if (::getsockopt(sock_fd, SOL_SOCKET, SO_PEERPIDFD, &raw_pidfd, &pidfd_len) == 0 &&
      pidfd_len == sizeof(raw_pidfd) && raw_pidfd >= 0) {
    pidfd_owner.reset(raw_pidfd);
    (void)set_cloexec(pidfd_owner.get());
    (void)set_nonblock(pidfd_owner.get());
    pidfd_supported = true;
  } else {
    const int saved = errno;
    if (saved == ENOPROTOOPT || saved == EOPNOTSUPP) {
#ifdef SYS_pidfd_open
      long fb =
          ::syscall(SYS_pidfd_open, cred.pid, static_cast<unsigned int>(PIDFD_NONBLOCK));
      if (fb >= 0) {
        pidfd_owner.reset(static_cast<int>(fb));
        (void)set_cloexec(pidfd_owner.get());
        (void)set_nonblock(pidfd_owner.get());
        pidfd_supported = true;
        used_fallback = true;
      } else if (errno == ESRCH) {
        if (err_out) *err_out = "peer process exited before pidfd_open";
        return false;
      } else if (errno == ENOSYS) {
        pidfd_supported = false;
      } else {
        if (err_out) *err_out = "pidfd_open failed for peer";
        return false;
      }
#else
      pidfd_supported = false;
#endif
    } else if (saved == ESRCH) {
      if (err_out) *err_out = "peer process already exited";
      return false;
    } else {
      if (err_out) *err_out = "SO_PEERPIDFD extraction failed";
      return false;
    }
  }

  if (pidfd_supported && !is_pidfd_alive(pidfd_owner.get())) {
    if (err_out) *err_out = "peer process exited";
    return false;
  }

  std::string status_raw;
  if (!read_proc_file(join_proc_path(proc_root_, std::to_string(cred.pid) + "/status"),
                      &status_raw, err_out))
    return false;

  ProcStatusInfo status_info;
  if (!parse_proc_status(status_raw, &status_info)) {
    if (err_out) *err_out = "failed to parse peer proc status";
    return false;
  }
  if (!status_info.has_groups_line) {
    if (err_out) *err_out = "peer proc status missing Groups line";
    return false;
  }

  /* Anti-PID-reuse fallback checks (ISSUE-46). */
  if (used_fallback || !pidfd_supported) {
    if (!status_info.tgid.has_value() || *status_info.tgid != cred.pid) {
      if (err_out) *err_out = "peer Tgid mismatch during pidfd fallback";
      return false;
    }
    const bool uid_matches =
        (status_info.euid.has_value() && *status_info.euid == cred.uid) ||
        (status_info.ruid.has_value() && *status_info.ruid == cred.uid);
    if (!uid_matches) {
      if (err_out) *err_out = "peer Uid mismatch during pidfd fallback";
      return false;
    }
    if (pidfd_supported && !is_pidfd_alive(pidfd_owner.get())) {
      if (err_out) *err_out = "peer exited during pidfd fallback";
      return false;
    }
    struct ucred cred2{};
    socklen_t cred2_len = sizeof(cred2);
    if (::getsockopt(sock_fd, SOL_SOCKET, SO_PEERCRED, &cred2, &cred2_len) != 0 ||
        cred2_len != sizeof(cred2) || cred2.pid != cred.pid || cred2.uid != cred.uid ||
        cred2.gid != cred.gid) {
      if (err_out) *err_out = "second SO_PEERCRED verification failed";
      return false;
    }
  }

  uint64_t start_ticks = 0;
  if (!read_proc_start_time(proc_root_, cred.pid, &start_ticks)) {
    if (err_out) *err_out = "failed to read peer proc stat start time";
    return false;
  }

  if (pidfd_supported) {
    if (!is_pidfd_alive(pidfd_owner.get())) {
      if (err_out) *err_out = "peer exited during identity extraction";
      return false;
    }
  } else {
    struct ucred cred3{};
    socklen_t cred3_len = sizeof(cred3);
    if (::getsockopt(sock_fd, SOL_SOCKET, SO_PEERCRED, &cred3, &cred3_len) != 0 ||
        cred3_len != sizeof(cred3) || cred3.pid != cred.pid || cred3.uid != cred.uid ||
        cred3.gid != cred.gid) {
      if (err_out) *err_out = "post-stat SO_PEERCRED verification failed";
      return false;
    }
  }

  out->principal.uid = cred.uid;
  out->principal.gid = cred.gid;
  out->principal.pid = cred.pid;
  out->principal.start_time_ticks = start_ticks;
  out->principal.pidfd_supported = pidfd_supported;
  out->principal.supplementary_gids = std::move(status_info.supplementary_gids);
  out->pidfd_owner = std::move(pidfd_owner);
  out->principal.pidfd = out->pidfd_owner.get();
  return true;
}

}  // namespace mtlm2
