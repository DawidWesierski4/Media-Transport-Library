/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef MTLM2_SHM_HPP
#define MTLM2_SHM_HPP

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mtlm2 {

/* ABI-compatible with struct mt_user_info / mt_lcore_shm_entry in lib/src/mt_main.h. */
struct ShmUserInfo {
  char hostname[64];
  char user[32];
  char comm[64];
  pid_t pid;
};

struct ShmLcoreEntry {
  ShmUserInfo u_info;
  pid_t pid;
  int type;
  bool active;
};

struct ShmLcoreTable {
  int used;
  ShmLcoreEntry lcores_info[1];
};

enum class ShmMode {
  Disabled,
  CaseACoexist,
  CaseBNoWrite,
};

struct ShmConfig {
  bool enabled = true;
  std::string lock_path = "/tmp/kahawai_lcore.lock";
  std::string ftok_path = "/dev/null";
  int ftok_proj_id = 21;
  size_t host_cpus = 0;            /* 0 -> sysconf(_SC_NPROCESSORS_CONF) */
  size_t default_slot_count = 256; /* Used if creating a new shm segment */
  bool fail_on_legacy_shm = false; /* --fail-on-legacy-shm */
};

class LegacyShmBridge {
 public:
  LegacyShmBridge() = default;
  ~LegacyShmBridge();

  LegacyShmBridge(const LegacyShmBridge&) = delete;
  LegacyShmBridge& operator=(const LegacyShmBridge&) = delete;

  bool init(const ShmConfig& cfg, std::string* err_out);
  void shutdown(bool preserve_live_entries);

  /* Case A: checks live foreign owner under flock(LOCK_EX) and writes tenant PID/user. */
  bool try_acquire_cpu(uint32_t cpu_id, pid_t tenant_pid, uid_t tenant_uid,
                       const std::string& client_name, bool* out_busy,
                       std::string* err_out);

  /* Case A: clears slot under flock(LOCK_EX) only if owned by expected_tenant_pid. */
  void release_cpu(uint32_t cpu_id, pid_t expected_tenant_pid);

  /* Case A: returns true if cpu_id is currently held by a live foreign PID in shm. */
  bool is_cpu_busy_foreign(uint32_t cpu_id, pid_t exclude_pid = 0);

  /* Case A: takes one bounded non-blocking flock and snapshots all CPUs held by live
   * foreign PIDs. Returns false if shm lock cannot be acquired cleanly. */
  bool snapshot_foreign_busy(std::set<uint32_t>* out_busy, std::string* err_out);

  /* Case A: non-blocking audit checking whether any authoritative v2 lease had its shm
   * slot cleared or overwritten externally. */
  std::vector<uint32_t> audit_owned_leases(
      const std::vector<std::pair<uint32_t, pid_t>>& owned_leases);

  ShmMode mode() const noexcept {
    return mode_;
  }
  const char* mode_string() const noexcept;
  size_t host_cpus() const noexcept {
    return host_cpus_;
  }
  size_t shm_slot_count() const noexcept {
    return shm_slot_count_;
  }

 private:
  int lock_file(std::string* err_out, bool nonblocking_only = false) const;
  static void unlock_file(int fd);
  static bool is_pid_live(pid_t pid);

  ShmConfig cfg_{};
  ShmMode mode_ = ShmMode::Disabled;
  size_t host_cpus_ = 0;
  size_t shm_slot_count_ = 0;
  int shm_id_ = -1;
  ShmLcoreTable* shm_ptr_ = nullptr;
};

}  // namespace mtlm2

#endif  // MTLM2_SHM_HPP
