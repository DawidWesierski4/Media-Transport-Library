/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mtlm2_shm.hpp"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

namespace mtlm2 {

namespace {

void copy_cstr(char* dst, size_t dst_sz, const char* src) {
  if (!dst || dst_sz == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = ::strnlen(src, dst_sz - 1u);
  std::memcpy(dst, src, len);
  dst[len] = '\0';
}

std::string resolve_username(uid_t uid) {
  long buf_sz = ::sysconf(_SC_GETPW_R_SIZE_MAX);
  if (buf_sz < 1024) buf_sz = 1024;
  if (buf_sz > 65536) buf_sz = 65536;
  std::vector<char> buf(static_cast<size_t>(buf_sz));
  struct passwd pwd{};
  struct passwd* res = nullptr;
  if (::getpwuid_r(uid, &pwd, buf.data(), buf.size(), &res) == 0 && res != nullptr &&
      res->pw_name != nullptr) {
    return std::string(res->pw_name);
  }
  return std::to_string(uid);
}

std::string resolve_hostname() {
  char host[64];
  std::memset(host, 0, sizeof(host));
  if (::gethostname(host, sizeof(host) - 1u) == 0 && host[0] != '\0') {
    host[sizeof(host) - 1u] = '\0';
    return std::string(host);
  }
  return "localhost";
}

size_t compute_slot_count(size_t seg_bytes) {
  const size_t hdr = offsetof(ShmLcoreTable, lcores_info);
  if (seg_bytes <= hdr) return 0;
  return (seg_bytes - hdr) / sizeof(ShmLcoreEntry);
}

}  // namespace

LegacyShmBridge::~LegacyShmBridge() {
  shutdown(/*preserve_live_entries=*/true);
}

bool LegacyShmBridge::is_pid_live(pid_t pid) {
  if (pid <= 0) return false;
  if (::kill(pid, 0) == 0) return true;
  return errno == EPERM;
}

const char* LegacyShmBridge::mode_string() const noexcept {
  switch (mode_) {
    case ShmMode::Disabled:
      return "disabled";
    case ShmMode::CaseACoexist:
      return "case_a_coexist";
    case ShmMode::CaseBNoWrite:
      return "case_b_no_write";
  }
  return "disabled";
}

int LegacyShmBridge::lock_file(std::string* err_out, bool nonblocking_only) const {
  /* SEC-FS-01: open with O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, never follow symlinks,
   * verify S_ISREG and trusted owner (root or effective UID). */
  int fd = ::open(cfg_.lock_path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    int err = errno;
    if (err == ELOOP) {
      if (err_out)
        *err_out = "refusing symlink at legacy shm lock '" + cfg_.lock_path + "'";
      return -1;
    }
    if (err == ENOENT) {
      fd =
          ::open(cfg_.lock_path.c_str(),
                 O_RDONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0444);
      if (fd < 0 && errno == EEXIST) {
        fd = ::open(cfg_.lock_path.c_str(),
                    O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      }
      if (fd < 0) {
        int err2 = errno;
        if (err_out) {
          if (err2 == ELOOP)
            *err_out = "refusing symlink at legacy shm lock '" + cfg_.lock_path + "'";
          else
            *err_out = "failed to open legacy shm lock '" + cfg_.lock_path +
                       "': " + std::strerror(err2);
        }
        return -1;
      }
    } else {
      if (err_out)
        *err_out = "failed to open legacy shm lock '" + cfg_.lock_path +
                   "': " + std::strerror(err);
      return -1;
    }
  }

  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    int saved = errno;
    ::close(fd);
    if (err_out)
      *err_out = "fstat failed on legacy shm lock '" + cfg_.lock_path +
                 "': " + std::strerror(saved);
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    if (err_out)
      *err_out = "legacy shm lock '" + cfg_.lock_path + "' is not a regular file";
    return -1;
  }
  if (st.st_uid != 0 && st.st_uid != ::geteuid()) {
    ::close(fd);
    if (err_out)
      *err_out = "legacy shm lock '" + cfg_.lock_path + "' has untrusted owner";
    return -1;
  }

  /* SEC-FS-02: non-blocking flock with bounded short retry (<= 4 ms) so event loop never
   * blocks indefinitely if an external process holds LOCK_EX. */
  constexpr int kMaxFlockRetries = 4;
  int max_attempts = nonblocking_only ? 1 : kMaxFlockRetries;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    int rc = ::flock(fd, LOCK_EX | LOCK_NB);
    if (rc == 0) return fd;
    int saved = errno;
    if (saved == EINTR) continue;
    if (saved == EWOULDBLOCK || saved == EAGAIN) {
      if (attempt + 1 < max_attempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      ::close(fd);
      if (err_out)
        *err_out = "legacy shm lock '" + cfg_.lock_path + "' is busy (flock contended)";
      return -1;
    }
    ::close(fd);
    if (err_out)
      *err_out = "failed to flock legacy shm lock '" + cfg_.lock_path +
                 "': " + std::strerror(saved);
    return -1;
  }
  ::close(fd);
  if (err_out)
    *err_out = "legacy shm lock '" + cfg_.lock_path + "' is busy (flock contended)";
  return -1;
}

void LegacyShmBridge::unlock_file(int fd) {
  if (fd >= 0) {
    (void)::flock(fd, LOCK_UN);
    ::close(fd);
  }
}

bool LegacyShmBridge::init(const ShmConfig& cfg, std::string* err_out) {
  shutdown(/*preserve_live_entries=*/true);
  cfg_ = cfg;

  if (!cfg_.enabled) {
    mode_ = ShmMode::Disabled;
    return true;
  }

  if (cfg_.host_cpus > 0) {
    host_cpus_ = cfg_.host_cpus;
  } else {
    long n = ::sysconf(_SC_NPROCESSORS_CONF);
    host_cpus_ = (n > 0) ? static_cast<size_t>(n) : 1u;
  }

  key_t key = ::ftok(cfg_.ftok_path.c_str(), cfg_.ftok_proj_id);
  if (key == static_cast<key_t>(-1)) {
    if (err_out)
      *err_out = "ftok failed for '" + cfg_.ftok_path + "': " + std::strerror(errno);
    return false;
  }

  int lock_fd = lock_file(err_out);
  if (lock_fd < 0) return false;

  int existing_id = ::shmget(key, 0, 0);
  if (existing_id >= 0) {
    struct shmid_ds ds{};
    if (::shmctl(existing_id, IPC_STAT, &ds) != 0) {
      int saved = errno;
      unlock_file(lock_fd);
      if (err_out)
        *err_out =
            "shmctl(IPC_STAT) failed on legacy shm: " + std::string(std::strerror(saved));
      return false;
    }

    shm_slot_count_ = compute_slot_count(ds.shm_segsz);
    if (host_cpus_ > shm_slot_count_) {
      /* Case B: host_cpus > shm_slot_count -> shm writes disabled; refuse if any live
       * foreign PID. */
      void* ro = ::shmat(existing_id, nullptr, SHM_RDONLY);
      if (ro == reinterpret_cast<void*>(-1)) {
        int saved = errno;
        unlock_file(lock_fd);
        if (err_out)
          *err_out = "shmat(SHM_RDONLY) failed on legacy shm: " +
                     std::string(std::strerror(saved));
        return false;
      }
      const auto* tbl = static_cast<const ShmLcoreTable*>(ro);
      std::vector<pid_t> conflicting_pids;
      for (size_t i = 0; i < shm_slot_count_; ++i) {
        const auto& ent = tbl->lcores_info[i];
        if (ent.active && is_pid_live(ent.pid)) conflicting_pids.push_back(ent.pid);
      }
      ::shmdt(ro);
      unlock_file(lock_fd);

      if (!conflicting_pids.empty()) {
        if (err_out) {
          std::ostringstream oss;
          oss << "refusing startup: host_cpus (" << host_cpus_ << ") > legacy shm slots ("
              << shm_slot_count_
              << ") and live legacy shm MTL processes are active (pids:";
          for (pid_t p : conflicting_pids) oss << " " << p;
          oss << "); stop legacy shm processes or rebuild DPDK with DPDK_MAX_LCORES="
              << host_cpus_;
          *err_out = oss.str();
        }
        return false;
      }

      mode_ = ShmMode::CaseBNoWrite;
      return true;
    }

    /* Case A: host_cpus <= shm_slot_count -> cooperative membership is default. */
    void* rw = ::shmat(existing_id, nullptr, 0);
    if (rw == reinterpret_cast<void*>(-1)) {
      int saved = errno;
      unlock_file(lock_fd);
      if (err_out)
        *err_out = "shmat failed on legacy shm: " + std::string(std::strerror(saved));
      return false;
    }
    auto* tbl = static_cast<ShmLcoreTable*>(rw);
    if (cfg_.fail_on_legacy_shm) {
      std::vector<pid_t> live_pids;
      for (size_t i = 0; i < shm_slot_count_; ++i) {
        if (tbl->lcores_info[i].active && is_pid_live(tbl->lcores_info[i].pid))
          live_pids.push_back(tbl->lcores_info[i].pid);
      }
      if (!live_pids.empty()) {
        ::shmdt(rw);
        unlock_file(lock_fd);
        if (err_out) {
          std::ostringstream oss;
          oss << "--fail-on-legacy-shm set and live legacy shm processes are active "
                 "(pids:";
          for (pid_t p : live_pids) oss << " " << p;
          oss << ")";
          *err_out = oss.str();
        }
        return false;
      }
    }

    shm_id_ = existing_id;
    shm_ptr_ = tbl;
    mode_ = ShmMode::CaseACoexist;
    unlock_file(lock_fd);
    return true;
  }

  if (errno != ENOENT) {
    int saved = errno;
    unlock_file(lock_fd);
    if (err_out) *err_out = "shmget probe failed: " + std::string(std::strerror(saved));
    return false;
  }

  /* No existing shm segment. */
  shm_slot_count_ = cfg_.default_slot_count;
  if (host_cpus_ > shm_slot_count_) {
    mode_ = ShmMode::CaseBNoWrite;
    unlock_file(lock_fd);
    return true;
  }

  const size_t seg_size =
      offsetof(ShmLcoreTable, lcores_info) + shm_slot_count_ * sizeof(ShmLcoreEntry);
  int new_id = ::shmget(key, seg_size, 0666 | IPC_CREAT);
  if (new_id < 0) {
    int saved = errno;
    unlock_file(lock_fd);
    if (err_out)
      *err_out = "shmget(IPC_CREAT) failed: " + std::string(std::strerror(saved));
    return false;
  }
  void* rw = ::shmat(new_id, nullptr, 0);
  if (rw == reinterpret_cast<void*>(-1)) {
    int saved = errno;
    unlock_file(lock_fd);
    if (err_out)
      *err_out =
          "shmat failed on newly created shm: " + std::string(std::strerror(saved));
    return false;
  }
  struct shmid_ds ds{};
  if (::shmctl(new_id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 1) {
    std::memset(rw, 0, seg_size);
  }
  shm_id_ = new_id;
  shm_ptr_ = static_cast<ShmLcoreTable*>(rw);
  mode_ = ShmMode::CaseACoexist;
  unlock_file(lock_fd);
  return true;
}

void LegacyShmBridge::shutdown(bool preserve_live_entries) {
  if (!shm_ptr_ && shm_id_ < 0) {
    mode_ = ShmMode::Disabled;
    return;
  }
  int lock_fd = cfg_.enabled ? lock_file(nullptr) : -1;
  if (shm_ptr_) {
    (void)::shmdt(shm_ptr_);
    shm_ptr_ = nullptr;
  }
  if (shm_id_ >= 0) {
    if (!preserve_live_entries) {
      struct shmid_ds ds{};
      if (::shmctl(shm_id_, IPC_STAT, &ds) == 0 && ds.shm_nattch == 0) {
        (void)::shmctl(shm_id_, IPC_RMID, nullptr);
      }
    }
    shm_id_ = -1;
  }
  unlock_file(lock_fd);
  mode_ = ShmMode::Disabled;
}

bool LegacyShmBridge::is_cpu_busy_foreign(uint32_t cpu_id, pid_t exclude_pid) {
  if (mode_ != ShmMode::CaseACoexist || !shm_ptr_ || cpu_id >= shm_slot_count_)
    return false;
  int lock_fd = lock_file(nullptr);
  if (lock_fd < 0) return true; /* Fail-closed if lock fails. */
  bool busy = false;
  const auto& ent = shm_ptr_->lcores_info[cpu_id];
  if (ent.active && ent.pid != exclude_pid && is_pid_live(ent.pid)) busy = true;
  unlock_file(lock_fd);
  return busy;
}

bool LegacyShmBridge::snapshot_foreign_busy(std::set<uint32_t>* out_busy,
                                            std::string* err_out) {
  if (out_busy) out_busy->clear();
  if (mode_ != ShmMode::CaseACoexist || !shm_ptr_) return true;
  int lock_fd = lock_file(err_out);
  if (lock_fd < 0) return false;
  if (out_busy) {
    for (size_t i = 0; i < shm_slot_count_; ++i) {
      const auto& ent = shm_ptr_->lcores_info[i];
      if (ent.active && is_pid_live(ent.pid)) out_busy->insert(static_cast<uint32_t>(i));
    }
  }
  unlock_file(lock_fd);
  return true;
}

std::vector<uint32_t> LegacyShmBridge::audit_owned_leases(
    const std::vector<std::pair<uint32_t, pid_t>>& owned_leases) {
  std::vector<uint32_t> tampered;
  if (mode_ != ShmMode::CaseACoexist || !shm_ptr_ || owned_leases.empty())
    return tampered;
  int lock_fd = lock_file(nullptr, /*nonblocking_only=*/true);
  if (lock_fd < 0) return tampered;
  for (const auto& [cpu_id, expected_pid] : owned_leases) {
    if (cpu_id >= shm_slot_count_) continue;
    const auto& ent = shm_ptr_->lcores_info[cpu_id];
    if (!ent.active || ent.pid != expected_pid) {
      tampered.push_back(cpu_id);
    }
  }
  unlock_file(lock_fd);
  return tampered;
}

bool LegacyShmBridge::try_acquire_cpu(uint32_t cpu_id, pid_t tenant_pid, uid_t tenant_uid,
                                      const std::string& client_name, bool* out_busy,
                                      std::string* err_out) {
  if (out_busy) *out_busy = false;
  if (mode_ != ShmMode::CaseACoexist || !shm_ptr_) return true;
  if (cpu_id >= shm_slot_count_) {
    if (err_out) *err_out = "cpu id exceeds legacy shm slot count";
    return false;
  }

  int lock_fd = lock_file(err_out);
  if (lock_fd < 0) return false;

  auto& ent = shm_ptr_->lcores_info[cpu_id];
  if (ent.active) {
    if (is_pid_live(ent.pid)) {
      if (out_busy) *out_busy = true;
      if (err_out)
        *err_out = "cpu " + std::to_string(cpu_id) + " is active in legacy shm for pid " +
                   std::to_string(ent.pid);
      unlock_file(lock_fd);
      return false;
    }
    /* Dead owner in legacy shm: reclaim stale slot. */
    ent.active = false;
    if (shm_ptr_->used > 0) shm_ptr_->used--;
  }

  std::memset(&ent, 0, sizeof(ent));
  const std::string user = resolve_username(tenant_uid);
  const std::string host = resolve_hostname();
  copy_cstr(ent.u_info.hostname, sizeof(ent.u_info.hostname), host.c_str());
  copy_cstr(ent.u_info.user, sizeof(ent.u_info.user), user.c_str());
  copy_cstr(ent.u_info.comm, sizeof(ent.u_info.comm), client_name.c_str());
  ent.u_info.pid = tenant_pid;
  ent.pid = tenant_pid;
  ent.type = 0;
  ent.active = true;
  shm_ptr_->used++;

  unlock_file(lock_fd);
  return true;
}

void LegacyShmBridge::release_cpu(uint32_t cpu_id, pid_t expected_tenant_pid) {
  if (mode_ != ShmMode::CaseACoexist || !shm_ptr_ || cpu_id >= shm_slot_count_) return;
  int lock_fd = lock_file(nullptr);
  if (lock_fd < 0) return;

  auto& ent = shm_ptr_->lcores_info[cpu_id];
  if (ent.active && ent.pid == expected_tenant_pid) {
    ent.active = false;
    if (shm_ptr_->used > 0) shm_ptr_->used--;
  }
  unlock_file(lock_fd);
}

}  // namespace mtlm2
