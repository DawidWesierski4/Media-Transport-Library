/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mtlm2_journal.hpp"

#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace mtlm2 {

namespace {

constexpr size_t kMaxJournalBytes = 16u * 1024u * 1024u;
constexpr int kMaxTmpOpenAttempts = 8;
constexpr int kMaxLockRetries = 5;

std::atomic<uint64_t> g_tmp_counter{0};

uint64_t next_random_u64() {
  uint64_t val = 0;
  if (::getrandom(&val, sizeof(val), 0) == static_cast<ssize_t>(sizeof(val))) return val;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return static_cast<uint64_t>(now) ^ (static_cast<uint64_t>(::getpid()) << 32);
}

bool is_trusted_owner(uid_t uid) {
  return uid == ::geteuid() || uid == 0;
}

struct JsonDeleter {
  void operator()(json_object* p) const noexcept {
    if (p) json_object_put(p);
  }
};
using JsonPtr = std::unique_ptr<json_object, JsonDeleter>;

struct TokDeleter {
  void operator()(json_tokener* t) const noexcept {
    if (t) json_tokener_free(t);
  }
};
using TokPtr = std::unique_ptr<json_tokener, TokDeleter>;

bool open_verified_state_dir(const std::string& state_dir, bool create_if_missing,
                             UniqueFd* out_fd, bool* out_enoent, std::string* err_out) {
  if (out_enoent) *out_enoent = false;
  if (state_dir.empty()) {
    if (err_out) *err_out = "state directory path is empty";
    return false;
  }

  if (create_if_missing) {
    if (::mkdir(state_dir.c_str(), 0700) != 0 && errno != EEXIST) {
      if (err_out)
        *err_out = "failed to create state directory '" + state_dir +
                   "': " + std::strerror(errno);
      return false;
    }
  }

  UniqueFd dir_fd(
      ::open(state_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!dir_fd.valid()) {
    int err = errno;
    if (out_enoent && err == ENOENT) *out_enoent = true;
    if (err_out)
      *err_out =
          "failed to open state directory '" + state_dir + "': " + std::strerror(err);
    return false;
  }

  struct stat st{};
  if (::fstat(dir_fd.get(), &st) != 0) {
    if (err_out)
      *err_out = "failed to fstat state directory: " + std::string(std::strerror(errno));
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    if (err_out) *err_out = "state directory path is not a directory";
    return false;
  }
  if (!is_trusted_owner(st.st_uid)) {
    if (err_out) *err_out = "state directory has untrusted owner";
    return false;
  }
  if ((st.st_mode & 0022u) != 0) {
    if (err_out) *err_out = "state directory must not be group- or world-writable";
    return false;
  }

  *out_fd = std::move(dir_fd);
  return true;
}

bool write_all(int fd, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool get_req_str(json_object* obj, const char* key, std::string* out) {
  json_object* f = nullptr;
  if (!json_object_object_get_ex(obj, key, &f) || !f ||
      !json_object_is_type(f, json_type_string))
    return false;
  const char* s = json_object_get_string(f);
  int len = json_object_get_string_len(f);
  if (!s || len < 0 || std::memchr(s, '\0', static_cast<size_t>(len)) != nullptr)
    return false;
  out->assign(s, static_cast<size_t>(len));
  return true;
}

bool get_req_u64(json_object* obj, const char* key, uint64_t* out) {
  json_object* f = nullptr;
  if (!json_object_object_get_ex(obj, key, &f) || !f ||
      !json_object_is_type(f, json_type_int))
    return false;
  errno = 0;
  int64_t v = json_object_get_int64(f);
  if (errno == ERANGE || v < 0) return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

bool get_opt_bool(json_object* obj, const char* key, bool* out) {
  json_object* f = nullptr;
  if (!json_object_object_get_ex(obj, key, &f) || !f) return true;
  if (!json_object_is_type(f, json_type_boolean)) return false;
  *out = json_object_get_boolean(f) != 0;
  return true;
}

}  // namespace

StateJournal::StateJournal(std::string state_dir)
    : state_dir_(std::move(state_dir)), journal_path_(state_dir_ + "/journal.json") {
}

bool StateJournal::acquire_state_dir_lock(UniqueFd* out_lock_fd,
                                          std::string* err_out) const {
  if (!out_lock_fd) return false;
  UniqueFd dir_fd;
  if (!open_verified_state_dir(state_dir_, /*create_if_missing=*/true, &dir_fd, nullptr,
                               err_out))
    return false;

  const char* lock_name = ".mtlm2d.lock";
  for (int attempt = 0; attempt < kMaxLockRetries; ++attempt) {
    UniqueFd lfd(::openat(dir_fd.get(), lock_name,
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (!lfd.valid()) {
      if (err_out)
        *err_out = "failed to open state dir lock: " + std::string(std::strerror(errno));
      return false;
    }
    struct stat lst{};
    if (::fstat(lfd.get(), &lst) != 0 || !S_ISREG(lst.st_mode) ||
        !is_trusted_owner(lst.st_uid) || (lst.st_mode & 0022u) != 0) {
      if (err_out) *err_out = "state dir lock failed security verification";
      return false;
    }
    if (::flock(lfd.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        if (err_out)
          *err_out = "state directory '" + state_dir_ +
                     "' is locked by another live mtlm2d instance";
        return false;
      }
      if (err_out)
        *err_out = "flock failed on state dir lock: " + std::string(std::strerror(errno));
      return false;
    }
    struct stat pst{};
    if (::fstatat(dir_fd.get(), lock_name, &pst, AT_SYMLINK_NOFOLLOW) == 0 &&
        pst.st_dev == lst.st_dev && pst.st_ino == lst.st_ino) {
      *out_lock_fd = std::move(lfd);
      return true;
    }
  }
  if (err_out) *err_out = "state dir lock changed concurrently";
  return false;
}

bool StateJournal::save(const JournalState& state, std::string* err_out) const {
  UniqueFd dir_fd;
  if (!open_verified_state_dir(state_dir_, /*create_if_missing=*/true, &dir_fd, nullptr,
                               err_out))
    return false;

  JsonPtr root(json_object_new_object());
  if (!root) {
    if (err_out) *err_out = "failed to allocate journal JSON";
    return false;
  }

  json_object_object_add(root.get(), "schema_version",
                         json_object_new_int(state.schema_version));
  json_object_object_add(root.get(), "manager_id",
                         json_object_new_string(state.manager_id.c_str()));

  json_object* cpus = json_object_new_array();
  for (const auto& c : state.cpu_leases) {
    json_object* item = json_object_new_object();
    json_object_object_add(item, "lease_id", json_object_new_string(c.lease_id.c_str()));
    json_object_object_add(item, "cpu_id",
                           json_object_new_int64(static_cast<int64_t>(c.cpu_id)));
    json_object_object_add(item, "pid",
                           json_object_new_int64(static_cast<int64_t>(c.pid)));
    json_object_object_add(
        item, "start_time_ticks",
        json_object_new_int64(static_cast<int64_t>(c.start_time_ticks)));
    json_object_object_add(item, "uid",
                           json_object_new_int64(static_cast<int64_t>(c.uid)));
    json_object_object_add(
        item, "client_name",
        json_object_new_string_len(c.client_name.data(),
                                   static_cast<int>(c.client_name.size())));
    json_object_object_add(item, "quarantined",
                           json_object_new_boolean(c.quarantined ? 1 : 0));
    json_object_array_add(cpus, item);
  }
  json_object_object_add(root.get(), "cpu_leases", cpus);

  auto add_str = [](json_object* obj, const char* k, const std::string& v) {
    json_object_object_add(
        obj, k, json_object_new_string_len(v.data(), static_cast<int>(v.size())));
  };

  json_object* queues = json_object_new_array();
  for (const auto& q : state.queues) {
    json_object* item = json_object_new_object();
    add_str(item, "lease_id", q.lease_id);
    add_str(item, "if_name", q.if_name);
    json_object_object_add(item, "ifindex",
                           json_object_new_int64(static_cast<int64_t>(q.ifindex)));
    json_object_object_add(item, "queue_id",
                           json_object_new_int64(static_cast<int64_t>(q.queue_id)));
    json_object_object_add(item, "pid",
                           json_object_new_int64(static_cast<int64_t>(q.pid)));
    json_object_object_add(
        item, "start_time_ticks",
        json_object_new_int64(static_cast<int64_t>(q.start_time_ticks)));
    json_object_object_add(item, "uid",
                           json_object_new_int64(static_cast<int64_t>(q.uid)));
    add_str(item, "client_name", q.client_name);
    json_object_object_add(item, "quarantined",
                           json_object_new_boolean(q.quarantined ? 1 : 0));
    json_object_array_add(queues, item);
  }
  json_object_object_add(root.get(), "queues", queues);

  json_object* flows = json_object_new_array();
  for (const auto& f : state.flows) {
    json_object* item = json_object_new_object();
    add_str(item, "lease_id", f.lease_id);
    add_str(item, "queue_lease_id", f.queue_lease_id);
    add_str(item, "if_name", f.if_name);
    json_object_object_add(item, "ifindex",
                           json_object_new_int64(static_cast<int64_t>(f.ifindex)));
    json_object_object_add(item, "queue_id",
                           json_object_new_int64(static_cast<int64_t>(f.queue_id)));
    json_object_object_add(item, "rule_location",
                           json_object_new_int64(static_cast<int64_t>(f.rule_location)));
    add_str(item, "protocol", f.protocol);
    add_str(item, "src_ip", f.src_ip);
    add_str(item, "dst_ip", f.dst_ip);
    json_object_object_add(item, "src_port",
                           json_object_new_int64(static_cast<int64_t>(f.src_port)));
    json_object_object_add(item, "dst_port",
                           json_object_new_int64(static_cast<int64_t>(f.dst_port)));
    json_object_object_add(item, "pid",
                           json_object_new_int64(static_cast<int64_t>(f.pid)));
    json_object_object_add(
        item, "start_time_ticks",
        json_object_new_int64(static_cast<int64_t>(f.start_time_ticks)));
    json_object_object_add(item, "uid",
                           json_object_new_int64(static_cast<int64_t>(f.uid)));
    add_str(item, "client_name", f.client_name);
    json_object_array_add(flows, item);
  }
  json_object_object_add(root.get(), "flows", flows);

  json_object* udps = json_object_new_array();
  for (const auto& u : state.udp_filters) {
    json_object* item = json_object_new_object();
    add_str(item, "lease_id", u.lease_id);
    add_str(item, "queue_lease_id", u.queue_lease_id);
    add_str(item, "if_name", u.if_name);
    json_object_object_add(item, "ifindex",
                           json_object_new_int64(static_cast<int64_t>(u.ifindex)));
    json_object_object_add(item, "queue_id",
                           json_object_new_int64(static_cast<int64_t>(u.queue_id)));
    json_object_object_add(item, "dst_port",
                           json_object_new_int64(static_cast<int64_t>(u.dst_port)));
    json_object_object_add(item, "pid",
                           json_object_new_int64(static_cast<int64_t>(u.pid)));
    json_object_object_add(
        item, "start_time_ticks",
        json_object_new_int64(static_cast<int64_t>(u.start_time_ticks)));
    json_object_object_add(item, "uid",
                           json_object_new_int64(static_cast<int64_t>(u.uid)));
    add_str(item, "client_name", u.client_name);
    json_object_array_add(udps, item);
  }
  json_object_object_add(root.get(), "udp_filters", udps);

  json_object* xsks = json_object_new_array();
  for (const auto& x : state.xsk_attachments) {
    json_object* item = json_object_new_object();
    add_str(item, "lease_id", x.lease_id);
    add_str(item, "queue_lease_id", x.queue_lease_id);
    add_str(item, "if_name", x.if_name);
    json_object_object_add(item, "ifindex",
                           json_object_new_int64(static_cast<int64_t>(x.ifindex)));
    json_object_object_add(item, "queue_id",
                           json_object_new_int64(static_cast<int64_t>(x.queue_id)));
    json_object_object_add(item, "pid",
                           json_object_new_int64(static_cast<int64_t>(x.pid)));
    json_object_object_add(
        item, "start_time_ticks",
        json_object_new_int64(static_cast<int64_t>(x.start_time_ticks)));
    json_object_object_add(item, "uid",
                           json_object_new_int64(static_cast<int64_t>(x.uid)));
    add_str(item, "client_name", x.client_name);
    json_object_array_add(xsks, item);
  }
  json_object_object_add(root.get(), "xsk_attachments", xsks);

  json_object* xdps = json_object_new_array();
  for (const auto& xa : state.xdp_attachments) {
    json_object* item = json_object_new_object();
    add_str(item, "if_name", xa.if_name);
    json_object_object_add(item, "ifindex",
                           json_object_new_int64(static_cast<int64_t>(xa.ifindex)));
    add_str(item, "mode", xa.mode);
    json_object_array_add(xdps, item);
  }
  json_object_object_add(root.get(), "xdp_attachments", xdps);

  const char* json_str =
      json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
  if (!json_str) {
    if (err_out) *err_out = "failed to serialize journal JSON";
    return false;
  }
  std::string payload(json_str);
  payload.push_back('\n');

  std::string tmp_name;
  UniqueFd fd;
  int last_err = 0;
  for (int attempt = 0; attempt < kMaxTmpOpenAttempts; ++attempt) {
    uint64_t ctr = g_tmp_counter.fetch_add(1, std::memory_order_relaxed);
    uint64_t rnd = next_random_u64();
    tmp_name = "journal.json.tmp." + std::to_string(::getpid()) + "." +
               std::to_string(ctr) + "." + std::to_string(rnd);
    fd.reset(::openat(dir_fd.get(), tmp_name.c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                      0600));
    if (fd.valid()) break;
    last_err = errno;
    if (last_err != EEXIST) break;
  }
  if (!fd.valid()) {
    if (err_out)
      *err_out =
          "failed to create temp journal file: " + std::string(std::strerror(last_err));
    return false;
  }

  auto cleanup_tmp = [&]() {
    fd.reset();
    (void)::unlinkat(dir_fd.get(), tmp_name.c_str(), 0);
  };

  struct stat tst{};
  if (::fstat(fd.get(), &tst) != 0 || !S_ISREG(tst.st_mode) || tst.st_nlink != 1) {
    cleanup_tmp();
    if (err_out) *err_out = "temp journal file failed regular/link-count verification";
    return false;
  }
  if (::fchmod(fd.get(), 0600) != 0) {
    int err = errno;
    cleanup_tmp();
    if (err_out)
      *err_out = "fchmod(0600) failed on journal: " + std::string(std::strerror(err));
    return false;
  }
  if (!write_all(fd.get(), payload.data(), payload.size())) {
    int err = errno;
    cleanup_tmp();
    if (err_out)
      *err_out = "write failed on journal temp file: " + std::string(std::strerror(err));
    return false;
  }
  if (::fsync(fd.get()) != 0) {
    int err = errno;
    cleanup_tmp();
    if (err_out)
      *err_out = "fsync failed on journal temp file: " + std::string(std::strerror(err));
    return false;
  }
  fd.reset();

  if (::renameat(dir_fd.get(), tmp_name.c_str(), dir_fd.get(), "journal.json") != 0) {
    int err = errno;
    (void)::unlinkat(dir_fd.get(), tmp_name.c_str(), 0);
    if (err_out)
      *err_out = "renameat failed for journal.json: " + std::string(std::strerror(err));
    return false;
  }
  (void)::fsync(dir_fd.get());
  return true;
}

bool StateJournal::load(JournalState* out_state, bool* out_not_found,
                        bool* out_quarantined, std::string* err_out) const {
  if (out_not_found) *out_not_found = false;
  if (out_quarantined) *out_quarantined = false;
  if (!out_state) return false;

  bool dir_enoent = false;
  UniqueFd dir_fd;
  if (!open_verified_state_dir(state_dir_, /*create_if_missing=*/false, &dir_fd,
                               &dir_enoent, err_out)) {
    if (dir_enoent && out_not_found) *out_not_found = true;
    return false;
  }

  UniqueFd fd(::openat(dir_fd.get(), "journal.json",
                       O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (!fd.valid()) {
    int err = errno;
    if (err == ENOENT) {
      if (out_not_found) *out_not_found = true;
      if (err_out) *err_out = "journal file not found";
      return false;
    }
    if (err_out)
      *err_out = "failed to open journal.json: " + std::string(std::strerror(err));
    return false;
  }

  struct stat st{};
  if (::fstat(fd.get(), &st) != 0) {
    if (err_out) *err_out = "fstat failed on journal.json";
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    if (err_out) *err_out = "journal.json is not a regular file";
    return false;
  }
  if (!is_trusted_owner(st.st_uid)) {
    if (err_out) *err_out = "journal.json has untrusted owner";
    return false;
  }
  if ((st.st_mode & 0022u) != 0) {
    if (err_out) *err_out = "journal.json must not be group- or world-writable";
    return false;
  }

  auto quarantine = [&]() {
    fd.reset();
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    std::string qname = "journal.json.corrupt." + std::to_string(now_us);
    if (::renameat(dir_fd.get(), "journal.json", dir_fd.get(), qname.c_str()) == 0) {
      (void)::fsync(dir_fd.get());
      if (out_quarantined) *out_quarantined = true;
    }
  };

  std::string content;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd.get(), buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      if (err_out) *err_out = "failed to read journal.json";
      return false;
    }
    if (n == 0) break;
    if (content.size() + static_cast<size_t>(n) > kMaxJournalBytes) {
      quarantine();
      if (err_out) *err_out = "journal.json exceeds max size";
      return false;
    }
    content.append(buf, static_cast<size_t>(n));
  }

  if (content.empty()) {
    quarantine();
    if (err_out) *err_out = "journal.json is empty";
    return false;
  }

  TokPtr tok(json_tokener_new_ex(16));
  if (!tok) {
    if (err_out) *err_out = "failed to allocate JSON tokener";
    return false;
  }
  JsonPtr root(
      json_tokener_parse_ex(tok.get(), content.data(), static_cast<int>(content.size())));
  enum json_tokener_error jerr = json_tokener_get_error(tok.get());
  if (!root || jerr != json_tokener_success ||
      !json_object_is_type(root.get(), json_type_object)) {
    quarantine();
    if (err_out) *err_out = "corrupt journal JSON";
    return false;
  }
  size_t parsed = json_tokener_get_parse_end(tok.get());
  for (size_t i = parsed; i < content.size(); ++i) {
    if (!std::isspace(static_cast<unsigned char>(content[i]))) {
      quarantine();
      if (err_out) *err_out = "trailing data in journal JSON";
      return false;
    }
  }

  JournalState state;
  json_object* sv = nullptr;
  if (!json_object_object_get_ex(root.get(), "schema_version", &sv) || !sv ||
      !json_object_is_type(sv, json_type_int)) {
    quarantine();
    if (err_out) *err_out = "journal missing schema_version";
    return false;
  }
  state.schema_version = json_object_get_int(sv);
  if (state.schema_version > JournalState::kSupportedSchemaVersion) {
    /* VER-UP-02: preserve newer schema_version untouched (do NOT quarantine/delete). */
    if (err_out)
      *err_out = "journal schema_version " + std::to_string(state.schema_version) +
                 " exceeds supported schema_version " +
                 std::to_string(JournalState::kSupportedSchemaVersion) +
                 "; refusing to start";
    return false;
  }
  if (state.schema_version != JournalState::kSupportedSchemaVersion) {
    quarantine();
    if (err_out)
      *err_out =
          "unsupported journal schema_version " + std::to_string(state.schema_version) +
          " (supported: " + std::to_string(JournalState::kSupportedSchemaVersion) + ")";
    return false;
  }
  if (!get_req_str(root.get(), "manager_id", &state.manager_id)) {
    quarantine();
    if (err_out) *err_out = "journal missing manager_id";
    return false;
  }

  json_object* cpus = nullptr;
  if (json_object_object_get_ex(root.get(), "cpu_leases", &cpus) && cpus) {
    if (!json_object_is_type(cpus, json_type_array)) {
      quarantine();
      if (err_out) *err_out = "journal cpu_leases is not an array";
      return false;
    }
    size_t n = json_object_array_length(cpus);
    state.cpu_leases.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      json_object* item = json_object_array_get_idx(cpus, i);
      if (!item || !json_object_is_type(item, json_type_object)) {
        quarantine();
        if (err_out) *err_out = "journal cpu_leases item is not an object";
        return false;
      }
      JournalCpuLease e;
      uint64_t cpu_v = 0, pid_v = 0, uid_v = 0;
      if (!get_req_str(item, "lease_id", &e.lease_id) || e.lease_id.empty() ||
          !get_req_u64(item, "cpu_id", &cpu_v) ||
          cpu_v > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
          !get_req_u64(item, "pid", &pid_v) || pid_v == 0 ||
          pid_v > static_cast<uint64_t>(std::numeric_limits<pid_t>::max()) ||
          !get_req_u64(item, "start_time_ticks", &e.start_time_ticks) ||
          !get_req_u64(item, "uid", &uid_v) ||
          uid_v > static_cast<uint64_t>(std::numeric_limits<uid_t>::max()) ||
          !get_req_str(item, "client_name", &e.client_name) ||
          !get_opt_bool(item, "quarantined", &e.quarantined)) {
        quarantine();
        if (err_out) *err_out = "journal cpu_leases item has invalid fields";
        return false;
      }
      e.cpu_id = static_cast<uint32_t>(cpu_v);
      e.pid = static_cast<pid_t>(pid_v);
      e.uid = static_cast<uid_t>(uid_v);
      state.cpu_leases.push_back(std::move(e));
    }
  }

  json_object* jqueues = nullptr;
  if (json_object_object_get_ex(root.get(), "queues", &jqueues) && jqueues) {
    if (!json_object_is_type(jqueues, json_type_array)) {
      quarantine();
      if (err_out) *err_out = "journal queues is not an array";
      return false;
    }
    size_t n = json_object_array_length(jqueues);
    state.queues.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      json_object* item = json_object_array_get_idx(jqueues, i);
      if (!item || !json_object_is_type(item, json_type_object)) {
        quarantine();
        if (err_out) *err_out = "journal queues item is not an object";
        return false;
      }
      JournalQueueLease e;
      uint64_t ifi = 0, qid = 0, pid_v = 0, uid_v = 0;
      if (!get_req_str(item, "lease_id", &e.lease_id) || e.lease_id.empty() ||
          !get_req_str(item, "if_name", &e.if_name) || e.if_name.empty() ||
          !get_req_u64(item, "ifindex", &ifi) ||
          ifi > static_cast<uint64_t>(std::numeric_limits<unsigned int>::max()) ||
          !get_req_u64(item, "queue_id", &qid) || qid == 0 ||
          qid > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
          !get_req_u64(item, "pid", &pid_v) || pid_v == 0 ||
          pid_v > static_cast<uint64_t>(std::numeric_limits<pid_t>::max()) ||
          !get_req_u64(item, "start_time_ticks", &e.start_time_ticks) ||
          !get_req_u64(item, "uid", &uid_v) ||
          uid_v > static_cast<uint64_t>(std::numeric_limits<uid_t>::max()) ||
          !get_req_str(item, "client_name", &e.client_name) ||
          !get_opt_bool(item, "quarantined", &e.quarantined)) {
        quarantine();
        if (err_out) *err_out = "journal queues item has invalid fields";
        return false;
      }
      e.ifindex = static_cast<unsigned int>(ifi);
      e.queue_id = static_cast<uint32_t>(qid);
      e.pid = static_cast<pid_t>(pid_v);
      e.uid = static_cast<uid_t>(uid_v);
      state.queues.push_back(std::move(e));
    }
  }

  json_object* jflows = nullptr;
  if (json_object_object_get_ex(root.get(), "flows", &jflows) && jflows) {
    if (!json_object_is_type(jflows, json_type_array)) {
      quarantine();
      if (err_out) *err_out = "journal flows is not an array";
      return false;
    }
    size_t n = json_object_array_length(jflows);
    state.flows.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      json_object* item = json_object_array_get_idx(jflows, i);
      if (!item || !json_object_is_type(item, json_type_object)) {
        quarantine();
        if (err_out) *err_out = "journal flows item is not an object";
        return false;
      }
      JournalFlowLease e;
      uint64_t ifi = 0, qid = 0, loc = 0, sp = 0, dp = 0, pid_v = 0, uid_v = 0;
      if (!get_req_str(item, "lease_id", &e.lease_id) || e.lease_id.empty() ||
          !get_req_str(item, "queue_lease_id", &e.queue_lease_id) ||
          e.queue_lease_id.empty() || !get_req_str(item, "if_name", &e.if_name) ||
          e.if_name.empty() || !get_req_u64(item, "ifindex", &ifi) ||
          ifi > static_cast<uint64_t>(std::numeric_limits<unsigned int>::max()) ||
          !get_req_u64(item, "queue_id", &qid) ||
          qid > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
          !get_req_u64(item, "rule_location", &loc) ||
          loc > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
          !get_req_str(item, "protocol", &e.protocol) ||
          !get_req_str(item, "src_ip", &e.src_ip) ||
          !get_req_str(item, "dst_ip", &e.dst_ip) ||
          !get_req_u64(item, "src_port", &sp) || sp > 65535u ||
          !get_req_u64(item, "dst_port", &dp) || dp < 1u || dp > 65535u ||
          !get_req_u64(item, "pid", &pid_v) || pid_v == 0 ||
          pid_v > static_cast<uint64_t>(std::numeric_limits<pid_t>::max()) ||
          !get_req_u64(item, "start_time_ticks", &e.start_time_ticks) ||
          !get_req_u64(item, "uid", &uid_v) ||
          uid_v > static_cast<uint64_t>(std::numeric_limits<uid_t>::max()) ||
          !get_req_str(item, "client_name", &e.client_name)) {
        quarantine();
        if (err_out) *err_out = "journal flows item has invalid fields";
        return false;
      }
      e.ifindex = static_cast<unsigned int>(ifi);
      e.queue_id = static_cast<uint32_t>(qid);
      e.rule_location = static_cast<uint32_t>(loc);
      e.src_port = static_cast<uint16_t>(sp);
      e.dst_port = static_cast<uint16_t>(dp);
      e.pid = static_cast<pid_t>(pid_v);
      e.uid = static_cast<uid_t>(uid_v);
      state.flows.push_back(std::move(e));
    }
  }

  json_object* judps = nullptr;
  if (json_object_object_get_ex(root.get(), "udp_filters", &judps) && judps) {
    if (!json_object_is_type(judps, json_type_array)) {
      quarantine();
      if (err_out) *err_out = "journal udp_filters is not an array";
      return false;
    }
    size_t n = json_object_array_length(judps);
    state.udp_filters.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      json_object* item = json_object_array_get_idx(judps, i);
      if (!item || !json_object_is_type(item, json_type_object)) {
        quarantine();
        if (err_out) *err_out = "journal udp_filters item is not an object";
        return false;
      }
      JournalUdpFilterLease e;
      uint64_t ifi = 0, qid = 0, dp = 0, pid_v = 0, uid_v = 0;
      if (!get_req_str(item, "lease_id", &e.lease_id) || e.lease_id.empty() ||
          !get_req_str(item, "queue_lease_id", &e.queue_lease_id) ||
          e.queue_lease_id.empty() || !get_req_str(item, "if_name", &e.if_name) ||
          e.if_name.empty() || !get_req_u64(item, "ifindex", &ifi) ||
          ifi > static_cast<uint64_t>(std::numeric_limits<unsigned int>::max()) ||
          !get_req_u64(item, "queue_id", &qid) ||
          qid > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
          !get_req_u64(item, "dst_port", &dp) || dp < 1u || dp > 65535u ||
          !get_req_u64(item, "pid", &pid_v) || pid_v == 0 ||
          pid_v > static_cast<uint64_t>(std::numeric_limits<pid_t>::max()) ||
          !get_req_u64(item, "start_time_ticks", &e.start_time_ticks) ||
          !get_req_u64(item, "uid", &uid_v) ||
          uid_v > static_cast<uint64_t>(std::numeric_limits<uid_t>::max()) ||
          !get_req_str(item, "client_name", &e.client_name)) {
        quarantine();
        if (err_out) *err_out = "journal udp_filters item has invalid fields";
        return false;
      }
      e.ifindex = static_cast<unsigned int>(ifi);
      e.queue_id = static_cast<uint32_t>(qid);
      e.dst_port = static_cast<uint16_t>(dp);
      e.pid = static_cast<pid_t>(pid_v);
      e.uid = static_cast<uid_t>(uid_v);
      state.udp_filters.push_back(std::move(e));
    }
  }

  json_object* jxsks = nullptr;
  if (json_object_object_get_ex(root.get(), "xsk_attachments", &jxsks) && jxsks) {
    if (!json_object_is_type(jxsks, json_type_array)) {
      quarantine();
      if (err_out) *err_out = "journal xsk_attachments is not an array";
      return false;
    }
    size_t n = json_object_array_length(jxsks);
    state.xsk_attachments.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      json_object* item = json_object_array_get_idx(jxsks, i);
      if (!item || !json_object_is_type(item, json_type_object)) {
        quarantine();
        if (err_out) *err_out = "journal xsk_attachments item is not an object";
        return false;
      }
      JournalXskAttachment e;
      uint64_t ifi = 0, qid = 0, pid_v = 0, uid_v = 0;
      if (!get_req_str(item, "lease_id", &e.lease_id) || e.lease_id.empty() ||
          !get_req_str(item, "queue_lease_id", &e.queue_lease_id) ||
          e.queue_lease_id.empty() || !get_req_str(item, "if_name", &e.if_name) ||
          e.if_name.empty() || !get_req_u64(item, "ifindex", &ifi) ||
          ifi > static_cast<uint64_t>(std::numeric_limits<unsigned int>::max()) ||
          !get_req_u64(item, "queue_id", &qid) ||
          qid > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
          !get_req_u64(item, "pid", &pid_v) || pid_v == 0 ||
          pid_v > static_cast<uint64_t>(std::numeric_limits<pid_t>::max()) ||
          !get_req_u64(item, "start_time_ticks", &e.start_time_ticks) ||
          !get_req_u64(item, "uid", &uid_v) ||
          uid_v > static_cast<uint64_t>(std::numeric_limits<uid_t>::max()) ||
          !get_req_str(item, "client_name", &e.client_name)) {
        quarantine();
        if (err_out) *err_out = "journal xsk_attachments item has invalid fields";
        return false;
      }
      e.ifindex = static_cast<unsigned int>(ifi);
      e.queue_id = static_cast<uint32_t>(qid);
      e.pid = static_cast<pid_t>(pid_v);
      e.uid = static_cast<uid_t>(uid_v);
      state.xsk_attachments.push_back(std::move(e));
    }
  }

  json_object* jxdps = nullptr;
  if (json_object_object_get_ex(root.get(), "xdp_attachments", &jxdps) && jxdps) {
    if (!json_object_is_type(jxdps, json_type_array)) {
      quarantine();
      if (err_out) *err_out = "journal xdp_attachments is not an array";
      return false;
    }
    size_t n = json_object_array_length(jxdps);
    state.xdp_attachments.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      json_object* item = json_object_array_get_idx(jxdps, i);
      if (!item || !json_object_is_type(item, json_type_object)) {
        quarantine();
        if (err_out) *err_out = "journal xdp_attachments item is not an object";
        return false;
      }
      JournalXdpAttachment e;
      uint64_t ifi = 0;
      if (!get_req_str(item, "if_name", &e.if_name) || e.if_name.empty() ||
          !get_req_u64(item, "ifindex", &ifi) ||
          ifi > static_cast<uint64_t>(std::numeric_limits<unsigned int>::max()) ||
          !get_req_str(item, "mode", &e.mode) ||
          (e.mode != "native" && e.mode != "skb")) {
        quarantine();
        if (err_out) *err_out = "journal xdp_attachments item has invalid fields";
        return false;
      }
      e.ifindex = static_cast<unsigned int>(ifi);
      state.xdp_attachments.push_back(std::move(e));
    }
  }

  *out_state = std::move(state);
  return true;
}

}  // namespace mtlm2
