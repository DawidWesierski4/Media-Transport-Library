/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <json-c/json.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef MTL_HAS_XDP_BACKEND
#include <bpf/libbpf.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "mt_varlink.h"
#include "mtlm2_journal.hpp"
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
    std::string tmpl = "/tmp/mtlm2_pr1b_test_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* created = ::mkdtemp(buf.data());
    EXPECT_NE(created, nullptr);
    if (created) path_ = created;
  }
  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      fs::permissions(path_, fs::perms::owner_all, fs::perm_options::replace, ec);
      for (const auto& entry : fs::recursive_directory_iterator(path_, ec)) {
        fs::permissions(entry.path(), fs::perms::owner_all, fs::perm_options::add, ec);
      }
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

    /* Wait up to 2 s for socket readiness or early child exit. */
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
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
  const std::string& state_dir() const {
    return state_dir_;
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

int rpc_open_session(int fd, const char* name, std::string* sid_out = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "clientName", json_object_new_string(name));
  JsonGuard reply;
  int rc = mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".OpenSession", params.obj,
                           nullptr, 0, 2000, &reply.obj, nullptr, 0, nullptr, 0);
  if (rc == 0 && sid_out && reply.obj) {
    json_object* p = nullptr;
    json_object* s = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) &&
        json_object_object_get_ex(p, "sessionId", &s))
      *sid_out = json_object_get_string(s);
  }
  return rc;
}

int rpc_acquire_queue(int fd, const char* if_name, int64_t preferred_queue,
                      std::string* lid_out = nullptr, int64_t* qid_out = nullptr,
                      int64_t* ifindex_out = nullptr, std::string* err_name = nullptr) {
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
                      nullptr, 0, 2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && reply.obj) {
    json_object* p = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p) {
      json_object* v = nullptr;
      if (lid_out && json_object_object_get_ex(p, "leaseId", &v))
        *lid_out = json_object_get_string(v);
      if (qid_out && json_object_object_get_ex(p, "queueId", &v))
        *qid_out = json_object_get_int64(v);
      if (ifindex_out && json_object_object_get_ex(p, "ifindex", &v))
        *ifindex_out = json_object_get_int64(v);
    }
  }
  return rc;
}

int rpc_attach_xsk(int fd, const std::string& queue_lease, int xsk_fd,
                   std::string* lid_out = nullptr, std::string* err_name = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "queueLease",
                         json_object_new_string(queue_lease.c_str()));
  JsonGuard reply;
  char ename[128] = {};
  const int* fds = (xsk_fd >= 0) ? &xsk_fd : nullptr;
  size_t fd_cnt = (xsk_fd >= 0) ? 1u : 0u;
  int rc = mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AttachXsk", params.obj, fds,
                           fd_cnt, 2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && lid_out && reply.obj) {
    json_object* p = nullptr;
    json_object* v = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p &&
        json_object_object_get_ex(p, "leaseId", &v))
      *lid_out = json_object_get_string(v);
  }
  return rc;
}

int rpc_add_flow(int fd, const std::string& queue_lease, const char* protocol,
                 const char* src_ip, const char* dst_ip, int src_port, int dst_port,
                 std::string* lid_out = nullptr, int64_t* loc_out = nullptr,
                 std::string* err_name = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "queueLease",
                         json_object_new_string(queue_lease.c_str()));
  if (protocol)
    json_object_object_add(params.obj, "protocol", json_object_new_string(protocol));
  if (src_ip) json_object_object_add(params.obj, "srcIp", json_object_new_string(src_ip));
  if (dst_ip) json_object_object_add(params.obj, "dstIp", json_object_new_string(dst_ip));
  if (src_port >= 0)
    json_object_object_add(params.obj, "srcPort", json_object_new_int64(src_port));
  json_object_object_add(params.obj, "dstPort", json_object_new_int64(dst_port));
  JsonGuard reply;
  char ename[128] = {};
  int rc =
      mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AddFlow", params.obj, nullptr, 0,
                      2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && reply.obj) {
    json_object* p = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p) {
      json_object* v = nullptr;
      if (lid_out && json_object_object_get_ex(p, "leaseId", &v))
        *lid_out = json_object_get_string(v);
      if (loc_out && json_object_object_get_ex(p, "ruleLocation", &v))
        *loc_out = json_object_get_int64(v);
    }
  }
  return rc;
}

int rpc_add_udp_filter(int fd, const std::string& queue_lease, int dst_port,
                       std::string* lid_out = nullptr, std::string* err_name = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "queueLease",
                         json_object_new_string(queue_lease.c_str()));
  json_object_object_add(params.obj, "destinationPort", json_object_new_int64(dst_port));
  JsonGuard reply;
  char ename[128] = {};
  int rc =
      mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AddUdpFilter", params.obj,
                      nullptr, 0, 2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && lid_out && reply.obj) {
    json_object* p = nullptr;
    json_object* v = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p) && p &&
        json_object_object_get_ex(p, "leaseId", &v))
      *lid_out = json_object_get_string(v);
  }
  return rc;
}

int rpc_release_lease(int fd, const std::string& lid, std::string* err_name = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "leaseId", json_object_new_string(lid.c_str()));
  JsonGuard reply;
  char ename[128] = {};
  int rc =
      mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease", params.obj,
                      nullptr, 0, 2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  return rc;
}

json_object* rpc_get_status(const std::string& admin_sock) {
  FdGuard afd(mt_varlink_connect_unix(admin_sock.c_str(), 2000));
  if (afd.fd < 0) return nullptr;
  json_object* reply = nullptr;
  int rc = mt_varlink_call(afd.fd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr,
                           nullptr, 0, 2000, &reply, nullptr, 0, nullptr, 0);
  if (rc != 0) {
    if (reply) json_object_put(reply);
    return nullptr;
  }
  return reply;
}

json_object* find_backend_iface(json_object* status_reply, const char* if_name) {
  if (!status_reply) return nullptr;
  json_object* params = nullptr;
  json_object* arr = nullptr;
  if (!json_object_object_get_ex(status_reply, "parameters", &params) || !params ||
      !json_object_object_get_ex(params, "backendInterfaces", &arr) || !arr ||
      !json_object_is_type(arr, json_type_array))
    return nullptr;
  size_t n = json_object_array_length(arr);
  for (size_t i = 0; i < n; ++i) {
    json_object* item = json_object_array_get_idx(arr, i);
    json_object* jn = nullptr;
    if (item && json_object_object_get_ex(item, "ifName", &jn) && jn &&
        std::strcmp(json_object_get_string(jn), if_name) == 0)
      return item;
  }
  return nullptr;
}

bool backend_has_flow_location(json_object* iface_obj, int64_t loc, bool expect_mtl) {
  if (!iface_obj) return false;
  json_object* rules = nullptr;
  if (!json_object_object_get_ex(iface_obj, "flowRules", &rules) || !rules ||
      !json_object_is_type(rules, json_type_array))
    return false;
  size_t n = json_object_array_length(rules);
  for (size_t i = 0; i < n; ++i) {
    json_object* r = json_object_array_get_idx(rules, i);
    json_object* jloc = nullptr;
    json_object* jmtl = nullptr;
    if (r && json_object_object_get_ex(r, "location", &jloc) &&
        json_object_object_get_ex(r, "mtlOwned", &jmtl)) {
      if (json_object_get_int64(jloc) == loc &&
          (json_object_get_boolean(jmtl) != 0) == expect_mtl)
        return true;
    }
  }
  return false;
}

bool backend_has_udp_filter(json_object* iface_obj, int64_t qid, int64_t port) {
  if (!iface_obj) return false;
  json_object* filters = nullptr;
  if (!json_object_object_get_ex(iface_obj, "udpFilters", &filters) || !filters ||
      !json_object_is_type(filters, json_type_array))
    return false;
  size_t n = json_object_array_length(filters);
  for (size_t i = 0; i < n; ++i) {
    json_object* f = json_object_array_get_idx(filters, i);
    json_object* jq = nullptr;
    json_object* jp = nullptr;
    if (f && json_object_object_get_ex(f, "queueId", &jq) &&
        json_object_object_get_ex(f, "dstPort", &jp)) {
      if (json_object_get_int64(jq) == qid && json_object_get_int64(jp) == port)
        return true;
    }
  }
  return false;
}

bool backend_has_xsk_queue(json_object* iface_obj, int64_t qid) {
  if (!iface_obj) return false;
  json_object* xsks = nullptr;
  if (!json_object_object_get_ex(iface_obj, "xskQueues", &xsks) || !xsks ||
      !json_object_is_type(xsks, json_type_array))
    return false;
  size_t n = json_object_array_length(xsks);
  for (size_t i = 0; i < n; ++i) {
    json_object* jq = json_object_array_get_idx(xsks, i);
    if (jq && json_object_get_int64(jq) == qid) return true;
  }
  return false;
}

bool send_raw_with_fds(int sock_fd, const void* data, size_t len, const int* fds,
                       size_t fd_count) {
  struct iovec iov{};
  iov.iov_base = const_cast<void*>(data);
  iov.iov_len = len;

  struct msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  std::vector<unsigned char> cbuf;
  if (fd_count > 0) {
    cbuf.resize(CMSG_SPACE(sizeof(int) * fd_count), 0);
    msg.msg_control = cbuf.data();
    msg.msg_controllen = cbuf.size();
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
    std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
  }
  ssize_t n = ::sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
  return n == static_cast<ssize_t>(len);
}

/* =========================================================================
 * 1. BpfObjectValidElf: Evolved BSD-3 mtl.xdp.o ELF64 BPF & 8-byte key
 * ========================================================================= */

TEST(BpfObjectTest, BpfObjectValidElfAndQueueScopedKeySize8) {
#ifndef MTLM2_BPF_OBJ_PATH
  GTEST_SKIP() << "BPF build disabled (clang/libbpf/libxdp unavailable)";
#else
  ASSERT_TRUE(fs::exists(MTLM2_BPF_OBJ_PATH)) << "Missing " << MTLM2_BPF_OBJ_PATH;

  /* Verify ELF64 little-endian Linux BPF header. */
  std::ifstream in(MTLM2_BPF_OBJ_PATH, std::ios::binary);
  ASSERT_TRUE(in.good());
  Elf64_Ehdr ehdr{};
  in.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
  ASSERT_EQ(in.gcount(), static_cast<std::streamsize>(sizeof(ehdr)));
  EXPECT_EQ(std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG), 0);
  EXPECT_EQ(ehdr.e_ident[EI_CLASS], ELFCLASS64);
  EXPECT_EQ(ehdr.e_ident[EI_DATA], ELFDATA2LSB);
  EXPECT_EQ(ehdr.e_machine, EM_BPF);

#ifdef MTL_HAS_XDP_BACKEND
  struct bpf_object* obj = bpf_object__open_file(MTLM2_BPF_OBJ_PATH, nullptr);
  ASSERT_NE(obj, nullptr);
  struct bpf_program* prog = bpf_object__find_program_by_name(obj, "mtl_dp_filter");
  EXPECT_NE(prog, nullptr);
  struct bpf_map* map = bpf_object__find_map_by_name(obj, "udp4_dp_filter");
  ASSERT_NE(map, nullptr);
  /* struct mtl_queue_port_key { __u32 rx_queue_index; __u16 dst_port; __u16 pad; } */
  EXPECT_EQ(bpf_map__key_size(map), 8u);
  EXPECT_EQ(bpf_map__value_size(map), 1u);
  EXPECT_EQ(bpf_map__max_entries(map), 1024u);
  bpf_object__close(obj);
#endif
#endif
}

/* =========================================================================
 * 2. RES-Q-01: Queue 0 permanently protected, exhaustion -> ResourceBusy
 * ========================================================================= */

TEST(Pr1bQueueTest, ResQ01_QueueZeroProtectedAndExhaustionReturnsResourceBusy) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(cfd.fd, 0);
  ASSERT_EQ(rpc_open_session(cfd.fd, "tenant-q0"), 0);

  /* Explicit request for queue 0 must be rejected. */
  std::string err_name;
  EXPECT_LT(rpc_acquire_queue(cfd.fd, "eth0", 0, nullptr, nullptr, nullptr, &err_name),
            0);
  EXPECT_NE(err_name, "");

  /* Mock eth0 has 8 queues (0..7); queue 0 is protected so exactly 7 succeed (1..7). */
  std::set<int64_t> queues;
  std::vector<std::string> leases;
  for (int i = 0; i < 7; ++i) {
    std::string lid;
    int64_t qid = -1;
    int64_t ifindex = -1;
    ASSERT_EQ(rpc_acquire_queue(cfd.fd, "eth0", -1, &lid, &qid, &ifindex, &err_name), 0)
        << "AcquireQueue #" << i << " failed: " << err_name;
    EXPECT_FALSE(lid.empty());
    EXPECT_GT(ifindex, 0);
    EXPECT_GT(qid, 0) << "Queue 0 must never be allocated";
    EXPECT_LT(qid, 8);
    EXPECT_TRUE(queues.insert(qid).second) << "Duplicate queue ID allocated: " << qid;
    leases.push_back(lid);
  }
  EXPECT_EQ(queues.count(0), 0u);
  EXPECT_EQ(queues.size(), 7u);

  /* 8th AcquireQueue must return ResourceBusy. */
  err_name.clear();
  EXPECT_EQ(rpc_acquire_queue(cfd.fd, "eth0", -1, nullptr, nullptr, nullptr, &err_name),
            -EBUSY);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy");

  /* Releasing one queue frees it for re-acquisition. */
  ASSERT_EQ(rpc_release_lease(cfd.fd, leases.front()), 0);
  std::string lid2;
  int64_t qid2 = -1;
  EXPECT_EQ(rpc_acquire_queue(cfd.fd, "eth0", -1, &lid2, &qid2), 0);
  EXPECT_GT(qid2, 0);
}

/* =========================================================================
 * 3. RES-Q-02: Foreign flow rule at location 10 preserved across add/del/SIGKILL/restart
 * ========================================================================= */

TEST(Pr1bFlowTest, ResQ02_ForeignFlowRuleAtLocation10Preserved) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  /* Verify foreign rule at location 10 exists before any tenant runs. */
  {
    JsonGuard st{rpc_get_status(daemon.admin_sock())};
    ASSERT_NE(st.obj, nullptr);
    json_object* eth0 = find_backend_iface(st.obj, "eth0");
    ASSERT_NE(eth0, nullptr);
    EXPECT_TRUE(backend_has_flow_location(eth0, 10, /*expect_mtl=*/false));
  }

  /* Spawn a child tenant that acquires a queue, adds a flow, and gets SIGKILLed. */
  int ready_pipe[2] = {-1, -1};
  ASSERT_EQ(::pipe2(ready_pipe, O_CLOEXEC), 0);
  FdGuard rp_r(ready_pipe[0]), rp_w(ready_pipe[1]);

  pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    rp_r.reset();
    int cfd = mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000);
    if (cfd < 0) ::_exit(10);
    if (rpc_open_session(cfd, "tenant-foreign-test") != 0) ::_exit(11);
    std::string qlid, flid;
    int64_t loc = -1;
    if (rpc_acquire_queue(cfd, "eth0", -1, &qlid) != 0) ::_exit(12);
    if (rpc_add_flow(cfd, qlid, "udp4", nullptr, "239.1.1.10", -1, 20010, &flid, &loc) !=
        0)
      ::_exit(13);
    if (loc == 10) ::_exit(14);
    char ok = '1';
    if (::write(rp_w.fd, &ok, 1) != 1) ::_exit(15);
    for (;;) ::pause();
  }
  rp_w.reset();

  char ch = 0;
  ASSERT_EQ(::read(rp_r.fd, &ch, 1), 1);
  ASSERT_EQ(ch, '1');

  /* Kill daemon via SIGKILL while child is alive, then kill child, then restart daemon
   * so startup reconciliation cleans dead owner's flow without touching location 10. */
  daemon.stop(SIGKILL);
  (void)::kill(child, SIGKILL);
  int cst = 0;
  ::waitpid(child, &cst, 0);

  ASSERT_TRUE(daemon.start(tmp.path()));

  /* Foreign rule at location 10 MUST still be present; dead MTL flow MUST be gone. */
  {
    JsonGuard st{rpc_get_status(daemon.admin_sock())};
    ASSERT_NE(st.obj, nullptr);
    json_object* eth0 = find_backend_iface(st.obj, "eth0");
    ASSERT_NE(eth0, nullptr);
    EXPECT_TRUE(backend_has_flow_location(eth0, 10, /*expect_mtl=*/false));
    json_object* rules = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(eth0, "flowRules", &rules));
    EXPECT_EQ(json_object_array_length(rules), 1u);
  }
}

/* =========================================================================
 * 4. RES-Q-03 (ISSUE-52, ISSUE-88): Duplicate cross-session flow -> ResourceBusy
 * ========================================================================= */

TEST(Pr1bFlowTest,
     ResQ03_DuplicateCrossSessionFlowReturnsResourceBusyAndRejectsEmptySrcIp) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  FdGuard fdA(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  FdGuard fdB(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(fdA.fd, 0);
  ASSERT_GE(fdB.fd, 0);
  ASSERT_EQ(rpc_open_session(fdA.fd, "tenantA"), 0);
  ASSERT_EQ(rpc_open_session(fdB.fd, "tenantB"), 0);

  std::string qA, qB;
  ASSERT_EQ(rpc_acquire_queue(fdA.fd, "eth0", -1, &qA), 0);
  ASSERT_EQ(rpc_acquire_queue(fdB.fd, "eth0", -1, &qB), 0);

  /* ISSUE-88: explicit srcIp == "" must be rejected with InvalidParameter. */
  std::string err_name;
  EXPECT_EQ(rpc_add_flow(fdA.fd, qA, "udp4", "", "239.2.2.2", -1, 20000, nullptr, nullptr,
                         &err_name),
            -EINVAL);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");

  /* Tenant A installs valid udp4 flow. */
  std::string fA;
  int64_t locA = -1;
  ASSERT_EQ(rpc_add_flow(fdA.fd, qA, "udp4", nullptr, "239.2.2.2", -1, 20000, &fA, &locA),
            0);
  EXPECT_FALSE(fA.empty());
  EXPECT_NE(locA, 10);

  /* Tenant B tries to install duplicate flow tuple on same interface -> ResourceBusy. */
  err_name.clear();
  EXPECT_EQ(rpc_add_flow(fdB.fd, qB, nullptr, nullptr, "239.2.2.2", -1, 20000, nullptr,
                         nullptr, &err_name),
            -EBUSY);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy");

  /* Same tuple on a DIFFERENT interface (eth1) is allowed. */
  std::string qB1, fB1;
  ASSERT_EQ(rpc_acquire_queue(fdB.fd, "eth1", -1, &qB1), 0);
  EXPECT_EQ(rpc_add_flow(fdB.fd, qB1, "udp4", nullptr, "239.2.2.2", -1, 20000, &fB1), 0);
}

/* =========================================================================
 * 5. RES-Q-04: Cross-session AddFlow / AddUdpFilter / AttachXsk -> LeaseNotOwned
 * ========================================================================= */

TEST(Pr1bSecurityTest, ResQ04_CrossSessionQueueOperationsReturnLeaseNotOwned) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  FdGuard fdA(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  FdGuard fdB(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(fdA.fd, 0);
  ASSERT_GE(fdB.fd, 0);
  ASSERT_EQ(rpc_open_session(fdA.fd, "tenantA"), 0);
  ASSERT_EQ(rpc_open_session(fdB.fd, "tenantB"), 0);

  std::string qA;
  ASSERT_EQ(rpc_acquire_queue(fdA.fd, "eth0", -1, &qA), 0);

  std::string err_name;
  EXPECT_EQ(rpc_add_flow(fdB.fd, qA, "udp4", nullptr, "239.3.3.3", -1, 20003, nullptr,
                         nullptr, &err_name),
            -ENOENT);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned");

  err_name.clear();
  EXPECT_EQ(rpc_add_udp_filter(fdB.fd, qA, 20003, nullptr, &err_name), -ENOENT);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned");

  /* Same LeaseNotOwned for random non-existent lease ID (zero oracle). */
  err_name.clear();
  EXPECT_EQ(
      rpc_add_udp_filter(fdB.fd, "que-000000000000-ffff", 20003, nullptr, &err_name),
      -ENOENT);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned");

  /* Cross-session AttachXsk also returns LeaseNotOwned and closes the FD. */
  int sv[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv), 0);
  FdGuard s0(sv[0]), s1(sv[1]);
  err_name.clear();
  EXPECT_EQ(rpc_attach_xsk(fdB.fd, qA, s0.fd, nullptr, &err_name), -ENOENT);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned");

  /* Verify no UDP filter and no MTL flow rule were added. */
  JsonGuard st{rpc_get_status(daemon.admin_sock())};
  ASSERT_NE(st.obj, nullptr);
  json_object* eth0 = find_backend_iface(st.obj, "eth0");
  ASSERT_NE(eth0, nullptr);
  EXPECT_FALSE(backend_has_udp_filter(eth0, 1, 20003));
}

/* =========================================================================
 * 6. RES-Q-05: Queue-scoped {rx_queue_index, dst_port} UDP filter isolation
 * ========================================================================= */

TEST(Pr1bUdpFilterTest, ResQ05_QueueScopedUdpFilterIsolationBetweenTenants) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  FdGuard fdA(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  FdGuard fdB(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(fdA.fd, 0);
  ASSERT_GE(fdB.fd, 0);
  ASSERT_EQ(rpc_open_session(fdA.fd, "tenantA"), 0);
  ASSERT_EQ(rpc_open_session(fdB.fd, "tenantB"), 0);

  std::string qA, qB;
  int64_t qidA = -1, qidB = -1;
  ASSERT_EQ(rpc_acquire_queue(fdA.fd, "eth0", 1, &qA, &qidA), 0);
  ASSERT_EQ(rpc_acquire_queue(fdB.fd, "eth0", 2, &qB, &qidB), 0);
  ASSERT_EQ(qidA, 1);
  ASSERT_EQ(qidB, 2);

  std::string uA, uB;
  ASSERT_EQ(rpc_add_udp_filter(fdA.fd, qA, 20000, &uA), 0);
  ASSERT_EQ(rpc_add_udp_filter(fdB.fd, qB, 20001, &uB), 0);

  /* Tenant A has (q1, 20000), Tenant B has (q2, 20001); (q2, 20000) MUST NOT exist. */
  {
    JsonGuard st{rpc_get_status(daemon.admin_sock())};
    ASSERT_NE(st.obj, nullptr);
    json_object* eth0 = find_backend_iface(st.obj, "eth0");
    ASSERT_NE(eth0, nullptr);
    EXPECT_TRUE(backend_has_udp_filter(eth0, 1, 20000));
    EXPECT_FALSE(backend_has_udp_filter(eth0, 2, 20000));
    EXPECT_TRUE(backend_has_udp_filter(eth0, 2, 20001));
    EXPECT_FALSE(backend_has_udp_filter(eth0, 1, 20001));
  }

  /* Tenant B can also independently register port 20000 on queue 2 if desired, and
   * releasing Tenant A's filter leaves Tenant B's entries intact. */
  ASSERT_EQ(rpc_release_lease(fdA.fd, uA), 0);
  {
    JsonGuard st{rpc_get_status(daemon.admin_sock())};
    ASSERT_NE(st.obj, nullptr);
    json_object* eth0 = find_backend_iface(st.obj, "eth0");
    ASSERT_NE(eth0, nullptr);
    EXPECT_FALSE(backend_has_udp_filter(eth0, 1, 20000));
    EXPECT_TRUE(backend_has_udp_filter(eth0, 2, 20001));
  }
}

/* =========================================================================
 * 7. RES-Q-06: AddFlow with udp6 returns UnsupportedFlowType
 * ========================================================================= */

TEST(Pr1bFlowTest, ResQ06_AddFlowRejectsUdp6WithUnsupportedFlowType) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(cfd.fd, 0);
  ASSERT_EQ(rpc_open_session(cfd.fd, "tenant-v6"), 0);

  std::string qlid;
  ASSERT_EQ(rpc_acquire_queue(cfd.fd, "eth0", -1, &qlid), 0);

  std::string err_name;
  EXPECT_EQ(rpc_add_flow(cfd.fd, qlid, "udp6", nullptr, "ff02::1", -1, 20000, nullptr,
                         nullptr, &err_name),
            -ENOTSUP);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".UnsupportedFlowType");
}

/* =========================================================================
 * 8. RES-Q-07 (ISSUE-32): Read-only --state-dir rolls back all mutating RPCs
 * ========================================================================= */

TEST(Pr1bJournalRollbackTest, ResQ07_ReadOnlyStateDirRollsBackQueueFlowUdpAndXsk) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(cfd.fd, 0);
  ASSERT_EQ(rpc_open_session(cfd.fd, "tenant-ro"), 0);

  /* Acquire one queue on eth0 while state-dir is writable so we can test child ops. */
  std::string q0;
  int64_t qid0 = -1;
  ASSERT_EQ(rpc_acquire_queue(cfd.fd, "eth0", -1, &q0, &qid0), 0);

  /* Make state-dir read-only (0500) so journal.save() fails on temp file creation. */
  ASSERT_EQ(::chmod(daemon.state_dir().c_str(), 0500), 0);

  /* 1) AcquireQueue on eth1 must fail and roll back lazy XDP attach on eth1. */
  std::string err_name;
  EXPECT_LT(rpc_acquire_queue(cfd.fd, "eth1", -1, nullptr, nullptr, nullptr, &err_name),
            0);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".InternalError");

  /* 2) AddFlow on q0 must fail and roll back NIC rule. */
  err_name.clear();
  EXPECT_LT(rpc_add_flow(cfd.fd, q0, "udp4", nullptr, "239.7.7.7", -1, 20007, nullptr,
                         nullptr, &err_name),
            0);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".InternalError");

  /* 3) AddUdpFilter on q0 must fail and roll back BPF map entry. */
  err_name.clear();
  EXPECT_LT(rpc_add_udp_filter(cfd.fd, q0, 20007, nullptr, &err_name), 0);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".InternalError");

  /* 4) AttachXsk on q0 must fail and roll back xsks_map entry. */
  int sv[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv), 0);
  FdGuard s0(sv[0]), s1(sv[1]);
  err_name.clear();
  EXPECT_LT(rpc_attach_xsk(cfd.fd, q0, s0.fd, nullptr, &err_name), 0);
  EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".InternalError");

  /* Verify backend has ZERO leaked mutations on eth0 and eth1. */
  {
    JsonGuard st{rpc_get_status(daemon.admin_sock())};
    ASSERT_NE(st.obj, nullptr);
    json_object* eth0 = find_backend_iface(st.obj, "eth0");
    json_object* eth1 = find_backend_iface(st.obj, "eth1");
    ASSERT_NE(eth0, nullptr);
    ASSERT_NE(eth1, nullptr);
    json_object* xdp1 = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(eth1, "xdpMode", &xdp1));
    EXPECT_STREQ(json_object_get_string(xdp1), "none");
    EXPECT_FALSE(backend_has_udp_filter(eth0, qid0, 20007));
    EXPECT_FALSE(backend_has_xsk_queue(eth0, qid0));
    json_object* rules = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(eth0, "flowRules", &rules));
    EXPECT_EQ(json_object_array_length(rules),
              1u); /* Only foreign rule at location 10. */
  }

  /* Restore permissions and verify normal operations work again. */
  ASSERT_EQ(::chmod(daemon.state_dir().c_str(), 0700), 0);
  std::string flid, ulid, xlid;
  EXPECT_EQ(rpc_add_flow(cfd.fd, q0, "udp4", nullptr, "239.7.7.7", -1, 20007, &flid), 0);
  EXPECT_EQ(rpc_add_udp_filter(cfd.fd, q0, 20007, &ulid), 0);
  EXPECT_EQ(rpc_attach_xsk(cfd.fd, q0, s0.fd, &xlid), 0);
}

/* =========================================================================
 * 9. SEC-WIRE-05 (ISSUE-17): AttachXsk SCM_RIGHTS framing abuse + 0 FD leak
 * ========================================================================= */

TEST(Pr1bWireSecurityTest, SecWire05_AttachXskFramingAbuseRejectedWithZeroFdLeak) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  int baseline_fds = stable_process_fds(daemon.pid());
  ASSERT_GT(baseline_fds, 0);

  {
    FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(cfd.fd, 0);
    ASSERT_EQ(rpc_open_session(cfd.fd, "tenant-wire05"), 0);
    std::string qlid;
    ASSERT_EQ(rpc_acquire_queue(cfd.fd, "eth0", -1, &qlid), 0);

    /* Case A: AttachXsk with 0 FDs -> rejected with InvalidParameter. */
    std::string err_name;
    EXPECT_EQ(rpc_attach_xsk(cfd.fd, qlid, -1, nullptr, &err_name), -EINVAL);
    EXPECT_EQ(err_name, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");

    /* Case B: AttachXsk with 2 FDs in SCM_RIGHTS -> rejected + both FDs closed. */
    {
      FdGuard bad_fd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
      ASSERT_GE(bad_fd.fd, 0);
      int p1[2] = {-1, -1}, p2[2] = {-1, -1};
      ASSERT_EQ(::pipe2(p1, O_CLOEXEC), 0);
      ASSERT_EQ(::pipe2(p2, O_CLOEXEC), 0);
      FdGuard g1(p1[0]), g2(p1[1]), g3(p2[0]), g4(p2[1]);
      std::string frame =
          R"({"method":"org.openvisualcloud.mtl.Manager.AttachXsk","parameters":{"queueLease":")" +
          qlid + R"("}})";
      frame.push_back('\0');
      int two_fds[2] = {p1[0], p2[0]};
      ASSERT_TRUE(send_raw_with_fds(bad_fd.fd, frame.data(), frame.size(), two_fds, 2));
      char buf[512];
      struct pollfd pfd = {bad_fd.fd, POLLIN, 0};
      EXPECT_GT(::poll(&pfd, 1, 1000), 0);
      ssize_t rn = ::recv(bad_fd.fd, buf, sizeof(buf) - 1, 0);
      EXPECT_GE(rn, 0);
      if (rn > 0) {
        buf[rn] = '\0';
        EXPECT_NE(std::strstr(buf, "error"), nullptr);
      }
    }

    /* Case C: Coalesced pipelined frames with FD attached -> rejected (ISSUE-17). */
    {
      FdGuard bad_fd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
      ASSERT_GE(bad_fd.fd, 0);
      int p1[2] = {-1, -1};
      ASSERT_EQ(::pipe2(p1, O_CLOEXEC), 0);
      FdGuard g1(p1[0]), g2(p1[1]);
      std::string f1 = R"({"method":"org.varlink.service.GetInfo","parameters":{}})";
      std::string f2 =
          R"({"method":"org.openvisualcloud.mtl.Manager.AttachXsk","parameters":{"queueLease":")" +
          qlid + R"("}})";
      std::string coalesced = f1;
      coalesced.push_back('\0');
      coalesced += f2;
      coalesced.push_back('\0');
      ASSERT_TRUE(
          send_raw_with_fds(bad_fd.fd, coalesced.data(), coalesced.size(), &p1[0], 1));
      char buf[512];
      struct pollfd pfd = {bad_fd.fd, POLLIN, 0};
      EXPECT_GT(::poll(&pfd, 1, 1000), 0);
      ssize_t rn = ::recv(bad_fd.fd, buf, sizeof(buf) - 1, 0);
      EXPECT_GE(rn, 0);
      if (rn > 0) {
        buf[rn] = '\0';
        EXPECT_NE(std::strstr(buf, "error"), nullptr);
      }
    }

    /* Case D: Split frame where FD arrives mid-frame -> rejected (ISSUE-17). */
    {
      FdGuard bad_fd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
      ASSERT_GE(bad_fd.fd, 0);
      int p1[2] = {-1, -1};
      ASSERT_EQ(::pipe2(p1, O_CLOEXEC), 0);
      FdGuard g1(p1[0]), g2(p1[1]);
      std::string part1 = R"({"method":"org.openvisualcloud.mtl.Manager.AttachXsk",)";
      std::string part2 = R"("parameters":{"queueLease":")" + qlid + R"("}})";
      part2.push_back('\0');
      ASSERT_TRUE(send_raw_with_fds(bad_fd.fd, part1.data(), part1.size(), nullptr, 0));
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      ASSERT_TRUE(send_raw_with_fds(bad_fd.fd, part2.data(), part2.size(), &p1[0], 1));
      char buf[512];
      struct pollfd pfd = {bad_fd.fd, POLLIN, 0};
      EXPECT_GT(::poll(&pfd, 1, 1000), 0);
      ssize_t rn = ::recv(bad_fd.fd, buf, sizeof(buf) - 1, 0);
      EXPECT_GE(rn, 0);
      if (rn > 0) {
        buf[rn] = '\0';
        EXPECT_NE(std::strstr(buf, "error"), nullptr);
      }
    }

    /* Cleanly close session so its queue lease + pidfd are freed. */
    JsonGuard rep;
    ASSERT_EQ(
        mt_varlink_call(cfd.fd, MT_VARLINK_MANAGER_INTERFACE ".CloseSession", nullptr,
                        nullptr, 0, 2000, &rep.obj, nullptr, 0, nullptr, 0),
        0);
  }

  /* Wait briefly for daemon epoll to reap closed client connections, then verify 0 FD
   * leak in /proc/<daemon_pid>/fd. */
  int after_fds = -1;
  for (int i = 0; i < 40; ++i) {
    after_fds = count_process_fds(daemon.pid());
    if (after_fds == baseline_fds) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_EQ(after_fds, baseline_fds) << "Daemon leaked FDs after SCM_RIGHTS abuse";
}

/* =========================================================================
 * 10. SEC-WIRE-06: AttachXsk rejects wrong FD types and closes FD
 * ========================================================================= */

TEST(Pr1bWireSecurityTest, SecWire06_AttachXskRejectsNonXdpFdTypesAndClosesFd) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  int baseline_fds = stable_process_fds(daemon.pid());
  ASSERT_GT(baseline_fds, 0);

  {
    FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(cfd.fd, 0);
    ASSERT_EQ(rpc_open_session(cfd.fd, "tenant-wire06"), 0);
    std::string qlid;
    int64_t qid = -1;
    ASSERT_EQ(rpc_acquire_queue(cfd.fd, "eth0", -1, &qlid, &qid), 0);

    /* 1) TCP socket */
    {
      FdGuard tcp(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      ASSERT_GE(tcp.fd, 0);
      std::string ename;
      EXPECT_LT(rpc_attach_xsk(cfd.fd, qlid, tcp.fd, nullptr, &ename), 0);
      EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");
    }

    /* 2) UNIX stream socket */
    {
      int sv[2] = {-1, -1};
      ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), 0);
      FdGuard s0(sv[0]), s1(sv[1]);
      std::string ename;
      EXPECT_LT(rpc_attach_xsk(cfd.fd, qlid, s0.fd, nullptr, &ename), 0);
      EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");
    }

    /* 3) Pipe */
    {
      int p[2] = {-1, -1};
      ASSERT_EQ(::pipe2(p, O_CLOEXEC), 0);
      FdGuard r(p[0]), w(p[1]);
      std::string ename;
      EXPECT_LT(rpc_attach_xsk(cfd.fd, qlid, r.fd, nullptr, &ename), 0);
      EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");
    }

    /* 4) Regular file */
    {
      std::string fpath = tmp.path() + "/regular.txt";
      FdGuard reg(::open(fpath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
      ASSERT_GE(reg.fd, 0);
      std::string ename;
      EXPECT_LT(rpc_attach_xsk(cfd.fd, qlid, reg.fd, nullptr, &ename), 0);
      EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");
    }

    /* 5) memfd */
    {
      FdGuard mfd(::memfd_create("not_xsk", MFD_CLOEXEC));
      ASSERT_GE(mfd.fd, 0);
      std::string ename;
      EXPECT_LT(rpc_attach_xsk(cfd.fd, qlid, mfd.fd, nullptr, &ename), 0);
      EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");
    }

    /* 6) Unbound AF_XDP socket (when kernel allows creating one). */
#ifdef AF_XDP
    {
      FdGuard xsk(::socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0));
      if (xsk.fd >= 0) {
        std::string ename;
        EXPECT_LT(rpc_attach_xsk(cfd.fd, qlid, xsk.fd, nullptr, &ename), 0);
        EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".InvalidParameter");
      }
    }
#endif

    /* Verify xsks_map was never modified. */
    {
      JsonGuard st{rpc_get_status(daemon.admin_sock())};
      ASSERT_NE(st.obj, nullptr);
      json_object* eth0 = find_backend_iface(st.obj, "eth0");
      ASSERT_NE(eth0, nullptr);
      EXPECT_FALSE(backend_has_xsk_queue(eth0, qid));
    }

    JsonGuard rep;
    ASSERT_EQ(
        mt_varlink_call(cfd.fd, MT_VARLINK_MANAGER_INTERFACE ".CloseSession", nullptr,
                        nullptr, 0, 2000, &rep.obj, nullptr, 0, nullptr, 0),
        0);
  }

  int after_fds = -1;
  for (int i = 0; i < 40; ++i) {
    after_fds = count_process_fds(daemon.pid());
    if (after_fds == baseline_fds) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_EQ(after_fds, baseline_fds) << "Daemon leaked FDs after invalid AttachXsk FDs";
}

/* =========================================================================
 * 11. LIFE-07 (R4-1): Live AF_XDP state preserved across SIGTERM and SIGKILL
 *     with no timer, then reclaimed via ClaimSession + RevokeSession quarantine
 * ========================================================================= */

TEST(Pr1bLifecycleTest, Life07_SigtermAndSigkillRestartPreservesLiveAfXdpAndReclaims) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  /* Tenant opens session and acquires full AF_XDP stack: Queue + XSK + Flow + UdpFilter.
   */
  FdGuard tfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(tfd.fd, 0);
  ASSERT_EQ(rpc_open_session(tfd.fd, "afxdp-tenant"), 0);

  std::string qlid, xlid, flid, ulid;
  int64_t qid = -1, loc = -1;
  ASSERT_EQ(rpc_acquire_queue(tfd.fd, "eth0", 3, &qlid, &qid), 0);
  ASSERT_EQ(qid, 3);

  int sv[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv), 0);
  FdGuard s0(sv[0]), s1(sv[1]);
  ASSERT_EQ(rpc_attach_xsk(tfd.fd, qlid, s0.fd, &xlid), 0);
  ASSERT_EQ(
      rpc_add_flow(tfd.fd, qlid, "udp4", nullptr, "239.8.8.8", -1, 20088, &flid, &loc),
      0);
  ASSERT_EQ(rpc_add_udp_filter(tfd.fd, qlid, 20088, &ulid), 0);

  /* --- Phase A: Normal stop (SIGTERM, R4-1) --- */
  int stop_st = daemon.stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stop_st));
  EXPECT_EQ(WEXITSTATUS(stop_st), 0);

  /* Restart daemon: all live AF_XDP state must enter RECOVERING intact. */
  ASSERT_TRUE(daemon.start(tmp.path()));

  {
    JsonGuard st{rpc_get_status(daemon.admin_sock())};
    ASSERT_NE(st.obj, nullptr);
    json_object* eth0 = find_backend_iface(st.obj, "eth0");
    ASSERT_NE(eth0, nullptr);
    EXPECT_TRUE(backend_has_xsk_queue(eth0, 3));
    EXPECT_TRUE(backend_has_udp_filter(eth0, 3, 20088));
    EXPECT_TRUE(backend_has_flow_location(eth0, loc, /*expect_mtl=*/true));
    EXPECT_TRUE(backend_has_flow_location(eth0, 10, /*expect_mtl=*/false));
  }

  /* Another tenant CANNOT steal recovering queue 3 or duplicate flow tuple. */
  {
    FdGuard other(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(other.fd, 0);
    ASSERT_EQ(rpc_open_session(other.fd, "other-tenant"), 0);
    std::string ename;
    EXPECT_EQ(rpc_acquire_queue(other.fd, "eth0", 3, nullptr, nullptr, nullptr, &ename),
              -EBUSY);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy");

    std::string other_q;
    ASSERT_EQ(rpc_acquire_queue(other.fd, "eth0", 4, &other_q), 0);
    ename.clear();
    EXPECT_EQ(rpc_add_flow(other.fd, other_q, "udp4", nullptr, "239.8.8.8", -1, 20088,
                           nullptr, nullptr, &ename),
              -EBUSY);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy");
    JsonGuard rep;
    ASSERT_EQ(
        mt_varlink_call(other.fd, MT_VARLINK_MANAGER_INTERFACE ".CloseSession", nullptr,
                        nullptr, 0, 2000, &rep.obj, nullptr, 0, nullptr, 0),
        0);
  }

  /* --- Phase B: Crash (SIGKILL) while still in RECOVERING -> restart again --- */
  daemon.stop(SIGKILL);
  ASSERT_TRUE(daemon.start(tmp.path()));

  /* Original tenant reconnects and reclaims ALL 4 leases via ClaimSession. */
  tfd.reset(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(tfd.fd, 0);
  {
    JsonGuard params{json_object_new_object()};
    json_object_object_add(params.obj, "clientName",
                           json_object_new_string("afxdp-tenant"));
    json_object* arr = json_object_new_array();
    json_object_array_add(arr, json_object_new_string(qlid.c_str()));
    json_object_array_add(arr, json_object_new_string(xlid.c_str()));
    json_object_array_add(arr, json_object_new_string(flid.c_str()));
    json_object_array_add(arr, json_object_new_string(ulid.c_str()));
    json_object_object_add(params.obj, "leaseIds", arr);
    JsonGuard reply;
    ASSERT_EQ(
        mt_varlink_call(tfd.fd, MT_VARLINK_MANAGER_INTERFACE ".ClaimSession", params.obj,
                        nullptr, 0, 2000, &reply.obj, nullptr, 0, nullptr, 0),
        0);
    json_object* p = nullptr;
    json_object* claimed = nullptr;
    json_object* missing = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(reply.obj, "parameters", &p));
    ASSERT_TRUE(json_object_object_get_ex(p, "claimed", &claimed));
    ASSERT_TRUE(json_object_object_get_ex(p, "missing", &missing));
    EXPECT_EQ(json_object_array_length(claimed), 4u);
    EXPECT_EQ(json_object_array_length(missing), 0u);
  }

  /* Clean CloseSession triggers reverse cascade: XSK -> Flow -> UdpFilter -> Queue ->
   * XDP.
   */
  {
    JsonGuard rep;
    ASSERT_EQ(
        mt_varlink_call(tfd.fd, MT_VARLINK_MANAGER_INTERFACE ".CloseSession", nullptr,
                        nullptr, 0, 2000, &rep.obj, nullptr, 0, nullptr, 0),
        0);
  }

  {
    JsonGuard st{rpc_get_status(daemon.admin_sock())};
    ASSERT_NE(st.obj, nullptr);
    json_object* eth0 = find_backend_iface(st.obj, "eth0");
    ASSERT_NE(eth0, nullptr);
    EXPECT_FALSE(backend_has_xsk_queue(eth0, 3));
    EXPECT_FALSE(backend_has_udp_filter(eth0, 3, 20088));
    EXPECT_FALSE(backend_has_flow_location(eth0, loc, /*expect_mtl=*/true));
    EXPECT_TRUE(backend_has_flow_location(eth0, 10, /*expect_mtl=*/false));
    json_object* xdp_mode = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(eth0, "xdpMode", &xdp_mode));
    EXPECT_STREQ(json_object_get_string(xdp_mode), "none");
  }
}

/* =========================================================================
 * 12. Admin RevokeSession holds live queue in QUARANTINED while removing
 *     XSK / Flow / UdpFilter immediately until owner pidfd exits
 * ========================================================================= */

TEST(Pr1bLifecycleTest, AdminRevokeSessionQuarantinesQueueAndStripsSteering) {
  TempDir tmp;
  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path()));

  int p_r2[2] = {-1, -1};
  ASSERT_EQ(::pipe2(p_r2, O_CLOEXEC), 0);
  FdGuard p2_r(p_r2[0]), p2_w(p_r2[1]);

  pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    p2_r.reset();
    int cfd = mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000);
    if (cfd < 0) ::_exit(10);
    std::string sid;
    if (rpc_open_session(cfd, "tenant-quar", &sid) != 0) ::_exit(11);
    std::string qlid, xlid, flid, ulid;
    int64_t qid = -1;
    if (rpc_acquire_queue(cfd, "eth0", 5, &qlid, &qid) != 0) ::_exit(12);
    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) ::_exit(13);
    if (rpc_attach_xsk(cfd, qlid, sv[0], &xlid) != 0) ::_exit(14);
    if (rpc_add_flow(cfd, qlid, "udp4", nullptr, "239.9.9.9", -1, 20099, &flid) != 0)
      ::_exit(15);
    if (rpc_add_udp_filter(cfd, qlid, 20099, &ulid) != 0) ::_exit(16);
    std::string msg = sid + "\n";
    if (::write(p2_w.fd, msg.data(), msg.size()) != static_cast<ssize_t>(msg.size()))
      ::_exit(17);
    for (;;) ::pause();
  }
  p2_w.reset();

  char sbuf[128] = {};
  ssize_t sn = ::read(p2_r.fd, sbuf, sizeof(sbuf) - 1);
  ASSERT_GT(sn, 0);
  std::string sid(sbuf, static_cast<size_t>(sn));
  while (!sid.empty() && (sid.back() == '\n' || sid.back() == '\r')) sid.pop_back();

  /* Admin calls RevokeSession while child is alive. */
  {
    FdGuard afd(mt_varlink_connect_unix(daemon.admin_sock().c_str(), 2000));
    ASSERT_GE(afd.fd, 0);
    JsonGuard params{json_object_new_object()};
    json_object_object_add(params.obj, "sessionId", json_object_new_string(sid.c_str()));
    JsonGuard rep;
    ASSERT_EQ(
        mt_varlink_call(afd.fd, MT_VARLINK_MANAGER_INTERFACE ".RevokeSession", params.obj,
                        nullptr, 0, 2000, &rep.obj, nullptr, 0, nullptr, 0),
        0);
  }

  /* XSK, Flow, and UdpFilter MUST be removed immediately, while queue 5 stays
   * QUARANTINED and cannot be acquired by another tenant while child is alive. */
  {
    JsonGuard st{rpc_get_status(daemon.admin_sock())};
    ASSERT_NE(st.obj, nullptr);
    json_object* eth0 = find_backend_iface(st.obj, "eth0");
    ASSERT_NE(eth0, nullptr);
    EXPECT_FALSE(backend_has_xsk_queue(eth0, 5));
    EXPECT_FALSE(backend_has_udp_filter(eth0, 5, 20099));
    json_object* rules = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(eth0, "flowRules", &rules));
    EXPECT_EQ(json_object_array_length(rules),
              1u); /* Only foreign rule at location 10. */

    json_object* params = nullptr;
    json_object* qcnt = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(st.obj, "parameters", &params));
    ASSERT_TRUE(json_object_object_get_ex(params, "quarantinedCount", &qcnt));
    EXPECT_GE(json_object_get_int64(qcnt), 1);
  }

  {
    FdGuard other(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(other.fd, 0);
    ASSERT_EQ(rpc_open_session(other.fd, "tenant-other"), 0);
    EXPECT_EQ(rpc_acquire_queue(other.fd, "eth0", 5), -EBUSY);
  }

  /* Kill child -> pidfd fires -> queue 5 becomes FREE and XDP detaches. */
  (void)::kill(child, SIGKILL);
  int cst = 0;
  ::waitpid(child, &cst, 0);

  bool freed = false;
  for (int i = 0; i < 40; ++i) {
    FdGuard other(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    if (other.fd >= 0 && rpc_open_session(other.fd, "tenant-after-kill") == 0 &&
        rpc_acquire_queue(other.fd, "eth0", 5) == 0) {
      freed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(freed) << "Quarantined queue 5 was not freed after owner exited";
}

/* =========================================================================
 * 13. Policy enforcement & cross-session per-UID max_queues / max_flows (ISSUE-18)
 * ========================================================================= */

TEST(Pr1bPolicyTest, PolicyInterfacesPortsAndCrossSessionPerUidQuotas) {
  TempDir tmp;
  std::string ppath = tmp.path() + "/policy.json";
  {
    std::ofstream pf(ppath);
    pf << R"({
      "allowed_cpus": "0-3",
      "allowed_interfaces": ["eth0"],
      "allowed_udp_ports": [[20000, 20010]],
      "default_quota": {"max_cpus": 2, "max_queues": 2, "max_flows": 2}
    })";
  }
  ASSERT_EQ(::chmod(ppath.c_str(), 0600), 0);

  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path(), {"--policy-file", ppath}));

  FdGuard fd1(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  FdGuard fd2(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(fd1.fd, 0);
  ASSERT_GE(fd2.fd, 0);
  ASSERT_EQ(rpc_open_session(fd1.fd, "pol-s1"), 0);
  ASSERT_EQ(rpc_open_session(fd2.fd, "pol-s2"), 0);

  /* Disallowed interface eth1 -> AccessDenied. */
  std::string ename;
  EXPECT_EQ(rpc_acquire_queue(fd1.fd, "eth1", -1, nullptr, nullptr, nullptr, &ename),
            -EACCES);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied");

  /* Acquire 1 queue in session 1 and 1 queue in session 2 -> total 2 (reaches
   * max_queues).
   */
  std::string q1, q2;
  ASSERT_EQ(rpc_acquire_queue(fd1.fd, "eth0", -1, &q1), 0);
  ASSERT_EQ(rpc_acquire_queue(fd2.fd, "eth0", -1, &q2), 0);

  /* 3rd queue across same UID -> AccessDenied (ISSUE-18). */
  ename.clear();
  EXPECT_EQ(rpc_acquire_queue(fd2.fd, "eth0", -1, nullptr, nullptr, nullptr, &ename),
            -EACCES);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied");

  /* Disallowed UDP port 30000 -> AccessDenied for AddFlow and AddUdpFilter. */
  ename.clear();
  EXPECT_EQ(rpc_add_flow(fd1.fd, q1, "udp4", nullptr, "239.1.1.1", -1, 30000, nullptr,
                         nullptr, &ename),
            -EACCES);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied");

  ename.clear();
  EXPECT_EQ(rpc_add_udp_filter(fd1.fd, q1, 30000, nullptr, &ename), -EACCES);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied");

  /* 2 allowed flows across two sessions -> 3rd hits per-UID max_flows=2. */
  std::string f1, f2;
  ASSERT_EQ(rpc_add_flow(fd1.fd, q1, "udp4", nullptr, "239.1.1.1", -1, 20001, &f1), 0);
  ASSERT_EQ(rpc_add_flow(fd2.fd, q2, "udp4", nullptr, "239.1.1.2", -1, 20002, &f2), 0);
  ename.clear();
  EXPECT_EQ(rpc_add_flow(fd2.fd, q2, "udp4", nullptr, "239.1.1.3", -1, 20003, nullptr,
                         nullptr, &ename),
            -EACCES);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".AccessDenied");
}

/* =========================================================================
 * 14. Direct LinuxNetdevBackend::validate_xsk_fd unit test (SEC-WIRE-06)
 * ========================================================================= */

TEST(Pr1bNetdevUnitTest, LinuxValidateXskFdRejectsAllNonBoundAfXdpDescriptors) {
  TempDir tmp;
  std::string err;

  EXPECT_FALSE(mtlm2::validate_linux_xsk_fd(-1, 1, 1, &err));

  FdGuard tcp(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  ASSERT_GE(tcp.fd, 0);
  EXPECT_FALSE(mtlm2::validate_linux_xsk_fd(tcp.fd, 1, 1, &err));

  int sv[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv), 0);
  FdGuard s0(sv[0]), s1(sv[1]);
  EXPECT_FALSE(mtlm2::validate_linux_xsk_fd(s0.fd, 1, 1, &err));

  int p[2] = {-1, -1};
  ASSERT_EQ(::pipe2(p, O_CLOEXEC), 0);
  FdGuard pr(p[0]), pw(p[1]);
  EXPECT_FALSE(mtlm2::validate_linux_xsk_fd(pr.fd, 1, 1, &err));

  std::string fpath = tmp.path() + "/f.txt";
  FdGuard reg(::open(fpath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
  ASSERT_GE(reg.fd, 0);
  EXPECT_FALSE(mtlm2::validate_linux_xsk_fd(reg.fd, 1, 1, &err));

  FdGuard mfd(::memfd_create("xsk_test", MFD_CLOEXEC));
  ASSERT_GE(mfd.fd, 0);
  EXPECT_FALSE(mtlm2::validate_linux_xsk_fd(mfd.fd, 1, 1, &err));

#ifdef AF_XDP
  FdGuard xsk(::socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0));
  if (xsk.fd >= 0) {
    EXPECT_FALSE(mtlm2::validate_linux_xsk_fd(xsk.fd, 1, 1, &err));
  }
#endif
}

}  // namespace
