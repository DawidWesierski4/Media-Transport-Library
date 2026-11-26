/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef MTLM2_NETDEV_HPP
#define MTLM2_NETDEV_HPP

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mtlm2_peer.hpp"

namespace mtlm2 {

enum class XdpAttachMode {
  None,
  Native,
  Skb,
};

const char* xdp_attach_mode_to_string(XdpAttachMode mode) noexcept;
bool parse_xdp_attach_mode(const std::string& text, XdpAttachMode* out) noexcept;

struct InterfaceInfo {
  std::string if_name;
  unsigned int ifindex = 0;
  uint32_t max_queues = 0;
};

struct FlowSpec {
  std::string protocol = "udp4";
  uint32_t src_ip_be = 0; /* Network byte order; 0 = wildcard */
  uint32_t dst_ip_be = 0; /* Network byte order; 0 = wildcard */
  uint16_t src_port = 0;  /* Host byte order; 0 = wildcard */
  uint16_t dst_port = 0;  /* Host byte order; 1..65535 */

  bool operator==(const FlowSpec& o) const noexcept {
    return protocol == o.protocol && src_ip_be == o.src_ip_be &&
           dst_ip_be == o.dst_ip_be && src_port == o.src_port && dst_port == o.dst_port;
  }
  bool operator<(const FlowSpec& o) const noexcept {
    if (protocol != o.protocol) return protocol < o.protocol;
    if (src_ip_be != o.src_ip_be) return src_ip_be < o.src_ip_be;
    if (dst_ip_be != o.dst_ip_be) return dst_ip_be < o.dst_ip_be;
    if (src_port != o.src_port) return src_port < o.src_port;
    return dst_port < o.dst_port;
  }
};

/* Helpers for strict IPv4 parsing and canonical formatting. */
bool parse_strict_ipv4(const std::string& text, uint32_t* out_be);
std::string format_ipv4(uint32_t ip_be);

/* Real kernel validation of an AF_XDP socket descriptor (ISSUE-51, SEC-WIRE-06/07/08). */
bool validate_linux_xsk_fd(int xsk_fd, unsigned int expected_ifindex,
                           uint32_t expected_queue_id, std::string* err_out);

struct BackendFlowRule {
  uint32_t location = 0;
  uint32_t queue_id = 0;
  FlowSpec spec;
  bool mtl_owned = false;
};

struct BackendInterfaceSnapshot {
  std::string if_name;
  unsigned int ifindex = 0;
  uint32_t max_queues = 0;
  XdpAttachMode xdp_mode = XdpAttachMode::None;
  std::set<uint32_t> xsk_queues;
  std::set<std::pair<uint32_t, uint16_t>> udp_filters;
  std::vector<BackendFlowRule> flow_rules;
};

class NetdevBackend {
 public:
  virtual ~NetdevBackend() = default;

  virtual bool query_interface(const std::string& if_name, InterfaceInfo* out,
                               std::string* err_out) = 0;

  virtual bool attach_xdp(const std::string& if_name, unsigned int ifindex,
                          XdpAttachMode* out_mode, std::string* err_out) = 0;
  virtual bool detach_xdp(const std::string& if_name, unsigned int ifindex,
                          XdpAttachMode mode, std::string* err_out) = 0;

  virtual bool validate_xsk_fd(int xsk_fd, unsigned int expected_ifindex,
                               uint32_t expected_queue_id, std::string* err_out) = 0;
  virtual bool update_xsk_map(const std::string& if_name, unsigned int ifindex,
                              uint32_t queue_id, int xsk_fd, std::string* err_out) = 0;
  virtual bool delete_xsk_map(const std::string& if_name, unsigned int ifindex,
                              uint32_t queue_id, std::string* err_out) = 0;

  virtual bool add_udp_filter(const std::string& if_name, unsigned int ifindex,
                              uint32_t queue_id, uint16_t dst_port,
                              std::string* err_out) = 0;
  virtual bool delete_udp_filter(const std::string& if_name, unsigned int ifindex,
                                 uint32_t queue_id, uint16_t dst_port,
                                 std::string* err_out) = 0;

  virtual bool add_flow(const std::string& if_name, unsigned int ifindex,
                        uint32_t queue_id, const FlowSpec& spec,
                        uint32_t* out_rule_location, std::string* err_out) = 0;
  virtual bool delete_flow(const std::string& if_name, unsigned int ifindex,
                           uint32_t rule_location, std::string* err_out) = 0;

  virtual void restore_xdp_state(const std::string& if_name, unsigned int ifindex,
                                 XdpAttachMode mode) = 0;
  virtual void restore_flow_state(const std::string& if_name, uint32_t queue_id,
                                  uint32_t rule_location, const FlowSpec& spec) = 0;
  virtual void restore_udp_filter_state(const std::string& if_name, uint32_t queue_id,
                                        uint16_t dst_port) = 0;
  virtual void restore_xsk_state(const std::string& if_name, uint32_t queue_id) = 0;

  virtual std::vector<BackendInterfaceSnapshot> snapshot_state() const = 0;
};

class LinuxNetdevBackend final : public NetdevBackend {
 public:
  explicit LinuxNetdevBackend(std::string xdp_prog_path = {});
  ~LinuxNetdevBackend() override;

  bool query_interface(const std::string& if_name, InterfaceInfo* out,
                       std::string* err_out) override;

  bool attach_xdp(const std::string& if_name, unsigned int ifindex,
                  XdpAttachMode* out_mode, std::string* err_out) override;
  bool detach_xdp(const std::string& if_name, unsigned int ifindex, XdpAttachMode mode,
                  std::string* err_out) override;

  bool validate_xsk_fd(int xsk_fd, unsigned int expected_ifindex,
                       uint32_t expected_queue_id, std::string* err_out) override;
  bool update_xsk_map(const std::string& if_name, unsigned int ifindex, uint32_t queue_id,
                      int xsk_fd, std::string* err_out) override;
  bool delete_xsk_map(const std::string& if_name, unsigned int ifindex, uint32_t queue_id,
                      std::string* err_out) override;

  bool add_udp_filter(const std::string& if_name, unsigned int ifindex, uint32_t queue_id,
                      uint16_t dst_port, std::string* err_out) override;
  bool delete_udp_filter(const std::string& if_name, unsigned int ifindex,
                         uint32_t queue_id, uint16_t dst_port,
                         std::string* err_out) override;

  bool add_flow(const std::string& if_name, unsigned int ifindex, uint32_t queue_id,
                const FlowSpec& spec, uint32_t* out_rule_location,
                std::string* err_out) override;
  bool delete_flow(const std::string& if_name, unsigned int ifindex,
                   uint32_t rule_location, std::string* err_out) override;

  void restore_xdp_state(const std::string& if_name, unsigned int ifindex,
                         XdpAttachMode mode) override;
  void restore_flow_state(const std::string& if_name, uint32_t queue_id,
                          uint32_t rule_location, const FlowSpec& spec) override;
  void restore_udp_filter_state(const std::string& if_name, uint32_t queue_id,
                                uint16_t dst_port) override;
  void restore_xsk_state(const std::string& if_name, uint32_t queue_id) override;

  std::vector<BackendInterfaceSnapshot> snapshot_state() const override;

 private:
  struct AttachedXdp {
    unsigned int ifindex = 0;
    XdpAttachMode mode = XdpAttachMode::None;
    UniqueFd xsks_map_fd;
    UniqueFd udp_filter_map_fd;
  };

  bool refresh_map_fds(const std::string& if_name, unsigned int ifindex,
                       std::string* err_out);
  std::string resolve_xdp_obj_path() const;

  std::string xdp_prog_path_;
  std::map<std::string, AttachedXdp> attached_;
  std::map<std::string, std::map<uint32_t, BackendFlowRule>> tracked_flows_;
  std::map<std::string, std::set<uint32_t>> tracked_xsks_;
  std::map<std::string, std::set<std::pair<uint32_t, uint16_t>>> tracked_udp_filters_;
};

class MockNetdevBackend final : public NetdevBackend {
 public:
  MockNetdevBackend();
  ~MockNetdevBackend() override = default;

  bool query_interface(const std::string& if_name, InterfaceInfo* out,
                       std::string* err_out) override;

  bool attach_xdp(const std::string& if_name, unsigned int ifindex,
                  XdpAttachMode* out_mode, std::string* err_out) override;
  bool detach_xdp(const std::string& if_name, unsigned int ifindex, XdpAttachMode mode,
                  std::string* err_out) override;

  bool validate_xsk_fd(int xsk_fd, unsigned int expected_ifindex,
                       uint32_t expected_queue_id, std::string* err_out) override;
  bool update_xsk_map(const std::string& if_name, unsigned int ifindex, uint32_t queue_id,
                      int xsk_fd, std::string* err_out) override;
  bool delete_xsk_map(const std::string& if_name, unsigned int ifindex, uint32_t queue_id,
                      std::string* err_out) override;

  bool add_udp_filter(const std::string& if_name, unsigned int ifindex, uint32_t queue_id,
                      uint16_t dst_port, std::string* err_out) override;
  bool delete_udp_filter(const std::string& if_name, unsigned int ifindex,
                         uint32_t queue_id, uint16_t dst_port,
                         std::string* err_out) override;

  bool add_flow(const std::string& if_name, unsigned int ifindex, uint32_t queue_id,
                const FlowSpec& spec, uint32_t* out_rule_location,
                std::string* err_out) override;
  bool delete_flow(const std::string& if_name, unsigned int ifindex,
                   uint32_t rule_location, std::string* err_out) override;

  void restore_xdp_state(const std::string& if_name, unsigned int ifindex,
                         XdpAttachMode mode) override;
  void restore_flow_state(const std::string& if_name, uint32_t queue_id,
                          uint32_t rule_location, const FlowSpec& spec) override;
  void restore_udp_filter_state(const std::string& if_name, uint32_t queue_id,
                                uint16_t dst_port) override;
  void restore_xsk_state(const std::string& if_name, uint32_t queue_id) override;

  std::vector<BackendInterfaceSnapshot> snapshot_state() const override;

  /* Fault injection hooks for unit tests. */
  bool fail_attach_xdp = false;
  bool fail_add_flow = false;
  bool fail_add_udp_filter = false;
  bool fail_update_xsk_map = false;

 private:
  struct MockIface {
    InterfaceInfo info;
    XdpAttachMode xdp_mode = XdpAttachMode::None;
    std::set<uint32_t> xsk_queues;
    std::set<std::pair<uint32_t, uint16_t>> udp_filters;
    std::map<uint32_t, BackendFlowRule> flow_rules;
    uint32_t max_flow_slots = 64;
  };

  std::map<std::string, MockIface> ifaces_;
};

}  // namespace mtlm2

#endif  // MTLM2_NETDEV_HPP
