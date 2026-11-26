/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <errno.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "main/mt_instance_harness.h"
#include "mtl_api.h"

namespace {

volatile sig_atomic_t g_sigpipe_cnt;

void count_sigpipe(int) {
  g_sigpipe_cnt = g_sigpipe_cnt + 1;
}

struct CapturedLog {
  enum mtl_log_level level;
  std::string text;
};

std::mutex g_log_mu;
std::vector<CapturedLog> g_logs;

void capture_log_printer(enum mtl_log_level level, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> lk(g_log_mu);
  g_logs.push_back({level, std::string(buf)});
}

class ScopedLogCapture {
 public:
  ScopedLogCapture() {
    {
      std::lock_guard<std::mutex> lk(g_log_mu);
      g_logs.clear();
    }
    mtl_set_log_printer(capture_log_printer);
  }
  ~ScopedLogCapture() {
    mtl_set_log_printer(nullptr);
  }
  std::vector<CapturedLog> snapshot() const {
    std::lock_guard<std::mutex> lk(g_log_mu);
    return g_logs;
  }
};

class ScopedEnvVar {
 public:
  ScopedEnvVar(const char* name, const char* val) : name_(name) {
    const char* prev = std::getenv(name);
    if (prev) {
      had_prev_ = true;
      prev_ = prev;
    }
    if (val)
      ::setenv(name, val, 1);
    else
      ::unsetenv(name);
  }
  ~ScopedEnvVar() {
    if (had_prev_)
      ::setenv(name_.c_str(), prev_.c_str(), 1);
    else
      ::unsetenv(name_.c_str());
  }

 private:
  std::string name_;
  bool had_prev_ = false;
  std::string prev_;
};

class ScopedTempDir {
 public:
  ScopedTempDir() {
    std::string tmpl = "/tmp/mt_inst_ut_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* res = ::mkdtemp(buf.data());
    if (res) path_ = res;
  }
  ~ScopedTempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }
  const std::string& path() const {
    return path_;
  }

 private:
  std::string path_;
};

int create_listening_unix_sock(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 8) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

class FakeVarlinkServer {
 public:
  explicit FakeVarlinkServer(std::string sock_path) : path_(std::move(sock_path)) {
  }
  ~FakeVarlinkServer() {
    stop();
  }

  bool start() {
    listen_fd_ = create_listening_unix_sock(path_);
    if (listen_fd_ < 0) return false;
    stop_.store(false);
    th_ = std::thread([this]() { serve(); });
    return true;
  }

  void stop() {
    stop_.store(true);
    if (th_.joinable()) th_.join();
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    ::unlink(path_.c_str());
  }

  std::vector<std::string> methods() const {
    std::lock_guard<std::mutex> lk(mu_);
    return methods_;
  }

  int accepted_count() const {
    return accepted_.load();
  }

 private:
  static bool send_frame(int fd, const std::string& json) {
    std::string wire = json;
    wire.push_back('\0');
    size_t off = 0;
    while (off < wire.size()) {
      ssize_t n = ::send(fd, wire.data() + off, wire.size() - off, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          struct pollfd p = {fd, POLLOUT, 0};
          (void)::poll(&p, 1, 100);
          continue;
        }
        return false;
      }
      off += static_cast<size_t>(n);
    }
    return true;
  }

  void serve() {
    while (!stop_.load()) {
      struct pollfd p = {listen_fd_, POLLIN, 0};
      int rc = ::poll(&p, 1, 20);
      if (rc <= 0) continue;
      int cfd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (cfd < 0) continue;
      accepted_.fetch_add(1);
      handle_client(cfd);
      ::close(cfd);
    }
  }

  void handle_client(int cfd) {
    std::string buf;
    char tmp[1024];
    while (!stop_.load()) {
      struct pollfd p = {cfd, POLLIN, 0};
      int rc = ::poll(&p, 1, 20);
      if (rc < 0) break;
      if (rc == 0) continue;
      ssize_t n = ::recv(cfd, tmp, sizeof(tmp), 0);
      if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        break;
      }
      buf.append(tmp, static_cast<size_t>(n));
      size_t nul = std::string::npos;
      while ((nul = buf.find('\0')) != std::string::npos) {
        std::string frame = buf.substr(0, nul);
        buf.erase(0, nul + 1);
        std::string method = extract_method(frame);
        {
          std::lock_guard<std::mutex> lk(mu_);
          methods_.push_back(method);
        }
        /* Include extra unknown fields in every reply to test VER-PROTO-02 / D6. */
        if (method == "org.varlink.service.GetInfo") {
          send_frame(
              cfd,
              R"({"parameters":{"vendor":"OpenVisualCloud","product":"MtlManager 2","version":"2.99.0-test","url":"https://example.invalid","interfaces":["org.varlink.service","org.openvisualcloud.mtl.Manager"],"unknownFutureField":42,"nestedUnknown":{"ok":true}}})");
        } else if (method == "org.openvisualcloud.mtl.Manager.OpenSession") {
          send_frame(
              cfd,
              R"({"parameters":{"sessionId":"ses-fake-1","extraUnknown":"ignored"}})");
        } else if (method == "org.openvisualcloud.mtl.Manager.AcquireCpu") {
          send_frame(
              cfd,
              R"({"parameters":{"leaseId":"cpu-fake-1","cpuId":3,"futureHint":"numa0"}})");
        } else if (method == "org.openvisualcloud.mtl.Manager.ReleaseLease") {
          send_frame(cfd, R"({"parameters":{"extraAck":true}})");
        } else if (method == "org.openvisualcloud.mtl.Manager.Ping") {
          send_frame(
              cfd,
              R"({"parameters":{"sequence":7,"serverMonotonicUsec":1234567,"extraPing":true}})");
        } else if (method == "org.openvisualcloud.mtl.Manager.Heartbeat") {
          int hb = hb_seq_.fetch_add(1) + 1;
          send_frame(
              cfd,
              std::string(R"({"parameters":{"sequence":)") + std::to_string(hb) +
                  R"(,"serverMonotonicUsec":2345678,"status":"ok","nextDeadlineMs":30000,"extraHb":1}})");
        } else if (method == "org.openvisualcloud.mtl.Manager.CloseSession") {
          send_frame(cfd, R"({"parameters":{"extraBye":1}})");
        } else {
          send_frame(
              cfd,
              R"({"error":"org.varlink.service.MethodNotFound","parameters":{"method":"unknown"}})");
        }
      }
    }
  }

  static std::string extract_method(const std::string& frame) {
    const std::string key = "\"method\":\"";
    size_t pos = frame.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    size_t end = frame.find('"', pos);
    if (end == std::string::npos) return {};
    return frame.substr(pos, end - pos);
  }

  std::string path_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::atomic<int> accepted_{0};
  std::atomic<int> hb_seq_{0};
  std::thread th_;
  mutable std::mutex mu_;
  std::vector<std::string> methods_;
};

}  // namespace

/* SIGPIPE is caught rather than left at its default, which would end the process. */
class MtInstanceManagerGoneTest : public ::testing::Test {
 protected:
  void SetUp() override {
    struct sigaction sa = {};
    sa.sa_handler = count_sigpipe;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(sigaction(SIGPIPE, &sa, &old_sa_), 0);
    g_sigpipe_cnt = 0;
    ctx_ = ut_instance_create_orphan();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_instance_destroy(ctx_);
    sigaction(SIGPIPE, &old_sa_, nullptr);
  }

  ut_instance_ctx* ctx_ = nullptr;
  struct sigaction old_sa_ = {};
};

TEST_F(MtInstanceManagerGoneTest, GetCpuFailsWithoutSigpipe) {
  char lease_id[40] = {};
  EXPECT_LT(ut_instance_get_cpu(ctx_, 1, lease_id), 0);
  EXPECT_EQ(g_sigpipe_cnt, 0);
}

TEST_F(MtInstanceManagerGoneTest, ReleaseLeaseFailsWithoutSigpipe) {
  EXPECT_LT(ut_instance_release_lease(ctx_, "cpu-orphan-1"), 0);
  EXPECT_EQ(g_sigpipe_cnt, 0);
}

/* D4 (VER-M new libmtl x v1): when v2 socket returns ENOENT and v1 socket accepts
 * connect(), fail cleanly with -EPROTONOSUPPORT instead of falling back to SysV shm. */
TEST(MtInstanceConnectTest, D4_V1SocketDetectedRefusesShmFallback) {
  ScopedTempDir tmp;
  ASSERT_FALSE(tmp.path().empty());
  const std::string v2_missing = tmp.path() + "/missing_manager2.sock";
  const std::string v1_sock = tmp.path() + "/mtl_manager.sock";

  int v1_fd = create_listening_unix_sock(v1_sock);
  ASSERT_GE(v1_fd, 0);

  ScopedEnvVar env_v2("MTL_MANAGER_SOCK", v2_missing.c_str());
  ScopedEnvVar env_v1("MTL_V1_MANAGER_SOCK", v1_sock.c_str());
  ScopedEnvVar env_req("MTL_REQUIRE_MANAGER", nullptr);
  ScopedLogCapture logs;

  ut_instance_ctx* ctx = nullptr;
  int rc = ut_instance_connect(&ctx);
  ::close(v1_fd);
  ::unlink(v1_sock.c_str());

  if (ctx) ut_instance_destroy(ctx);
  EXPECT_EQ(rc, -EPROTONOSUPPORT);
  EXPECT_EQ(ctx, nullptr);

  bool saw_v1_msg = false;
  for (const auto& entry : logs.snapshot()) {
    if (entry.level == MTL_LOG_LEVEL_ERR &&
        entry.text.find("v1 MtlManager detected; upgrade MtlManager to v2") !=
            std::string::npos) {
      saw_v1_msg = true;
    }
  }
  EXPECT_TRUE(saw_v1_msg);
}

/* D5 (SEC-AC-14): client checks server SO_PEERCRED and refuses untrusted UID. */
TEST(MtInstanceConnectTest, D5_ServerPeerCredRefusesUntrustedUidWhenRootRequired) {
  if (::geteuid() == 0) GTEST_SKIP() << "Requires non-root test UID";

  ScopedTempDir tmp;
  ASSERT_FALSE(tmp.path().empty());
  const std::string fake_sock = tmp.path() + "/fake_manager2.sock";
  FakeVarlinkServer srv(fake_sock);
  ASSERT_TRUE(srv.start());

  ScopedEnvVar env_v2("MTL_MANAGER_SOCK", fake_sock.c_str());
  ScopedEnvVar env_root("MTL_REQUIRE_ROOT_MANAGER", "1");
  ScopedLogCapture logs;

  ut_instance_ctx* ctx = nullptr;
  int rc = ut_instance_connect(&ctx);
  if (ctx) ut_instance_destroy(ctx);
  srv.stop();

  EXPECT_EQ(rc, -EACCES);
  EXPECT_EQ(ctx, nullptr);
  EXPECT_TRUE(srv.methods().empty());

  bool saw_untrusted = false;
  for (const auto& entry : logs.snapshot()) {
    if (entry.level == MTL_LOG_LEVEL_ERR &&
        entry.text.find("untrusted manager uid") != std::string::npos) {
      saw_untrusted = true;
    }
  }
  EXPECT_TRUE(saw_untrusted);
}

/* D6 (VER-PROTO-02) & VER-PROTO-03: client calls GetInfo at connect, logs version and
 * interfaces once at info level, and ignores unknown extra fields in daemon replies. */
TEST(MtInstanceConnectTest, D6_VerProto02And03_GetInfoLoggedOnceAndUnknownFieldsIgnored) {
  ScopedTempDir tmp;
  ASSERT_FALSE(tmp.path().empty());
  const std::string fake_sock = tmp.path() + "/proto_manager2.sock";
  FakeVarlinkServer srv(fake_sock);
  ASSERT_TRUE(srv.start());

  ScopedEnvVar env_v2("MTL_MANAGER_SOCK", fake_sock.c_str());
  ScopedEnvVar env_root("MTL_REQUIRE_ROOT_MANAGER", nullptr);
  ScopedLogCapture logs;

  ut_instance_ctx* ctx = nullptr;
  int rc = ut_instance_connect(&ctx);
  ASSERT_EQ(rc, 0);
  ASSERT_NE(ctx, nullptr);

  char lease_id[40] = {};
  EXPECT_EQ(ut_instance_get_cpu(ctx, 3, lease_id), 0);
  EXPECT_STREQ(lease_id, "cpu-fake-1");
  EXPECT_EQ(ut_instance_release_lease(ctx, lease_id), 0);

  ut_instance_destroy(ctx);
  srv.stop();

  const auto methods = srv.methods();
  ASSERT_GE(methods.size(), 4u);
  EXPECT_EQ(methods[0], "org.varlink.service.GetInfo");
  EXPECT_EQ(methods[1], "org.openvisualcloud.mtl.Manager.OpenSession");
  EXPECT_EQ(methods[2], "org.openvisualcloud.mtl.Manager.AcquireCpu");
  EXPECT_EQ(methods[3], "org.openvisualcloud.mtl.Manager.ReleaseLease");

  int getinfo_log_hits = 0;
  for (const auto& entry : logs.snapshot()) {
    if (entry.level == MTL_LOG_LEVEL_INFO &&
        entry.text.find("2.99.0-test") != std::string::npos &&
        entry.text.find("org.openvisualcloud.mtl.Manager") != std::string::npos) {
      ++getinfo_log_hits;
    }
  }
  EXPECT_EQ(getinfo_log_hits, 1);
}

TEST(MtInstanceConnectTest, Pr2_PingAndHeartbeatThinClientRpc) {
  ScopedTempDir tmp;
  ASSERT_FALSE(tmp.path().empty());
  const std::string fake_sock = tmp.path() + "/hb_manager2.sock";
  FakeVarlinkServer srv(fake_sock);
  ASSERT_TRUE(srv.start());

  ScopedEnvVar env_v2("MTL_MANAGER_SOCK", fake_sock.c_str());
  ScopedEnvVar env_root("MTL_REQUIRE_ROOT_MANAGER", nullptr);

  ut_instance_ctx* ctx = nullptr;
  ASSERT_EQ(ut_instance_connect(&ctx), 0);
  ASSERT_NE(ctx, nullptr);

  EXPECT_EQ(ut_instance_ping(ctx, 7), 0);
  EXPECT_EQ(ut_instance_heartbeat(ctx, "running"), 0);
  EXPECT_EQ(ut_instance_heartbeat(ctx, nullptr), 0);
  EXPECT_EQ(ut_instance_ping(ctx, -1), -EINVAL);

  ut_instance_destroy(ctx);
  srv.stop();

  const auto methods = srv.methods();
  ASSERT_GE(methods.size(), 5u);
  EXPECT_EQ(methods[2], "org.openvisualcloud.mtl.Manager.Ping");
  EXPECT_EQ(methods[3], "org.openvisualcloud.mtl.Manager.Heartbeat");
  EXPECT_EQ(methods[4], "org.openvisualcloud.mtl.Manager.Heartbeat");
}
