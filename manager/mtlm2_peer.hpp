/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef MTLM2_PEER_HPP
#define MTLM2_PEER_HPP

#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mtlm2 {

class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {
  }
  ~UniqueFd() noexcept {
    reset();
  }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.fd_);
      other.fd_ = -1;
    }
    return *this;
  }

  int get() const noexcept {
    return fd_;
  }
  bool valid() const noexcept {
    return fd_ >= 0;
  }

  int release() noexcept {
    int out = fd_;
    fd_ = -1;
    return out;
  }

  void reset(int new_fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = new_fd;
  }

 private:
  int fd_ = -1;
};

bool set_cloexec(int fd);
bool set_nonblock(int fd);

struct PeerPrincipal {
  uid_t uid = 0;
  gid_t gid = 0;
  pid_t pid = 0;
  uint64_t start_time_ticks = 0;
  int pidfd = -1;
  bool pidfd_supported = false;
  std::vector<gid_t> supplementary_gids;
};

struct PeerIdentity {
  PeerPrincipal principal;
  UniqueFd pidfd_owner;
};

class PeerCredExtractor {
 public:
  explicit PeerCredExtractor(std::string proc_root = "/proc");

  bool extract(int sock_fd, PeerIdentity* out, std::string* err_out) const;

  /* Reads unified cgroup v2 path, rejecting '..' traversal and ' (deleted)' (ISSUE-46).
   */
  bool read_cgroup_v2_path(pid_t pid, int pidfd, std::string* out_path,
                           std::string* err_out) const;

  static bool is_pidfd_alive(int pidfd);
  static bool read_proc_start_time(const std::string& proc_root, pid_t pid,
                                   uint64_t* out_ticks);
  static bool open_verified_pidfd(const std::string& proc_root, pid_t pid,
                                  uint64_t expected_start_ticks, UniqueFd* out_pidfd);
  static bool send_sigkill_pidfd(int pidfd, std::string* err_out);
  static std::string sanitize_label(std::string_view raw, size_t max_len = 64);

  const std::string& proc_root() const noexcept {
    return proc_root_;
  }

 private:
  std::string proc_root_;
};

}  // namespace mtlm2

#endif  // MTLM2_PEER_HPP
