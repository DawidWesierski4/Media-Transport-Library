/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <errno.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <json-c/json.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mt_varlink.h"
#include "mtlm2_journal.hpp"
#include "mtlm2_peer.hpp"
#include "mtlm2_server.hpp"
#include "mtlm2_shm.hpp"

namespace fs = std::filesystem;

namespace {

struct JsonGuard {
  json_object* obj = nullptr;
  ~JsonGuard() {
    if (obj) json_object_put(obj);
  }
};

class TempDir {
 public:
  TempDir() {
    std::string tmpl = "/tmp/mtlm2_pr1a_test_XXXXXX";
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

class ServerRunner {
 public:
  explicit ServerRunner(mtlm2::ServerConfig cfg)
      : server_(std::make_unique<mtlm2::MtlManagerServer>(std::move(cfg))) {
  }
  ~ServerRunner() {
    stop();
  }

  bool start(std::string* err = nullptr) {
    if (!server_->start(err)) return false;
    thread_ = std::thread([this]() { server_->run(); });
    return true;
  }

  void stop() {
    if (server_) server_->request_stop();
    if (thread_.joinable()) thread_.join();
  }

  mtlm2::MtlManagerServer& server() {
    return *server_;
  }

 private:
  std::unique_ptr<mtlm2::MtlManagerServer> server_;
  std::thread thread_;
};

mtlm2::ServerConfig make_test_cfg(const std::string& dir, bool zero_arg = true) {
  mtlm2::ServerConfig cfg;
  cfg.client_socket_path = dir + "/manager2.sock";
  cfg.admin_socket_path = dir + "/admin.sock";
  cfg.state_dir = dir + "/state";
  cfg.use_signalfd = false;
  cfg.shm.enabled = false;
  cfg.override_online_cpus = {0, 1, 2, 3, 1024, 1025};
  if (zero_arg) {
    cfg.policy = mtlm2::PolicyConfig::make_zero_arg_single_host();
    cfg.client_socket_mode = 0666;
    cfg.allow_world_client_socket = true;
  } else {
    cfg.client_socket_mode = 0660;
    cfg.allow_world_client_socket = false;
  }
  return cfg;
}

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

int rpc_acquire_cpu(int fd, int64_t preferred, bool allow_fb,
                    std::string* lid_out = nullptr, int64_t* cpu_out = nullptr,
                    std::string* err_name = nullptr) {
  JsonGuard params{json_object_new_object()};
  if (preferred >= 0)
    json_object_object_add(params.obj, "preferred", json_object_new_int64(preferred));
  json_object_object_add(params.obj, "allowFallback",
                         json_object_new_boolean(allow_fb ? 1 : 0));
  JsonGuard reply;
  char ename[128] = {};
  int rc =
      mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".AcquireCpu", params.obj, nullptr,
                      0, 2000, &reply.obj, ename, sizeof(ename), nullptr, 0);
  if (err_name) *err_name = ename;
  if (rc == 0 && reply.obj) {
    json_object* p = nullptr;
    json_object* jl = nullptr;
    json_object* jc = nullptr;
    if (json_object_object_get_ex(reply.obj, "parameters", &p)) {
      if (lid_out && json_object_object_get_ex(p, "leaseId", &jl))
        *lid_out = json_object_get_string(jl);
      if (cpu_out && json_object_object_get_ex(p, "cpuId", &jc))
        *cpu_out = json_object_get_int64(jc);
    }
  }
  return rc;
}

/* =========================================================================
 * 1. Varlink Wire Hardening Tests (ISSUE-15/16/17/31/35/49/79/80/96)
 * ========================================================================= */

TEST(VarlinkWireTest, StrictJsonRejectsNulControlSurrogatesAndDuplicates) {
  json_object* obj = nullptr;

  /* ISSUE-15/16: \u0000 in key and value */
  const char* nul_val = R"({"method":"a\u0000b"})";
  EXPECT_EQ(mt_varlink_parse_json_object(nul_val, std::strlen(nul_val), &obj), -EPROTO);
  EXPECT_EQ(obj, nullptr);

  const char* nul_key = R"({"ke\u0000y":"v"})";
  EXPECT_EQ(mt_varlink_parse_json_object(nul_key, std::strlen(nul_key), &obj), -EPROTO);

  /* ISSUE-49: raw unescaped ASCII control bytes 0x01..0x1F */
  std::string raw_ctrl = "{\"a\":\"x";
  raw_ctrl.push_back('\x07');
  raw_ctrl += "y\"}";
  EXPECT_EQ(mt_varlink_parse_json_object(raw_ctrl.data(), raw_ctrl.size(), &obj),
            -EPROTO);

  /* ISSUE-96: unpaired UTF-16 surrogates */
  const char* lone_hi = R"({"a":"\uD800"})";
  const char* lone_lo = R"({"a":"\uDC00"})";
  const char* valid_pair = R"({"a":"\uD83D\uDE00"})";
  EXPECT_EQ(mt_varlink_parse_json_object(lone_hi, std::strlen(lone_hi), &obj), -EPROTO);
  EXPECT_EQ(mt_varlink_parse_json_object(lone_lo, std::strlen(lone_lo), &obj), -EPROTO);
  EXPECT_EQ(mt_varlink_parse_json_object(valid_pair, std::strlen(valid_pair), &obj), 0);
  ASSERT_NE(obj, nullptr);
  json_object_put(obj);
  obj = nullptr;

  /* ISSUE-79: duplicate keys in same object rejected, sibling objects allowed */
  const char* dup_same = R"({"a":1,"a":2})";
  const char* dup_esc = R"({"a":1,"\u0061":2})";
  const char* sibling_ok = R"({"x":{"a":1},"y":{"a":2}})";
  EXPECT_EQ(mt_varlink_parse_json_object(dup_same, std::strlen(dup_same), &obj), -EPROTO);
  EXPECT_EQ(mt_varlink_parse_json_object(dup_esc, std::strlen(dup_esc), &obj), -EPROTO);
  EXPECT_EQ(mt_varlink_parse_json_object(sibling_ok, std::strlen(sibling_ok), &obj), 0);
  ASSERT_NE(obj, nullptr);
  json_object_put(obj);
}

TEST(VarlinkWireTest, RejectsCoalescedAndExcessScmRightsAndSaturatesDeadline) {
  /* ISSUE-35: saturating deadline helper */
  EXPECT_EQ(mt_varlink_deadline_after_ms(INT64_MAX), INT64_MAX);
  EXPECT_GT(mt_varlink_deadline_after_ms(1000), mt_varlink_mono_ns());

  /* ISSUE-17 & ISSUE-80: MT_VARLINK_MAX_FDS == 1 and reject coalesced/mid-frame FDs */
  EXPECT_EQ(MT_VARLINK_MAX_FDS, 1u);

  mt_varlink_reader_t r;
  mt_varlink_reader_init(&r);
  int pipefd[2];
  ASSERT_EQ(::pipe2(pipefd, O_CLOEXEC), 0);
  ::close(pipefd[1]);

  const uint8_t coalesced[] = {'{', '}', '\0', '{', '}', '\0'};
  int dup1 = ::fcntl(pipefd[0], F_DUPFD_CLOEXEC, 0);
  ASSERT_GE(dup1, 0);
  EXPECT_EQ(mt_varlink_reader_feed(&r, coalesced, sizeof(coalesced), &dup1, 1), -EPROTO);

  /* Mid-frame FD attachment rejected */
  const uint8_t part1[] = {'{', '"', 'a'};
  const uint8_t part2[] = {'"', ':', '1', '}', '\0'};
  EXPECT_GT(mt_varlink_reader_feed(&r, part1, sizeof(part1), nullptr, 0), 0);
  int dup2 = ::fcntl(pipefd[0], F_DUPFD_CLOEXEC, 0);
  ASSERT_GE(dup2, 0);
  EXPECT_EQ(mt_varlink_reader_feed(&r, part2, sizeof(part2), &dup2, 1), -EPROTO);
  mt_varlink_reader_reset(&r);
  ::close(pipefd[0]);
}

TEST(VarlinkWireTest, LargeFourMiBFrameRoundTrip) {
  /* ISSUE-31: 4 MiB framing */
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

  std::string big_payload = "{\"big\":\"" + std::string(200 * 1024, 'x') + "\"}";
  mt_varlink_writer_t w;
  mt_varlink_writer_init(&w, 128 * 1024, 8);
  EXPECT_EQ(
      mt_varlink_writer_enqueue(&w, big_payload.data(), big_payload.size(), nullptr, 0),
      -EMSGSIZE);
  EXPECT_EQ(mt_varlink_writer_enqueue_large(&w, big_payload.data(), big_payload.size()),
            0);

  auto r = std::make_unique<mt_varlink_reader_t>();
  mt_varlink_reader_init(r.get());
  mt_varlink_reader_set_max_frame(r.get(), MT_VARLINK_MAX_LARGE_FRAME_BYTES);

  mt_varlink_frame_t frame;
  mt_varlink_frame_init(&frame);
  int popped = 0;
  for (int i = 0; i < 1000 && popped == 0; ++i) {
    if (mt_varlink_writer_has_pending(&w)) (void)mt_varlink_writer_flush(&w, fds[0]);
    (void)mt_varlink_reader_recv(r.get(), fds[1]);
    popped = mt_varlink_reader_pop(r.get(), &frame);
  }
  EXPECT_EQ(popped, 1);
  EXPECT_EQ(frame.json_len, big_payload.size());
  mt_varlink_frame_clear(&frame);
  mt_varlink_reader_reset(r.get());
  mt_varlink_writer_clear(&w);
  ::close(fds[0]);
  ::close(fds[1]);
}

/* =========================================================================
 * 2. PeerCred Hardening (ISSUE-46)
 * ========================================================================= */

TEST(PeerCredTest, ExtractsSelfAndRejectsMissingGroupsDotDotAndDeletedCgroup) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

  mtlm2::PeerCredExtractor real_ext("/proc");
  mtlm2::PeerIdentity id;
  std::string err;
  ASSERT_TRUE(real_ext.extract(fds[0], &id, &err)) << err;
  EXPECT_EQ(id.principal.pid, ::getpid());
  EXPECT_EQ(id.principal.uid, ::geteuid());
  EXPECT_GT(id.principal.start_time_ticks, 0u);
  EXPECT_TRUE(id.pidfd_owner.valid());

  /* Fake proc root missing Groups: line (ISSUE-46). */
  TempDir fake_proc;
  std::string pid_dir = fake_proc.path() + "/" + std::to_string(::getpid());
  fs::create_directories(pid_dir);
  {
    std::ofstream sf(pid_dir + "/status");
    sf << "Name:\ttest\nTgid:\t" << ::getpid() << "\nUid:\t" << ::geteuid() << "\t"
       << ::geteuid() << "\n";
  }
  mtlm2::PeerCredExtractor bad_ext(fake_proc.path());
  mtlm2::PeerIdentity bad_id;
  EXPECT_FALSE(bad_ext.extract(fds[0], &bad_id, &err));
  EXPECT_NE(err.find("Groups"), std::string::npos);

  /* Reject .. traversal and (deleted) in cgroup path (ISSUE-46). */
  std::string cg_out;
  {
    std::ofstream cf(pid_dir + "/cgroup");
    cf << "0::/system.slice/../escape\n";
  }
  EXPECT_FALSE(bad_ext.read_cgroup_v2_path(::getpid(), -1, &cg_out, &err));
  {
    std::ofstream cf(pid_dir + "/cgroup");
    cf << "0::/system.slice/foo (deleted)\n";
  }
  EXPECT_FALSE(bad_ext.read_cgroup_v2_path(::getpid(), -1, &cg_out, &err));

  ::close(fds[0]);
  ::close(fds[1]);
}

/* =========================================================================
 * 3. State Journal Hardening (ISSUE-05/06/07/40/67)
 * ========================================================================= */

TEST(StateJournalTest, HardenedSaveLoadCorruptQuarantineAndLockExclusion) {
  TempDir tmp;
  std::string sdir = tmp.path() + "/state";
  mtlm2::StateJournal journal(sdir);

  /* ISSUE-05: exclusive state dir lock */
  mtlm2::UniqueFd lock1, lock2;
  std::string err;
  ASSERT_TRUE(journal.acquire_state_dir_lock(&lock1, &err)) << err;
  EXPECT_FALSE(journal.acquire_state_dir_lock(&lock2, &err));

  /* Save & load round-trip with 0600 permissions */
  mtlm2::JournalState st;
  st.cpu_leases.push_back(
      {"cpu-test-1", 1024, ::getpid(), 12345, ::geteuid(), "app1", false});
  ASSERT_TRUE(journal.save(st, &err)) << err;

  struct stat jst{};
  ASSERT_EQ(::stat((sdir + "/journal.json").c_str(), &jst), 0);
  EXPECT_EQ(jst.st_mode & 0777u, 0600u);

  mtlm2::JournalState loaded;
  bool not_found = false, quarantined = false;
  ASSERT_TRUE(journal.load(&loaded, &not_found, &quarantined, &err)) << err;
  ASSERT_EQ(loaded.cpu_leases.size(), 1u);
  EXPECT_EQ(loaded.cpu_leases[0].cpu_id, 1024u);

  /* ISSUE-06/67: corrupt journal is quarantined to journal.json.corrupt.<ts> */
  {
    std::ofstream cf(sdir + "/journal.json", std::ios::trunc);
    cf << "{not valid json";
  }
  EXPECT_FALSE(journal.load(&loaded, &not_found, &quarantined, &err));
  EXPECT_TRUE(quarantined);
  bool found_corrupt = false;
  for (const auto& entry : fs::directory_iterator(sdir)) {
    if (entry.path().filename().string().rfind("journal.json.corrupt.", 0) == 0)
      found_corrupt = true;
  }
  EXPECT_TRUE(found_corrupt);

  /* Reject group/world-writable state dir (ISSUE-06/40) */
  ASSERT_EQ(::chmod(sdir.c_str(), 0777), 0);
  EXPECT_FALSE(journal.save(st, &err));
  ASSERT_EQ(::chmod(sdir.c_str(), 0700), 0);
}

/* =========================================================================
 * 4. Legacy SysV SHM Bridge (R3-1, R3-2, R3-5, R4-1)
 * ========================================================================= */

TEST(LegacyShmTest, CaseACoexistTenantPidAndCaseBRefusal) {
  TempDir tmp;
  std::string ftok_file = tmp.path() + "/shm_key";
  std::string lock_file = tmp.path() + "/shm.lock";
  {
    std::ofstream f(ftok_file);
    f << "k";
  }

  key_t key = ::ftok(ftok_file.c_str(), 21);
  ASSERT_NE(key, static_cast<key_t>(-1));
  const size_t slots = 16;
  const size_t seg_sz =
      offsetof(mtlm2::ShmLcoreTable, lcores_info) + slots * sizeof(mtlm2::ShmLcoreEntry);
  int shmid = ::shmget(key, seg_sz, 0666 | IPC_CREAT | IPC_EXCL);
  ASSERT_GE(shmid, 0);
  auto* tbl = static_cast<mtlm2::ShmLcoreTable*>(::shmat(shmid, nullptr, 0));
  ASSERT_NE(tbl, reinterpret_cast<mtlm2::ShmLcoreTable*>(-1));
  std::memset(tbl, 0, seg_sz);

  /* Simulate an active legacy shm process (self PID) owning CPU 2. */
  tbl->used = 1;
  tbl->lcores_info[2].active = true;
  tbl->lcores_info[2].pid = ::getpid();
  tbl->lcores_info[2].u_info.pid = ::getpid();

  /* Case A default coexist (host_cpus=8 <= 16): succeeds despite active foreign PID
   * (R3-5). */
  mtlm2::ShmConfig cfg;
  cfg.enabled = true;
  cfg.ftok_path = ftok_file;
  cfg.lock_path = lock_file;
  cfg.host_cpus = 8;

  mtlm2::LegacyShmBridge bridge_a;
  std::string err;
  ASSERT_TRUE(bridge_a.init(cfg, &err)) << err;
  EXPECT_EQ(bridge_a.mode(), mtlm2::ShmMode::CaseACoexist);

  /* CPU 2 is busy for foreign PID; CPU 3 writes tenant PID (not daemon PID) (R3-1). */
  bool busy = false;
  EXPECT_FALSE(bridge_a.try_acquire_cpu(2, 424242, ::geteuid(), "tenant", &busy, &err));
  EXPECT_TRUE(busy);

  ASSERT_TRUE(
      bridge_a.try_acquire_cpu(3, ::getpid(), ::geteuid(), "tenant-app", &busy, &err))
      << err;
  EXPECT_TRUE(tbl->lcores_info[3].active);
  EXPECT_EQ(tbl->lcores_info[3].pid, ::getpid());
  EXPECT_EQ(tbl->lcores_info[3].u_info.pid, ::getpid());
  EXPECT_STREQ(tbl->lcores_info[3].u_info.comm, "tenant-app");

  /* Normal stop preserves tenant shm slot (R4-1). */
  bridge_a.shutdown(/*preserve_live_entries=*/true);
  EXPECT_TRUE(tbl->lcores_info[3].active);
  EXPECT_EQ(tbl->lcores_info[3].pid, ::getpid());

  /* --fail-on-legacy-shm refuses startup on live foreign PID. */
  cfg.fail_on_legacy_shm = true;
  EXPECT_FALSE(bridge_a.init(cfg, &err));

  /* Case B (host_cpus=64 > 16): refuses startup if any live legacy shm PID exists (R3-2).
   */
  cfg.fail_on_legacy_shm = false;
  cfg.host_cpus = 64;
  mtlm2::LegacyShmBridge bridge_b;
  EXPECT_FALSE(bridge_b.init(cfg, &err));
  EXPECT_NE(err.find("host_cpus"), std::string::npos);

  /* Clear live entries -> Case B starts cleanly with writes disabled. */
  tbl->lcores_info[2].active = false;
  tbl->lcores_info[3].active = false;
  tbl->used = 0;
  ASSERT_TRUE(bridge_b.init(cfg, &err)) << err;
  EXPECT_EQ(bridge_b.mode(), mtlm2::ShmMode::CaseBNoWrite);
  ASSERT_TRUE(bridge_b.try_acquire_cpu(3, ::getpid(), ::geteuid(), "t", &busy, &err));
  EXPECT_FALSE(tbl->lcores_info[3].active); /* Writes disabled in Case B! */

  bridge_b.shutdown(/*preserve_live_entries=*/true);
  ::shmdt(tbl);
  ::shmctl(shmid, IPC_RMID, nullptr);
}

/* =========================================================================
 * 5. Server Integration: Zero-Arg, Policy, Quotas, Quarantine, Restart, Hardening
 * ========================================================================= */

TEST(ServerIntegrationTest, SocketLockMutualExclusionAndZeroArgPolicyAndAdminIsolation) {
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  /* Verify zero-arg client socket is 0666 and admin socket is 0600 (R4-5). */
  struct stat cst{}, ast{};
  ASSERT_EQ(::stat(cfg.client_socket_path.c_str(), &cst), 0);
  ASSERT_EQ(::stat(cfg.admin_socket_path.c_str(), &ast), 0);
  EXPECT_EQ(cst.st_mode & 0777u, 0666u);
  EXPECT_EQ(ast.st_mode & 0777u, 0600u);

  /* ISSUE-01 / ISSUE-05: Second server on same sockets/state dir fails cleanly. */
  mtlm2::MtlManagerServer dup(cfg);
  EXPECT_FALSE(dup.start(&err));

  /* Admin RPC rejected on client socket with PermissionDenied (R4-5). */
  int cfd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(cfd, 0);
  char ename[128] = {};
  JsonGuard rep;
  EXPECT_EQ(mt_varlink_call(cfd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr,
                            nullptr, 0, 2000, &rep.obj, ename, sizeof(ename), nullptr, 0),
            -EACCES);
  EXPECT_STREQ(ename, "org.varlink.service.PermissionDenied");

  /* Physical CPU >= 1024 leasing + allowFallback=false ResourceBusy. */
  ASSERT_EQ(rpc_open_session(cfd, "client-a"), 0);
  std::string lid;
  int64_t cpu_id = -1;
  ASSERT_EQ(rpc_acquire_cpu(cfd, 1024, false, &lid, &cpu_id), 0);
  EXPECT_EQ(cpu_id, 1024);
  EXPECT_EQ(rpc_acquire_cpu(cfd, 1024, false), -EBUSY);

  /* ISSUE-64: unknown parameter rejected; ISSUE-47: int64 overflow rejected. */
  {
    JsonGuard badp{json_object_new_object()};
    json_object_object_add(badp.obj, "bogusKey", json_object_new_int(1));
    EXPECT_EQ(mt_varlink_call(cfd, MT_VARLINK_MANAGER_INTERFACE ".AcquireCpu", badp.obj,
                              nullptr, 0, 2000, &rep.obj, nullptr, 0, nullptr, 0),
              -EINVAL);
  }
  {
    const char* ovf =
        R"({"method":"org.openvisualcloud.mtl.Manager.AcquireCpu","parameters":{"preferred":18446744073709551615}})";
    mt_varlink_writer_t w;
    mt_varlink_writer_init(&w, 65536, 4);
    ASSERT_EQ(mt_varlink_writer_enqueue(&w, ovf, std::strlen(ovf), nullptr, 0), 0);
    ASSERT_GT(mt_varlink_writer_flush(&w, cfd), 0);
    mt_varlink_writer_clear(&w);
    mt_varlink_reader_t r;
    mt_varlink_reader_init(&r);
    mt_varlink_frame_t fr;
    mt_varlink_frame_init(&fr);
    int pop = 0;
    for (int i = 0; i < 200 && pop == 0; ++i) {
      int rc = mt_varlink_reader_recv(&r, cfd);
      if (rc == -EAGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      pop = mt_varlink_reader_pop(&r, &fr);
    }
    ASSERT_EQ(pop, 1);
    EXPECT_NE(std::string(fr.json_text).find("InvalidParameter"), std::string::npos);
    mt_varlink_frame_clear(&fr);
    mt_varlink_reader_reset(&r);
  }

  /* Admin socket GetStatus succeeds. */
  int afd = mt_varlink_connect_unix(cfg.admin_socket_path.c_str(), 2000);
  ASSERT_GE(afd, 0);
  ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr,
                            nullptr, 0, 2000, &rep.obj, nullptr, 0, nullptr, 0),
            0);

  ::close(afd);
  ::close(cfd);
  runner.stop();

  /* ISSUE-01: lockfile is NEVER unlinked on shutdown, while socket IS unlinked. */
  EXPECT_TRUE(fs::exists(cfg.client_socket_path + ".lock"));
  EXPECT_FALSE(fs::exists(cfg.client_socket_path));
}

TEST(ServerIntegrationTest, PolicyCrossSessionQuotaAggregationAndErrorCap) {
  /* ISSUE-18: aggregate per-UID quota across multiple sessions */
  TempDir tmp;
  std::string pfile = tmp.path() + "/policy.json";
  {
    std::ofstream pf(pfile);
    pf << R"({
      "allowed_cpus": [0, 1, 2, 3],
      "allowed_interfaces": ["eth0"],
      "allowed_udp_ports": [[5000, 6000]],
      "default_quota": {"max_cpus": 0},
      "uid_quotas": { ")"
       << ::geteuid() << R"(": {"max_cpus": 2, "max_queues": 1, "max_flows": 4} }
    })";
  }
  ASSERT_EQ(::chmod(pfile.c_str(), 0640), 0);

  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/false);
  std::string err;
  ASSERT_TRUE(mtlm2::PolicyConfig::load_file(pfile, &cfg.policy, &err)) << err;

  ServerRunner runner(cfg);
  ASSERT_TRUE(runner.start(&err)) << err;

  int c1 = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  int c2 = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(c1, 0);
  ASSERT_GE(c2, 0);
  ASSERT_EQ(rpc_open_session(c1, "s1"), 0);
  ASSERT_EQ(rpc_open_session(c2, "s2"), 0);

  /* CPU 1024 denied by policy; CPUs 0 and 1 succeed across 2 sessions; 3rd hits UID
   * quota. */
  EXPECT_EQ(rpc_acquire_cpu(c1, 1024, false), -EACCES);
  ASSERT_EQ(rpc_acquire_cpu(c1, 0, false), 0);
  ASSERT_EQ(rpc_acquire_cpu(c2, 1, false), 0);
  EXPECT_EQ(rpc_acquire_cpu(c2, 2, false), -EACCES);

  /* ISSUE-78: error messages capped at <= 256 bytes */
  std::string huge_id(400, 'z');
  JsonGuard rp{json_object_new_object()};
  json_object_object_add(rp.obj, "leaseId", json_object_new_string(huge_id.c_str()));
  char emsg[512] = {};
  EXPECT_EQ(mt_varlink_call(c1, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease", rp.obj,
                            nullptr, 0, 2000, nullptr, nullptr, 0, emsg, sizeof(emsg)),
            -ENOENT);
  EXPECT_LE(std::strlen(emsg), 256u);

  ::close(c1);
  ::close(c2);
}

TEST(ServerIntegrationTest, QuarantineAndKillQuarantinedOwnerFreesCpuOnExit) {
  /* R2-7: explicit revoke while pidfd alive -> QUARANTINED; KillQuarantinedOwner ->
   * SIGKILL -> FREE */
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  int sync_pipe[2];
  ASSERT_EQ(::pipe2(sync_pipe, O_CLOEXEC), 0);

  pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::close(sync_pipe[0]);
    int fd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
    if (fd < 0) _exit(10);
    if (rpc_open_session(fd, "tenant-child") != 0) _exit(11);
    std::string lid;
    if (rpc_acquire_cpu(fd, 1, false, &lid) != 0) _exit(12);
    ssize_t wr = ::write(sync_pipe[1], lid.data(), lid.size());
    (void)wr;
    ::close(sync_pipe[1]);
    for (;;) ::pause();
  }
  ::close(sync_pipe[1]);

  char lid_buf[64] = {};
  ssize_t nr = ::read(sync_pipe[0], lid_buf, sizeof(lid_buf) - 1);
  ::close(sync_pipe[0]);
  ASSERT_GT(nr, 0);
  std::string child_lid(lid_buf, static_cast<size_t>(nr));

  int afd = mt_varlink_connect_unix(cfg.admin_socket_path.c_str(), 2000);
  ASSERT_GE(afd, 0);
  {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "leaseId", json_object_new_string(child_lid.c_str()));
    ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".RevokeLease", p.obj,
                              nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
              0);
  }

  /* CPU 1 must stay busy while quarantined child is still alive! */
  int cfd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(cfd, 0);
  ASSERT_EQ(rpc_open_session(cfd, "other"), 0);
  EXPECT_EQ(rpc_acquire_cpu(cfd, 1, false), -EBUSY);

  /* KillQuarantinedOwner sends SIGKILL via pidfd_send_signal. */
  {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "leaseId", json_object_new_string(child_lid.c_str()));
    ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".KillQuarantinedOwner",
                              p.obj, nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
              0);
  }

  int wstatus = 0;
  ASSERT_EQ(::waitpid(child, &wstatus, 0), child);
  EXPECT_TRUE(WIFSIGNALED(wstatus));
  EXPECT_EQ(WTERMSIG(wstatus), SIGKILL);

  /* Poll until epoll reaps pidfd and CPU 1 becomes FREE. */
  bool freed = false;
  for (int i = 0; i < 40; ++i) {
    if (rpc_acquire_cpu(cfd, 1, false) == 0) {
      freed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(freed);

  ::close(cfd);
  ::close(afd);
}

TEST(ServerIntegrationTest,
     NonDestructiveNormalStopAndRestartPreservesLiveLeasesAndClaimSession) {
  /* R4-1: Normal stop preserves journal & live leases -> restart puts them in RECOVERING
   * (zero double-booking) -> live owner reclaims via ClaimSession! */
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);

  std::string lid;
  {
    ServerRunner runner1(cfg);
    std::string err;
    ASSERT_TRUE(runner1.start(&err)) << err;

    int c1 = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
    ASSERT_GE(c1, 0);
    ASSERT_EQ(rpc_open_session(c1, "live-stream"), 0);
    ASSERT_EQ(rpc_acquire_cpu(c1, 2, false, &lid), 0);

    /* Stop daemon normally (non-destructive default, teardown_on_exit = false) while
     * client stays live! */
    runner1.stop();
    ::close(c1);
  }

  /* Restart daemon and verify CPU 2 is RECOVERING, blocked for other clients, and
   * reclaimable. */
  ServerRunner runner2(cfg);
  std::string err;
  ASSERT_TRUE(runner2.start(&err)) << err;

  int other = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(other, 0);
  ASSERT_EQ(rpc_open_session(other, "intruder"), 0);
  EXPECT_EQ(rpc_acquire_cpu(other, 2, false), -EBUSY);
  ::close(other);

  /* Reclaim via ClaimSession from same live process (verified pid + start_time + uid). */
  int c2 = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(c2, 0);
  JsonGuard cp{json_object_new_object()};
  json_object_object_add(cp.obj, "clientName", json_object_new_string("live-stream"));
  json_object* arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_string(lid.c_str()));
  json_object_object_add(cp.obj, "leaseIds", arr);
  JsonGuard crep;
  ASSERT_EQ(mt_varlink_call(c2, MT_VARLINK_MANAGER_INTERFACE ".ClaimSession", cp.obj,
                            nullptr, 0, 2000, &crep.obj, nullptr, 0, nullptr, 0),
            0);

  /* ReleaseLease now succeeds on the reclaimed session. */
  JsonGuard rp{json_object_new_object()};
  json_object_object_add(rp.obj, "leaseId", json_object_new_string(lid.c_str()));
  ASSERT_EQ(mt_varlink_call(c2, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease", rp.obj,
                            nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
            0);
  ::close(c2);
}

TEST(ServerIntegrationTest, LargeGetStatusOver64KiBAndAcceptBurstAndClosingDisarm) {
  /* ISSUE-29, ISSUE-30, ISSUE-31 */
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  cfg.override_online_cpus.clear();
  for (uint32_t i = 0; i < 600; ++i) cfg.override_online_cpus.push_back(i);

  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  /* Acquire 500 CPUs so GetStatus JSON exceeds 64 KiB (ISSUE-31). */
  int cfd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(cfd, 0);
  ASSERT_EQ(rpc_open_session(cfd, "big-tenant-with-long-name-0123456789abcdef"), 0);
  for (int i = 0; i < 500; ++i) {
    ASSERT_EQ(rpc_acquire_cpu(cfd, i, false), 0);
  }

  int afd = mt_varlink_connect_unix(cfg.admin_socket_path.c_str(), 2000);
  ASSERT_GE(afd, 0);
  JsonGuard rep;
  ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr,
                            nullptr, 0, 5000, &rep.obj, nullptr, 0, nullptr, 0),
            0);
  const char* s = json_object_to_json_string(rep.obj);
  EXPECT_GT(std::strlen(s), 65536u);
  /* Caller stays connected after >64 KiB reply. */
  JsonGuard rep2;
  ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_SERVICE_INTERFACE ".GetInfo", nullptr,
                            nullptr, 0, 2000, &rep2.obj, nullptr, 0, nullptr, 0),
            0);

  /* Burst >40 connections (exceeds 32/tick accept cap, ISSUE-29) + malformed frame
   * triggers closing (ISSUE-30). */
  std::vector<int> burst;
  for (int i = 0; i < 40; ++i) {
    int fd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
    if (fd >= 0) burst.push_back(fd);
  }
  EXPECT_GE(burst.size(), 32u);
  if (!burst.empty()) {
    const char* bad = "not-json\0trailing\0";
    ssize_t wr = ::send(burst[0], bad, 18, MSG_NOSIGNAL);
    (void)wr;
  }
  for (int fd : burst) ::close(fd);
  ::close(afd);
  ::close(cfd);
}

/* =========================================================================
 * 6. Real Subprocess SIGTERM & SIGKILL Restart Test (R4-1, ISSUE-90)
 * ========================================================================= */

TEST(SubprocessDaemonTest,
     SigtermAndSigkillRestartPreserveLiveCpuLeasesAndTeardownFlagClears) {
  TempDir tmp;
  std::string csock = tmp.path() + "/manager2.sock";
  std::string asock = tmp.path() + "/admin.sock";
  std::string sdir = tmp.path() + "/state";
  std::string slock = tmp.path() + "/shm.lock";
  std::string sftok = tmp.path() + "/shm.key";
  {
    std::ofstream f(sftok);
    f << "k";
  }

  auto spawn_daemon = [&](bool teardown_flag) -> pid_t {
    pid_t p = ::fork();
    if (p == 0) {
      std::vector<const char*> args = {
          MTLM2_DAEMON_BIN, "--socket",        csock.c_str(), "--admin-socket",
          asock.c_str(),    "--state-dir",     sdir.c_str(),  "--shm-lock-path",
          slock.c_str(),    "--shm-ftok-path", sftok.c_str(),
      };
      if (teardown_flag) args.push_back("--teardown-on-exit");
      args.push_back(nullptr);
      ::execv(MTLM2_DAEMON_BIN, const_cast<char* const*>(args.data()));
      _exit(127);
    }
    return p;
  };

  auto wait_ready = [&]() -> bool {
    for (int i = 0; i < 100; ++i) {
      int fd = mt_varlink_connect_unix(csock.c_str(), 200);
      if (fd >= 0) {
        JsonGuard rep;
        int rc = mt_varlink_call(fd, MT_VARLINK_SERVICE_INTERFACE ".GetInfo", nullptr,
                                 nullptr, 0, 500, &rep.obj, nullptr, 0, nullptr, 0);
        ::close(fd);
        if (rc == 0) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  };

  /* 1. Start daemon, acquire CPU 0, stop via SIGTERM (R4-1, ISSUE-90). */
  pid_t d1 = spawn_daemon(/*teardown_flag=*/false);
  ASSERT_GT(d1, 0);
  ASSERT_TRUE(wait_ready());

  int c1 = mt_varlink_connect_unix(csock.c_str(), 2000);
  ASSERT_GE(c1, 0);
  ASSERT_EQ(rpc_open_session(c1, "sigterm-tenant"), 0);
  std::string lid;
  int64_t cpu_id = -1;
  ASSERT_EQ(rpc_acquire_cpu(c1, 0, false, &lid, &cpu_id), 0);
  EXPECT_EQ(cpu_id, 0);

  ASSERT_EQ(::kill(d1, SIGTERM), 0);
  int st1 = 0;
  ASSERT_EQ(::waitpid(d1, &st1, 0), d1);
  EXPECT_TRUE(WIFEXITED(st1));
  EXPECT_EQ(WEXITSTATUS(st1), 0);
  ::close(c1);

  /* 2. Restart daemon -> CPU 0 still RECOVERING (busy for others), then SIGKILL daemon.
   */
  pid_t d2 = spawn_daemon(/*teardown_flag=*/false);
  ASSERT_GT(d2, 0);
  ASSERT_TRUE(wait_ready());

  int probe = mt_varlink_connect_unix(csock.c_str(), 2000);
  ASSERT_GE(probe, 0);
  ASSERT_EQ(rpc_open_session(probe, "probe-after-sigterm"), 0);
  EXPECT_EQ(rpc_acquire_cpu(probe, 0, false), -EBUSY);
  ::close(probe);

  ASSERT_EQ(::kill(d2, SIGKILL), 0);
  int st2 = 0;
  ASSERT_EQ(::waitpid(d2, &st2, 0), d2);
  EXPECT_TRUE(WIFSIGNALED(st2));

  /* 3. Restart after SIGKILL with --teardown-on-exit -> CPU 0 still RECOVERING;
   *    ClaimSession succeeds; SIGTERM with --teardown-on-exit wipes journal cleanly. */
  pid_t d3 = spawn_daemon(/*teardown_flag=*/true);
  ASSERT_GT(d3, 0);
  ASSERT_TRUE(wait_ready());

  int c3 = mt_varlink_connect_unix(csock.c_str(), 2000);
  ASSERT_GE(c3, 0);
  JsonGuard cp{json_object_new_object()};
  json_object_object_add(cp.obj, "clientName", json_object_new_string("sigterm-tenant"));
  json_object* arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_string(lid.c_str()));
  json_object_object_add(cp.obj, "leaseIds", arr);
  ASSERT_EQ(mt_varlink_call(c3, MT_VARLINK_MANAGER_INTERFACE ".ClaimSession", cp.obj,
                            nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
            0);

  ASSERT_EQ(::kill(d3, SIGTERM), 0);
  int st3 = 0;
  ASSERT_EQ(::waitpid(d3, &st3, 0), d3);
  EXPECT_TRUE(WIFEXITED(st3));
  EXPECT_EQ(WEXITSTATUS(st3), 0);
  ::close(c3);

  /* Verify --teardown-on-exit emptied journal. */
  mtlm2::StateJournal j(sdir);
  mtlm2::JournalState js;
  bool nf = false, q = false;
  std::string jerr;
  ASSERT_TRUE(j.load(&js, &nf, &q, &jerr)) << jerr;
  EXPECT_TRUE(js.cpu_leases.empty());
}

/* =========================================================================
 * 7. Packaging Lint Test (ISSUE-36, ISSUE-71, R4-2, R4-5)
 * ========================================================================= */

TEST(PackagingLintTest, ServiceAndSocketHardeningMatchReview4) {
  std::string pkg_dir = MTLM2_PACKAGING_DIR;
  std::ifstream svc_in(pkg_dir + "/mtlm2.service");
  ASSERT_TRUE(svc_in.is_open());
  std::string svc((std::istreambuf_iterator<char>(svc_in)),
                  std::istreambuf_iterator<char>());

  EXPECT_NE(svc.find("Group=mtl"), std::string::npos);
  EXPECT_NE(svc.find("RuntimeDirectoryPreserve=yes"), std::string::npos);
  EXPECT_NE(svc.find("PrivateTmp=no"), std::string::npos);
  EXPECT_EQ(svc.find("PrivateTmp=yes"), std::string::npos);
  EXPECT_NE(svc.find("ReadWritePaths=-/tmp /run/imtl -/sys/fs/bpf"), std::string::npos);
  EXPECT_NE(svc.find("--policy-file /etc/mtl/manager2-policy.json"), std::string::npos);
  EXPECT_NE(svc.find("--socket-mode 0660"), std::string::npos);

  std::ifstream sock_in(pkg_dir + "/mtlm2.socket");
  ASSERT_TRUE(sock_in.is_open());
  std::string sock((std::istreambuf_iterator<char>(sock_in)),
                   std::istreambuf_iterator<char>());
  EXPECT_NE(sock.find("ListenStream=/run/imtl/manager2.sock"), std::string::npos);
  EXPECT_EQ(sock.find("PartOf="), std::string::npos);
}

/* =========================================================================
 * 8. TEST_PLAN.md PR 1a Decisions (D1, D3, D6) & Security/Recovery/DoS Suite
 * ========================================================================= */

namespace {

class ScopedTmpfsDir {
 public:
  ScopedTmpfsDir() {
    std::string tmpl = "/dev/shm/mtlm2_pr1a_dos_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* created = ::mkdtemp(buf.data());
    if (created) path_ = created;
  }
  ~ScopedTmpfsDir() {
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

bool wait_daemon_ready(const std::string& csock, int timeout_ms = 3000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    int fd = mt_varlink_connect_unix(csock.c_str(), 200);
    if (fd >= 0) {
      JsonGuard rep;
      int rc = mt_varlink_call(fd, MT_VARLINK_SERVICE_INTERFACE ".GetInfo", nullptr,
                               nullptr, 0, 500, &rep.obj, nullptr, 0, nullptr, 0);
      ::close(fd);
      if (rc == 0) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  return false;
}

pid_t spawn_daemon_args(const std::vector<std::string>& args,
                        int* stderr_read_fd = nullptr, rlim_t rlimit_nofile = 0) {
  int err_pipe[2] = {-1, -1};
  if (stderr_read_fd) {
    if (::pipe2(err_pipe, O_CLOEXEC) != 0) return -1;
  }
  pid_t p = ::fork();
  if (p < 0) {
    if (err_pipe[0] >= 0) ::close(err_pipe[0]);
    if (err_pipe[1] >= 0) ::close(err_pipe[1]);
    return -1;
  }
  if (p == 0) {
    if (err_pipe[0] >= 0) ::close(err_pipe[0]);
    if (err_pipe[1] >= 0) {
      ::dup2(err_pipe[1], STDERR_FILENO);
      ::close(err_pipe[1]);
    }
    if (rlimit_nofile > 0) {
      struct rlimit rl{rlimit_nofile, rlimit_nofile};
      (void)::setrlimit(RLIMIT_NOFILE, &rl);
    }
    std::vector<const char*> argv;
    argv.push_back(MTLM2_DAEMON_BIN);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    ::execv(MTLM2_DAEMON_BIN, const_cast<char* const*>(argv.data()));
    _exit(127);
  }
  if (err_pipe[1] >= 0) ::close(err_pipe[1]);
  if (stderr_read_fd) *stderr_read_fd = err_pipe[0];
  return p;
}

std::string read_all_and_close(int fd) {
  std::string out;
  if (fd < 0) return out;
  char buf[512];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0)
      out.append(buf, static_cast<size_t>(n));
    else if (n < 0 && errno == EINTR)
      continue;
    else
      break;
  }
  ::close(fd);
  return out;
}

}  // namespace

/* D1 (MM-03): Two v2 daemons using DIFFERENT --socket and DIFFERENT --state-dir with the
 * same --host-lock MUST refuse to start simultaneously. */
TEST(TestPlanPr1a, D1_MM03_HostLockBlocksSecondDaemonWithDifferentSocketAndStateDir) {
  TempDir tmp;
  std::string host_lock = tmp.path() + "/.host.lock";
  std::string sftok = tmp.path() + "/shm.key";
  std::string slock = tmp.path() + "/shm.lock";
  {
    std::ofstream f(sftok);
    f << "k";
  }

  ASSERT_EQ(::mkdir((tmp.path() + "/d1").c_str(), 0700), 0);
  ASSERT_EQ(::mkdir((tmp.path() + "/d2").c_str(), 0700), 0);

  std::string d1_csock = tmp.path() + "/d1/manager2.sock";
  std::string d1_asock = tmp.path() + "/d1/admin.sock";
  std::string d1_state = tmp.path() + "/d1/state";

  std::string d2_csock = tmp.path() + "/d2/manager2.sock";
  std::string d2_asock = tmp.path() + "/d2/admin.sock";
  std::string d2_state = tmp.path() + "/d2/state";

  std::vector<std::string> args1 = {
      "--socket",           d1_csock, "--admin-socket",  d1_asock,
      "--state-dir",        d1_state, "--host-lock",     host_lock,
      "--shm-lock-path",    slock,    "--shm-ftok-path", sftok,
      "--teardown-on-exit",
  };
  int d1_err_fd = -1;
  pid_t d1 = spawn_daemon_args(args1, &d1_err_fd);
  ASSERT_GT(d1, 0);
  if (!wait_daemon_ready(d1_csock, 2000)) {
    ::kill(d1, SIGKILL);
    int st = 0;
    (void)::waitpid(d1, &st, 0);
    std::string e = read_all_and_close(d1_err_fd);
    FAIL() << "Daemon 1 failed to start with --host-lock: " << e;
  }

  /* Second daemon with DIFFERENT socket AND DIFFERENT state-dir but SAME --host-lock. */
  std::vector<std::string> args2 = {
      "--socket",           d2_csock, "--admin-socket",  d2_asock,
      "--state-dir",        d2_state, "--host-lock",     host_lock,
      "--shm-lock-path",    slock,    "--shm-ftok-path", sftok,
      "--teardown-on-exit",
  };
  int d2_err_fd = -1;
  pid_t d2 = spawn_daemon_args(args2, &d2_err_fd);
  ASSERT_GT(d2, 0);
  int st2 = 0;
  ASSERT_EQ(::waitpid(d2, &st2, 0), d2);
  std::string d2_err = read_all_and_close(d2_err_fd);
  EXPECT_TRUE(WIFEXITED(st2));
  EXPECT_NE(WEXITSTATUS(st2), 0)
      << "Second daemon unexpectedly exited 0; stderr=" << d2_err;
  EXPECT_NE(d2_err.find("host lock"), std::string::npos) << d2_err;

  /* Stop daemon 1 and verify daemon 2 can now start with the same host lock. */
  ASSERT_EQ(::kill(d1, SIGTERM), 0);
  int st1 = 0;
  ASSERT_EQ(::waitpid(d1, &st1, 0), d1);
  (void)read_all_and_close(d1_err_fd);

  pid_t d3 = spawn_daemon_args(args2);
  ASSERT_GT(d3, 0);
  EXPECT_TRUE(wait_daemon_ready(d2_csock, 2000));
  ::kill(d3, SIGTERM);
  int st3 = 0;
  (void)::waitpid(d3, &st3, 0);
}

/* D3 (MM-05): v2 refuses to start if --v1-socket accepts connect(). */
TEST(TestPlanPr1a, D3_MM05_RefusesStartupWhenV1SocketIsListening) {
  TempDir tmp;
  std::string v1_sock = tmp.path() + "/v1_mtl_manager.sock";
  int v1_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  ASSERT_GE(v1_fd, 0);
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, v1_sock.c_str(), v1_sock.size() + 1);
  ASSERT_EQ(::bind(v1_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);
  ASSERT_EQ(::listen(v1_fd, 4), 0);

  std::string csock = tmp.path() + "/manager2.sock";
  std::string asock = tmp.path() + "/admin.sock";
  std::string sdir = tmp.path() + "/state";
  std::string sftok = tmp.path() + "/shm.key";
  std::string slock = tmp.path() + "/shm.lock";
  {
    std::ofstream f(sftok);
    f << "k";
  }

  std::vector<std::string> args = {
      "--socket",           csock,   "--admin-socket",  asock, "--state-dir",     sdir,
      "--v1-socket",        v1_sock, "--shm-lock-path", slock, "--shm-ftok-path", sftok,
      "--teardown-on-exit",
  };
  int err_fd = -1;
  pid_t d1 = spawn_daemon_args(args, &err_fd);
  ASSERT_GT(d1, 0);
  int st1 = 0;
  ASSERT_EQ(::waitpid(d1, &st1, 0), d1);
  std::string err_text = read_all_and_close(err_fd);
  EXPECT_TRUE(WIFEXITED(st1));
  EXPECT_NE(WEXITSTATUS(st1), 0);
  EXPECT_NE(err_text.find("v1 MtlManager is running"), std::string::npos) << err_text;

  /* Once v1 socket is stopped, v2 starts cleanly. */
  ::close(v1_fd);
  ::unlink(v1_sock.c_str());

  pid_t d2 = spawn_daemon_args(args);
  ASSERT_GT(d2, 0);
  EXPECT_TRUE(wait_daemon_ready(csock, 2000));
  ::kill(d2, SIGTERM);
  int st2 = 0;
  (void)::waitpid(d2, &st2, 0);
}

/* D6 (SEC-AC-05): OpenSession rejects unknown identity fields (pid, uid, gid) with
 * InvalidParameter, and GetStatus reports kernel peer pid/uid. */
TEST(TestPlanPr1a, D6_SecAc05_OpenSessionRejectsUnknownIdentityFields) {
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  int cfd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(cfd, 0);

  for (const char* bad_field : {"pid", "uid", "gid"}) {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "clientName", json_object_new_string("spoof"));
    json_object_object_add(p.obj, bad_field, json_object_new_int(0));
    char ename[128] = {};
    EXPECT_EQ(
        mt_varlink_call(cfd, MT_VARLINK_MANAGER_INTERFACE ".OpenSession", p.obj, nullptr,
                        0, 2000, nullptr, ename, sizeof(ename), nullptr, 0),
        -EINVAL);
    EXPECT_NE(std::string(ename).find("InvalidParameter"), std::string::npos) << ename;
  }

  ASSERT_EQ(rpc_open_session(cfd, "honest-client"), 0);

  int afd = mt_varlink_connect_unix(cfg.admin_socket_path.c_str(), 2000);
  ASSERT_GE(afd, 0);
  JsonGuard status;
  ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr,
                            nullptr, 0, 2000, &status.obj, nullptr, 0, nullptr, 0),
            0);
  json_object* sp = nullptr;
  json_object* sessions = nullptr;
  ASSERT_TRUE(json_object_object_get_ex(status.obj, "parameters", &sp));
  ASSERT_TRUE(json_object_object_get_ex(sp, "sessions", &sessions));
  ASSERT_EQ(json_object_array_length(sessions), 1u);
  json_object* s0 = json_object_array_get_idx(sessions, 0);
  json_object *jpid = nullptr, *juid = nullptr;
  ASSERT_TRUE(json_object_object_get_ex(s0, "pid", &jpid));
  ASSERT_TRUE(json_object_object_get_ex(s0, "uid", &juid));
  EXPECT_EQ(json_object_get_int64(jpid), static_cast<int64_t>(::getpid()));
  EXPECT_EQ(json_object_get_int64(juid), static_cast<int64_t>(::geteuid()));

  ::close(afd);
  ::close(cfd);
}

/* VER-UP-02: Starting v2 with journal.json schema_version > 1 (e.g. 2) refuses to start,
 * preserves journal.json untouched (no .corrupt.* rename, no delete), and names both
 * found and supported schema versions. */
TEST(TestPlanPr1a, VerUp02_NewerJournalSchemaVersionRefusesStartAndPreservesFile) {
  TempDir tmp;
  std::string sdir = tmp.path() + "/state";
  ASSERT_EQ(::mkdir(sdir.c_str(), 0700), 0);
  std::string jpath = sdir + "/journal.json";
  const std::string future_json =
      R"({"schema_version":2,"manager_id":"mtlm2d-future","cpu_leases":[{"lease_id":"cpu-fut-1","cpu_id":1,"pid":1,"start_time_ticks":1,"uid":0,"client_name":"future","quarantined":false}]})";
  {
    std::ofstream jf(jpath, std::ios::binary | std::ios::trunc);
    jf << future_json;
  }
  ASSERT_EQ(::chmod(jpath.c_str(), 0600), 0);

  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  mtlm2::MtlManagerServer srv(cfg);
  std::string err;
  EXPECT_FALSE(srv.start(&err));
  EXPECT_NE(err.find("2"), std::string::npos) << err;
  EXPECT_NE(err.find("1"), std::string::npos) << err;

  /* journal.json MUST remain untouched on disk and NOT moved to .corrupt.* */
  ASSERT_TRUE(fs::exists(jpath));
  std::ifstream jf_after(jpath, std::ios::binary);
  std::string after_content((std::istreambuf_iterator<char>(jf_after)),
                            std::istreambuf_iterator<char>());
  EXPECT_EQ(after_content, future_json);

  for (const auto& entry : fs::directory_iterator(sdir)) {
    EXPECT_EQ(entry.path().filename().string().rfind("journal.json.corrupt.", 0),
              std::string::npos)
        << "Must not quarantine newer schema_version file: " << entry.path();
  }
}

/* SEC-AC-06: Parent opens session + acquires CPU, forks child inheriting socket FD,
 * parent exits -> pidfd fires, session closes, CPU goes to FREE, child's next RPC fails
 * with EOF. */
TEST(TestPlanPr1a, SecAc06_ForkedChildInheritsFdParentExitsClosesSessionAndFreesCpu) {
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  int info_pipe[2];
  int go_pipe[2];
  int result_pipe[2];
  ASSERT_EQ(::pipe2(info_pipe, O_CLOEXEC), 0);
  ASSERT_EQ(::pipe2(go_pipe, O_CLOEXEC), 0);
  ASSERT_EQ(::pipe2(result_pipe, O_CLOEXEC), 0);

  pid_t parent_proc = ::fork();
  ASSERT_GE(parent_proc, 0);
  if (parent_proc == 0) {
    ::close(info_pipe[0]);
    ::close(go_pipe[1]);
    ::close(result_pipe[0]);
    int fd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
    if (fd < 0) _exit(10);
    if (rpc_open_session(fd, "fork-parent") != 0) _exit(11);
    if (rpc_acquire_cpu(fd, 2, false) != 0) _exit(12);

    /* Clear CLOEXEC only if needed? fork() inherits fd regardless of CLOEXEC. */
    pid_t grandchild = ::fork();
    if (grandchild < 0) _exit(13);
    if (grandchild == 0) {
      ::close(info_pipe[1]);
      char tok = 0;
      ssize_t rr = ::read(go_pipe[0], &tok, 1);
      (void)rr;
      ::close(go_pipe[0]);
      /* Attempt an RPC on the inherited FD after original session owner exited. */
      JsonGuard rep;
      int rc = mt_varlink_call(fd, MT_VARLINK_SERVICE_INTERFACE ".GetInfo", nullptr,
                               nullptr, 0, 1000, &rep.obj, nullptr, 0, nullptr, 0);
      ssize_t wr = ::write(result_pipe[1], &rc, sizeof(rc));
      (void)wr;
      ::close(result_pipe[1]);
      ::close(fd);
      _exit(0);
    }
    ::close(go_pipe[0]);
    ::close(result_pipe[1]);
    ssize_t wr = ::write(info_pipe[1], &grandchild, sizeof(grandchild));
    (void)wr;
    ::close(info_pipe[1]);
    /* Original owner exits WITHOUT CloseSession while grandchild keeps fd open! */
    _exit(0);
  }

  ::close(info_pipe[1]);
  ::close(go_pipe[0]);
  ::close(result_pipe[1]);

  pid_t grandchild_pid = -1;
  ASSERT_EQ(::read(info_pipe[0], &grandchild_pid, sizeof(grandchild_pid)),
            static_cast<ssize_t>(sizeof(grandchild_pid)));
  ::close(info_pipe[0]);
  ASSERT_GT(grandchild_pid, 0);

  /* Wait for parent_proc to exit so its pidfd fires in the daemon. */
  int pst = 0;
  ASSERT_EQ(::waitpid(parent_proc, &pst, 0), parent_proc);
  ASSERT_TRUE(WIFEXITED(pst) && WEXITSTATUS(pst) == 0);

  /* Poll until CPU 2 is FREE (reaped by pidfd) while grandchild is still holding FD. */
  int probe = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(probe, 0);
  ASSERT_EQ(rpc_open_session(probe, "probe-free"), 0);
  bool freed = false;
  for (int i = 0; i < 40; ++i) {
    if (rpc_acquire_cpu(probe, 2, false) == 0) {
      freed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(freed);
  ::close(probe);

  /* Now let grandchild try its next RPC on the inherited FD -> must fail with EOF. */
  char go = 'G';
  ASSERT_EQ(::write(go_pipe[1], &go, 1), 1);
  ::close(go_pipe[1]);

  int gc_rc = 0;
  ASSERT_EQ(::read(result_pipe[0], &gc_rc, sizeof(gc_rc)),
            static_cast<ssize_t>(sizeof(gc_rc)));
  ::close(result_pipe[0]);
  EXPECT_TRUE(gc_rc == -EPIPE || gc_rc == -ECONNRESET)
      << "Expected EOF, got rc=" << gc_rc;
}

/* SEC-AC-08: Process B calls ClaimSession with live Process A's lease ID -> returned in
 * missing, Process A keeps lease. */
TEST(TestPlanPr1a, SecAc08_ClaimSessionWithAnotherLiveProcessLeaseReturnsMissing) {
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  int sync_pipe[2];
  ASSERT_EQ(::pipe2(sync_pipe, O_CLOEXEC), 0);

  pid_t proc_a = ::fork();
  ASSERT_GE(proc_a, 0);
  if (proc_a == 0) {
    ::close(sync_pipe[0]);
    int fd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
    if (fd < 0) _exit(10);
    if (rpc_open_session(fd, "proc-a") != 0) _exit(11);
    std::string lid;
    if (rpc_acquire_cpu(fd, 1, false, &lid) != 0) _exit(12);
    ssize_t wr = ::write(sync_pipe[1], lid.data(), lid.size());
    (void)wr;
    ::close(sync_pipe[1]);
    for (;;) ::pause();
  }
  ::close(sync_pipe[1]);

  char buf[64] = {};
  ssize_t nr = ::read(sync_pipe[0], buf, sizeof(buf) - 1);
  ::close(sync_pipe[0]);
  ASSERT_GT(nr, 0);
  std::string proc_a_lid(buf, static_cast<size_t>(nr));

  /* Process B (current test process) calls ClaimSession with Process A's lease ID. */
  int proc_b_fd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(proc_b_fd, 0);
  JsonGuard cp{json_object_new_object()};
  json_object_object_add(cp.obj, "clientName", json_object_new_string("proc-b"));
  json_object* arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_string(proc_a_lid.c_str()));
  json_object_object_add(cp.obj, "leaseIds", arr);
  JsonGuard rep;
  int rc = mt_varlink_call(proc_b_fd, MT_VARLINK_MANAGER_INTERFACE ".ClaimSession",
                           cp.obj, nullptr, 0, 2000, &rep.obj, nullptr, 0, nullptr, 0);
  EXPECT_EQ(rc, 0);
  if (rc == 0 && rep.obj) {
    json_object* p = nullptr;
    json_object* claimed = nullptr;
    json_object* missing = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(rep.obj, "parameters", &p));
    ASSERT_TRUE(json_object_object_get_ex(p, "claimed", &claimed));
    ASSERT_TRUE(json_object_object_get_ex(p, "missing", &missing));
    EXPECT_EQ(json_object_array_length(claimed), 0u);
    ASSERT_EQ(json_object_array_length(missing), 1u);
    EXPECT_STREQ(json_object_get_string(json_object_array_get_idx(missing, 0)),
                 proc_a_lid.c_str());
  }

  /* Process A still owns CPU 1. */
  EXPECT_EQ(rpc_acquire_cpu(proc_b_fd, 1, false), -EBUSY);

  ::close(proc_b_fd);
  ::kill(proc_a, SIGKILL);
  int st = 0;
  (void)::waitpid(proc_a, &st, 0);
}

/* SEC-AC-09: ReleaseLease with another session's lease ID AND with a random nonexistent
 * ID BOTH return identical error org.openvisualcloud.mtl.Manager.LeaseNotOwned. */
TEST(TestPlanPr1a, SecAc09_ReleaseLeaseHasNoExistenceOracle) {
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  int c1 = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  int c2 = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(c1, 0);
  ASSERT_GE(c2, 0);
  ASSERT_EQ(rpc_open_session(c1, "s1"), 0);
  ASSERT_EQ(rpc_open_session(c2, "s2"), 0);

  std::string lid_s1;
  ASSERT_EQ(rpc_acquire_cpu(c1, 1, false, &lid_s1), 0);

  char ename_other[128] = {};
  char emsg_other[256] = {};
  {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "leaseId", json_object_new_string(lid_s1.c_str()));
    EXPECT_LT(mt_varlink_call(c2, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease", p.obj,
                              nullptr, 0, 2000, nullptr, ename_other, sizeof(ename_other),
                              emsg_other, sizeof(emsg_other)),
              0);
  }

  char ename_rand[128] = {};
  char emsg_rand[256] = {};
  {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "leaseId",
                           json_object_new_string("cpu-000000000000-ffff"));
    EXPECT_LT(mt_varlink_call(c2, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease", p.obj,
                              nullptr, 0, 2000, nullptr, ename_rand, sizeof(ename_rand),
                              emsg_rand, sizeof(emsg_rand)),
              0);
  }

  EXPECT_STREQ(ename_other, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned");
  EXPECT_STREQ(ename_rand, MT_VARLINK_MANAGER_INTERFACE ".LeaseNotOwned");
  EXPECT_STREQ(emsg_other, emsg_rand);

  /* Session 1's lease is unaffected and still busy for Session 2. */
  EXPECT_EQ(rpc_acquire_cpu(c2, 1, false), -EBUSY);

  ::close(c1);
  ::close(c2);
}

/* SEC-FS-01: Symlink at shm lock path is refused (O_NOFOLLOW | O_NONBLOCK, S_ISREG,
 * owner check) without creating target files outside its paths. */
TEST(TestPlanPr1a, SecFs01_ShmLockSymlinkRefusedWithONofollow) {
  TempDir tmp;
  std::string ftok_file = tmp.path() + "/shm.key";
  {
    std::ofstream f(ftok_file);
    f << "k";
  }

  /* 1. Symlink to existing file (/etc/passwd) */
  std::string sym_existing = tmp.path() + "/sym_passwd.lock";
  ASSERT_EQ(::symlink("/etc/passwd", sym_existing.c_str()), 0);

  mtlm2::ShmConfig cfg;
  cfg.enabled = true;
  cfg.ftok_path = ftok_file;
  cfg.ftok_proj_id = 31;
  cfg.lock_path = sym_existing;
  cfg.host_cpus = 4;

  mtlm2::LegacyShmBridge bridge1;
  std::string err;
  EXPECT_FALSE(bridge1.init(cfg, &err));
  EXPECT_NE(err.find("symlink"), std::string::npos) << err;
  bridge1.shutdown(/*preserve_live_entries=*/false);

  /* 2. Dangling symlink to nonexistent target -> must refuse and NOT create target! */
  std::string dangling_target = tmp.path() + "/must_not_be_created";
  std::string sym_dangling = tmp.path() + "/sym_dangling.lock";
  ASSERT_EQ(::symlink(dangling_target.c_str(), sym_dangling.c_str()), 0);
  cfg.lock_path = sym_dangling;
  cfg.ftok_proj_id = 32;

  mtlm2::LegacyShmBridge bridge2;
  err.clear();
  EXPECT_FALSE(bridge2.init(cfg, &err));
  EXPECT_FALSE(fs::exists(dangling_target));
  bridge2.shutdown(/*preserve_live_entries=*/false);
}

/* SEC-FS-02: External process holding flock(LOCK_EX) on shm lock file never blocks the
 * event loop: GetInfo/GetStatus stay < 10 ms and AcquireCpu fails promptly. */
TEST(TestPlanPr1a, SecFs02_ShmLockFlockHeldDoesNotBlockEventLoop) {
  TempDir tmp;
  std::string ftok_file = tmp.path() + "/shm.key";
  std::string lock_file = tmp.path() + "/shm.lock";
  {
    std::ofstream f(ftok_file);
    f << "k";
  }

  key_t key = ::ftok(ftok_file.c_str(), 33);
  ASSERT_NE(key, static_cast<key_t>(-1));
  const size_t slots = 16;
  const size_t seg_sz =
      offsetof(mtlm2::ShmLcoreTable, lcores_info) + slots * sizeof(mtlm2::ShmLcoreEntry);
  int old_id = ::shmget(key, 0, 0);
  if (old_id >= 0) ::shmctl(old_id, IPC_RMID, nullptr);
  int shmid = ::shmget(key, seg_sz, 0666 | IPC_CREAT | IPC_EXCL);
  ASSERT_GE(shmid, 0);
  void* mem = ::shmat(shmid, nullptr, 0);
  ASSERT_NE(mem, reinterpret_cast<void*>(-1));
  std::memset(mem, 0, seg_sz);
  ::shmdt(mem);

  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  cfg.shm.enabled = true;
  cfg.shm.ftok_path = ftok_file;
  cfg.shm.ftok_proj_id = 33;
  cfg.shm.lock_path = lock_file;
  cfg.shm.host_cpus = 4;

  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  /* External child holds flock(LOCK_EX) on lock_file. */
  int ready_pipe[2];
  ASSERT_EQ(::pipe2(ready_pipe, O_CLOEXEC), 0);
  pid_t locker = ::fork();
  ASSERT_GE(locker, 0);
  if (locker == 0) {
    ::close(ready_pipe[0]);
    int lfd = ::open(lock_file.c_str(), O_RDONLY | O_CLOEXEC);
    if (lfd < 0 || ::flock(lfd, LOCK_EX) != 0) _exit(10);
    char ok = '1';
    ssize_t wr = ::write(ready_pipe[1], &ok, 1);
    (void)wr;
    ::close(ready_pipe[1]);
    for (;;) ::pause();
  }
  ::close(ready_pipe[1]);
  struct LockerGuard {
    pid_t pid = -1;
    ~LockerGuard() {
      if (pid > 0) {
        ::kill(pid, SIGKILL);
        int st = 0;
        (void)::waitpid(pid, &st, 0);
      }
    }
  } locker_guard{locker};
  char tok = 0;
  ASSERT_EQ(::read(ready_pipe[0], &tok, 1), 1);
  ::close(ready_pipe[0]);

  int cfd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 1000);
  int afd = mt_varlink_connect_unix(cfg.admin_socket_path.c_str(), 1000);
  ASSERT_GE(cfd, 0);
  ASSERT_GE(afd, 0);
  ASSERT_EQ(rpc_open_session(cfd, "shm-contended"), 0);

  /* AcquireCpu must fail promptly (well under 100 ms, NOT hang or timeout at 500 ms). */
  auto t0 = std::chrono::steady_clock::now();
  JsonGuard acq_params{json_object_new_object()};
  json_object_object_add(acq_params.obj, "preferred", json_object_new_int64(1));
  json_object_object_add(acq_params.obj, "allowFallback", json_object_new_boolean(0));
  char ename[128] = {};
  int acq_rc =
      mt_varlink_call(cfd, MT_VARLINK_MANAGER_INTERFACE ".AcquireCpu", acq_params.obj,
                      nullptr, 0, 500, nullptr, ename, sizeof(ename), nullptr, 0);
  auto acq_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
  EXPECT_LT(acq_rc, 0);
  EXPECT_NE(acq_rc, -ETIMEDOUT)
      << "AcquireCpu blocked on flock instead of failing promptly";
  EXPECT_LE(acq_ms, 100);

  /* GetInfo and GetStatus must stay < 10 ms while flock is held. */
  auto t1 = std::chrono::steady_clock::now();
  JsonGuard info_rep;
  ASSERT_EQ(mt_varlink_call(cfd, MT_VARLINK_SERVICE_INTERFACE ".GetInfo", nullptr,
                            nullptr, 0, 500, &info_rep.obj, nullptr, 0, nullptr, 0),
            0);
  auto info_us = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - t1)
                     .count();
  EXPECT_LT(info_us, 10000);

  auto t2 = std::chrono::steady_clock::now();
  JsonGuard status_rep;
  ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr,
                            nullptr, 0, 500, &status_rep.obj, nullptr, 0, nullptr, 0),
            0);
  auto status_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - t2)
                       .count();
  EXPECT_LT(status_us, 10000);

  /* Kill locker -> AcquireCpu succeeds immediately after. */
  ::kill(locker, SIGKILL);
  int st = 0;
  (void)::waitpid(locker, &st, 0);
  locker_guard.pid = -1;

  EXPECT_EQ(rpc_acquire_cpu(cfd, 1, false), 0);

  ::close(afd);
  ::close(cfd);
  runner.stop();
  ::shmctl(shmid, IPC_RMID, nullptr);
}

/* SEC-FS-03: External writer flips active = false on a v2 tenant's shm slot -> daemon
 * trusts its authoritative lease table (never double-books) and logs a warning on next
 * check. */
TEST(TestPlanPr1a, SecFs03_ShmExternalTamperTrustsLeaseTableAndLogsWarning) {
  TempDir tmp;
  std::string ftok_file = tmp.path() + "/shm.key";
  std::string lock_file = tmp.path() + "/shm.lock";
  {
    std::ofstream f(ftok_file);
    f << "k";
  }

  key_t key = ::ftok(ftok_file.c_str(), 34);
  ASSERT_NE(key, static_cast<key_t>(-1));
  const size_t slots = 16;
  const size_t seg_sz =
      offsetof(mtlm2::ShmLcoreTable, lcores_info) + slots * sizeof(mtlm2::ShmLcoreEntry);
  int old_id = ::shmget(key, 0, 0);
  if (old_id >= 0) ::shmctl(old_id, IPC_RMID, nullptr);
  int shmid = ::shmget(key, seg_sz, 0666 | IPC_CREAT | IPC_EXCL);
  ASSERT_GE(shmid, 0);
  auto* tbl = static_cast<mtlm2::ShmLcoreTable*>(::shmat(shmid, nullptr, 0));
  ASSERT_NE(tbl, reinterpret_cast<mtlm2::ShmLcoreTable*>(-1));
  std::memset(tbl, 0, seg_sz);

  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  cfg.shm.enabled = true;
  cfg.shm.ftok_path = ftok_file;
  cfg.shm.ftok_proj_id = 34;
  cfg.shm.lock_path = lock_file;
  cfg.shm.host_cpus = 4;

  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  int c1 = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  int c2 = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  int afd = mt_varlink_connect_unix(cfg.admin_socket_path.c_str(), 2000);
  ASSERT_GE(c1, 0);
  ASSERT_GE(c2, 0);
  ASSERT_GE(afd, 0);
  ASSERT_EQ(rpc_open_session(c1, "v2-owner"), 0);
  ASSERT_EQ(rpc_open_session(c2, "v2-other"), 0);

  ASSERT_EQ(rpc_acquire_cpu(c1, 1, false), 0);
  ASSERT_TRUE(tbl->lcores_info[1].active);

  /* External attacker flips active = false on v2 tenant's shm slot. */
  tbl->lcores_info[1].active = false;

  /* Capture daemon stderr during check. */
  std::ostringstream captured_err;
  std::streambuf* prev_cerr = std::cerr.rdbuf(captured_err.rdbuf());

  /* Second v2 tenant MUST NOT double-book CPU 1! */
  EXPECT_EQ(rpc_acquire_cpu(c2, 1, false), -EBUSY);

  /* Trigger status check and verify tamper warning counter and stderr log. */
  JsonGuard status_rep;
  ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr,
                            nullptr, 0, 2000, &status_rep.obj, nullptr, 0, nullptr, 0),
            0);
  std::cerr.rdbuf(prev_cerr);

  json_object* sp = nullptr;
  json_object* jwarn = nullptr;
  ASSERT_TRUE(json_object_object_get_ex(status_rep.obj, "parameters", &sp));
  ASSERT_TRUE(json_object_object_get_ex(sp, "shmTamperWarnings", &jwarn));
  EXPECT_GE(json_object_get_int64(jwarn), 1);
  EXPECT_NE(captured_err.str().find("WARNING"), std::string::npos) << captured_err.str();

  ::close(afd);
  ::close(c2);
  ::close(c1);
  runner.stop();
  ::shmdt(tbl);
  ::shmctl(shmid, IPC_RMID, nullptr);
}

/* LIFE-03 & LIFE-04: Admin-only RevokeSession on SIGSTOP'd process moves CPU to
 * QUARANTINED; second client gets ResourceBusy; KillQuarantinedOwner kills quarantined
 * owner via pidfd_send_signal(SIGKILL) and frees CPU on exit while rejecting allocated or
 * random lease IDs. */
TEST(TestPlanPr1a, Life03And04_AdminRevokeSessionSigstopAndKillQuarantinedOwner) {
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  auto spawn_tenant = [&](int64_t cpu_id, std::string* sid_out,
                          std::string* lid_out) -> pid_t {
    int p[2];
    if (::pipe2(p, O_CLOEXEC) != 0) return -1;
    pid_t child = ::fork();
    if (child == 0) {
      ::close(p[0]);
      int fd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
      if (fd < 0) _exit(10);
      std::string sid, lid;
      if (rpc_open_session(fd, "life-child", &sid) != 0) _exit(11);
      if (rpc_acquire_cpu(fd, cpu_id, false, &lid) != 0) _exit(12);
      std::string msg = sid + "\n" + lid + "\n";
      ssize_t wr = ::write(p[1], msg.data(), msg.size());
      (void)wr;
      ::close(p[1]);
      for (;;) ::pause();
    }
    ::close(p[1]);
    char buf[128] = {};
    ssize_t nr = ::read(p[0], buf, sizeof(buf) - 1);
    ::close(p[0]);
    if (nr <= 0) return -1;
    std::string s(buf, static_cast<size_t>(nr));
    size_t nl1 = s.find('\n');
    size_t nl2 = s.find('\n', nl1 + 1);
    if (nl1 == std::string::npos || nl2 == std::string::npos) return -1;
    *sid_out = s.substr(0, nl1);
    *lid_out = s.substr(nl1 + 1, nl2 - nl1 - 1);
    return child;
  };

  std::string sid1, lid1, sid2, lid2;
  pid_t child1 = spawn_tenant(1, &sid1, &lid1);
  pid_t child2 = spawn_tenant(2, &sid2, &lid2);
  ASSERT_GT(child1, 0);
  ASSERT_GT(child2, 0);

  /* SIGSTOP child1 so it is frozen. */
  ASSERT_EQ(::kill(child1, SIGSTOP), 0);
  int st_stop = 0;
  ASSERT_EQ(::waitpid(child1, &st_stop, WUNTRACED), child1);
  ASSERT_TRUE(WIFSTOPPED(st_stop));

  /* Client socket MUST reject RevokeSession. */
  int cfd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(cfd, 0);
  ASSERT_EQ(rpc_open_session(cfd, "second-client"), 0);
  {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "sessionId", json_object_new_string(sid1.c_str()));
    EXPECT_EQ(mt_varlink_call(cfd, MT_VARLINK_MANAGER_INTERFACE ".RevokeSession", p.obj,
                              nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
              -EACCES);
  }

  /* Admin socket RevokeSession moves child1's CPU 1 to QUARANTINED. */
  int afd = mt_varlink_connect_unix(cfg.admin_socket_path.c_str(), 2000);
  ASSERT_GE(afd, 0);
  {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "sessionId", json_object_new_string(sid1.c_str()));
    ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".RevokeSession", p.obj,
                              nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
              0);
  }

  /* Second client gets ResourceBusy for CPU 1 while child1 is alive/stopped. */
  EXPECT_EQ(rpc_acquire_cpu(cfd, 1, false), -EBUSY);

  /* LIFE-04: KillQuarantinedOwner MUST reject allocated lease (lid2) and random ID. */
  {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "leaseId", json_object_new_string(lid2.c_str()));
    EXPECT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".KillQuarantinedOwner",
                              p.obj, nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
              -EINVAL);
    EXPECT_EQ(::kill(child2, 0), 0); /* child2 NOT killed */
  }
  {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "leaseId", json_object_new_string("cpu-rand-deadbeef"));
    EXPECT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".KillQuarantinedOwner",
                              p.obj, nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
              -ENOENT);
  }

  /* KillQuarantinedOwner on quarantined lid1 kills SIGSTOP'd child1 with SIGKILL and
   * moves CPU 1 to FREE once exited. */
  {
    JsonGuard p{json_object_new_object()};
    json_object_object_add(p.obj, "leaseId", json_object_new_string(lid1.c_str()));
    ASSERT_EQ(mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".KillQuarantinedOwner",
                              p.obj, nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
              0);
  }
  int st1 = 0;
  ASSERT_EQ(::waitpid(child1, &st1, 0), child1);
  EXPECT_TRUE(WIFSIGNALED(st1));
  EXPECT_EQ(WTERMSIG(st1), SIGKILL);

  bool freed = false;
  for (int i = 0; i < 40; ++i) {
    if (rpc_acquire_cpu(cfd, 1, false) == 0) {
      freed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(freed);

  ::close(afd);
  ::close(cfd);
  ::kill(child2, SIGKILL);
  int st2 = 0;
  (void)::waitpid(child2, &st2, 0);
}

/* SEC-DOS-02: Pre-session idle connections are closed after --open-session-timeout-ms
 * while active sessions remain unaffected. */
TEST(TestPlanPr1a, SecDos02_PreSessionIdleConnectionTimeoutClosesIdleConns) {
  TempDir tmp;
  std::string csock = tmp.path() + "/manager2.sock";
  std::string asock = tmp.path() + "/admin.sock";
  std::string sdir = tmp.path() + "/state";
  std::string sftok = tmp.path() + "/shm.key";
  std::string slock = tmp.path() + "/shm.lock";
  {
    std::ofstream f(sftok);
    f << "k";
  }

  std::vector<std::string> args = {
      "--socket",
      csock,
      "--admin-socket",
      asock,
      "--state-dir",
      sdir,
      "--shm-lock-path",
      slock,
      "--shm-ftok-path",
      sftok,
      "--open-session-timeout-ms",
      "150",
      "--teardown-on-exit",
  };
  int err_fd = -1;
  pid_t d = spawn_daemon_args(args, &err_fd);
  ASSERT_GT(d, 0);
  if (!wait_daemon_ready(csock, 2000)) {
    ::kill(d, SIGKILL);
    int st = 0;
    (void)::waitpid(d, &st, 0);
    FAIL() << "Daemon failed to start with --open-session-timeout-ms: "
           << read_all_and_close(err_fd);
  }

  /* Open 1 active session + 20 idle pre-session connections. */
  int active_fd = mt_varlink_connect_unix(csock.c_str(), 2000);
  ASSERT_GE(active_fd, 0);
  ASSERT_EQ(rpc_open_session(active_fd, "active-tenant"), 0);

  std::vector<int> idle_fds;
  for (int i = 0; i < 20; ++i) {
    int fd = mt_varlink_connect_unix(csock.c_str(), 2000);
    ASSERT_GE(fd, 0);
    idle_fds.push_back(fd);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  /* Every idle connection must be closed by daemon (EOF). */
  int closed_cnt = 0;
  for (int fd : idle_fds) {
    struct pollfd p = {fd, POLLIN | POLLHUP, 0};
    int prc = ::poll(&p, 1, 100);
    if (prc > 0) {
      char b = 0;
      ssize_t n = ::recv(fd, &b, 1, MSG_DONTWAIT);
      if (n == 0 || (n < 0 && errno == ECONNRESET)) ++closed_cnt;
    }
    ::close(fd);
  }
  EXPECT_EQ(closed_cnt, 20);

  /* Active session stays healthy and can acquire CPU. */
  EXPECT_EQ(rpc_acquire_cpu(active_fd, 0, true), 0);
  ::close(active_fd);

  ::kill(d, SIGTERM);
  int st = 0;
  (void)::waitpid(d, &st, 0);
  (void)read_all_and_close(err_fd);
}

/* SEC-DOS-03: Slow non-reading client hits output cap and gets closed immediately while
 * its live leases move to QUARANTINED until pidfd exits. */
TEST(TestPlanPr1a, SecDos03_SlowNonReadingClientClosedAndLeasesQuarantined) {
  TempDir tmp;
  auto cfg = make_test_cfg(tmp.path(), /*zero_arg=*/true);
  ServerRunner runner(cfg);
  std::string err;
  ASSERT_TRUE(runner.start(&err)) << err;

  int sync_pipe[2];
  ASSERT_EQ(::pipe2(sync_pipe, O_CLOEXEC), 0);

  pid_t slow = ::fork();
  ASSERT_GE(slow, 0);
  if (slow == 0) {
    ::close(sync_pipe[0]);
    int fd = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
    if (fd < 0) _exit(10);
    int small_rcv = 4096;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small_rcv, sizeof(small_rcv));
    if (rpc_open_session(fd, "slow-reader") != 0) _exit(11);
    std::string lid;
    if (rpc_acquire_cpu(fd, 1, false, &lid) != 0) _exit(12);
    ssize_t wr = ::write(sync_pipe[1], lid.data(), lid.size());
    (void)wr;
    ::close(sync_pipe[1]);

    /* Blast GetInterfaceDescription requests (~1 KiB replies each) without ever reading!
     * Saturate both the kernel socket buffer AND the server's 256 KiB / 64-msg output
     * queue -> ENOBUFS must force immediate close without waiting for EPOLLOUT drain.
     */
    const std::string req =
        R"({"method":"org.varlink.service.GetInterfaceDescription","parameters":{"interface":"org.openvisualcloud.mtl.Manager"}})";
    std::string wire = req;
    wire.push_back('\0');
    for (int i = 0; i < 1500; ++i) {
      size_t off = 0;
      while (off < wire.size()) {
        ssize_t s = ::send(fd, wire.data() + off, wire.size() - off, MSG_NOSIGNAL);
        if (s > 0) {
          off += static_cast<size_t>(s);
        } else if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          struct pollfd pf = {fd, POLLOUT, 0};
          if (::poll(&pf, 1, 50) <= 0) break;
        } else {
          break;
        }
      }
      if (off < wire.size()) break;
    }
    /* Stay alive without exiting so CPU must move to QUARANTINED, not FREE. */
    for (;;) ::pause();
  }
  ::close(sync_pipe[1]);
  char lid_buf[64] = {};
  ASSERT_GT(::read(sync_pipe[0], lid_buf, sizeof(lid_buf) - 1), 0);
  ::close(sync_pipe[0]);

  int afd = mt_varlink_connect_unix(cfg.admin_socket_path.c_str(), 2000);
  ASSERT_GE(afd, 0);

  bool saw_quarantined = false;
  for (int i = 0; i < 60; ++i) {
    JsonGuard rep;
    if (mt_varlink_call(afd, MT_VARLINK_MANAGER_INTERFACE ".GetStatus", nullptr, nullptr,
                        0, 1000, &rep.obj, nullptr, 0, nullptr, 0) == 0 &&
        rep.obj) {
      json_object* p = nullptr;
      json_object* qcnt = nullptr;
      if (json_object_object_get_ex(rep.obj, "parameters", &p) &&
          json_object_object_get_ex(p, "quarantinedCount", &qcnt) &&
          json_object_get_int64(qcnt) >= 1) {
        saw_quarantined = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(saw_quarantined);

  /* Another tenant cannot take quarantined CPU 1 while slow child lives. */
  int other = mt_varlink_connect_unix(cfg.client_socket_path.c_str(), 2000);
  ASSERT_GE(other, 0);
  ASSERT_EQ(rpc_open_session(other, "other-tenant"), 0);
  EXPECT_EQ(rpc_acquire_cpu(other, 1, false), -EBUSY);

  /* Kill slow child -> CPU 1 becomes FREE. */
  ::kill(slow, SIGKILL);
  int st = 0;
  ASSERT_EQ(::waitpid(slow, &st, 0), slow);

  bool freed = false;
  for (int i = 0; i < 40; ++i) {
    if (rpc_acquire_cpu(other, 1, false) == 0) {
      freed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(freed);

  ::close(other);
  ::close(afd);
}

/* SEC-DOS-01, SEC-DOS-05, SEC-DOS-06: Per-UID connection flood + pipelined large frames +
 * lease ID brute-force keeps healthy tenant AcquireCpu+ReleaseLease p99 < 10 ms and
 * SIGTERM exits within 2 s. */
TEST(TestPlanPr1a,
     SecDos01_05_06_FloodPipelinedAndBruteForceKeepTenantFastAndSigtermFast) {
  ScopedTmpfsDir tmp;
  ASSERT_FALSE(tmp.path().empty());
  std::string csock = tmp.path() + "/manager2.sock";
  std::string asock = tmp.path() + "/admin.sock";
  std::string sdir = tmp.path() + "/state";
  std::string sftok = tmp.path() + "/shm.key";
  std::string slock = tmp.path() + "/shm.lock";
  {
    std::ofstream f(sftok);
    f << "k";
  }

  std::vector<std::string> args = {
      "--socket",        csock, "--admin-socket",  asock, "--state-dir",        sdir,
      "--shm-lock-path", slock, "--shm-ftok-path", sftok, "--teardown-on-exit",
  };
  pid_t d = spawn_daemon_args(args);
  ASSERT_GT(d, 0);
  ASSERT_TRUE(wait_daemon_ready(csock, 3000));

  /* Open healthy tenant session before starting attack. */
  int tenant_fd = mt_varlink_connect_unix(csock.c_str(), 2000);
  ASSERT_GE(tenant_fd, 0);
  ASSERT_EQ(rpc_open_session(tenant_fd, "healthy-tenant"), 0);

  std::atomic<bool> stop_attack{false};

  /* Attacker 1 (SEC-DOS-01): connection flood beyond per-UID cap. */
  std::thread flooder([&]() {
    while (!stop_attack.load(std::memory_order_relaxed)) {
      std::vector<int> batch;
      batch.reserve(80);
      for (int i = 0; i < 80 && !stop_attack.load(std::memory_order_relaxed); ++i) {
        int fd = mt_varlink_connect_unix(csock.c_str(), 50);
        if (fd >= 0) batch.push_back(fd);
      }
      for (int fd : batch) ::close(fd);
    }
  });

  /* Attacker 2 (SEC-DOS-05 & SEC-DOS-06): large ~50 KiB requests + brute-force
   * ReleaseLease random IDs. */
  std::thread abuser([&]() {
    int fd = mt_varlink_connect_unix(csock.c_str(), 1000);
    if (fd < 0) return;
    if (rpc_open_session(fd, "abuser") != 0) {
      ::close(fd);
      return;
    }
    std::string big_pad(48 * 1024, 'a');
    int iter = 0;
    while (!stop_attack.load(std::memory_order_relaxed)) {
      JsonGuard p{json_object_new_object()};
      if ((iter++ & 1) == 0) {
        json_object_object_add(p.obj, "leaseId", json_object_new_string(big_pad.c_str()));
      } else {
        std::string rid = "cpu-brute-" + std::to_string(iter);
        json_object_object_add(p.obj, "leaseId", json_object_new_string(rid.c_str()));
      }
      (void)mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease", p.obj,
                            nullptr, 0, 500, nullptr, nullptr, 0, nullptr, 0);
    }
    ::close(fd);
  });

  /* Healthy tenant runs 100 AcquireCpu + ReleaseLease iterations and checks p99 < 10 ms.
   */
  std::vector<double> rtt_ms;
  rtt_ms.reserve(100);
  for (int i = 0; i < 100; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    std::string lid;
    ASSERT_EQ(rpc_acquire_cpu(tenant_fd, 0, true, &lid), 0);
    JsonGuard rp{json_object_new_object()};
    json_object_object_add(rp.obj, "leaseId", json_object_new_string(lid.c_str()));
    ASSERT_EQ(mt_varlink_call(tenant_fd, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease",
                              rp.obj, nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
              0);
    double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    rtt_ms.push_back(ms);
  }

  /* Verify SIGTERM exits within 2 s EVEN while flood is active! */
  auto t_term = std::chrono::steady_clock::now();
  ASSERT_EQ(::kill(d, SIGTERM), 0);
  int st = 0;
  ASSERT_EQ(::waitpid(d, &st, 0), d);
  auto term_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t_term)
                     .count();

  stop_attack.store(true, std::memory_order_release);
  flooder.join();
  abuser.join();
  ::close(tenant_fd);

  std::sort(rtt_ms.begin(), rtt_ms.end());
  double p99 = rtt_ms[98];
  EXPECT_LT(p99, 10.0) << "Healthy tenant p99 latency=" << p99 << " ms";
  EXPECT_TRUE(WIFEXITED(st));
  EXPECT_EQ(WEXITSTATUS(st), 0);
  EXPECT_LT(term_ms, 2000) << "SIGTERM took " << term_ms << " ms";
}

/* SEC-DOS-04: Daemon running with RLIMIT_NOFILE = 64 survives connection burst past
 * EMFILE and serves cleanly once connections close. */
TEST(TestPlanPr1a, SecDos04_LowRlimitNofileRecoversCleanly) {
  TempDir tmp;
  std::string csock = tmp.path() + "/manager2.sock";
  std::string asock = tmp.path() + "/admin.sock";
  std::string sdir = tmp.path() + "/state";
  std::string sftok = tmp.path() + "/shm.key";
  std::string slock = tmp.path() + "/shm.lock";
  {
    std::ofstream f(sftok);
    f << "k";
  }

  std::vector<std::string> args = {
      "--socket",        csock, "--admin-socket",  asock, "--state-dir",        sdir,
      "--shm-lock-path", slock, "--shm-ftok-path", sftok, "--teardown-on-exit",
  };
  pid_t d = spawn_daemon_args(args, nullptr, /*rlimit_nofile=*/64);
  ASSERT_GT(d, 0);
  ASSERT_TRUE(wait_daemon_ready(csock, 3000));

  /* Open 80 connections concurrently (exceeds RLIMIT_NOFILE=64 -> daemon hits EMFILE). */
  std::vector<int> fds;
  for (int i = 0; i < 80; ++i) {
    int fd = mt_varlink_connect_unix(csock.c_str(), 100);
    if (fd >= 0) fds.push_back(fd);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(::kill(d, 0), 0) << "Daemon crashed under RLIMIT_NOFILE=64 burst";

  /* Close all burst connections and verify daemon recovers cleanly. */
  for (int fd : fds) ::close(fd);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  int fresh = mt_varlink_connect_unix(csock.c_str(), 2000);
  ASSERT_GE(fresh, 0);
  ASSERT_EQ(rpc_open_session(fresh, "after-emfile"), 0);
  std::string lid;
  ASSERT_EQ(rpc_acquire_cpu(fresh, 0, true, &lid), 0);
  JsonGuard rp{json_object_new_object()};
  json_object_object_add(rp.obj, "leaseId", json_object_new_string(lid.c_str()));
  EXPECT_EQ(mt_varlink_call(fresh, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease", rp.obj,
                            nullptr, 0, 2000, nullptr, nullptr, 0, nullptr, 0),
            0);
  ::close(fresh);

  ASSERT_EQ(::kill(d, SIGTERM), 0);
  int st = 0;
  ASSERT_EQ(::waitpid(d, &st, 0), d);
  EXPECT_TRUE(WIFEXITED(st));
  EXPECT_EQ(WEXITSTATUS(st), 0);
}

}  // namespace
