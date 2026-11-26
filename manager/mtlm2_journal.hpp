/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef MTLM2_JOURNAL_HPP
#define MTLM2_JOURNAL_HPP

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mtlm2_peer.hpp"

namespace mtlm2 {

struct JournalCpuLease {
  std::string lease_id;
  uint32_t cpu_id = 0;
  pid_t pid = 0;
  uint64_t start_time_ticks = 0;
  uid_t uid = 0;
  std::string client_name;
  bool quarantined = false;
};

struct JournalQueueLease {
  std::string lease_id;
  std::string if_name;
  unsigned int ifindex = 0;
  uint32_t queue_id = 0;
  pid_t pid = 0;
  uint64_t start_time_ticks = 0;
  uid_t uid = 0;
  std::string client_name;
  bool quarantined = false;
};

struct JournalFlowLease {
  std::string lease_id;
  std::string queue_lease_id;
  std::string if_name;
  unsigned int ifindex = 0;
  uint32_t queue_id = 0;
  uint32_t rule_location = 0;
  std::string protocol = "udp4";
  std::string src_ip;
  std::string dst_ip;
  uint16_t src_port = 0;
  uint16_t dst_port = 0;
  pid_t pid = 0;
  uint64_t start_time_ticks = 0;
  uid_t uid = 0;
  std::string client_name;
};

struct JournalUdpFilterLease {
  std::string lease_id;
  std::string queue_lease_id;
  std::string if_name;
  unsigned int ifindex = 0;
  uint32_t queue_id = 0;
  uint16_t dst_port = 0;
  pid_t pid = 0;
  uint64_t start_time_ticks = 0;
  uid_t uid = 0;
  std::string client_name;
};

struct JournalXskAttachment {
  std::string lease_id;
  std::string queue_lease_id;
  std::string if_name;
  unsigned int ifindex = 0;
  uint32_t queue_id = 0;
  pid_t pid = 0;
  uint64_t start_time_ticks = 0;
  uid_t uid = 0;
  std::string client_name;
};

struct JournalXdpAttachment {
  std::string if_name;
  unsigned int ifindex = 0;
  std::string mode; /* "native" or "skb" */
};

struct JournalState {
  static constexpr int kSupportedSchemaVersion = 1;
  int schema_version = kSupportedSchemaVersion;
  std::string manager_id = "mtlm2d";
  std::vector<JournalCpuLease> cpu_leases;
  std::vector<JournalQueueLease> queues;
  std::vector<JournalFlowLease> flows;
  std::vector<JournalUdpFilterLease> udp_filters;
  std::vector<JournalXskAttachment> xsk_attachments;
  std::vector<JournalXdpAttachment> xdp_attachments;
};

class StateJournal {
 public:
  explicit StateJournal(std::string state_dir);

  /* Acquires exclusive lifetime flock on <state_dir>/.mtlm2d.lock (ISSUE-05). */
  bool acquire_state_dir_lock(UniqueFd* out_lock_fd, std::string* err_out) const;

  /* Hardened atomic save via unique O_CREAT|O_EXCL|O_NOFOLLOW|O_NONBLOCK temp + fsync +
   * renameat (ISSUE-06/07/40/67). */
  bool save(const JournalState& state, std::string* err_out) const;

  /* Hardened load verifying S_ISREG, trusted owner, (mode & 0022)==0.
   * Quarantines corrupt regular journal to journal.json.corrupt.<ts> (ISSUE-06/67). */
  bool load(JournalState* out_state, bool* out_not_found, bool* out_quarantined,
            std::string* err_out) const;

  const std::string& state_dir() const noexcept {
    return state_dir_;
  }
  const std::string& journal_path() const noexcept {
    return journal_path_;
  }

 private:
  std::string state_dir_;
  std::string journal_path_;
};

}  // namespace mtlm2

#endif  // MTLM2_JOURNAL_HPP
