/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef MTLM2_HEARTBEAT_HPP
#define MTLM2_HEARTBEAT_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mtlm2_peer.hpp"

namespace mtlm2 {

enum class HeartbeatExpiryPolicy {
  WarnOnly,
  Revoke,
};

enum class HeartbeatEvictionAction {
  None,
  Revoke,
};

enum class HeartbeatHealth {
  Healthy,
  Degraded,
  Warned,
};

const char* heartbeat_expiry_policy_to_string(HeartbeatExpiryPolicy policy) noexcept;
bool parse_heartbeat_expiry_policy(const std::string& text,
                                   HeartbeatExpiryPolicy* out) noexcept;

const char* heartbeat_eviction_action_to_string(HeartbeatEvictionAction action) noexcept;
bool parse_heartbeat_eviction_action(const std::string& text,
                                     HeartbeatEvictionAction* out) noexcept;

const char* heartbeat_health_to_string(HeartbeatHealth health) noexcept;

struct HeartbeatConfig {
  /* Safe defaults per Review Finding 3 & R2-5. */
  HeartbeatExpiryPolicy expiry_policy = HeartbeatExpiryPolicy::WarnOnly;
  HeartbeatEvictionAction eviction_action = HeartbeatEvictionAction::None;
  int64_t default_interval_ms = 5000;
  int64_t default_missed_beats = 6;
  int64_t min_timeout_ms = 5000;
  int64_t max_timeout_ms = 3600000;
  int64_t stall_grace_ms = 5000;

  bool should_revoke_on_expiry() const noexcept {
    return expiry_policy == HeartbeatExpiryPolicy::Revoke ||
           eviction_action == HeartbeatEvictionAction::Revoke;
  }

  void resolve_session_params(std::optional<int64_t> req_interval_ms,
                              std::optional<int64_t> req_max_missed,
                              int64_t* out_interval_ms,
                              int64_t* out_max_missed) const noexcept;
};

/* Per-connection token bucket and 20-strike flood guard (ISSUE-20, ISSUE-82). */
class HeartbeatFloodTracker {
 public:
  static constexpr int kMaxStrikes = 20;
  static constexpr double kBurstTokens = 8.0;

  enum class Decision {
    Allow,
    RateLimited,
    RevokeAndDisconnect,
  };

  Decision check_and_consume(int64_t now_ns, int64_t interval_ms) noexcept;
  void reset_strikes() noexcept {
    strikes_ = 0;
  }
  int strikes() const noexcept {
    return strikes_;
  }

 private:
  bool initialized_ = false;
  double tokens_ = kBurstTokens;
  int64_t last_refill_ns_ = 0;
  int strikes_ = 0;
};

struct HeartbeatSessionState {
  bool active = false;
  int64_t interval_ms = 5000;
  int64_t max_missed = 6;
  int64_t last_beat_ns = 0;
  int64_t next_deadline_ns = 0;
  int64_t missed_beats = 0;
  bool has_sequence = false;
  int64_t last_sequence = 0;
  HeartbeatHealth health = HeartbeatHealth::Healthy;
  bool warned_episode = false;
  std::string client_state;

  int64_t total_timeout_ms() const noexcept {
    return interval_ms * max_missed;
  }
};

/* Single CLOCK_MONOTONIC timerfd backed by an O(log N) ordered deadline heap
 * embedded directly in the single-threaded epoll event loop (R2-5). */
class HeartbeatTimerHeap {
 public:
  HeartbeatTimerHeap() = default;
  ~HeartbeatTimerHeap() = default;

  HeartbeatTimerHeap(const HeartbeatTimerHeap&) = delete;
  HeartbeatTimerHeap& operator=(const HeartbeatTimerHeap&) = delete;

  bool init(std::string* err_out);
  void close() noexcept;

  int fd() const noexcept {
    return timer_fd_.get();
  }
  bool valid() const noexcept {
    return timer_fd_.valid();
  }
  bool empty() const noexcept {
    return heap_.empty();
  }
  int64_t armed_deadline_ns() const noexcept {
    return armed_deadline_ns_;
  }

  void schedule(uint64_t conn_id, int64_t next_deadline_ns, int64_t total_timeout_ms);
  void remove(uint64_t conn_id);
  void clear();

  /* Scaled stall threshold (ISSUE-53):
   * min(500 ms, max(20 ms, min_active_timeout_ms / 2)). */
  int64_t stall_threshold_ms() const noexcept;

  void arm_timerfd(int64_t now_ns);
  uint64_t drain_timerfd();
  std::vector<uint64_t> pop_due(int64_t now_ns);

 private:
  UniqueFd timer_fd_;
  std::set<std::pair<int64_t, uint64_t>> heap_;
  std::map<uint64_t, int64_t> conn_deadline_;
  std::map<uint64_t, int64_t> conn_timeout_ms_;
  std::multiset<int64_t> active_timeouts_ms_;
  int64_t armed_deadline_ns_ = 0;
};

}  // namespace mtlm2

#endif  // MTLM2_HEARTBEAT_HPP
