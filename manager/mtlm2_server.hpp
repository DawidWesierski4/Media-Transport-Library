/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef MTLM2_SERVER_HPP
#define MTLM2_SERVER_HPP

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mt_varlink.h"
#include "mtlm2_journal.hpp"
#include "mtlm2_netdev.hpp"
#include "mtlm2_peer.hpp"
#include "mtlm2_shm.hpp"

namespace mtlm2 {

struct UidQuota {
  uint32_t max_cpus = 0;
  uint32_t max_queues = 0;
  uint32_t max_flows = 0;
};

struct PolicyConfig {
  bool allow_all_single_host = false;
  std::set<uint32_t> allowed_cpus;
  std::set<std::string> allowed_interfaces;
  std::vector<std::pair<uint16_t, uint16_t>> allowed_udp_ports;
  std::map<uid_t, UidQuota> uid_quotas;
  UidQuota default_quota{};

  static PolicyConfig make_zero_arg_single_host();
  static bool load_file(const std::string& path, PolicyConfig* out, std::string* err_out);

  bool is_cpu_allowed(uint32_t cpu_id) const;
  bool is_interface_allowed(const std::string& if_name) const;
  bool is_udp_port_allowed(uint16_t port) const;
  uint32_t max_cpus_for_uid(uid_t uid) const;
  uint32_t max_queues_for_uid(uid_t uid) const;
  uint32_t max_flows_for_uid(uid_t uid) const;
};

struct ServerConfig {
  std::string client_socket_path = MT_VARLINK_DEFAULT_SOCK_PATH;
  std::string admin_socket_path = MT_VARLINK_DEFAULT_ADMIN_SOCK_PATH;
  std::string host_lock_path;
  std::string v1_socket_path = "/var/run/imtl/mtl_manager.sock";
  mode_t client_socket_mode = 0660;
  bool allow_world_client_socket = false;
  std::optional<gid_t> client_socket_gid;
  std::string state_dir = "/run/imtl/state";
  std::string proc_root = "/proc";
  PolicyConfig policy{};
  ShmConfig shm{};
  bool teardown_on_exit = false;
  bool use_signalfd = true;
  bool use_mock_netdev = false;
  std::string xdp_prog_path;
  size_t max_connections = 256;
  size_t max_conns_per_uid = 64;
  int64_t open_session_timeout_ms = 5000;
  std::vector<uint32_t> override_online_cpus;
};

enum class LeaseState {
  Allocated,
  Recovering,
  Quarantined,
};

const char* lease_state_to_string(LeaseState state) noexcept;

class MtlManagerServer {
 public:
  explicit MtlManagerServer(ServerConfig cfg,
                            std::unique_ptr<NetdevBackend> netdev = nullptr);
  ~MtlManagerServer();

  MtlManagerServer(const MtlManagerServer&) = delete;
  MtlManagerServer& operator=(const MtlManagerServer&) = delete;

  bool start(std::string* err_out);
  int run();
  void request_stop();
  void shutdown();

  bool is_started() const noexcept {
    return started_;
  }
  ShmMode shm_mode() const noexcept {
    return shm_.mode();
  }
  NetdevBackend& netdev() noexcept {
    return *netdev_;
  }

 private:
  struct Listener {
    UniqueFd fd;
    UniqueFd lock_fd;
    UniqueFd parent_dir_fd;
    std::string path;
    std::string basename;
    dev_t socket_dev = 0;
    ino_t socket_ino = 0;
    bool is_admin = false;
    bool owns_socket_file = false;
  };

  struct Connection {
    uint64_t id = 0;
    UniqueFd fd;
    bool is_admin = false;
    PeerIdentity peer;
    std::string session_id;
    std::string client_name;
    mt_varlink_reader_t reader{};
    mt_varlink_writer_t writer{};
    bool closing = false;
    bool drop_immediately = false;
    int64_t connected_mono_ns = 0;
    uint32_t epoll_events = 0;

    Connection() {
      mt_varlink_reader_init(&reader);
      mt_varlink_writer_init(&writer, 256u * 1024u, 64u);
    }
    ~Connection() {
      mt_varlink_reader_reset(&reader);
      mt_varlink_writer_clear(&writer);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
  };

  struct CpuLease {
    std::string lease_id;
    uint32_t cpu_id = 0;
    LeaseState state = LeaseState::Allocated;
    std::string session_id;
    pid_t pid = 0;
    uint64_t start_time_ticks = 0;
    uid_t uid = 0;
    std::string client_name;
    UniqueFd pidfd;
  };

  struct QueueLease {
    std::string lease_id;
    std::string if_name;
    unsigned int ifindex = 0;
    uint32_t queue_id = 0;
    LeaseState state = LeaseState::Allocated;
    std::string session_id;
    pid_t pid = 0;
    uint64_t start_time_ticks = 0;
    uid_t uid = 0;
    std::string client_name;
    UniqueFd pidfd;
  };

  struct XskLease {
    std::string lease_id;
    std::string queue_lease_id;
    std::string if_name;
    unsigned int ifindex = 0;
    uint32_t queue_id = 0;
    LeaseState state = LeaseState::Allocated;
    std::string session_id;
    pid_t pid = 0;
    uint64_t start_time_ticks = 0;
    uid_t uid = 0;
    std::string client_name;
  };

  struct FlowLease {
    std::string lease_id;
    std::string queue_lease_id;
    std::string if_name;
    unsigned int ifindex = 0;
    uint32_t queue_id = 0;
    uint32_t rule_location = 0;
    FlowSpec spec;
    LeaseState state = LeaseState::Allocated;
    std::string session_id;
    pid_t pid = 0;
    uint64_t start_time_ticks = 0;
    uid_t uid = 0;
    std::string client_name;
  };

  struct UdpFilterLease {
    std::string lease_id;
    std::string queue_lease_id;
    std::string if_name;
    unsigned int ifindex = 0;
    uint32_t queue_id = 0;
    uint16_t dst_port = 0;
    LeaseState state = LeaseState::Allocated;
    std::string session_id;
    pid_t pid = 0;
    uint64_t start_time_ticks = 0;
    uid_t uid = 0;
    std::string client_name;
  };

  struct XdpAttachment {
    std::string if_name;
    unsigned int ifindex = 0;
    XdpAttachMode mode = XdpAttachMode::None;
  };

  bool open_listener(const std::string& path, mode_t mode, std::optional<gid_t> gid,
                     bool is_admin, bool allow_world, Listener* out,
                     std::string* err_out);
  void close_listener(Listener* ls);

  bool discover_online_cpus(std::string* err_out);
  bool acquire_host_lock(std::string* err_out);
  bool check_v1_manager_absent(std::string* err_out) const;
  bool reconcile_journal_startup(std::string* err_out);
  bool persist_journal(std::string* err_out);

  bool add_epoll(int fd, uint32_t events, uint64_t tag);
  bool mod_epoll(int fd, uint32_t events, uint64_t tag);
  void del_epoll(int fd);

  void handle_accept(const Listener& ls);
  void handle_conn_event(uint64_t conn_id, uint32_t events);
  void handle_pidfd_event(int pidfd);
  void handle_queue_pidfd_event(int pidfd);
  void handle_conn_pidfd_event(int pidfd);
  void reap_dead_pidfds();
  void sweep_idle_pre_session_conns();
  void audit_shm_integrity();

  void update_conn_epoll(Connection& conn);
  void begin_conn_close(Connection& conn);
  void destroy_conn(uint64_t conn_id);

  void dispatch_frame(Connection& conn, const mt_varlink_frame_t& frame);
  void enqueue_reply(Connection& conn, struct json_object* params_owned,
                     bool allow_large = false);
  void enqueue_error(Connection& conn, const std::string& error_name,
                     const std::string& message, const std::string& param = {});

  void rpc_get_info(Connection& conn, struct json_object* params);
  void rpc_get_interface_desc(Connection& conn, struct json_object* params);
  void rpc_open_session(Connection& conn, struct json_object* params);
  void rpc_claim_session(Connection& conn, struct json_object* params);
  void rpc_close_session(Connection& conn, struct json_object* params);
  void rpc_acquire_cpu(Connection& conn, struct json_object* params);
  void rpc_acquire_queue(Connection& conn, struct json_object* params);
  void rpc_attach_xsk(Connection& conn, struct json_object* params, const int* fds,
                      size_t fd_count);
  void rpc_add_flow(Connection& conn, struct json_object* params);
  void rpc_add_udp_filter(Connection& conn, struct json_object* params);
  void rpc_release_lease(Connection& conn, struct json_object* params);
  void rpc_get_status(Connection& conn, struct json_object* params);
  void rpc_revoke_lease(Connection& conn, struct json_object* params);
  void rpc_revoke_session(Connection& conn, struct json_object* params);
  void rpc_kill_quarantined_owner(Connection& conn, struct json_object* params);

  uint32_t count_uid_cpu_leases(uid_t uid) const;
  uint32_t count_uid_queue_leases(uid_t uid) const;
  uint32_t count_uid_flow_leases(uid_t uid) const;
  bool is_cpu_allocated(uint32_t cpu_id) const;
  bool is_queue_allocated(const std::string& if_name, uint32_t queue_id) const;
  bool has_active_queue_on_iface(const std::string& if_name) const;
  bool has_duplicate_flow(const std::string& if_name, const FlowSpec& spec) const;
  bool has_xsk_on_queue_lease(const std::string& queue_lease_id) const;
  bool has_udp_filter(const std::string& if_name, uint32_t queue_id,
                      uint16_t dst_port) const;

  void strip_queue_children(const std::string& queue_lease_id);
  void maybe_detach_xdp_if_unused(const std::string& if_name);
  void release_queue_cascade(const std::string& queue_lease_id);

  std::string next_id(const char* prefix);
  void watch_lease_pidfd(CpuLease& lease);
  void watch_queue_pidfd(QueueLease& lease);

  ServerConfig cfg_;
  PeerCredExtractor peer_extractor_;
  StateJournal journal_;
  LegacyShmBridge shm_;
  std::unique_ptr<NetdevBackend> netdev_;

  UniqueFd host_lock_fd_;
  UniqueFd state_dir_lock_fd_;
  UniqueFd emfile_reserve_fd_;
  UniqueFd epoll_fd_;
  UniqueFd signal_fd_;
  UniqueFd wake_fd_;
  Listener client_listener_;
  Listener admin_listener_;

  std::vector<uint32_t> online_cpus_;
  std::map<uint32_t, int> cpu_numa_node_;
  std::map<uint64_t, std::unique_ptr<Connection>> connections_;
  std::map<int, uint64_t> conn_pidfd_to_id_;
  std::map<std::string, CpuLease> leases_;
  std::map<int, std::string> pidfd_to_lease_;
  std::map<std::string, QueueLease> queue_leases_;
  std::map<int, std::string> pidfd_to_queue_lease_;
  std::map<std::string, XskLease> xsk_leases_;
  std::map<std::string, FlowLease> flow_leases_;
  std::map<std::string, UdpFilterLease> udp_filter_leases_;
  std::map<std::string, XdpAttachment> xdp_attachments_;

  uint64_t next_conn_id_ = 1;
  uint64_t next_id_seq_ = 1;
  uint64_t shm_tamper_warnings_ = 0;
  bool started_ = false;
  std::atomic<bool> stop_requested_{false};
};

}  // namespace mtlm2

#endif  // MTLM2_SERVER_HPP
