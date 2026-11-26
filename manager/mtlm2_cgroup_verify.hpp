/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef MTLM2_CGROUP_VERIFY_HPP
#define MTLM2_CGROUP_VERIFY_HPP

#include <sys/types.h>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "mtlm2_peer.hpp"

namespace mtlm2 {

enum class CgroupVerifyMode {
  Disabled,
  VerifyOnly,
};

const char* cgroup_verify_mode_to_string(CgroupVerifyMode mode) noexcept;
bool parse_cgroup_verify_mode(const std::string& text, CgroupVerifyMode* out);

struct CgroupVerifyConfig {
  CgroupVerifyMode mode = CgroupVerifyMode::Disabled;
  std::string mount_path = "/sys/fs/cgroup";
  std::string expected_cgroup_pattern;
};

struct CgroupEffectiveSnapshot {
  std::string cgroup_path;
  std::string effective_cpus_str;
  std::string effective_mems_str;
  std::set<uint32_t> effective_cpus;
  std::set<uint32_t> effective_mems;
};

/* Parses Linux cpuset ID list syntax (e.g. "0,2-5,8"). Empty/whitespace text produces an
 * empty set and returns true so callers can distinguish syntax errors from empty sets. */
bool parse_linux_id_list(std::string_view text, uint32_t max_id, std::set<uint32_t>* out);

/* Formats a sorted ID set as a compact Linux range string (e.g. "0,2-5"). */
std::string format_linux_id_list(const std::set<uint32_t>& ids);

/* Validates and normalizes a cgroup v2 glob pattern (ISSUE-77): rejects unclosed '[',
 * trailing '\\', '..' / '.' segments, missing leading '/', and collapses '//'. */
bool normalize_and_validate_cgroup_pattern(std::string_view raw,
                                           std::string* normalized_out,
                                           std::string* err_out);

// Matches a peer cgroup v2 path against a validated glob pattern, preventing non-root
// patterns such as "/*" from matching bare "/" (ISSUE-77).
bool match_cgroup_pattern(std::string_view pattern, std::string_view cgroup_path);

/* Strictly read-only cgroup v2 verifier (zero writes to cgroupfs). */
class CgroupVerifier {
 public:
  CgroupVerifier(CgroupVerifyConfig cfg, const PeerCredExtractor& peer_extractor);

  const CgroupVerifyConfig& config() const noexcept {
    return cfg_;
  }
  CgroupVerifyMode mode() const noexcept {
    return cfg_.mode;
  }

  /* Probes mount_path read-only with O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC (ISSUE-76). */
  bool probe_mount(std::string* warn_out) const;

  /* Live read of peer unified cgroup v2 path and cpuset effective sets, bracketed by
   * pidfd liveness checks and non-blocking S_ISREG checks (ISSUE-59, ISSUE-76, ISSUE-86).
   */
  bool read_peer_effective(pid_t pid, int pidfd, CgroupEffectiveSnapshot* out,
                           std::string* err_out) const;

  /* Live read plus optional expected_cgroup_pattern verification (ISSUE-77). */
  bool verify_peer(pid_t pid, int pidfd, const std::string& expected_pattern,
                   CgroupEffectiveSnapshot* out, std::string* err_out) const;

 private:
  CgroupVerifyConfig cfg_;
  const PeerCredExtractor& peer_extractor_;
};

}  // namespace mtlm2

#endif  // MTLM2_CGROUP_VERIFY_HPP
