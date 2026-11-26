/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mtlm2_cgroup_verify.hpp"

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <charconv>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

namespace mtlm2 {

namespace {

constexpr size_t kMaxCgroupFileBytes = 64u * 1024u;
constexpr size_t kMaxPatternBytes = 512u;
constexpr uint32_t kMaxCpuId = 65535u;
constexpr uint32_t kMaxMemNodeId = 1024u;

bool is_ws(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool normalize_path_slashes(std::string_view raw, std::string* out) {
  if (!out || raw.empty() || raw.front() != '/') return false;
  std::string norm;
  norm.reserve(raw.size());
  bool prev_slash = false;
  for (char c : raw) {
    if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) == 0x7Fu) {
      return false;
    }
    if (c == '/') {
      if (!prev_slash) norm.push_back('/');
      prev_slash = true;
    } else {
      norm.push_back(c);
      prev_slash = false;
    }
  }
  while (norm.size() > 1 && norm.back() == '/') norm.pop_back();
  *out = std::move(norm);
  return true;
}

bool read_cgroup_reg_file_at(int dir_fd, const char* filename, std::string* out,
                             std::string* err_out) {
  int raw_fd = -1;
  do {
    raw_fd = ::openat(dir_fd, filename, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  } while (raw_fd < 0 && errno == EINTR);

  if (raw_fd < 0) {
    if (err_out)
      *err_out = std::string("failed to open cgroup file '") + filename +
                 "': " + std::strerror(errno);
    return false;
  }
  UniqueFd fd(raw_fd);

  struct stat st{};
  if (::fstat(fd.get(), &st) != 0) {
    if (err_out)
      *err_out = std::string("fstat failed on cgroup file '") + filename +
                 "': " + std::strerror(errno);
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    if (err_out)
      *err_out = std::string("cgroup file '") + filename + "' is not a regular file";
    return false;
  }

  std::string content;
  char buf[4096];
  while (content.size() < kMaxCgroupFileBytes) {
    const size_t rem = kMaxCgroupFileBytes - content.size();
    const size_t chunk = rem < sizeof(buf) ? rem : sizeof(buf);
    ssize_t n = ::read(fd.get(), buf, chunk);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (err_out)
        *err_out = std::string("failed to read cgroup file '") + filename +
                   "': " + std::strerror(errno);
      return false;
    }
    if (n == 0) break;
    content.append(buf, static_cast<size_t>(n));
  }

  if (content.size() == kMaxCgroupFileBytes) {
    char probe = 0;
    ssize_t extra = 0;
    do {
      extra = ::read(fd.get(), &probe, 1);
    } while (extra < 0 && errno == EINTR);
    if (extra != 0) {
      if (err_out)
        *err_out = std::string("cgroup file '") + filename + "' exceeds size limit";
      return false;
    }
  }

  *out = std::move(content);
  return true;
}

bool open_cgroup_dir_readonly(const std::string& mount_path,
                              const std::string& peer_cgroup_path, UniqueFd* out_dir_fd,
                              std::string* err_out) {
  if (mount_path.empty()) {
    if (err_out) *err_out = "cgroup mount path is empty";
    return false;
  }

  int root_raw = -1;
  do {
    root_raw =
        ::open(mount_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  } while (root_raw < 0 && errno == EINTR);

  if (root_raw < 0) {
    if (err_out)
      *err_out =
          "failed to open cgroup mount '" + mount_path + "': " + std::strerror(errno);
    return false;
  }
  UniqueFd cur_fd(root_raw);

  size_t pos = 0;
  while (pos < peer_cgroup_path.size()) {
    while (pos < peer_cgroup_path.size() && peer_cgroup_path[pos] == '/') ++pos;
    if (pos >= peer_cgroup_path.size()) break;
    size_t end = peer_cgroup_path.find('/', pos);
    std::string comp = (end == std::string::npos)
                           ? peer_cgroup_path.substr(pos)
                           : peer_cgroup_path.substr(pos, end - pos);
    if (comp.empty() || comp == "." || comp == ".." ||
        comp.find('\0') != std::string::npos) {
      if (err_out) *err_out = "invalid cgroup path component";
      return false;
    }
    int next_raw = -1;
    do {
      next_raw = ::openat(cur_fd.get(), comp.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (next_raw < 0 && errno == EINTR);
    if (next_raw < 0) {
      if (err_out)
        *err_out = "failed to open cgroup directory component '" + comp +
                   "': " + std::strerror(errno);
      return false;
    }
    cur_fd.reset(next_raw);
    if (end == std::string::npos) break;
    pos = end + 1;
  }

  *out_dir_fd = std::move(cur_fd);
  return true;
}

}  // namespace

const char* cgroup_verify_mode_to_string(CgroupVerifyMode mode) noexcept {
  switch (mode) {
    case CgroupVerifyMode::Disabled:
      return "disabled";
    case CgroupVerifyMode::VerifyOnly:
      return "verify_only";
  }
  return "disabled";
}

bool parse_cgroup_verify_mode(const std::string& text, CgroupVerifyMode* out) {
  if (!out) return false;
  if (text == "disabled") {
    *out = CgroupVerifyMode::Disabled;
    return true;
  }
  if (text == "verify_only") {
    *out = CgroupVerifyMode::VerifyOnly;
    return true;
  }
  return false;
}

bool parse_linux_id_list(std::string_view text, uint32_t max_id,
                         std::set<uint32_t>* out) {
  if (!out) return false;
  out->clear();

  size_t pos = 0;
  while (pos < text.size() && is_ws(text[pos])) ++pos;
  if (pos >= text.size()) return true;

  while (pos < text.size()) {
    while (pos < text.size() && is_ws(text[pos])) ++pos;
    if (pos >= text.size()) break;
    size_t comma = text.find(',', pos);
    std::string_view tok = (comma == std::string_view::npos)
                               ? text.substr(pos)
                               : text.substr(pos, comma - pos);
    while (!tok.empty() && is_ws(tok.back())) tok.remove_suffix(1);
    if (tok.empty()) return false;

    size_t dash = tok.find('-');
    if (dash == std::string_view::npos) {
      uint32_t id = 0;
      auto r = std::from_chars(tok.data(), tok.data() + tok.size(), id, 10);
      if (r.ec != std::errc{} || r.ptr != tok.data() + tok.size() || id > max_id)
        return false;
      out->insert(id);
    } else {
      uint32_t lo = 0, hi = 0;
      auto r1 = std::from_chars(tok.data(), tok.data() + dash, lo, 10);
      auto r2 = std::from_chars(tok.data() + dash + 1, tok.data() + tok.size(), hi, 10);
      if (r1.ec != std::errc{} || r1.ptr != tok.data() + dash || r2.ec != std::errc{} ||
          r2.ptr != tok.data() + tok.size() || lo > hi || hi > max_id)
        return false;
      for (uint32_t id = lo; id <= hi; ++id) out->insert(id);
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
    /* Reject trailing comma (e.g. "2-3,"). */
    size_t peek = pos;
    while (peek < text.size() && is_ws(text[peek])) ++peek;
    if (peek >= text.size()) return false;
  }
  return true;
}

std::string format_linux_id_list(const std::set<uint32_t>& ids) {
  if (ids.empty()) return {};
  std::ostringstream oss;
  bool first = true;
  auto it = ids.begin();
  while (it != ids.end()) {
    uint32_t lo = *it;
    uint32_t hi = lo;
    ++it;
    while (it != ids.end() && *it == hi + 1) {
      hi = *it;
      ++it;
    }
    if (!first) oss << ',';
    first = false;
    if (lo == hi) {
      oss << lo;
    } else {
      oss << lo << '-' << hi;
    }
  }
  return oss.str();
}

bool normalize_and_validate_cgroup_pattern(std::string_view raw,
                                           std::string* normalized_out,
                                           std::string* err_out) {
  if (raw.empty()) {
    if (err_out) *err_out = "cgroup pattern must not be empty";
    return false;
  }
  if (raw.size() > kMaxPatternBytes) {
    if (err_out) *err_out = "cgroup pattern exceeds maximum length";
    return false;
  }
  if (raw.front() != '/') {
    if (err_out) *err_out = "cgroup pattern must start with '/'";
    return false;
  }

  std::string norm;
  if (!normalize_path_slashes(raw, &norm)) {
    if (err_out) *err_out = "cgroup pattern contains invalid control characters";
    return false;
  }

  /* Reject '.' and '..' path components (ISSUE-77). */
  size_t seg_pos = 1;
  while (seg_pos < norm.size()) {
    size_t seg_end = norm.find('/', seg_pos);
    std::string_view comp =
        (seg_end == std::string::npos)
            ? std::string_view(norm).substr(seg_pos)
            : std::string_view(norm).substr(seg_pos, seg_end - seg_pos);
    if (comp == "." || comp == "..") {
      if (err_out) *err_out = "cgroup pattern must not contain '.' or '..' segments";
      return false;
    }
    if (seg_end == std::string::npos) break;
    seg_pos = seg_end + 1;
  }

  /* Validate glob escapes and bracket classes (ISSUE-77). */
  bool in_bracket = false;
  bool bracket_has_content = false;
  for (size_t i = 0; i < norm.size(); ++i) {
    const char c = norm[i];
    if (!in_bracket) {
      if (c == '\\') {
        if (i + 1 >= norm.size()) {
          if (err_out) *err_out = "cgroup pattern has trailing backslash";
          return false;
        }
        ++i;
      } else if (c == '[') {
        in_bracket = true;
        bracket_has_content = false;
        if (i + 1 < norm.size() && (norm[i + 1] == '!' || norm[i + 1] == '^')) {
          ++i;
        }
      }
    } else {
      if (c == '\\') {
        if (i + 1 >= norm.size()) {
          if (err_out)
            *err_out = "cgroup pattern has trailing backslash in bracket class";
          return false;
        }
        ++i;
        bracket_has_content = true;
      } else if (c == ']') {
        if (!bracket_has_content) {
          if (err_out) *err_out = "cgroup pattern has empty bracket class";
          return false;
        }
        in_bracket = false;
      } else {
        bracket_has_content = true;
      }
    }
  }
  if (in_bracket) {
    if (err_out) *err_out = "cgroup pattern has unclosed '[' bracket class";
    return false;
  }

  if (normalized_out) *normalized_out = std::move(norm);
  return true;
}

bool match_cgroup_pattern(std::string_view pattern, std::string_view cgroup_path) {
  std::string norm_pat;
  if (!normalize_and_validate_cgroup_pattern(pattern, &norm_pat, nullptr)) return false;

  std::string norm_path;
  if (!normalize_path_slashes(cgroup_path, &norm_path)) return false;

  // Root guard (ISSUE-77): only exact "/" matches root cgroup "/"; patterns like "/*"
  // require at least one child cgroup beneath root.
  if (norm_pat == "/") return norm_path == "/";
  if (norm_path == "/") return false;

  return ::fnmatch(norm_pat.c_str(), norm_path.c_str(), 0) == 0;
}

CgroupVerifier::CgroupVerifier(CgroupVerifyConfig cfg,
                               const PeerCredExtractor& peer_extractor)
    : cfg_(std::move(cfg)), peer_extractor_(peer_extractor) {
}

bool CgroupVerifier::probe_mount(std::string* warn_out) const {
  if (cfg_.mount_path.empty()) {
    if (warn_out) *warn_out = "cgroup mount path is empty";
    return false;
  }
  int raw_fd = -1;
  do {
    raw_fd =
        ::open(cfg_.mount_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  } while (raw_fd < 0 && errno == EINTR);

  if (raw_fd < 0) {
    if (warn_out)
      *warn_out =
          "cgroup mount '" + cfg_.mount_path + "' unavailable: " + std::strerror(errno);
    return false;
  }
  UniqueFd fd(raw_fd);
  return true;
}

bool CgroupVerifier::read_peer_effective(pid_t pid, int pidfd,
                                         CgroupEffectiveSnapshot* out,
                                         std::string* err_out) const {
  if (!out) {
    if (err_out) *err_out = "null cgroup snapshot output";
    return false;
  }
  if (pidfd >= 0 && !PeerCredExtractor::is_pidfd_alive(pidfd)) {
    if (err_out) *err_out = "peer exited before cgroup verification";
    return false;
  }

  std::string cg_path;
  if (!peer_extractor_.read_cgroup_v2_path(pid, pidfd, &cg_path, err_out)) {
    return false;
  }

  std::string norm_cg_path;
  if (!normalize_path_slashes(cg_path, &norm_cg_path)) {
    if (err_out) *err_out = "invalid peer cgroup v2 path";
    return false;
  }

  UniqueFd cg_dir_fd;
  if (!open_cgroup_dir_readonly(cfg_.mount_path, norm_cg_path, &cg_dir_fd, err_out)) {
    return false;
  }

  std::string raw_cpus;
  if (!read_cgroup_reg_file_at(cg_dir_fd.get(), "cpuset.cpus.effective", &raw_cpus,
                               err_out)) {
    return false;
  }

  std::string raw_mems;
  if (!read_cgroup_reg_file_at(cg_dir_fd.get(), "cpuset.mems.effective", &raw_mems,
                               err_out)) {
    return false;
  }

  /* Post-read pidfd liveness bracket to prevent stale or PID-reused reads. */
  if (pidfd >= 0 && !PeerCredExtractor::is_pidfd_alive(pidfd)) {
    if (err_out) *err_out = "peer exited during cgroup verification";
    return false;
  }

  std::set<uint32_t> eff_cpus;
  if (!parse_linux_id_list(raw_cpus, kMaxCpuId, &eff_cpus)) {
    if (err_out) *err_out = "malformed cpuset.cpus.effective";
    return false;
  }
  if (eff_cpus.empty()) {
    if (err_out) *err_out = "cpuset.cpus.effective is empty";
    return false;
  }

  std::set<uint32_t> eff_mems;
  if (!parse_linux_id_list(raw_mems, kMaxMemNodeId, &eff_mems)) {
    if (err_out) *err_out = "malformed cpuset.mems.effective";
    return false;
  }
  if (eff_mems.empty()) {
    if (err_out) *err_out = "cpuset.mems.effective is empty";
    return false;
  }

  out->cgroup_path = std::move(norm_cg_path);
  out->effective_cpus = std::move(eff_cpus);
  out->effective_mems = std::move(eff_mems);
  out->effective_cpus_str = format_linux_id_list(out->effective_cpus);
  out->effective_mems_str = format_linux_id_list(out->effective_mems);
  return true;
}

bool CgroupVerifier::verify_peer(pid_t pid, int pidfd,
                                 const std::string& expected_pattern,
                                 CgroupEffectiveSnapshot* out,
                                 std::string* err_out) const {
  if (!read_peer_effective(pid, pidfd, out, err_out)) return false;

  if (!expected_pattern.empty() &&
      !match_cgroup_pattern(expected_pattern, out->cgroup_path)) {
    if (err_out)
      *err_out = "peer cgroup '" + out->cgroup_path +
                 "' does not match expected pattern '" + expected_pattern + "'";
    return false;
  }
  return true;
}

}  // namespace mtlm2
