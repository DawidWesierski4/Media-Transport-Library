/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <errno.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <json-c/json.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
#include "mtlm2_peer.hpp"
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

void make_tree_writable(const fs::path& root) {
  std::error_code ec;
  if (!fs::exists(root, ec)) return;
  ::chmod(root.c_str(), 0755);
  for (auto it = fs::recursive_directory_iterator(
           root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::path p = it->path();
    struct stat st{};
    if (::lstat(p.c_str(), &st) == 0 && !S_ISLNK(st.st_mode)) {
      if (S_ISDIR(st.st_mode)) {
        ::chmod(p.c_str(), 0755);
      } else {
        ::chmod(p.c_str(), 0644);
      }
    }
  }
}

void make_tree_readonly(const fs::path& root) {
  std::vector<fs::path> dirs;
  dirs.push_back(root);
  std::error_code ec;
  for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
    struct stat st{};
    if (::lstat(entry.path().c_str(), &st) == 0 && !S_ISLNK(st.st_mode)) {
      if (S_ISDIR(st.st_mode)) {
        dirs.push_back(entry.path());
      } else {
        ::chmod(entry.path().c_str(), 0444);
      }
    }
  }
  /* Chmod deepest directories first so traversal stays permitted during loop. */
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
    ::chmod(it->c_str(), 0555);
  }
}

class TempDir {
 public:
  TempDir() {
    std::string tmpl = "/tmp/mtlm2_pr3_cg_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* created = ::mkdtemp(buf.data());
    EXPECT_NE(created, nullptr);
    if (created) path_ = created;
  }
  ~TempDir() {
    if (!path_.empty()) {
      make_tree_writable(path_);
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

bool write_text_file(const std::string& path, const std::string& content,
                     mode_t mode = 0644) {
  fs::path p(path);
  std::error_code ec;
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return false;
  out << content;
  out.close();
  if (!out) return false;
  return ::chmod(path.c_str(), mode) == 0;
}

bool read_text_file(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

std::string get_self_cgroup_v2_path() {
  mtlm2::PeerCredExtractor ext("/proc");
  std::string cg_path;
  std::string err;
  EXPECT_TRUE(ext.read_cgroup_v2_path(::getpid(), -1, &cg_path, &err)) << err;
  return cg_path;
}

std::string join_cgroup_dir(const std::string& mount_root, const std::string& cg_path) {
  if (cg_path.empty() || cg_path == "/") return mount_root;
  if (cg_path.front() == '/') return mount_root + cg_path;
  return mount_root + "/" + cg_path;
}

bool setup_fake_proc_for_self(const std::string& fake_proc_root,
                              const std::string& cgroup_v2_path) {
  const std::string pid_str = std::to_string(::getpid());
  const std::string dst_dir = fake_proc_root + "/" + pid_str;
  std::error_code ec;
  fs::create_directories(dst_dir, ec);
  if (ec) return false;

  std::string status_raw, stat_raw;
  if (!read_text_file("/proc/" + pid_str + "/status", &status_raw)) return false;
  if (!read_text_file("/proc/" + pid_str + "/stat", &stat_raw)) return false;
  if (!write_text_file(dst_dir + "/status", status_raw)) return false;
  if (!write_text_file(dst_dir + "/stat", stat_raw)) return false;
  if (!write_text_file(dst_dir + "/cgroup", "0::" + cgroup_v2_path + "\n")) return false;
  return true;
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

    int err_pipe[2] = {-1, -1};
    if (::pipe2(err_pipe, O_CLOEXEC) != 0) return false;

    pid_t pid = ::fork();
    if (pid < 0) {
      ::close(err_pipe[0]);
      ::close(err_pipe[1]);
      return false;
    }
    if (pid == 0) {
      ::close(err_pipe[0]);
      ::dup2(err_pipe[1], STDERR_FILENO);
      ::close(err_pipe[1]);
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
    ::close(err_pipe[1]);
    stderr_fd_.reset(err_pipe[0]);
    (void)mtlm2::set_nonblock(stderr_fd_.fd);
    pid_ = pid;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < deadline) {
      drain_stderr();
      int st = 0;
      pid_t w = ::waitpid(pid_, &st, WNOHANG);
      if (w == pid_) {
        drain_stderr();
        pid_ = -1;
        return false;
      }
      int fd = mt_varlink_connect_unix(client_sock_.c_str(), 100);
      if (fd >= 0) {
        JsonGuard rep;
        int rc = mt_varlink_call(fd, MT_VARLINK_SERVICE_INTERFACE ".GetInfo", nullptr,
                                 nullptr, 0, 500, &rep.obj, nullptr, 0, nullptr, 0);
        ::close(fd);
        if (rc == 0) {
          drain_stderr();
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    drain_stderr();
    return false;
  }

  void stop(int sig = SIGTERM) {
    if (pid_ <= 0) return;
    ::kill(pid_, sig);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
      drain_stderr();
      int st = 0;
      pid_t w = ::waitpid(pid_, &st, WNOHANG);
      if (w == pid_) {
        drain_stderr();
        pid_ = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(pid_, SIGKILL);
    int st = 0;
    (void)::waitpid(pid_, &st, 0);
    drain_stderr();
    pid_ = -1;
  }

  void drain_stderr() {
    if (stderr_fd_.fd < 0) return;
    char buf[1024];
    for (;;) {
      ssize_t n = ::read(stderr_fd_.fd, buf, sizeof(buf));
      if (n > 0) {
        stderr_log_.append(buf, static_cast<size_t>(n));
      } else {
        break;
      }
    }
  }

  const std::string& client_sock() const {
    return client_sock_;
  }
  const std::string& admin_sock() const {
    return admin_sock_;
  }
  const std::string& stderr_log() const {
    return stderr_log_;
  }

 private:
  pid_t pid_ = -1;
  FdGuard stderr_fd_;
  std::string stderr_log_;
  std::string client_sock_;
  std::string admin_sock_;
  std::string state_dir_;
  std::string host_lock_;
  std::string v1_sock_;
  std::string shm_lock_;
};

bool json_get_str(json_object* obj, const char* key, std::string* out) {
  json_object* v = nullptr;
  if (!obj || !json_object_object_get_ex(obj, key, &v) || !v ||
      !json_object_is_type(v, json_type_string))
    return false;
  *out = json_object_get_string(v);
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

bool get_admin_status(const std::string& admin_sock, JsonGuard* out) {
  FdGuard fd(mt_varlink_connect_unix(admin_sock.c_str(), 2000));
  if (fd.fd < 0) return false;
  JsonGuard reply;
  if (mt_varlink_call(fd.fd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr, nullptr,
                      0, 2000, &reply.obj, nullptr, 0, nullptr, 0) != 0 ||
      !reply.obj) {
    return false;
  }
  json_object* p = nullptr;
  if (!json_object_object_get_ex(reply.obj, "parameters", &p) || !p) return false;
  out->obj = json_object_get(p);
  return true;
}

json_object* find_session_in_status(json_object* status_obj, const std::string& sid) {
  json_object* arr = nullptr;
  if (!status_obj || !json_object_object_get_ex(status_obj, "sessions", &arr) || !arr ||
      !json_object_is_type(arr, json_type_array))
    return nullptr;
  const size_t n = json_object_array_length(arr);
  for (size_t i = 0; i < n; ++i) {
    json_object* item = json_object_array_get_idx(arr, i);
    std::string cur_sid;
    if (json_get_str(item, "sessionId", &cur_sid) && cur_sid == sid) return item;
  }
  return nullptr;
}

int rpc_open_session(int fd, const std::string& client_name, std::string* sid_out,
                     std::string* err_name_out = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "clientName",
                         json_object_new_string(client_name.c_str()));
  JsonGuard rep;
  char ename[128] = {};
  int rc = mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".OpenSession", params.obj,
                           nullptr, 0, 2000, &rep.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name_out) *err_name_out = ename;
  if (rc == 0 && rep.obj && sid_out) {
    json_object* p = nullptr;
    if (json_object_object_get_ex(rep.obj, "parameters", &p) && p) {
      (void)json_get_str(p, "sessionId", sid_out);
    }
  }
  return rc;
}

int rpc_acquire_cpu(int fd, std::optional<int64_t> preferred,
                    std::optional<int64_t> numa_node, std::optional<bool> allow_fallback,
                    std::string* lease_id_out, int64_t* cpu_id_out,
                    std::string* err_name_out = nullptr) {
  JsonGuard params{json_object_new_object()};
  if (preferred.has_value()) {
    json_object_object_add(params.obj, "preferred", json_object_new_int64(*preferred));
  }
  if (numa_node.has_value()) {
    json_object_object_add(params.obj, "numaNode", json_object_new_int64(*numa_node));
  }
  if (allow_fallback.has_value()) {
    json_object_object_add(params.obj, "allowFallback",
                           json_object_new_boolean(*allow_fallback ? 1 : 0));
  }
  JsonGuard rep;
  char ename[128] = {};
  int rc = mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AcquireCpu", params.obj,
                           nullptr, 0, 2000, &rep.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name_out) *err_name_out = ename;
  if (rc == 0 && rep.obj) {
    json_object* p = nullptr;
    if (json_object_object_get_ex(rep.obj, "parameters", &p) && p) {
      if (lease_id_out) (void)json_get_str(p, "leaseId", lease_id_out);
      if (cpu_id_out) (void)json_get_i64(p, "cpuId", cpu_id_out);
    }
  }
  return rc;
}

int rpc_release_lease(int fd, const std::string& lease_id,
                      std::string* err_name_out = nullptr) {
  JsonGuard params{json_object_new_object()};
  json_object_object_add(params.obj, "leaseId", json_object_new_string(lease_id.c_str()));
  JsonGuard rep;
  char ename[128] = {};
  int rc = mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease", params.obj,
                           nullptr, 0, 2000, &rep.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name_out) *err_name_out = ename;
  return rc;
}

}  // namespace

/* 1. CG-01: Read-only 0555 cgroupfs tree, OpenSession succeeds with ZERO writes,
 *    AcquireCpu rejects outside-effective-set CPU with CpuOutsideEffectiveSet,
 *    auto-select intersects with live effective set, and NUMA mismatch returns
 *    NumaNodeNotAllowed. */
TEST(Pr3CgroupVerifyTest, CG01_CpuOutsideEffectiveSetAndReadOnlyCgroupfs) {
  TempDir tmp;
  const std::string self_cg = get_self_cgroup_v2_path();
  ASSERT_FALSE(self_cg.empty());

  const std::string cg_mount = tmp.path() + "/cgroup_mount";
  const std::string peer_cg_dir = join_cgroup_dir(cg_mount, self_cg);
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.cpus.effective", "2-3\n"));
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.mems.effective", "0\n"));

  /* Lock the entire mock cgroup mount strictly read-only (0555 dirs / 0444 files)
   * to prove zero writes are performed to cgroupfs in verify_only mode. */
  make_tree_readonly(cg_mount);

  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path(),
                           {"--cgroup-mode", "verify_only", "--cgroup-mount", cg_mount}))
      << daemon.stderr_log();

  /* Admin GetStatus must report cgroupVerifyMode == "verify_only". */
  {
    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    std::string mode;
    ASSERT_TRUE(json_get_str(st.obj, "cgroupVerifyMode", &mode));
    EXPECT_EQ(mode, "verify_only");
  }

  FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(cfd.fd, 0);

  std::string sid, ename;
  ASSERT_EQ(rpc_open_session(cfd.fd, "cg01-client", &sid, &ename), 0) << ename;
  ASSERT_FALSE(sid.empty());

  /* Per-session status must expose cgroupPath, effectiveCpus, effectiveMems. */
  {
    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    json_object* sess = find_session_in_status(st.obj, sid);
    ASSERT_NE(sess, nullptr);
    std::string cg_path, eff_cpus, eff_mems;
    ASSERT_TRUE(json_get_str(sess, "cgroupPath", &cg_path));
    ASSERT_TRUE(json_get_str(sess, "effectiveCpus", &eff_cpus));
    ASSERT_TRUE(json_get_str(sess, "effectiveMems", &eff_mems));
    EXPECT_EQ(cg_path, self_cg);
    EXPECT_EQ(eff_cpus, "2-3");
    EXPECT_EQ(eff_mems, "0");
  }

  /* preferred=0 is outside cpuset.cpus.effective ("2-3") -> CpuOutsideEffectiveSet. */
  ename.clear();
  EXPECT_LT(rpc_acquire_cpu(cfd.fd, 0, std::nullopt, false, nullptr, nullptr, &ename), 0);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CpuOutsideEffectiveSet");

  /* preferred=2 succeeds. */
  std::string lease2;
  int64_t cpu2 = -1;
  ename.clear();
  ASSERT_EQ(rpc_acquire_cpu(cfd.fd, 2, std::nullopt, false, &lease2, &cpu2, &ename), 0)
      << ename;
  EXPECT_EQ(cpu2, 2);

  /* Auto-select picks 3 (never 0 or 1). */
  std::string lease3;
  int64_t cpu3 = -1;
  ename.clear();
  ASSERT_EQ(rpc_acquire_cpu(cfd.fd, std::nullopt, std::nullopt, std::nullopt, &lease3,
                            &cpu3, &ename),
            0)
      << ename;
  EXPECT_EQ(cpu3, 3);

  /* Exhausted effective set returns ResourceBusy (never escapes to CPU 0/1/4). */
  ename.clear();
  EXPECT_LT(rpc_acquire_cpu(cfd.fd, std::nullopt, std::nullopt, std::nullopt, nullptr,
                            nullptr, &ename),
            0);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".ResourceBusy");

  /* Release CPU 2, then preferred=0 with allowFallback=true must pick 2 (inside set). */
  ASSERT_EQ(rpc_release_lease(cfd.fd, lease2), 0);
  std::string lease_fb;
  int64_t cpu_fb = -1;
  ename.clear();
  ASSERT_EQ(rpc_acquire_cpu(cfd.fd, 0, std::nullopt, true, &lease_fb, &cpu_fb, &ename), 0)
      << ename;
  EXPECT_EQ(cpu_fb, 2);
  ASSERT_EQ(rpc_release_lease(cfd.fd, lease_fb), 0);
  ASSERT_EQ(rpc_release_lease(cfd.fd, lease3), 0);

  /* Verify NumaNodeNotAllowed: CPU 2 is on NUMA node 0; restrict mems.effective to "1".
   */
  make_tree_writable(cg_mount);
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.mems.effective", "1\n"));
  make_tree_readonly(cg_mount);

  ename.clear();
  EXPECT_LT(rpc_acquire_cpu(cfd.fd, 2, std::nullopt, false, nullptr, nullptr, &ename), 0);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".NumaNodeNotAllowed");
}

/* 2. ISSUE-59: Shrinking cpuset.cpus.effective after OpenSession must be reflected
 *    immediately on both GetStatus and AcquireCpu without restarting the session. */
TEST(Pr3CgroupVerifyTest,
     Issue59_LiveCpusetShrinkReflectedImmediatelyOnAcquireCpuAndGetStatus) {
  TempDir tmp;
  const std::string self_cg = get_self_cgroup_v2_path();
  ASSERT_FALSE(self_cg.empty());

  const std::string cg_mount = tmp.path() + "/cgroup_mount";
  const std::string peer_cg_dir = join_cgroup_dir(cg_mount, self_cg);
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.cpus.effective", "2-5\n"));
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.mems.effective", "0\n"));

  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path(),
                           {"--cgroup-mode", "verify_only", "--cgroup-mount", cg_mount}))
      << daemon.stderr_log();

  FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
  ASSERT_GE(cfd.fd, 0);

  std::string sid, ename;
  ASSERT_EQ(rpc_open_session(cfd.fd, "issue59-client", &sid, &ename), 0) << ename;

  /* Initial GetStatus shows "2-5". */
  {
    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    json_object* sess = find_session_in_status(st.obj, sid);
    ASSERT_NE(sess, nullptr);
    std::string eff_cpus;
    ASSERT_TRUE(json_get_str(sess, "effectiveCpus", &eff_cpus));
    EXPECT_EQ(eff_cpus, "2-5");
  }

  /* Shrink cpuset.cpus.effective live from "2-5" to "2-3" AFTER OpenSession. */
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.cpus.effective", "2-3\n"));

  /* GetStatus must IMMEDIATELY reflect "2-3". */
  {
    JsonGuard st;
    ASSERT_TRUE(get_admin_status(daemon.admin_sock(), &st));
    json_object* sess = find_session_in_status(st.obj, sid);
    ASSERT_NE(sess, nullptr);
    std::string eff_cpus;
    ASSERT_TRUE(json_get_str(sess, "effectiveCpus", &eff_cpus));
    EXPECT_EQ(eff_cpus, "2-3");
  }

  /* AcquireCpu(preferred=4, allowFallback=false) must IMMEDIATELY fail with
   * CpuOutsideEffectiveSet. */
  ename.clear();
  EXPECT_LT(rpc_acquire_cpu(cfd.fd, 4, std::nullopt, false, nullptr, nullptr, &ename), 0);
  EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CpuOutsideEffectiveSet");

  /* CPU 2 (still inside live "2-3") still succeeds. */
  std::string lease2;
  int64_t cpu2 = -1;
  ename.clear();
  ASSERT_EQ(rpc_acquire_cpu(cfd.fd, 2, std::nullopt, false, &lease2, &cpu2, &ename), 0)
      << ename;
  EXPECT_EQ(cpu2, 2);
}

/* 3. ISSUE-86: Empty cpuset.cpus.effective or cpuset.mems.effective must fail fast at
 *    OpenSession with CgroupVerificationFailed. */
TEST(Pr3CgroupVerifyTest, Issue86_EmptyEffectiveCpusOrMemsRejectedAtOpenSession) {
  TempDir tmp;
  const std::string self_cg = get_self_cgroup_v2_path();
  ASSERT_FALSE(self_cg.empty());

  const std::string cg_mount = tmp.path() + "/cgroup_mount";
  const std::string peer_cg_dir = join_cgroup_dir(cg_mount, self_cg);
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.cpus.effective", "\n"));
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.mems.effective", "0\n"));

  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path(),
                           {"--cgroup-mode", "verify_only", "--cgroup-mount", cg_mount}))
      << daemon.stderr_log();

  /* Empty cpuset.cpus.effective -> rejected at OpenSession. */
  {
    FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(cfd.fd, 0);
    std::string ename;
    EXPECT_LT(rpc_open_session(cfd.fd, "empty-cpus", nullptr, &ename), 0);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed");
  }

  /* Empty cpuset.mems.effective -> rejected at OpenSession. */
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.cpus.effective", "2-3\n"));
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.mems.effective", "  \n"));
  {
    FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(cfd.fd, 0);
    std::string ename;
    EXPECT_LT(rpc_open_session(cfd.fd, "empty-mems", nullptr, &ename), 0);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed");
  }
}

// 4. ISSUE-77: Policy load rejects malformed expected_cgroup_pattern globs (unclosed '[',
//    trailing '\\', '..'), normalizes '//', guards '/*' from matching bare '/', and
//    rejects non-matching peers at OpenSession with CgroupVerificationFailed.
TEST(Pr3CgroupVerifyTest, Issue77_ExpectedCgroupPatternGlobValidationAndRootGuard) {
  TempDir tmp;

  /* Policy load validation for malformed globs. */
  auto check_policy_load = [&](const std::string& name, const std::string& json_text,
                               bool expect_ok) {
    const std::string ppath = tmp.path() + "/" + name + ".json";
    ASSERT_TRUE(write_text_file(ppath, json_text, 0600));
    mtlm2::PolicyConfig pol;
    std::string err;
    bool ok = mtlm2::PolicyConfig::load_file(ppath, &pol, &err);
    EXPECT_EQ(ok, expect_ok) << "policy=" << name << " err=" << err;
  };

  check_policy_load(
      "bad_unclosed_bracket",
      R"({"allowed_cpus":"0-3","cgroup_verify":{"mode":"verify_only","expected_cgroup_pattern":"/mtl/[abc"}})",
      false);
  check_policy_load(
      "bad_trailing_backslash",
      R"({"allowed_cpus":"0-3","cgroup_verify":{"mode":"verify_only","expected_cgroup_pattern":"/mtl/foo\\"}})",
      false);
  check_policy_load(
      "bad_dotdot",
      R"({"allowed_cpus":"0-3","cgroup_verify":{"mode":"verify_only","expected_cgroup_pattern":"/mtl/../etc"}})",
      false);
  check_policy_load(
      "bad_no_leading_slash",
      R"({"allowed_cpus":"0-3","cgroup_verify":{"mode":"verify_only","expected_cgroup_pattern":"mtl/*"}})",
      false);
  check_policy_load(
      "bad_uid_pattern",
      std::string(R"({"allowed_cpus":"0-3","uid_quotas":{")") +
          std::to_string(::geteuid()) +
          R"(":{"max_cpus":2,"expected_cgroup_pattern":"/mtl/[unclosed"}}})",
      false);
  check_policy_load(
      "good_double_slash_normalized",
      R"({"allowed_cpus":"0-3","cgroup_verify":{"mode":"verify_only","expected_cgroup_pattern":"/mtl//tenants/*"}})",
      true);

  // Root guard: "/*" must NOT match bare "/".
  const std::string cg_mount = tmp.path() + "/cgroup_mount";
  ASSERT_TRUE(write_text_file(cg_mount + "/cpuset.cpus.effective", "2-3\n"));
  ASSERT_TRUE(write_text_file(cg_mount + "/cpuset.mems.effective", "0\n"));
  ASSERT_TRUE(
      write_text_file(cg_mount + "/mtl/tenants/pod-a/cpuset.cpus.effective", "2-3\n"));
  ASSERT_TRUE(
      write_text_file(cg_mount + "/mtl/tenants/pod-a/cpuset.mems.effective", "0\n"));
  ASSERT_TRUE(write_text_file(cg_mount + "/other/pod-b/cpuset.cpus.effective", "2-3\n"));
  ASSERT_TRUE(write_text_file(cg_mount + "/other/pod-b/cpuset.mems.effective", "0\n"));

  const std::string fake_proc = tmp.path() + "/fake_proc";
  ASSERT_TRUE(setup_fake_proc_for_self(fake_proc, "/"));

  const std::string root_guard_pol = tmp.path() + "/pol_root_guard.json";
  ASSERT_TRUE(write_text_file(
      root_guard_pol,
      std::string(R"({"allowed_cpus":"0-7","default_quota":{"max_cpus":4},)") +
          R"("cgroup_verify":{"mode":"verify_only","mount_path":")" + cg_mount +
          R"(","expected_cgroup_pattern":"/*"}})",
      0600));

  {
    TempDir run_dir;
    DaemonProc daemon;
    ASSERT_TRUE(daemon.start(
        run_dir.path(), {"--policy-file", root_guard_pol, "--test-proc-root", fake_proc}))
        << daemon.stderr_log();

    FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(cfd.fd, 0);
    std::string ename;
    EXPECT_LT(rpc_open_session(cfd.fd, "root-cgroup-peer", nullptr, &ename), 0);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed");
  }

  /* Matching vs non-matching pattern (with '//' normalization in policy). */
  const std::string tenant_pol = tmp.path() + "/pol_tenant.json";
  ASSERT_TRUE(write_text_file(
      tenant_pol,
      std::string(R"({"allowed_cpus":"0-7","default_quota":{"max_cpus":4},)") +
          R"("cgroup_verify":{"mode":"verify_only","mount_path":")" + cg_mount +
          R"(","expected_cgroup_pattern":"/mtl//tenants/*"}})",
      0600));

  {
    TempDir run_dir;
    DaemonProc daemon;
    ASSERT_TRUE(daemon.start(
        run_dir.path(), {"--policy-file", tenant_pol, "--test-proc-root", fake_proc}))
        << daemon.stderr_log();

    /* Non-matching peer (/other/pod-b) -> rejected with CgroupVerificationFailed. */
    ASSERT_TRUE(setup_fake_proc_for_self(fake_proc, "/other/pod-b"));
    {
      FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
      ASSERT_GE(cfd.fd, 0);
      std::string ename;
      EXPECT_LT(rpc_open_session(cfd.fd, "wrong-tenant", nullptr, &ename), 0);
      EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed");
    }

    /* Matching peer (/mtl/tenants/pod-a) -> succeeds. */
    ASSERT_TRUE(setup_fake_proc_for_self(fake_proc, "/mtl/tenants/pod-a"));
    {
      FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
      ASSERT_GE(cfd.fd, 0);
      std::string sid, ename;
      ASSERT_EQ(rpc_open_session(cfd.fd, "good-tenant", &sid, &ename), 0) << ename;
      ASSERT_FALSE(sid.empty());
    }
  }

  /* Per-UID expected_cgroup_pattern overrides global pattern for that UID. */
  const std::string uid_pol = tmp.path() + "/pol_uid_pattern.json";
  ASSERT_TRUE(write_text_file(
      uid_pol,
      std::string(R"({"allowed_cpus":"0-7","default_quota":{"max_cpus":4},)") +
          R"("uid_quotas":{")" + std::to_string(::geteuid()) +
          R"(":{"max_cpus":4,"expected_cgroup_pattern":"/mtl/tenants/*"}},)" +
          R"("cgroup_verify":{"mode":"verify_only","mount_path":")" + cg_mount +
          R"(","expected_cgroup_pattern":"/other/*"}})",
      0600));
  {
    TempDir run_dir;
    DaemonProc daemon;
    ASSERT_TRUE(daemon.start(run_dir.path(),
                             {"--policy-file", uid_pol, "--test-proc-root", fake_proc}))
        << daemon.stderr_log();

    ASSERT_TRUE(setup_fake_proc_for_self(fake_proc, "/other/pod-b"));
    {
      FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
      ASSERT_GE(cfd.fd, 0);
      std::string ename;
      EXPECT_LT(rpc_open_session(cfd.fd, "uid-mismatch", nullptr, &ename), 0);
      EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed");
    }

    ASSERT_TRUE(setup_fake_proc_for_self(fake_proc, "/mtl/tenants/pod-a"));
    {
      FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
      ASSERT_GE(cfd.fd, 0);
      std::string sid, ename;
      ASSERT_EQ(rpc_open_session(cfd.fd, "uid-match", &sid, &ename), 0) << ename;
      ASSERT_FALSE(sid.empty());
    }
  }
}

/* 5. ISSUE-76: Symlink or FIFO at cpuset.cpus.effective (or symlink in cgroup mount)
 *    is rejected cleanly without blocking/hanging or following symlinks. */
TEST(Pr3CgroupVerifyTest, Issue76_SymlinkOrFifoInCgroupMountRejectedNonBlocking) {
  TempDir tmp;
  const std::string self_cg = get_self_cgroup_v2_path();
  ASSERT_FALSE(self_cg.empty());

  const std::string cg_mount = tmp.path() + "/cgroup_mount";
  const std::string peer_cg_dir = join_cgroup_dir(cg_mount, self_cg);
  const std::string outside_file = tmp.path() + "/outside_cpus";
  ASSERT_TRUE(write_text_file(outside_file, "0-3\n"));
  ASSERT_TRUE(write_text_file(peer_cg_dir + "/cpuset.mems.effective", "0\n"));

  /* Case A: Symlink at cpuset.cpus.effective -> rejected with CgroupVerificationFailed.
   */
  const std::string cpus_path = peer_cg_dir + "/cpuset.cpus.effective";
  ASSERT_EQ(::symlink(outside_file.c_str(), cpus_path.c_str()), 0);

  DaemonProc daemon;
  ASSERT_TRUE(daemon.start(tmp.path(),
                           {"--cgroup-mode", "verify_only", "--cgroup-mount", cg_mount}))
      << daemon.stderr_log();

  {
    FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(cfd.fd, 0);
    std::string ename;
    EXPECT_LT(rpc_open_session(cfd.fd, "symlink-cpus", nullptr, &ename), 0);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed");
  }

  /* Case B: FIFO at cpuset.cpus.effective (no writer!) -> must reject promptly without
   * hanging. */
  ASSERT_EQ(::unlink(cpus_path.c_str()), 0);
  ASSERT_EQ(::mkfifo(cpus_path.c_str(), 0644), 0);

  {
    const auto t0 = std::chrono::steady_clock::now();
    FdGuard cfd(mt_varlink_connect_unix(daemon.client_sock().c_str(), 2000));
    ASSERT_GE(cfd.fd, 0);
    std::string ename;
    EXPECT_LT(rpc_open_session(cfd.fd, "fifo-cpus", nullptr, &ename), 0);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed");
    EXPECT_LT(elapsed_ms, 1000);
  }

  /* Case C: Symlink at --cgroup-mount itself logs startup warning and fails OpenSession.
   */
  const std::string symlink_mount = tmp.path() + "/cgroup_mount_link";
  ASSERT_EQ(::symlink(cg_mount.c_str(), symlink_mount.c_str()), 0);
  {
    TempDir run_dir;
    DaemonProc sym_daemon;
    ASSERT_TRUE(sym_daemon.start(run_dir.path(), {"--cgroup-mode", "verify_only",
                                                  "--cgroup-mount", symlink_mount}))
        << sym_daemon.stderr_log();
    FdGuard cfd(mt_varlink_connect_unix(sym_daemon.client_sock().c_str(), 2000));
    ASSERT_GE(cfd.fd, 0);
    std::string ename;
    EXPECT_LT(rpc_open_session(cfd.fd, "symlink-mount", nullptr, &ename), 0);
    EXPECT_EQ(ename, MT_VARLINK_MANAGER_INTERFACE ".CgroupVerificationFailed");
    sym_daemon.drain_stderr();
    EXPECT_NE(sym_daemon.stderr_log().find("cgroup"), std::string::npos);
  }
}

/* 6. Finding 5 / R2-8: Systemd unit explicitly sets ProtectControlGroups=yes. */
TEST(Pr3CgroupVerifyTest, SystemdServiceProtectControlGroupsYes) {
  std::string pkg_dir = MTLM2_PACKAGING_DIR;
  std::ifstream svc_in(pkg_dir + "/mtlm2.service");
  ASSERT_TRUE(svc_in.is_open());
  std::string svc((std::istreambuf_iterator<char>(svc_in)),
                  std::istreambuf_iterator<char>());
  EXPECT_NE(svc.find("ProtectControlGroups=yes"), std::string::npos);
  EXPECT_EQ(svc.find("Delegate=yes"), std::string::npos);
  EXPECT_EQ(svc.find("/sys/fs/cgroup"), std::string::npos);
}
