/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mtlm2_heartbeat.hpp"

#include <errno.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace mtlm2 {

const char* heartbeat_expiry_policy_to_string(HeartbeatExpiryPolicy policy) noexcept {
  switch (policy) {
    case HeartbeatExpiryPolicy::WarnOnly:
      return "warn_only";
    case HeartbeatExpiryPolicy::Revoke:
      return "revoke";
  }
  return "warn_only";
}

bool parse_heartbeat_expiry_policy(const std::string& text,
                                   HeartbeatExpiryPolicy* out) noexcept {
  if (!out) return false;
  if (text == "warn_only") {
    *out = HeartbeatExpiryPolicy::WarnOnly;
    return true;
  }
  if (text == "revoke") {
    *out = HeartbeatExpiryPolicy::Revoke;
    return true;
  }
  return false;
}

const char* heartbeat_eviction_action_to_string(HeartbeatEvictionAction action) noexcept {
  switch (action) {
    case HeartbeatEvictionAction::None:
      return "none";
    case HeartbeatEvictionAction::Revoke:
      return "revoke";
  }
  return "none";
}

bool parse_heartbeat_eviction_action(const std::string& text,
                                     HeartbeatEvictionAction* out) noexcept {
  if (!out) return false;
  if (text == "none") {
    *out = HeartbeatEvictionAction::None;
    return true;
  }
  if (text == "revoke") {
    *out = HeartbeatEvictionAction::Revoke;
    return true;
  }
  return false;
}

const char* heartbeat_health_to_string(HeartbeatHealth health) noexcept {
  switch (health) {
    case HeartbeatHealth::Healthy:
      return "healthy";
    case HeartbeatHealth::Degraded:
      return "degraded";
    case HeartbeatHealth::Warned:
      return "warned";
  }
  return "healthy";
}

void HeartbeatConfig::resolve_session_params(std::optional<int64_t> req_interval_ms,
                                             std::optional<int64_t> req_max_missed,
                                             int64_t* out_interval_ms,
                                             int64_t* out_max_missed) const noexcept {
  int64_t interval = req_interval_ms.value_or(default_interval_ms);
  int64_t missed = req_max_missed.value_or(default_missed_beats);

  if (missed < 1) missed = 1;
  if (missed > 1000) missed = 1000;
  if (interval < 1) interval = 1;
  if (interval > max_timeout_ms) interval = max_timeout_ms;

  const int64_t eff_min = std::max<int64_t>(1, min_timeout_ms);
  const int64_t eff_max = std::max<int64_t>(eff_min, max_timeout_ms);

  /* Avoid overflow when computing interval * missed. */
  if (interval > eff_max / missed) {
    interval = std::max<int64_t>(1, eff_max / missed);
  }
  if (interval * missed < eff_min) {
    interval = (eff_min + missed - 1) / missed;
  }

  if (out_interval_ms) *out_interval_ms = interval;
  if (out_max_missed) *out_max_missed = missed;
}

HeartbeatFloodTracker::Decision HeartbeatFloodTracker::check_and_consume(
    int64_t now_ns, int64_t interval_ms) noexcept {
  if (!initialized_) {
    initialized_ = true;
    tokens_ = kBurstTokens;
    last_refill_ns_ = now_ns;
    strikes_ = 0;
  } else if (now_ns > last_refill_ns_) {
    const double dt_sec = static_cast<double>(now_ns - last_refill_ns_) / 1e9;
    const double safe_interval = static_cast<double>(std::max<int64_t>(interval_ms, 20));
    const double refill_per_sec = std::clamp(3000.0 / safe_interval, 5.0, 60.0);
    tokens_ = std::min(kBurstTokens, tokens_ + dt_sec * refill_per_sec);
    last_refill_ns_ = now_ns;
  }

  if (tokens_ >= 1.0) {
    tokens_ -= 1.0;
    return Decision::Allow;
  }

  ++strikes_;
  if (strikes_ >= kMaxStrikes) {
    return Decision::RevokeAndDisconnect;
  }
  return Decision::RateLimited;
}

bool HeartbeatTimerHeap::init(std::string* err_out) {
  int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0) {
    if (err_out) *err_out = "timerfd_create failed: " + std::string(std::strerror(errno));
    return false;
  }
  timer_fd_.reset(fd);
  armed_deadline_ns_ = 0;
  return true;
}

void HeartbeatTimerHeap::close() noexcept {
  clear();
  timer_fd_.reset();
}

void HeartbeatTimerHeap::schedule(uint64_t conn_id, int64_t next_deadline_ns,
                                  int64_t total_timeout_ms) {
  remove(conn_id);
  heap_.emplace(next_deadline_ns, conn_id);
  conn_deadline_[conn_id] = next_deadline_ns;
  conn_timeout_ms_[conn_id] = total_timeout_ms;
  active_timeouts_ms_.insert(total_timeout_ms);
}

void HeartbeatTimerHeap::remove(uint64_t conn_id) {
  auto dit = conn_deadline_.find(conn_id);
  if (dit != conn_deadline_.end()) {
    heap_.erase({dit->second, conn_id});
    conn_deadline_.erase(dit);
  }
  auto tit = conn_timeout_ms_.find(conn_id);
  if (tit != conn_timeout_ms_.end()) {
    auto ait = active_timeouts_ms_.find(tit->second);
    if (ait != active_timeouts_ms_.end()) active_timeouts_ms_.erase(ait);
    conn_timeout_ms_.erase(tit);
  }
}

void HeartbeatTimerHeap::clear() {
  heap_.clear();
  conn_deadline_.clear();
  conn_timeout_ms_.clear();
  active_timeouts_ms_.clear();
  armed_deadline_ns_ = 0;
  if (timer_fd_.valid()) {
    struct itimerspec its{};
    (void)::timerfd_settime(timer_fd_.get(), 0, &its, nullptr);
  }
}

int64_t HeartbeatTimerHeap::stall_threshold_ms() const noexcept {
  if (active_timeouts_ms_.empty()) return 500;
  const int64_t min_active_timeout_ms = *active_timeouts_ms_.begin();
  return std::min<int64_t>(500, std::max<int64_t>(20, min_active_timeout_ms / 2));
}

void HeartbeatTimerHeap::arm_timerfd(int64_t now_ns) {
  if (!timer_fd_.valid()) return;
  struct itimerspec its{};
  if (heap_.empty()) {
    armed_deadline_ns_ = 0;
    (void)::timerfd_settime(timer_fd_.get(), 0, &its, nullptr);
    return;
  }

  int64_t earliest_ns = heap_.begin()->first;
  /* Ensure timerfd gets a strictly positive absolute timestamp so it is always armed. */
  int64_t arm_ns = (earliest_ns <= now_ns) ? (now_ns + 1000000LL) : earliest_ns;
  if (arm_ns < 1000000LL) arm_ns = 1000000LL;
  its.it_value.tv_sec = static_cast<time_t>(arm_ns / 1000000000LL);
  its.it_value.tv_nsec = static_cast<long>(arm_ns % 1000000000LL);
  armed_deadline_ns_ = arm_ns;
  (void)::timerfd_settime(timer_fd_.get(), TFD_TIMER_ABSTIME, &its, nullptr);
}

uint64_t HeartbeatTimerHeap::drain_timerfd() {
  if (!timer_fd_.valid()) return 0;
  uint64_t total = 0;
  uint64_t exp = 0;
  for (;;) {
    ssize_t n = ::read(timer_fd_.get(), &exp, sizeof(exp));
    if (n == static_cast<ssize_t>(sizeof(exp))) {
      total += exp;
    } else {
      break;
    }
  }
  return total;
}

std::vector<uint64_t> HeartbeatTimerHeap::pop_due(int64_t now_ns) {
  std::vector<uint64_t> due;
  while (!heap_.empty() && heap_.begin()->first <= now_ns) {
    uint64_t cid = heap_.begin()->second;
    remove(cid);
    due.push_back(cid);
  }
  return due;
}

}  // namespace mtlm2
