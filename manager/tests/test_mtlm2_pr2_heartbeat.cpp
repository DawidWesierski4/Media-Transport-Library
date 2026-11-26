/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <errno.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <json-c/json.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mt_varlink.h"
#include "mtlm2_server.hpp"

namespace fs = std::filesystem;

namespace {

struct JsonGuard {
  json_object* obj = nullptr;
  ~JsonGuard() {
    if (obj) json_object_put(obj);
  }
};

struct FdGuard {
  int fd = -1;
  explicit FdGuard(int f = -1) : fd(f) {
  }
  ~FdGuard() {
    if (fd >= 0) ::close(fd);
  }
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
  void reset(int f = -1) {
    if (fd >= 0) ::close(fd);
    fd = f;
  }
};

class TempDir {
 public:
  TempDir() {
    std::string tmpl = "/tmp/mtlm2_pr2_hb_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* created = ::mkdtemp(buf.data());
    EXPECT_NE(created, nullptr);
    if (created) path_ = created;
  }
  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      fs::remove_all(path_, ec);
    }
  }
  const std::string& path() const {
    return path_;
  }

 private:
  std::string path_;
};

int count_process_fds(pid_t pid) {
  std::string dir = "/proc/" + std::to_string(pid) + "/fd";
  std::error_code ec;
  int cnt = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    (void)e;
    ++cnt;
  }
  return ec ? -1 : cnt;
}

int stable_process_fds(pid_t pid) {
  int prev = count_process_fds(pid);
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    int cur = count_process_fds(pid);
    if (cur == prev && cur > 0) return cur;
    prev = cur;
  }
  return prev;
}

int read_proc_threads(pid_t pid) {
  std::ifstream f("/proc/" + std::to_string(pid) + "/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("Threads:", 0) == 0) {
      return std::atoi(line.c_str() + 8);
    }
  }
  return -1;
}

bool write_all_fd(int fd, const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::write(fd, p + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

bool read_line_fd(int fd, std::string* out, int timeout_ms) {
  out->clear();
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                   deadline - std::chrono::steady_clock::now())
                   .count();
    if (rem <= 0) rem = 1;
    struct pollfd pfd{fd, POLLIN, 0};
    int prc = ::poll(&pfd, 1, static_cast<int>(rem));
    if (prc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (prc == 0) return false;
    char ch = '\0';
    ssize_t n = ::read(fd, &ch, 1);
    if (n == 1) {
      if (ch == '\n') return true;
      out->push_back(ch);
    } else if (n == 0) {
      return !out->empty();
    } else if (errno != EINTR && errno != EAGAIN) {
      return false;
    }
  }
  return false;
}

class DaemonProc {
 public:
  ~DaemonProc() {
    stop(SIGKILL);
  }

  bool start(const std::string& dir, const std::vector<std::string>& extra_args = {}) {
    client_sock_ = dir + "/manager2.sock";
    admin_sock_ = dir + "/admin.sock";
    state_dir_ = dir + "/state";
    host_lock_ = dir + "/host.lock";
    v1_sock_ = dir + "/no_v1.sock";
    shm_lock_ = dir + "/shm.lock";

    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
      std::vector<std::string> args = {
          MTLM2_DAEMON_BIN, "--socket",           client_sock_, "--admin-socket",
          admin_sock_,      "--state-dir",        state_dir_,   "--host-lock",
          host_lock_,       "--v1-socket",        v1_sock_,     "--shm-lock-path",
          shm_lock_,        "--test-mock-netdev",
      };
      for (const auto& a : extra_args) args.push_back(a);
      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (auto& s : args) argv.push_back(s.data());
      argv.push_back(nullptr);
      ::execv(MTLM2_DAEMON_BIN, argv.data());
      ::_exit(127);
    }
    pid_ = pid;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < deadline) {
      int st = 0;
      pid_t w = ::waitpid(pid_, &st, WNOHANG);
      if (w == pid_) {
        pid_ = -1;
        return false;
      }
      int fd = mt_varlink_connect_unix(client_sock_.c_str(), 100);
      if (fd >= 0) {
        JsonGuard rep;
        int rc = mt_varlink_call(fd, MT_VARLINK_SERVICE_INTERFACE ".GetInfo", nullptr,
                                 nullptr, 0, 500, &rep.obj, nullptr, 0, nullptr, 0);
        ::close(fd);
        if (rc == 0) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    stop(SIGKILL);
    return false;
  }

  int stop(int sig) {
    if (pid_ <= 0) return 0;
    (void)::kill(pid_, sig);
    int st = 0;
    while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {
    }
    pid_ = -1;
    return st;
  }

  pid_t pid() const {
    return pid_;
  }
  const std::string& client_sock() const {
    return client_sock_;
  }
  const std::string& admin_sock() const {
    return admin_sock_;
  }

 private:
  pid_t pid_ = -1;
  std::string client_sock_;
  std::string admin_sock_;
  std::string state_dir_;
  std::string host_lock_;
  std::string v1_sock_;
  std::string shm_lock_;
};

int rpc_open_session_hb(int fd, const char* name, int64_t hb_interval_ms,
                        int64_t hb_max_missed, std::string* sid_out = nullptr,
                        int64_t* eff_interval_out = nullptr,
                        int64_t* eff_missed_out = nullptr,
                        std::string* err_name = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "clientName", json_object_new_string(name));
  if (hb_interval_ms >= 0) {
    json_object_object_add(params.obj, "heartbeatIntervalMs",
                           json_object_new_int64(hb_interval_ms));
  }
  if (hb_max_missed >= 0) {
    json_object_object_add(params.obj, "heartbeatMaxMissed",
                           json_object_new_int64(hb_max_missed));
  }
  JsonGuard reply;
  char ename[128] = {};
  int rc =
      mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".OpenSession", params.obj,
                      nullptr, 0, 2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && reply.obj) {
    json_object* p = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p) {
      json_object* v = nullptr;
      if (sid_out && json_object_object_get_ex(p, "sessionId", &v) && v)
        *sid_out = json_object_get_string(v);
      if (eff_interval_out && json_object_object_get_ex(p, "heartbeatIntervalMs", &v) &&
          v)
        *eff_interval_out = json_object_get_int64(v);
      if (eff_missed_out && json_object_object_get_ex(p, "heartbeatMaxMissed", &v) && v)
        *eff_missed_out = json_object_get_int64(v);
    }
  }
  return rc;
}

int rpc_ping(int fd, int64_t seq, int64_t* seq_out = nullptr,
             int64_t* mono_us_out = nullptr, std::string* err_name = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "sequence", json_object_new_int64(seq));
  JsonGuard reply;
  char ename[128] = {};
  int rc = mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".Ping", params.obj, nullptr,
                           0, 2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && reply.obj) {
    json_object* p = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p) {
      json_object* v = nullptr;
      if (seq_out && json_object_object_get_ex(p, "sequence", &v) && v)
        *seq_out = json_object_get_int64(v);
      if (mono_us_out && json_object_object_get_ex(p, "serverMonotonicUsec", &v) && v)
        *mono_us_out = json_object_get_int64(v);
    }
  }
  return rc;
}

int rpc_heartbeat(int fd, int64_t seq, const char* client_state,
                  int64_t* seq_out = nullptr, int64_t* mono_us_out = nullptr,
                  std::string* status_out = nullptr, int64_t* next_dl_ms_out = nullptr,
                  std::string* err_name = nullptr, int send_fd = -1) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "sequence", json_object_new_int64(seq));
  if (client_state) {
    json_object_object_add(params.obj, "clientState",
                           json_object_new_string(client_state));
  }
  JsonGuard reply;
  char ename[128] = {};
  const int* fds = (send_fd >= 0) ? &send_fd : nullptr;
  size_t fd_cnt = (send_fd >= 0) ? 1u : 0u;
  int rc = mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".Heartbeat", params.obj, fds,
                           fd_cnt, 2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && reply.obj) {
    json_object* p = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p) {
      json_object* v = nullptr;
      if (seq_out && json_object_object_get_ex(p, "sequence", &v) && v)
        *seq_out = json_object_get_int64(v);
      if (mono_us_out && json_object_object_get_ex(p, "serverMonotonicUsec", &v) && v)
        *mono_us_out = json_object_get_int64(v);
      if (status_out && json_object_object_get_ex(p, "status", &v) && v)
        *status_out = json_object_get_string(v);
      if (next_dl_ms_out && json_object_object_get_ex(p, "nextDeadlineMs", &v) && v)
        *next_dl_ms_out = json_object_get_int64(v);
    }
  }
  return rc;
}

int rpc_acquire_cpu(int fd, int64_t preferred, bool allow_fallback,
                    std::string* lid_out = nullptr, int64_t* cpu_out = nullptr,
                    std::string* err_name = nullptr) {
  JsonGuard params{json_object_new_object()};
  if (preferred >= 0) {
    json_object_object_add(params.obj, "preferred", json_object_new_int64(preferred));
  }
  json_object_object_add(params.obj, "allowFallback",
                         json_object_new_boolean(allow_fallback ? 1 : 0));
  JsonGuard reply;
  char ename[128] = {};
  int rc =
      mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AcquireCpu", params.obj, nullptr,
                      0, 2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && reply.obj) {
    json_object* p = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p) {
      json_object* v = nullptr;
      if (lid_out && json_object_object_get_ex(p, "leaseId", &v) && v)
        *lid_out = json_object_get_string(v);
      if (cpu_out && json_object_object_get_ex(p, "cpuId", &v) && v)
        *cpu_out = json_object_get_int64(v);
    }
  }
  return rc;
}

int rpc_acquire_queue(int fd, const char* if_name, int64_t preferred_queue,
                      std::string* lid_out = nullptr, int64_t* qid_out = nullptr,
                      std::string* err_name = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "ifName", json_object_new_string(if_name));
  if (preferred_queue >= 0) {
    json_object_object_add(params.obj, "preferredQueue",
                           json_object_new_int64(preferred_queue));
  }
  JsonGuard reply;
  char ename[128] = {};
  int rc =
      mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AcquireQueue", params.obj,
                      nullptr, 0, 4000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && reply.obj) {
    json_object* p = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p) {
      json_object* v = nullptr;
      if (lid_out && json_object_object_get_ex(p, "leaseId", &v) && v)
        *lid_out = json_object_get_string(v);
      if (qid_out && json_object_object_get_ex(p, "queueId", &v) && v)
        *qid_out = json_object_get_int64(v);
    }
  }
  return rc;
}

int rpc_attach_xsk(int fd, const std::string& queue_lease, int xsk_fd,
                   std::string* lid_out = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "queueLease",
                         json_object_new_string(queue_lease.c_str()));
  JsonGuard reply;
  int rc = mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AttachXsk", params.obj,
                           &xsk_fd, 1u, 2000, &reply.obj, nullptr, 0, nullptr, 0);
  if (rc == 0 && lid_out && reply.obj) {
    json_object* p = nullptr;
    json_object* v = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p &&
        json_object_object_get_ex(p, "leaseId", &v) && v)
      *lid_out = json_object_get_string(v);
  }
  return rc;
}

int rpc_add_flow(int fd, const std::string& queue_lease, const char* dst_ip,
                 int dst_port) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "queueLease",
                         json_object_new_string(queue_lease.c_str()));
  json_object_object_add(params.obj, "protocol", json_object_new_string("udp4"));
  json_object_object_add(params.obj, "dstIp", json_object_new_string(dst_ip));
  json_object_object_add(params.obj, "dstPort", json_object_new_int64(dst_port));
  JsonGuard reply;
  return mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AddFlow", params.obj, nullptr,
                         0, 2000, &reply.obj, nullptr, 0, nullptr, 0);
}

int rpc_add_udp_filter(int fd, const std::string& queue_lease, int dst_port) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "queueLease",
                         json_object_new_string(queue_lease.c_str()));
  json_object_object_add(params.obj, "destinationPort", json_object_new_int64(dst_port));
  JsonGuard reply;
  return mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AddUdpFilter", params.obj,
                         nullptr, 0, 2000, &reply.obj, nullptr, 0, nullptr, 0);
}

bool get_admin_status(const std::string& admin_sock, JsonGuard* out_params) {
  FdGuard afd(mt_varlink_connect_unix(admin_sock.c_str(), 2000));
  if (afd.fd < 0) return false;
  JsonGuard rep;
  int rc = mt_varlink_call(afd.fd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr,
                           nullptr, 0, 2000, &rep.obj, nullptr, 0, nullptr, 0);
  if (rc != 0 || !rep.obj) return false;
  json_object* p = nullptr;
  if (!json_object_object_get_ex(rep.obj, "parameters", &p) || !p) return false;
  out_params->obj = json_object_get(p);
  return true;
}

bool json_get_i64(json_object* obj, const char* key, int64_t* out) {
  json_object* v = nullptr;
  if (!obj || !json_object_object_get_ex(obj, key, &v) || !v ||
      !json_object_is_type(v, json_type_int))
    return false;
  *out = json_object_get_int64(v);
  return true;
}

bool json_get_str(json_object* obj, const char* key, std::string* out) {
  json_object* v = nullptr;
  if (!obj || !json_object_object_get_ex(obj, key, &v) || !v ||
      !json_object_is_type(v, json_type_string))
    return false;
  *out = json_object_get_string(v);
  return true;
}

json_object* find_session_in_status(json_object* status_params, const std::string& sid) {
  json_object* arr = nullptr;
  if (!status_params || !json_object_object_get_ex(status_params, "sessions", &arr) ||
      !arr || !json_object_is_type(arr, json_type_array))
    return nullptr;
  size_t n = json_object_array_length(arr);
  for (size_t i = 0; i < n; ++i) {
    json_object* item = json_object_array_get_idx(arr, i);
    std::string id;
    if (json_get_str(item, "sessionId", &id) && id == sid) return item;
  }
  return nullptr;
}

json_object* find_lease_in_array(json_object* status_params, const char* array_key,
                                 const std::string& lid) {
  json_object* arr = nullptr;
  if (!status_params || !json_object_object_get_ex(status_params, array_key, &arr) ||
      !arr || !json_object_is_type(arr, json_type_array))
    return nullptr;
  size_t n = json_object_array_length(arr);
  for (size_t i = 0; i < n; ++i) {
    json_object* item = json_object_array_get_idx(arr, i);
    std::string id;
    if (json_get_str(item, "leaseId", &id) && id == lid) return item;
  }
  return nullptr;
}

struct LiveTenantHandle {
  pid_t pid = -1;
  FdGuard cmd_wr;
  FdGuard rsp_rd;
  std::string session_id;
  std::string cpu_lease_id;
  int64_t cpu_id = -1;
  std::string queue_lease_id;
  int64_t queue_id = -1;

  ~LiveTenantHandle() {
    cleanup();
  }

  void cleanup() {
    if (pid > 0) {
      (void)::kill(pid, SIGCONT);
      (void)::kill(pid, SIGKILL);
      int st = 0;
      while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
      }
      pid = -1;
    }
    cmd_wr.reset();
    rsp_rd.reset();
  }
};

/* Forks a real tenant process that opens a session, acquires CPU + Queue + XSK + Flow +
 * UdpFilter, sends initial Heartbeat(seq=1), and waits for parent commands ("hb2",
 * "exit") so the parent can SIGSTOP / SIGCONT it cleanly. */
bool spawn_live_tenant(const std::string& client_sock, const std::string& name,
                       int64_t hb_interval_ms, int64_t hb_max_missed,
                       bool attach_steering, LiveTenantHandle* out) {
  int cmd_pipe[2] = {-1, -1};
  int rsp_pipe[2] = {-1, -1};
  if (::pipe2(cmd_pipe, O_CLOEXEC) != 0) return false;
  if (::pipe2(rsp_pipe, O_CLOEXEC) != 0) {
    ::close(cmd_pipe[0]);
    ::close(cmd_pipe[1]);
    return false;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(cmd_pipe[0]);
    ::close(cmd_pipe[1]);
    ::close(rsp_pipe[0]);
    ::close(rsp_pipe[1]);
    return false;
  }
  if (pid == 0) {
    ::close(cmd_pipe[1]);
    ::close(rsp_pipe[0]);

    int fd = mt_varlink_connect_unix(client_sock.c_str(), 2000);
    if (fd < 0) ::_exit(10);

    std::string sid, cpu_lid, q_lid;
    int64_t cpu_id = -1, q_id = -1;
    if (rpc_open_session_hb(fd, name.c_str(), hb_interval_ms, hb_max_missed, &sid) != 0)
      ::_exit(11);
    if (rpc_acquire_cpu(fd, -1, true, &cpu_lid, &cpu_id) != 0) ::_exit(12);
    if (rpc_acquire_queue(fd, "eth0", -1, &q_lid, &q_id) != 0) ::_exit(13);

    int xsk_pair[2] = {-1, -1};
    if (attach_steering) {
      if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, xsk_pair) != 0)
        ::_exit(14);
      if (rpc_attach_xsk(fd, q_lid, xsk_pair[0]) != 0) ::_exit(15);
      if (rpc_add_flow(fd, q_lid, "239.10.20.30", 20000 + static_cast<int>(q_id)) != 0)
        ::_exit(16);
      if (rpc_add_udp_filter(fd, q_lid, 20000 + static_cast<int>(q_id)) != 0) ::_exit(17);
    }

    std::string st1;
    if (rpc_heartbeat(fd, 1, "running", nullptr, nullptr, &st1) != 0) ::_exit(18);

    std::ostringstream oss;
    oss << sid << " " << cpu_lid << " " << cpu_id << " " << q_lid << " " << q_id << "\n";
    std::string ready = oss.str();
    (void)write_all_fd(rsp_pipe[1], ready.data(), ready.size());

    std::string cmd;
    while (read_line_fd(cmd_pipe[0], &cmd, 15000)) {
      if (cmd == "hb2") {
        std::string st2;
        std::string ename;
        int rc = rpc_heartbeat(fd, 2, "running", nullptr, nullptr, &st2, nullptr, &ename);
        std::string rep = (rc == 0) ? ("OK " + st2 + "\n") : ("ERR " + ename + "\n");
        (void)write_all_fd(rsp_pipe[1], rep.data(), rep.size());
      } else if (cmd == "exit") {
        break;
      }
    }

    if (xsk_pair[0] >= 0) ::close(xsk_pair[0]);
    if (xsk_pair[1] >= 0) ::close(xsk_pair[1]);
    ::close(fd);
    ::close(cmd_pipe[0]);
    ::close(rsp_pipe[1]);
    ::_exit(0);
  }

  ::close(cmd_pipe[0]);
  ::close(rsp_pipe[1]);
  out->pid = pid;
  out->cmd_wr.reset(cmd_pipe[1]);
  out->rsp_rd.reset(rsp_pipe[0]);

  std::string line;
  if (!read_line_fd(out->rsp_rd.fd, &line, 4000)) {
    out->cleanup();
    return false;
  }
  std::istringstream iss(line);
  if (!(iss >> out->session_id >> out->cpu_lease_id >> out->cpu_id >>
        out->queue_lease_id >> out->queue_id)) {
    out->cleanup();
    return false;
  }
  return true;
}

/* 1. Ping and Heartbeat monotonic sequence, strict key allowlist, safe defaults,
 *    and OpenSession heartbeat parameter validation/clamping. */
TEST(Pr2HeartbeatTest, PingAndHeartbeatMonotonicSequence) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  /* Verify safe defaults in GetStatus (Finding 3, R2-5). */
  JsonGuard status;
  ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &status));
  std::string hb_policy, evict_action;
  int64_t warnings = -1, revocations = -1, stalls = -1;
  ASSERT_TRUE(json_get_str(status.obj, "heartbeatPolicy", &hb_policy));
  ASSERT_TRUE(json_get_str(status.obj, "heartbeatEvictionAction", &evict_action));
  ASSERT_TRUE(json_get_i64(status.obj, "heartbeatWarnings", &warnings));
  ASSERT_TRUE(json_get_i64(status.obj, "heartbeatRevocations", &revocations));
  ASSERT_TRUE(json_get_i64(status.obj, "managerStallEvents", &stalls));
  EXPECT_EQ(hb_policy, "warn_only");
  EXPECT_EQ(evict_action, "none");
  EXPECT_EQ(warnings, 0);
  EXPECT_EQ(revocations, 0);
  EXPECT_EQ(stalls, 0);

  FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(cfd.fd, 0);

  /* Ping works before OpenSession and returns monotonic timestamp. */
  int64_t seq1 = -1, us1 = 0;
  ASSERT_EQ(rpc_ping(cfd.fd, 1, &seq1, &us1), 0);
  EXPECT_EQ(seq1, 1);
  EXPECT_GT(us1, 0);

  int64_t seq2 = -1, us2 = 0;
  ASSERT_EQ(rpc_ping(cfd.fd, 2, &seq2, &us2), 0);
  EXPECT_EQ(seq2, 2);
  EXPECT_GE(us2, us1);

  /* Ping rejects unknown parameter key with InvalidParameter. */
  {
    JsonGuard bad_params{json_object_new_object()};
    json_object_object_add(bad_params.obj, "sequence", json_object_new_int64(3));
    json_object_object_add(bad_params.obj, "unexpectedKey", json_object_new_int64(1));
    JsonGuard rep;
    char ename[128] = {};
    int rc =
        mt_varlink_call(cfd.fd, MT_VARLINK_MANAGER_INTERFACE ".Ping", bad_params.obj,
                        nullptr, 0, 2000, &rep.obj, ename, sizeof(ename), nullptr, 0);
    EXPECT_LT(rc, 0);
    EXPECT_EQ(std::string(ename), MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");
  }

  /* OpenSession with omitted heartbeat params uses safe 5s / 6-beat (30s) defaults. */
  std::string sid;
  int64_t eff_interval = -1, eff_missed = -1;
  ASSERT_EQ(rpc_open_session_hb(cfd.fd, "ping-hb-seq", -1, -1, &sid, &eff_interval,
                                &eff_missed),
            0);
  EXPECT_EQ(eff_interval, 5000);
  EXPECT_EQ(eff_missed, 6);

  /* Heartbeat round-trip and strict monotonic sequence enforcement. */
  int64_t hb_seq = -1, hb_us = 0, next_dl = -1;
  std::string hb_status;
  ASSERT_EQ(rpc_heartbeat(cfd.fd, 1, "running", &hb_seq, &hb_us, &hb_status, &next_dl),
            0);
  EXPECT_EQ(hb_seq, 1);
  EXPECT_GT(hb_us, 0);
  EXPECT_EQ(hb_status, "ok");
  EXPECT_EQ(next_dl, 30000);

  /* Omitted clientState succeeds. */
  ASSERT_EQ(rpc_heartbeat(cfd.fd, 2, nullptr, &hb_seq, &hb_us, &hb_status, &next_dl), 0);
  EXPECT_EQ(hb_seq, 2);
  EXPECT_EQ(hb_status, "ok");

  /* Duplicate sequence rejected. */
  std::string ename;
  EXPECT_LT(
      rpc_heartbeat(cfd.fd, 2, "running", nullptr, nullptr, nullptr, nullptr, &ename), 0);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");

  /* Rollback sequence rejected. */
  ename.clear();
  EXPECT_LT(
      rpc_heartbeat(cfd.fd, 1, "running", nullptr, nullptr, nullptr, nullptr, &ename), 0);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");

  /* Unknown parameter key on Heartbeat rejected with InvalidParameter. */
  {
    JsonGuard bad_params{json_object_new_object()};
    json_object_object_add(bad_params.obj, "sequence", json_object_new_int64(3));
    json_object_object_add(bad_params.obj, "bogusField", json_object_new_string("x"));
    JsonGuard rep;
    char ebuf[128] = {};
    int rc =
        mt_varlink_call(cfd.fd, MT_VARLINK_MANAGER_INTERFACE ".Heartbeat", bad_params.obj,
                        nullptr, 0, 2000, &rep.obj, ebuf, sizeof(ebuf), nullptr, 0);
    EXPECT_LT(rc, 0);
    EXPECT_EQ(std::string(ebuf), MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");
  }

  /* Verify GetStatus surfaces per-session heartbeat fields. */
  JsonGuard st2;
  ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st2));
  json_object* sess = find_session_in_status(st2.obj, sid);
  ASSERT_NE(sess, nullptr);
  std::string sstate;
  int64_t missed = -1, age_ms = -1;
  ASSERT_TRUE(json_get_str(sess, "heartbeatState", &sstate));
  ASSERT_TRUE(json_get_i64(sess, "missedBeats", &missed));
  ASSERT_TRUE(json_get_i64(sess, "lastBeatAgeMs", &age_ms));
  EXPECT_EQ(sstate, "healthy");
  EXPECT_EQ(missed, 0);
  EXPECT_GE(age_ms, 0);

  /* OpenSession clamps too-tight heartbeat requests up to policy min_timeout_ms (5000).
   */
  FdGuard cfd2(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(cfd2.fd, 0);
  int64_t clamped_interval = -1, clamped_missed = -1;
  ASSERT_EQ(rpc_open_session_hb(cfd2.fd, "clamped-client", 10, 2, nullptr,
                                &clamped_interval, &clamped_missed),
            0);
  EXPECT_GE(clamped_interval * clamped_missed, 5000);
}

/* 2. HB-01: SIGSTOP live tenant under default warn_only policy -> heartbeatWarnings
 *    increments, session marked warned, process stays alive and keeps CPU + queue leases,
 *    and SIGCONT recovers cleanly to "recovered" / "healthy". */
TEST(Pr2HeartbeatTest, HB01_SigstopWarnOnlyKeepsLeasesAndRecoversOnSigcont) {
  TempDir tmp;
  DaemonProc daemon;
  /* Default policy is warn_only; only lower min-timeout for fast T2 test execution. */
  ASSERT_TRUE(daemon.start(tmp.path(), {"--heartbeat-min-timeout-ms", "50"}));

  LiveTenantHandle tenant;
  ASSERT_TRUE(
      spawn_live_tenant(daemon.client_sock(), "hb01-tenant", 40, 3, true, &tenant));

  /* Verify daemon stays strictly single-threaded (R2-5). */
  EXPECT_EQ(read_proc_threads(daemon.pid()), 1);

  /* Freeze live tenant via SIGSTOP. */
  ASSERT_EQ(::kill(tenant.pid, SIGSTOP), 0);

  bool saw_warned = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < deadline) {
    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    int64_t warnings = 0, revocations = 0, qcnt = 0;
    if (json_get_i64(st.obj, "heartbeatWarnings", &warnings) &&
        json_get_i64(st.obj, "heartbeatRevocations", &revocations) &&
        json_get_i64(st.obj, "quarantinedCount", &qcnt) && warnings >= 1) {
      EXPECT_EQ(revocations, 0);
      EXPECT_EQ(qcnt, 0);

      json_object* sess = find_session_in_status(st.obj, tenant.session_id);
      ASSERT_NE(sess, nullptr);
      std::string sstate;
      int64_t missed = 0;
      ASSERT_TRUE(json_get_str(sess, "heartbeatState", &sstate));
      ASSERT_TRUE(json_get_i64(sess, "missedBeats", &missed));
      EXPECT_EQ(sstate, "warned");
      EXPECT_GE(missed, 3);

      /* Leases MUST stay ALLOCATED (never revoked or quarantined under warn_only). */
      json_object* cpu_l = find_lease_in_array(st.obj, "leases", tenant.cpu_lease_id);
      json_object* q_l = find_lease_in_array(st.obj, "queues", tenant.queue_lease_id);
      ASSERT_NE(cpu_l, nullptr);
      ASSERT_NE(q_l, nullptr);
      std::string cpu_state, q_state;
      ASSERT_TRUE(json_get_str(cpu_l, "state", &cpu_state));
      ASSERT_TRUE(json_get_str(q_l, "state", &q_state));
      EXPECT_EQ(cpu_state, "ALLOCATED");
      EXPECT_EQ(q_state, "ALLOCATED");

      saw_warned = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  ASSERT_TRUE(saw_warned);

  /* Child process must still be alive. */
  int wst = 0;
  EXPECT_EQ(::waitpid(tenant.pid, &wst, WNOHANG), 0);

  /* Another client cannot steal the warned tenant's CPU or queue. */
  FdGuard rival(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(rival.fd, 0);
  ASSERT_EQ(rpc_open_session_hb(rival.fd, "hb01-rival", -1, -1), 0);
  std::string ename;
  EXPECT_LT(rpc_acquire_cpu(rival.fd, tenant.cpu_id, false, nullptr, nullptr, &ename), 0);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy");
  ename.clear();
  EXPECT_LT(
      rpc_acquire_queue(rival.fd, "eth0", tenant.queue_id, nullptr, nullptr, &ename), 0);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy");

  /* Resume tenant with SIGCONT and send Heartbeat(seq=2) -> must recover cleanly. */
  ASSERT_EQ(::kill(tenant.pid, SIGCONT), 0);
  ASSERT_TRUE(write_all_fd(tenant.cmd_wr.fd, "hb2\n", 4));
  std::string rep_line;
  ASSERT_TRUE(read_line_fd(tenant.rsp_rd.fd, &rep_line, 3000));
  EXPECT_EQ(rep_line, "OK recovered");

  JsonGuard st_after;
  ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st_after));
  json_object* sess_after = find_session_in_status(st_after.obj, tenant.session_id);
  ASSERT_NE(sess_after, nullptr);
  std::string state_after;
  int64_t missed_after = -1;
  ASSERT_TRUE(json_get_str(sess_after, "heartbeatState", &state_after));
  ASSERT_TRUE(json_get_i64(sess_after, "missedBeats", &missed_after));
  EXPECT_EQ(state_after, "healthy");
  EXPECT_EQ(missed_after, 0);

  (void)write_all_fd(tenant.cmd_wr.fd, "exit\n", 5);
}

/* 3. HB-02: Simulated daemon dispatch stall (slow XDP attach > timeout) AND daemon
 *    SIGSTOP pause while healthy tenants beat -> stall guard fires (managerStallEvents
 *    >= 1), extends grace, and causes zero false warnings or revocations. */
TEST(Pr2HeartbeatTest, HB02_DaemonDispatchStallAndPauseCauseZeroFalseMisses) {
  /* Part A: slow attach_xdp dispatch stall simulating multi-second ice VSI reset. */
  {
    TempDir tmp;
    DaemonProc daemon;
    ASSERT_TRUE(daemon.start(
        tmp.path(),
        {"--heartbeat-min-timeout-ms", "50", "--heartbeat-policy=revoke",
         "--heartbeat-stall-grace-ms", "2000", "--test-attach-xdp-delay-ms", "450"}));

    constexpr int kTenants = 4;
    std::vector<FdGuard> tenants(kTenants);
    std::vector<std::string> sids(kTenants);
    for (int i = 0; i < kTenants; ++i) {
      tenants[i].reset(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
      ASSERT_GE(tenants[i].fd, 0);
      std::string cname = "hb02-tenant-" + std::to_string(i);
      /* 60 ms * 3 = 180 ms timeout, much shorter than the 450 ms XDP stall! */
      ASSERT_EQ(rpc_open_session_hb(tenants[i].fd, cname.c_str(), 60, 3, &sids[i]), 0);
      ASSERT_EQ(rpc_heartbeat(tenants[i].fd, 1, "running"), 0);
    }

    std::atomic<bool> stop_beating{false};
    std::atomic<int> beat_failures{0};
    std::thread beater([&]() {
      int64_t seq = 2;
      while (!stop_beating.load()) {
        for (int i = 0; i < kTenants; ++i) {
          if (rpc_heartbeat(tenants[i].fd, seq, "running") != 0) {
            beat_failures.fetch_add(1);
          }
        }
        ++seq;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
      }
    });

    /* Trigger the 450 ms XDP attach stall on the single-threaded daemon loop. */
    FdGuard xdp_caller(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(xdp_caller.fd, 0);
    ASSERT_EQ(rpc_open_session_hb(xdp_caller.fd, "hb02-xdp-caller", -1, -1), 0);
    ASSERT_EQ(rpc_acquire_queue(xdp_caller.fd, "eth0", -1), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop_beating.store(true);
    beater.join();

    EXPECT_EQ(beat_failures.load(), 0);

    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    int64_t stalls = 0, warnings = -1, revocations = -1;
    ASSERT_TRUE(json_get_i64(st.obj, "managerStallEvents", &stalls));
    ASSERT_TRUE(json_get_i64(st.obj, "heartbeatWarnings", &warnings));
    ASSERT_TRUE(json_get_i64(st.obj, "heartbeatRevocations", &revocations));
    EXPECT_GE(stalls, 1);
    EXPECT_EQ(warnings, 0);
    EXPECT_EQ(revocations, 0);
    for (int i = 0; i < kTenants; ++i) {
      json_object* sess = find_session_in_status(st.obj, sids[i]);
      ASSERT_NE(sess, nullptr);
      std::string sstate;
      ASSERT_TRUE(json_get_str(sess, "heartbeatState", &sstate));
      EXPECT_EQ(sstate, "healthy");
    }
  }

  /* Part B: real SIGSTOP pause on daemon > session timeout. */
  {
    TempDir tmp;
    DaemonProc daemon;
    ASSERT_TRUE(daemon.start(
        tmp.path(), {"--heartbeat-min-timeout-ms", "50", "--heartbeat-policy=revoke",
                     "--heartbeat-stall-grace-ms", "2000"}));

    FdGuard c1(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(c1.fd, 0);
    std::string sid;
    ASSERT_EQ(rpc_open_session_hb(c1.fd, "hb02-pause-tenant", 60, 3, &sid), 0);
    ASSERT_EQ(rpc_heartbeat(c1.fd, 1, "running"), 0);

    ASSERT_EQ(::kill(daemon.pid(), SIGSTOP), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ASSERT_EQ(::kill(daemon.pid(), SIGCONT), 0);

    std::string hb_st;
    ASSERT_EQ(rpc_heartbeat(c1.fd, 2, "running", nullptr, nullptr, &hb_st), 0);
    EXPECT_EQ(hb_st, "ok");

    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    int64_t stalls = 0, warnings = -1, revocations = -1;
    ASSERT_TRUE(json_get_i64(st.obj, "managerStallEvents", &stalls));
    ASSERT_TRUE(json_get_i64(st.obj, "heartbeatWarnings", &warnings));
    ASSERT_TRUE(json_get_i64(st.obj, "heartbeatRevocations", &revocations));
    EXPECT_GE(stalls, 1);
    EXPECT_EQ(warnings, 0);
    EXPECT_EQ(revocations, 0);
  }
}

/* 4. HB-03: Opt-in revoke policy on SIGSTOP tenant -> flows/filters/XSK stripped,
 *    CPU and queue move to QUARANTINED (NOT FREE), rival gets ResourceBusy; after
 *    SIGCONT + exit (and via KillQuarantinedOwner), resources move to FREE. */
TEST(Pr2HeartbeatTest, HB03_OptInRevokeOnSigstopQuarantinesUntilExit) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(
      tmp.path(), {"--heartbeat-policy=revoke", "--heartbeat-min-timeout-ms", "50"}));

  {
    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    std::string pol, evict;
    ASSERT_TRUE(json_get_str(st.obj, "heartbeatPolicy", &pol));
    ASSERT_TRUE(json_get_str(st.obj, "heartbeatEvictionAction", &evict));
    EXPECT_EQ(pol, "revoke");
    EXPECT_EQ(evict, "revoke");
  }

  /* Sub-case A: SIGCONT + natural exit frees quarantined CPU and queue. */
  {
    LiveTenantHandle tenant;
    ASSERT_TRUE(
        spawn_live_tenant(daemon.client_sock(), "hb03-tenant-a", 40, 3, true, &tenant));

    ASSERT_EQ(::kill(tenant.pid, SIGSTOP), 0);

    bool saw_revoked = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < deadline) {
      JsonGuard st;
      ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
      int64_t revocations = 0, qcnt = 0;
      if (json_get_i64(st.obj, "heartbeatRevocations", &revocations) &&
          json_get_i64(st.obj, "quarantinedCount", &qcnt) && revocations >= 1) {
        EXPECT_EQ(qcnt, 2);

        /* Steering children (flows, udpFilters, xskAttachments) must be stripped. */
        json_object* flows = nullptr;
        json_object* udps = nullptr;
        json_object* xsks = nullptr;
        ASSERT_TRUE(json_object_object_get_ex(st.obj, "flows", &flows));
        ASSERT_TRUE(json_object_object_get_ex(st.obj, "udpFilters", &udps));
        ASSERT_TRUE(json_object_object_get_ex(st.obj, "xskAttachments", &xsks));
        EXPECT_EQ(json_object_array_length(flows), 0u);
        EXPECT_EQ(json_object_array_length(udps), 0u);
        EXPECT_EQ(json_object_array_length(xsks), 0u);

        /* CPU and Queue must be QUARANTINED (never FREE while owner lives). */
        json_object* cpu_l = find_lease_in_array(st.obj, "leases", tenant.cpu_lease_id);
        json_object* q_l = find_lease_in_array(st.obj, "queues", tenant.queue_lease_id);
        ASSERT_NE(cpu_l, nullptr);
        ASSERT_NE(q_l, nullptr);
        std::string cpu_state, q_state;
        ASSERT_TRUE(json_get_str(cpu_l, "state", &cpu_state));
        ASSERT_TRUE(json_get_str(q_l, "state", &q_state));
        EXPECT_EQ(cpu_state, "QUARANTINED");
        EXPECT_EQ(q_state, "QUARANTINED");

        saw_revoked = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(saw_revoked);

    /* Stopped tenant is still alive (daemon did not auto-kill it). */
    int wst = 0;
    EXPECT_EQ(::waitpid(tenant.pid, &wst, WNOHANG), 0);

    /* Second client gets ResourceBusy for both quarantined CPU and queue. */
    FdGuard rival(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(rival.fd, 0);
    ASSERT_EQ(rpc_open_session_hb(rival.fd, "hb03-rival", -1, -1), 0);
    std::string ename;
    EXPECT_LT(rpc_acquire_cpu(rival.fd, tenant.cpu_id, false, nullptr, nullptr, &ename),
              0);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy");
    ename.clear();
    EXPECT_LT(
        rpc_acquire_queue(rival.fd, "eth0", tenant.queue_id, nullptr, nullptr, &ename),
        0);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy");

    /* SIGCONT + exit -> pidfd fires -> resources return to FREE. */
    ASSERT_EQ(::kill(tenant.pid, SIGCONT), 0);
    (void)write_all_fd(tenant.cmd_wr.fd, "exit\n", 5);
    while (::waitpid(tenant.pid, &wst, 0) < 0 && errno == EINTR) {
    }
    tenant.pid = -1;

    bool freed = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
      JsonGuard st;
      ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
      int64_t qcnt = -1;
      if (json_get_i64(st.obj, "quarantinedCount", &qcnt) && qcnt == 0) {
        freed = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(freed);

    /* Second client can now acquire the exact previously quarantined CPU and queue! */
    ASSERT_EQ(rpc_acquire_cpu(rival.fd, tenant.cpu_id, false), 0);
    ASSERT_EQ(rpc_acquire_queue(rival.fd, "eth0", tenant.queue_id), 0);
    /* Close rival's session cleanly so rival's leases return to FREE instead of
     * quarantining on socket close while the test process stays alive. */
    JsonGuard close_rep;
    ASSERT_EQ(
        mt_varlink_call(rival.fd, MT_VARLINK_MANAGER_INTERFACE ".CloseSession", nullptr,
                        nullptr, 0, 2000, &close_rep.obj, nullptr, 0, nullptr, 0),
        0);
  }

  /* Sub-case B: admin KillQuarantinedOwner frees quarantined resources of SIGSTOP owner.
   */
  {
    LiveTenantHandle tenant;
    ASSERT_TRUE(
        spawn_live_tenant(daemon.client_sock(), "hb03-tenant-b", 40, 3, false, &tenant));

    ASSERT_EQ(::kill(tenant.pid, SIGSTOP), 0);

    bool quarantined = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < deadline) {
      JsonGuard st;
      ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
      json_object* cpu_l = find_lease_in_array(st.obj, "leases", tenant.cpu_lease_id);
      std::string cpu_state;
      if (cpu_l && json_get_str(cpu_l, "state", &cpu_state) &&
          cpu_state == "QUARANTINED") {
        quarantined = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(quarantined);

    FdGuard afd(mt_varlink_connect_unix(daemon.admin_sock().c_str(), 2000));
    ASSERT_GE(afd.fd, 0);
    JsonGuard kparams{json_object_new_object()};
    json_object_object_add(kparams.obj, "leaseId",
                           json_object_new_string(tenant.cpu_lease_id.c_str()));
    JsonGuard krep;
    ASSERT_EQ(
        mt_varlink_call(afd.fd, MT_VARLINK_MANAGER_INTERFACE ".KillQuarantinedOwner",
                        kparams.obj, nullptr, 0, 2000, &krep.obj, nullptr, 0, nullptr, 0),
        0);

    int wst = 0;
    while (::waitpid(tenant.pid, &wst, 0) < 0 && errno == EINTR) {
    }
    tenant.pid = -1;

    bool freed = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
      JsonGuard st;
      ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
      int64_t qcnt = -1;
      if (json_get_i64(st.obj, "quarantinedCount", &qcnt) && qcnt == 0) {
        freed = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(freed);
  }
}

/* 5. ISSUE-20 & ISSUE-82: Per-session heartbeat flood protection runs BEFORE
 *    FD / session / sequence / clientState validation; 20 strikes disconnect and
 *    increment heartbeatRevocations with zero FD leaks. */
TEST(Pr2HeartbeatTest, Issue20And82_HeartbeatFloodProtectionBeforeValidation) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  /* Well-behaved bystander session stays healthy throughout all floods. */
  FdGuard good(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(good.fd, 0);
  ASSERT_EQ(rpc_open_session_hb(good.fd, "bystander", -1, -1), 0);
  ASSERT_EQ(rpc_heartbeat(good.fd, 1, "running"), 0);

  /* 5a. Pre-session Heartbeat flood. */
  {
    FdGuard attacker(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(attacker.fd, 0);

    /* Single pre-session Heartbeat returns SessionNotFound cleanly without disconnect. */
    std::string ename;
    EXPECT_LT(rpc_heartbeat(attacker.fd, 1, nullptr, nullptr, nullptr, nullptr, nullptr,
                            &ename),
              0);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".SessionNotFound");
    ASSERT_EQ(rpc_ping(attacker.fd, 1), 0);

    bool saw_rate_limited = false;
    bool disconnected = false;
    for (int i = 0; i < 60; ++i) {
      ename.clear();
      int rc = rpc_heartbeat(attacker.fd, 1, nullptr, nullptr, nullptr, nullptr, nullptr,
                             &ename);
      if (ename == MT_VARLINK_MANAGER_INTERFACE ".RateLimited") saw_rate_limited = true;
      if (rc == -EPIPE || rc == -ECONNRESET || rc == -ECONNABORTED || rc == -ENOTCONN) {
        disconnected = true;
        break;
      }
    }
    EXPECT_TRUE(saw_rate_limited);
    EXPECT_TRUE(disconnected);

    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    int64_t revocations = 0;
    ASSERT_TRUE(json_get_i64(st.obj, "heartbeatRevocations", &revocations));
    EXPECT_GE(revocations, 1);
  }

  /* 5b. FD-carrying Heartbeat flood (ISSUE-82) with zero FD leak. */
  {
    int fds_before = stable_process_fds(daemon.pid());
    ASSERT_GT(fds_before, 0);

    FdGuard attacker(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(attacker.fd, 0);
    ASSERT_EQ(rpc_open_session_hb(attacker.fd, "fd-flooder", -1, -1), 0);

    int pipe_fds[2] = {-1, -1};
    ASSERT_EQ(::pipe2(pipe_fds, O_CLOEXEC), 0);
    FdGuard prd(pipe_fds[0]);
    FdGuard pwr(pipe_fds[1]);

    /* Single FD-carrying Heartbeat rejected cleanly under normal rate limiting. */
    std::string ename;
    EXPECT_LT(rpc_heartbeat(attacker.fd, 1, "running", nullptr, nullptr, nullptr, nullptr,
                            &ename, prd.fd),
              0);
    EXPECT_EQ(ename, "org.varlink.service.InvalidParameter");

    int64_t rev_before = 0;
    {
      JsonGuard st;
      ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
      ASSERT_TRUE(json_get_i64(st.obj, "heartbeatRevocations", &rev_before));
    }

    bool saw_rate_limited = false;
    bool disconnected = false;
    for (int i = 0; i < 60; ++i) {
      ename.clear();
      int rc = rpc_heartbeat(attacker.fd, 1, "running", nullptr, nullptr, nullptr,
                             nullptr, &ename, prd.fd);
      if (ename == MT_VARLINK_MANAGER_INTERFACE ".RateLimited") saw_rate_limited = true;
      if (rc == -EPIPE || rc == -ECONNRESET || rc == -ECONNABORTED || rc == -ENOTCONN) {
        disconnected = true;
        break;
      }
    }
    EXPECT_TRUE(saw_rate_limited);
    EXPECT_TRUE(disconnected);

    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    int64_t rev_after = 0;
    ASSERT_TRUE(json_get_i64(st.obj, "heartbeatRevocations", &rev_after));
    EXPECT_GT(rev_after, rev_before);

    attacker.reset();
    int fds_after = stable_process_fds(daemon.pid());
    EXPECT_LE(fds_after, fds_before + 1);
  }

  /* 5c. Invalid-sequence Heartbeat flood (ISSUE-20). */
  {
    FdGuard attacker(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(attacker.fd, 0);
    ASSERT_EQ(rpc_open_session_hb(attacker.fd, "seq-flooder", -1, -1), 0);
    ASSERT_EQ(rpc_heartbeat(attacker.fd, 10, "running"), 0);

    int64_t rev_before = 0;
    {
      JsonGuard st;
      ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
      ASSERT_TRUE(json_get_i64(st.obj, "heartbeatRevocations", &rev_before));
    }

    bool saw_rate_limited = false;
    bool disconnected = false;
    for (int i = 0; i < 60; ++i) {
      std::string ename;
      int rc = rpc_heartbeat(attacker.fd, 1, "bad-seq", nullptr, nullptr, nullptr,
                             nullptr, &ename);
      if (ename == MT_VARLINK_MANAGER_INTERFACE ".RateLimited") saw_rate_limited = true;
      if (rc == -EPIPE || rc == -ECONNRESET || rc == -ECONNABORTED || rc == -ENOTCONN) {
        disconnected = true;
        break;
      }
    }
    EXPECT_TRUE(saw_rate_limited);
    EXPECT_TRUE(disconnected);

    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    int64_t rev_after = 0;
    ASSERT_TRUE(json_get_i64(st.obj, "heartbeatRevocations", &rev_after));
    EXPECT_GT(rev_after, rev_before);
  }

  /* Bystander session remains completely unaffected. */
  EXPECT_EQ(rpc_heartbeat(good.fd, 2, "running"), 0);
}

/* 6. ISSUE-53: Scaled stall threshold min(500, max(20, min_active_timeout_ms / 2))
 *    protects a tight-SLA 120 ms session during a 150 ms daemon pause. */
TEST(Pr2HeartbeatTest, Issue53_ScaledStallFloorProtectsTightSlaSession) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(
      tmp.path(), {"--heartbeat-min-timeout-ms", "50", "--heartbeat-policy=revoke",
                   "--heartbeat-stall-grace-ms", "2000"}));

  FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(cfd.fd, 0);
  std::string sid;
  /* 40 ms * 3 = 120 ms timeout -> scaled stall threshold = 60 ms (< 150 ms pause). */
  ASSERT_EQ(rpc_open_session_hb(cfd.fd, "tight-sla", 40, 3, &sid), 0);
  ASSERT_EQ(rpc_heartbeat(cfd.fd, 1, "running"), 0);

  ASSERT_EQ(::kill(daemon.pid(), SIGSTOP), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ASSERT_EQ(::kill(daemon.pid(), SIGCONT), 0);

  std::string hb_st;
  ASSERT_EQ(rpc_heartbeat(cfd.fd, 2, "running", nullptr, nullptr, &hb_st), 0);
  EXPECT_EQ(hb_st, "ok");

  JsonGuard st;
  ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
  int64_t stalls = 0, warnings = -1, revocations = -1;
  ASSERT_TRUE(json_get_i64(st.obj, "managerStallEvents", &stalls));
  ASSERT_TRUE(json_get_i64(st.obj, "heartbeatWarnings", &warnings));
  ASSERT_TRUE(json_get_i64(st.obj, "heartbeatRevocations", &revocations));
  EXPECT_GE(stalls, 1);
  EXPECT_EQ(warnings, 0);
  EXPECT_EQ(revocations, 0);
}

/* 7. Policy file heartbeat configuration and validation. */
TEST(Pr2HeartbeatTest, PolicyFileHeartbeatConfig) {
  TempDir tmp;
  std::string pol_path = tmp.path() + "/policy.json";
  {
    std::ofstream f(pol_path);
    f << R"({
      "allowed_cpus": "0-3",
      "allowed_interfaces": ["eth0"],
      "allowed_udp_ports": [[20000, 20099]],
      "default_quota": {"max_cpus": 4, "max_queues": 4, "max_flows": 8},
      "heartbeat": {
        "expiry_policy": "revoke",
        "eviction_action": "revoke",
        "default_interval_ms": 50,
        "default_missed_beats": 3,
        "min_timeout_ms": 100,
        "stall_grace_ms": 1500
      }
    })";
  }
  ASSERT_EQ(::chmod(pol_path.c_str(), 0640), 0);

  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path(), {"--policy-file", pol_path}));

  JsonGuard st;
  ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
  std::string pol, evict;
  ASSERT_TRUE(json_get_str(st.obj, "heartbeatPolicy", &pol));
  ASSERT_TRUE(json_get_str(st.obj, "heartbeatEvictionAction", &evict));
  EXPECT_EQ(pol, "revoke");
  EXPECT_EQ(evict, "revoke");

  FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(cfd.fd, 0);
  int64_t eff_interval = -1, eff_missed = -1;
  ASSERT_EQ(rpc_open_session_hb(cfd.fd, "pol-tenant", -1, -1, nullptr, &eff_interval,
                                &eff_missed),
            0);
  EXPECT_EQ(eff_interval, 50);
  EXPECT_EQ(eff_missed, 3);
}

}  // namespace
