/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mtlm2_netdev.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/if_xdp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MTL_HAS_XDP_BACKEND
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

namespace mtlm2 {

namespace {

struct MtlQueuePortKey {
  uint32_t rx_queue_index;
  uint16_t dst_port;
  uint16_t pad;
};
static_assert(sizeof(MtlQueuePortKey) == 8u, "MtlQueuePortKey must be 8 bytes");

bool check_socket_same_netns(int sock_fd, std::string* err_out) {
  struct stat self_ns{};
  if (::stat("/proc/self/ns/net", &self_ns) != 0) {
    if (err_out)
      *err_out = "failed to stat /proc/self/ns/net: " + std::string(std::strerror(errno));
    return false;
  }

  UniqueFd sk_ns(::ioctl(sock_fd, SIOCGSKNS));
  if (!sk_ns.valid()) {
    if (err_out)
      *err_out =
          "ioctl(SIOCGSKNS) failed on socket: " + std::string(std::strerror(errno));
    return false;
  }

  struct stat sk_ns_st{};
  if (::fstat(sk_ns.get(), &sk_ns_st) != 0) {
    if (err_out)
      *err_out = "fstat failed on socket netns fd: " + std::string(std::strerror(errno));
    return false;
  }

  if (sk_ns_st.st_dev != self_ns.st_dev || sk_ns_st.st_ino != self_ns.st_ino) {
    if (err_out) *err_out = "socket belongs to a different network namespace";
    return false;
  }
  return true;
}

uint32_t count_sysfs_rx_queues(const std::string& if_name) {
  std::string path = "/sys/class/net/" + if_name + "/queues";
  DIR* d = ::opendir(path.c_str());
  if (!d) return 0;
  uint32_t cnt = 0;
  while (struct dirent* ent = ::readdir(d)) {
    if (std::strncmp(ent->d_name, "rx-", 3) == 0 &&
        std::isdigit(static_cast<unsigned char>(ent->d_name[3]))) {
      ++cnt;
    }
  }
  ::closedir(d);
  return cnt;
}

}  // namespace

const char* xdp_attach_mode_to_string(XdpAttachMode mode) noexcept {
  switch (mode) {
    case XdpAttachMode::None:
      return "none";
    case XdpAttachMode::Native:
      return "native";
    case XdpAttachMode::Skb:
      return "skb";
  }
  return "none";
}

bool parse_xdp_attach_mode(const std::string& text, XdpAttachMode* out) noexcept {
  if (!out) return false;
  if (text == "none") {
    *out = XdpAttachMode::None;
    return true;
  }
  if (text == "native") {
    *out = XdpAttachMode::Native;
    return true;
  }
  if (text == "skb") {
    *out = XdpAttachMode::Skb;
    return true;
  }
  return false;
}

bool parse_strict_ipv4(const std::string& text, uint32_t* out_be) {
  if (!out_be || text.empty() || text.size() > INET_ADDRSTRLEN) return false;
  for (char c : text) {
    if (c == '\0' || std::isspace(static_cast<unsigned char>(c))) return false;
  }
  struct in_addr addr{};
  if (::inet_pton(AF_INET, text.c_str(), &addr) != 1) return false;
  *out_be = addr.s_addr;
  return true;
}

std::string format_ipv4(uint32_t ip_be) {
  struct in_addr addr{};
  addr.s_addr = ip_be;
  char buf[INET_ADDRSTRLEN] = {};
  if (!::inet_ntop(AF_INET, &addr, buf, sizeof(buf))) return "0.0.0.0";
  return std::string(buf);
}

bool validate_linux_xsk_fd(int xsk_fd, unsigned int expected_ifindex,
                           uint32_t expected_queue_id, std::string* err_out) {
  if (xsk_fd < 0) {
    if (err_out) *err_out = "invalid negative file descriptor";
    return false;
  }

  struct stat st{};
  if (::fstat(xsk_fd, &st) != 0) {
    if (err_out) *err_out = "fstat failed on XSK descriptor";
    return false;
  }
  if (!S_ISSOCK(st.st_mode)) {
    if (err_out) *err_out = "file descriptor is not a socket";
    return false;
  }

  int domain = 0;
  socklen_t dlen = sizeof(domain);
  if (::getsockopt(xsk_fd, SOL_SOCKET, SO_DOMAIN, &domain, &dlen) != 0 ||
      dlen != sizeof(domain) || domain != AF_XDP) {
    if (err_out) *err_out = "socket domain is not AF_XDP";
    return false;
  }

  int stype = 0;
  socklen_t tlen = sizeof(stype);
  if (::getsockopt(xsk_fd, SOL_SOCKET, SO_TYPE, &stype, &tlen) != 0 ||
      tlen != sizeof(stype) || stype != SOCK_RAW) {
    if (err_out) *err_out = "AF_XDP socket type is not SOCK_RAW";
    return false;
  }

  struct sockaddr_xdp sxdp{};
  socklen_t slen = sizeof(sxdp);
  if (::getsockname(xsk_fd, reinterpret_cast<struct sockaddr*>(&sxdp), &slen) != 0 ||
      slen < sizeof(sxdp) || sxdp.sxdp_family != AF_XDP) {
    if (err_out) *err_out = "getsockname failed on AF_XDP socket";
    return false;
  }
  if (sxdp.sxdp_ifindex == 0) {
    if (err_out) *err_out = "AF_XDP socket is not bound to an interface";
    return false;
  }
  if (sxdp.sxdp_ifindex != expected_ifindex || sxdp.sxdp_queue_id != expected_queue_id) {
    if (err_out)
      *err_out = "AF_XDP socket binding does not match leased (ifindex, queue_id)";
    return false;
  }

  /* ISSUE-51: verify socket belongs to daemon's network namespace. */
  if (!check_socket_same_netns(xsk_fd, err_out)) return false;

  return true;
}

/* =========================================================================
 * LinuxNetdevBackend
 * ========================================================================= */

LinuxNetdevBackend::LinuxNetdevBackend(std::string xdp_prog_path)
    : xdp_prog_path_(std::move(xdp_prog_path)) {
}

LinuxNetdevBackend::~LinuxNetdevBackend() = default;

std::string LinuxNetdevBackend::resolve_xdp_obj_path() const {
  if (!xdp_prog_path_.empty()) return xdp_prog_path_;
  std::vector<std::string> candidates;
#ifdef MTLM2_XDP_OBJ_INSTALL_PATH
  candidates.emplace_back(MTLM2_XDP_OBJ_INSTALL_PATH);
#endif
  candidates.emplace_back("/usr/local/share/mtl/mtl.xdp.o");
  candidates.emplace_back("/usr/share/mtl/mtl.xdp.o");
  candidates.emplace_back("./mtl.xdp.o");
  for (const auto& c : candidates) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(c, ec)) return c;
  }
  return candidates.front();
}

bool LinuxNetdevBackend::query_interface(const std::string& if_name, InterfaceInfo* out,
                                         std::string* err_out) {
  if (!out || if_name.empty() || if_name.size() >= IFNAMSIZ) {
    if (err_out) *err_out = "invalid interface name";
    return false;
  }
  unsigned int ifindex = ::if_nametoindex(if_name.c_str());
  if (ifindex == 0) {
    if (err_out) *err_out = "network interface '" + if_name + "' not found";
    return false;
  }

  uint32_t queues = 0;
  UniqueFd fd(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (fd.valid()) {
    struct ethtool_channels ch{};
    ch.cmd = ETHTOOL_GCHANNELS;
    struct ifreq ifr{};
    std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", if_name.c_str());
    ifr.ifr_data = reinterpret_cast<char*>(&ch);
    if (::ioctl(fd.get(), SIOCETHTOOL, &ifr) == 0) {
      queues = std::max(ch.combined_count, ch.rx_count);
      if (queues == 0) queues = std::max(ch.max_combined, ch.max_rx);
    }
  }
  if (queues == 0) queues = count_sysfs_rx_queues(if_name);
  if (queues == 0) {
    if (err_out)
      *err_out = "failed to determine RX queue count for interface '" + if_name + "'";
    return false;
  }

  out->if_name = if_name;
  out->ifindex = ifindex;
  out->max_queues = queues;
  return true;
}

bool LinuxNetdevBackend::refresh_map_fds(const std::string& if_name, unsigned int ifindex,
                                         std::string* err_out) {
#ifdef MTL_HAS_XDP_BACKEND
  auto& slot = attached_[if_name];
  slot.ifindex = ifindex;
  slot.xsks_map_fd.reset();
  slot.udp_filter_map_fd.reset();

  struct xdp_multiprog* mp = xdp_multiprog__get_from_ifindex(static_cast<int>(ifindex));
  if (!mp || libxdp_get_error(mp) != 0) {
    if (err_out) *err_out = "no XDP multiprog attached on '" + if_name + "'";
    return false;
  }

  auto inspect_prog = [&](const struct xdp_program* prog) {
    if (!prog) return;
    int pfd = xdp_program__fd(prog);
    if (pfd < 0) return;

    struct bpf_prog_info info{};
    uint32_t info_len = sizeof(info);
    if (bpf_obj_get_info_by_fd(pfd, &info, &info_len) != 0 || info.nr_map_ids == 0)
      return;

    std::vector<uint32_t> map_ids(info.nr_map_ids, 0);
    std::memset(&info, 0, sizeof(info));
    info.nr_map_ids = static_cast<uint32_t>(map_ids.size());
    info.map_ids = reinterpret_cast<uintptr_t>(map_ids.data());
    info_len = sizeof(info);
    if (bpf_obj_get_info_by_fd(pfd, &info, &info_len) != 0) return;

    for (uint32_t mid : map_ids) {
      UniqueFd mfd(bpf_map_get_fd_by_id(mid));
      if (!mfd.valid()) continue;
      struct bpf_map_info minfo{};
      uint32_t mlen = sizeof(minfo);
      if (bpf_obj_get_info_by_fd(mfd.get(), &minfo, &mlen) != 0) continue;
      if (std::strcmp(minfo.name, "xsks_map") == 0 && !slot.xsks_map_fd.valid()) {
        slot.xsks_map_fd = std::move(mfd);
      } else if (std::strcmp(minfo.name, "udp4_dp_filter") == 0 &&
                 !slot.udp_filter_map_fd.valid()) {
        slot.udp_filter_map_fd = std::move(mfd);
      }
    }
  };

  if (xdp_multiprog__is_legacy(mp)) {
    inspect_prog(xdp_multiprog__main_prog(mp));
  } else {
    for (struct xdp_program* p = xdp_multiprog__next_prog(nullptr, mp); p != nullptr;
         p = xdp_multiprog__next_prog(p, mp)) {
      inspect_prog(p);
    }
  }

  xdp_multiprog__close(mp);
  if (!slot.xsks_map_fd.valid() || !slot.udp_filter_map_fd.valid()) {
    if (err_out)
      *err_out =
          "attached XDP programs on '" + if_name + "' missing xsks_map or udp4_dp_filter";
    return false;
  }
  return true;
#else
  (void)if_name;
  (void)ifindex;
  if (err_out) *err_out = "XDP backend is not enabled in this build";
  return false;
#endif
}

bool LinuxNetdevBackend::attach_xdp(const std::string& if_name, unsigned int ifindex,
                                    XdpAttachMode* out_mode, std::string* err_out) {
#ifdef MTL_HAS_XDP_BACKEND
  auto existing = attached_.find(if_name);
  if (existing != attached_.end() && existing->second.mode != XdpAttachMode::None) {
    if (out_mode) *out_mode = existing->second.mode;
    return true;
  }

  std::string obj_path = resolve_xdp_obj_path();
  struct xdp_program* dp_prog = xdp_program__open_file(obj_path.c_str(), "xdp", nullptr);
  if (!dp_prog || libxdp_get_error(dp_prog) != 0) {
    if (err_out) *err_out = "failed to open XDP filter object '" + obj_path + "'";
    return false;
  }

  struct xdp_program* xsk_prog =
      xdp_program__find_file("xsk_def_xdp_prog.o", "xdp", nullptr);
  if (!xsk_prog || libxdp_get_error(xsk_prog) != 0) {
    xdp_program__close(dp_prog);
    if (err_out) *err_out = "failed to open built-in libxdp xsk_def_xdp_prog.o";
    return false;
  }

  struct xdp_program* progs[2] = {dp_prog, xsk_prog};
  XdpAttachMode attached_mode = XdpAttachMode::None;
  int rc =
      xdp_program__attach_multi(progs, 2, static_cast<int>(ifindex), XDP_MODE_NATIVE, 0);
  if (rc == 0) {
    attached_mode = XdpAttachMode::Native;
  } else {
    rc = xdp_program__attach_multi(progs, 2, static_cast<int>(ifindex), XDP_MODE_SKB, 0);
    if (rc == 0) {
      attached_mode = XdpAttachMode::Skb;
    }
  }

  xdp_program__close(dp_prog);
  xdp_program__close(xsk_prog);

  if (attached_mode == XdpAttachMode::None) {
    if (err_out)
      *err_out =
          "xdp_program__attach_multi failed on '" + if_name + "': " + std::strerror(-rc);
    return false;
  }

  auto& slot = attached_[if_name];
  slot.ifindex = ifindex;
  slot.mode = attached_mode;

  if (!refresh_map_fds(if_name, ifindex, err_out)) {
    std::string derr;
    (void)detach_xdp(if_name, ifindex, attached_mode, &derr);
    return false;
  }

  if (out_mode) *out_mode = attached_mode;
  return true;
#else
  (void)if_name;
  (void)ifindex;
  (void)out_mode;
  if (err_out) *err_out = "XDP backend is not enabled in this build";
  return false;
#endif
}

bool LinuxNetdevBackend::detach_xdp(const std::string& if_name, unsigned int ifindex,
                                    XdpAttachMode mode, std::string* err_out) {
#ifdef MTL_HAS_XDP_BACKEND
  (void)mode;
  auto it = attached_.find(if_name);
  if (it != attached_.end()) {
    it->second.xsks_map_fd.reset();
    it->second.udp_filter_map_fd.reset();
    attached_.erase(it);
  }
  tracked_xsks_.erase(if_name);
  tracked_udp_filters_.erase(if_name);

  struct xdp_multiprog* mp = xdp_multiprog__get_from_ifindex(static_cast<int>(ifindex));
  if (!mp || libxdp_get_error(mp) != 0) return true;
  int rc = xdp_multiprog__detach(mp);
  xdp_multiprog__close(mp);
  if (rc != 0) {
    if (err_out)
      *err_out =
          "xdp_multiprog__detach failed on '" + if_name + "': " + std::strerror(-rc);
    return false;
  }
  return true;
#else
  (void)if_name;
  (void)ifindex;
  (void)mode;
  if (err_out) *err_out = "XDP backend is not enabled in this build";
  return false;
#endif
}

bool LinuxNetdevBackend::validate_xsk_fd(int xsk_fd, unsigned int expected_ifindex,
                                         uint32_t expected_queue_id,
                                         std::string* err_out) {
  return validate_linux_xsk_fd(xsk_fd, expected_ifindex, expected_queue_id, err_out);
}

bool LinuxNetdevBackend::update_xsk_map(const std::string& if_name, unsigned int ifindex,
                                        uint32_t queue_id, int xsk_fd,
                                        std::string* err_out) {
#ifdef MTL_HAS_XDP_BACKEND
  auto it = attached_.find(if_name);
  if (it == attached_.end() || !it->second.xsks_map_fd.valid()) {
    if (!refresh_map_fds(if_name, ifindex, err_out)) return false;
    it = attached_.find(if_name);
  }
  uint32_t key = queue_id;
  int val = xsk_fd;
  if (bpf_map_update_elem(it->second.xsks_map_fd.get(), &key, &val, BPF_ANY) != 0) {
    if (err_out)
      *err_out =
          "bpf_map_update_elem(xsks_map) failed: " + std::string(std::strerror(errno));
    return false;
  }
  tracked_xsks_[if_name].insert(queue_id);
  return true;
#else
  (void)if_name;
  (void)ifindex;
  (void)queue_id;
  (void)xsk_fd;
  if (err_out) *err_out = "XDP backend is not enabled in this build";
  return false;
#endif
}

bool LinuxNetdevBackend::delete_xsk_map(const std::string& if_name, unsigned int ifindex,
                                        uint32_t queue_id, std::string* err_out) {
#ifdef MTL_HAS_XDP_BACKEND
  tracked_xsks_[if_name].erase(queue_id);
  auto it = attached_.find(if_name);
  if (it == attached_.end() || !it->second.xsks_map_fd.valid()) {
    if (!refresh_map_fds(if_name, ifindex, err_out)) return false;
    it = attached_.find(if_name);
  }
  uint32_t key = queue_id;
  if (bpf_map_delete_elem(it->second.xsks_map_fd.get(), &key) != 0 && errno != ENOENT) {
    if (err_out)
      *err_out =
          "bpf_map_delete_elem(xsks_map) failed: " + std::string(std::strerror(errno));
    return false;
  }
  return true;
#else
  (void)if_name;
  (void)ifindex;
  (void)queue_id;
  if (err_out) *err_out = "XDP backend is not enabled in this build";
  return false;
#endif
}

bool LinuxNetdevBackend::add_udp_filter(const std::string& if_name, unsigned int ifindex,
                                        uint32_t queue_id, uint16_t dst_port,
                                        std::string* err_out) {
#ifdef MTL_HAS_XDP_BACKEND
  auto it = attached_.find(if_name);
  if (it == attached_.end() || !it->second.udp_filter_map_fd.valid()) {
    if (!refresh_map_fds(if_name, ifindex, err_out)) return false;
    it = attached_.find(if_name);
  }
  MtlQueuePortKey key{queue_id, dst_port, 0};
  uint8_t val = 1;
  if (bpf_map_update_elem(it->second.udp_filter_map_fd.get(), &key, &val, BPF_ANY) != 0) {
    if (err_out)
      *err_out = "bpf_map_update_elem(udp4_dp_filter) failed: " +
                 std::string(std::strerror(errno));
    return false;
  }
  tracked_udp_filters_[if_name].insert({queue_id, dst_port});
  return true;
#else
  (void)if_name;
  (void)ifindex;
  (void)queue_id;
  (void)dst_port;
  if (err_out) *err_out = "XDP backend is not enabled in this build";
  return false;
#endif
}

bool LinuxNetdevBackend::delete_udp_filter(const std::string& if_name,
                                           unsigned int ifindex, uint32_t queue_id,
                                           uint16_t dst_port, std::string* err_out) {
#ifdef MTL_HAS_XDP_BACKEND
  tracked_udp_filters_[if_name].erase({queue_id, dst_port});
  auto it = attached_.find(if_name);
  if (it == attached_.end() || !it->second.udp_filter_map_fd.valid()) {
    if (!refresh_map_fds(if_name, ifindex, err_out)) return false;
    it = attached_.find(if_name);
  }
  MtlQueuePortKey key{queue_id, dst_port, 0};
  if (bpf_map_delete_elem(it->second.udp_filter_map_fd.get(), &key) != 0 &&
      errno != ENOENT) {
    if (err_out)
      *err_out = "bpf_map_delete_elem(udp4_dp_filter) failed: " +
                 std::string(std::strerror(errno));
    return false;
  }
  return true;
#else
  (void)if_name;
  (void)ifindex;
  (void)queue_id;
  (void)dst_port;
  if (err_out) *err_out = "XDP backend is not enabled in this build";
  return false;
#endif
}

bool LinuxNetdevBackend::add_flow(const std::string& if_name, unsigned int ifindex,
                                  uint32_t queue_id, const FlowSpec& spec,
                                  uint32_t* out_rule_location, std::string* err_out) {
  (void)ifindex;
  if (!out_rule_location || if_name.empty() || if_name.size() >= IFNAMSIZ) {
    if (err_out) *err_out = "invalid interface name for flow";
    return false;
  }
  if (spec.protocol != "udp4") {
    if (err_out) *err_out = "only udp4 flows are supported";
    return false;
  }

  UniqueFd fd(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (!fd.valid()) {
    if (err_out) *err_out = "failed to open ethtool control socket";
    return false;
  }

  struct ifreq ifr{};
  std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", if_name.c_str());

  struct ethtool_rxnfc cnt_cmd{};
  cnt_cmd.cmd = ETHTOOL_GRXCLSRLCNT;
  ifr.ifr_data = reinterpret_cast<char*>(&cnt_cmd);
  if (::ioctl(fd.get(), SIOCETHTOOL, &ifr) != 0) {
    if (err_out)
      *err_out =
          "ETHTOOL_GRXCLSRLCNT failed on '" + if_name + "': " + std::strerror(errno);
    return false;
  }

  const uint32_t table_size = cnt_cmd.data & ~static_cast<uint32_t>(RX_CLS_LOC_SPECIAL);
  const uint32_t rule_cnt = cnt_cmd.rule_cnt;
  std::set<uint32_t> occupied;
  for (const auto& [loc, r] : tracked_flows_[if_name]) {
    (void)r;
    occupied.insert(loc);
  }

  if (rule_cnt > 0 && rule_cnt <= 65536u) {
    size_t alloc_sz = sizeof(struct ethtool_rxnfc) + rule_cnt * sizeof(uint32_t);
    std::vector<uint8_t> buf(alloc_sz, 0);
    auto* all_cmd = reinterpret_cast<struct ethtool_rxnfc*>(buf.data());
    all_cmd->cmd = ETHTOOL_GRXCLSRLALL;
    all_cmd->rule_cnt = rule_cnt;
    ifr.ifr_data = reinterpret_cast<char*>(all_cmd);
    if (::ioctl(fd.get(), SIOCETHTOOL, &ifr) == 0) {
      for (uint32_t i = 0; i < all_cmd->rule_cnt; ++i) {
        occupied.insert(all_cmd->rule_locs[i]);
      }
    }
  }

  /* Downward ntuple slot allocation to avoid foreign low-index rules (RES-Q-02). */
  uint32_t chosen_loc = std::numeric_limits<uint32_t>::max();
  if (table_size > 0) {
    for (uint32_t loc = table_size; loc-- > 0;) {
      if (occupied.find(loc) == occupied.end()) {
        chosen_loc = loc;
        break;
      }
    }
  } else {
    chosen_loc = RX_CLS_LOC_ANY;
  }

  if (chosen_loc == std::numeric_limits<uint32_t>::max()) {
    if (err_out) *err_out = "no free ethtool flow rule slots on '" + if_name + "'";
    return false;
  }

  struct ethtool_rxnfc ins_cmd{};
  ins_cmd.cmd = ETHTOOL_SRXCLSRLINS;
  ins_cmd.fs.flow_type = UDP_V4_FLOW;
  ins_cmd.fs.ring_cookie = queue_id;
  ins_cmd.fs.location = chosen_loc;

  if (spec.src_ip_be != 0) {
    ins_cmd.fs.h_u.udp_ip4_spec.ip4src = spec.src_ip_be;
    ins_cmd.fs.m_u.udp_ip4_spec.ip4src = 0xFFFFFFFFu;
  }
  if (spec.dst_ip_be != 0) {
    ins_cmd.fs.h_u.udp_ip4_spec.ip4dst = spec.dst_ip_be;
    ins_cmd.fs.m_u.udp_ip4_spec.ip4dst = 0xFFFFFFFFu;
  }
  if (spec.src_port != 0) {
    ins_cmd.fs.h_u.udp_ip4_spec.psrc = htons(spec.src_port);
    ins_cmd.fs.m_u.udp_ip4_spec.psrc = 0xFFFFu;
  }
  if (spec.dst_port != 0) {
    ins_cmd.fs.h_u.udp_ip4_spec.pdst = htons(spec.dst_port);
    ins_cmd.fs.m_u.udp_ip4_spec.pdst = 0xFFFFu;
  }

  ifr.ifr_data = reinterpret_cast<char*>(&ins_cmd);
  if (::ioctl(fd.get(), SIOCETHTOOL, &ifr) != 0) {
    if (err_out)
      *err_out =
          "ETHTOOL_SRXCLSRLINS failed on '" + if_name + "': " + std::strerror(errno);
    return false;
  }

  const uint32_t final_loc = ins_cmd.fs.location;
  *out_rule_location = final_loc;
  tracked_flows_[if_name][final_loc] =
      BackendFlowRule{final_loc, queue_id, spec, /*mtl_owned=*/true};
  return true;
}

bool LinuxNetdevBackend::delete_flow(const std::string& if_name, unsigned int ifindex,
                                     uint32_t rule_location, std::string* err_out) {
  (void)ifindex;
  if (if_name.empty() || if_name.size() >= IFNAMSIZ) {
    if (err_out) *err_out = "invalid interface name";
    return false;
  }
  tracked_flows_[if_name].erase(rule_location);

  UniqueFd fd(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (!fd.valid()) {
    if (err_out) *err_out = "failed to open ethtool control socket";
    return false;
  }

  struct ethtool_rxnfc del_cmd{};
  del_cmd.cmd = ETHTOOL_SRXCLSRLDEL;
  del_cmd.fs.location = rule_location;
  struct ifreq ifr{};
  std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", if_name.c_str());
  ifr.ifr_data = reinterpret_cast<char*>(&del_cmd);
  if (::ioctl(fd.get(), SIOCETHTOOL, &ifr) != 0 && errno != ENOENT && errno != EINVAL) {
    if (err_out)
      *err_out =
          "ETHTOOL_SRXCLSRLDEL failed on '" + if_name + "': " + std::strerror(errno);
    return false;
  }
  return true;
}

void LinuxNetdevBackend::restore_xdp_state(const std::string& if_name,
                                           unsigned int ifindex, XdpAttachMode mode) {
  auto& slot = attached_[if_name];
  slot.ifindex = ifindex;
  slot.mode = mode;
  std::string err;
  (void)refresh_map_fds(if_name, ifindex, &err);
}

void LinuxNetdevBackend::restore_flow_state(const std::string& if_name, uint32_t queue_id,
                                            uint32_t rule_location,
                                            const FlowSpec& spec) {
  tracked_flows_[if_name][rule_location] =
      BackendFlowRule{rule_location, queue_id, spec, /*mtl_owned=*/true};
}

void LinuxNetdevBackend::restore_udp_filter_state(const std::string& if_name,
                                                  uint32_t queue_id, uint16_t dst_port) {
  tracked_udp_filters_[if_name].insert({queue_id, dst_port});
}

void LinuxNetdevBackend::restore_xsk_state(const std::string& if_name,
                                           uint32_t queue_id) {
  tracked_xsks_[if_name].insert(queue_id);
}

std::vector<BackendInterfaceSnapshot> LinuxNetdevBackend::snapshot_state() const {
  std::vector<BackendInterfaceSnapshot> out;
  for (const auto& [name, slot] : attached_) {
    BackendInterfaceSnapshot s;
    s.if_name = name;
    s.ifindex = slot.ifindex;
    s.xdp_mode = slot.mode;
    if (auto xit = tracked_xsks_.find(name); xit != tracked_xsks_.end())
      s.xsk_queues = xit->second;
    if (auto uit = tracked_udp_filters_.find(name); uit != tracked_udp_filters_.end())
      s.udp_filters = uit->second;
    if (auto fit = tracked_flows_.find(name); fit != tracked_flows_.end()) {
      for (const auto& [loc, rule] : fit->second) s.flow_rules.push_back(rule);
    }
    out.push_back(std::move(s));
  }
  return out;
}

/* =========================================================================
 * MockNetdevBackend (for T2 daemon tests and unit tests)
 * ========================================================================= */

MockNetdevBackend::MockNetdevBackend() {
  MockIface eth0;
  eth0.info = InterfaceInfo{"eth0", 100u, 8u};
  /* Pre-seed a non-MTL foreign flow rule at location 10 (RES-Q-02). */
  FlowSpec foreign_spec;
  foreign_spec.protocol = "udp4";
  (void)parse_strict_ipv4("192.0.2.10", &foreign_spec.dst_ip_be);
  foreign_spec.dst_port = 9999;
  eth0.flow_rules[10u] = BackendFlowRule{10u, 0u, foreign_spec, /*mtl_owned=*/false};
  ifaces_["eth0"] = std::move(eth0);

  MockIface eth1;
  eth1.info = InterfaceInfo{"eth1", 101u, 8u};
  ifaces_["eth1"] = std::move(eth1);
}

bool MockNetdevBackend::query_interface(const std::string& if_name, InterfaceInfo* out,
                                        std::string* err_out) {
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end()) {
    if (err_out) *err_out = "mock interface '" + if_name + "' not found";
    return false;
  }
  if (out) *out = it->second.info;
  return true;
}

bool MockNetdevBackend::attach_xdp(const std::string& if_name, unsigned int ifindex,
                                   XdpAttachMode* out_mode, std::string* err_out) {
  (void)ifindex;
  if (fail_attach_xdp) {
    if (err_out) *err_out = "injected attach_xdp failure";
    return false;
  }
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end()) {
    if (err_out) *err_out = "mock interface not found";
    return false;
  }
  it->second.xdp_mode = XdpAttachMode::Native;
  if (out_mode) *out_mode = XdpAttachMode::Native;
  return true;
}

bool MockNetdevBackend::detach_xdp(const std::string& if_name, unsigned int ifindex,
                                   XdpAttachMode mode, std::string* err_out) {
  (void)ifindex;
  (void)mode;
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end()) {
    if (err_out) *err_out = "mock interface not found";
    return false;
  }
  it->second.xdp_mode = XdpAttachMode::None;
  it->second.xsk_queues.clear();
  it->second.udp_filters.clear();
  return true;
}

bool MockNetdevBackend::validate_xsk_fd(int xsk_fd, unsigned int expected_ifindex,
                                        uint32_t expected_queue_id,
                                        std::string* err_out) {
  if (xsk_fd < 0) {
    if (err_out) *err_out = "invalid negative file descriptor";
    return false;
  }
  struct stat st{};
  if (::fstat(xsk_fd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
    if (err_out) *err_out = "file descriptor is not a socket";
    return false;
  }
  int domain = 0;
  socklen_t dlen = sizeof(domain);
  int stype = 0;
  socklen_t tlen = sizeof(stype);
  if (::getsockopt(xsk_fd, SOL_SOCKET, SO_DOMAIN, &domain, &dlen) != 0 ||
      ::getsockopt(xsk_fd, SOL_SOCKET, SO_TYPE, &stype, &tlen) != 0) {
    if (err_out) *err_out = "getsockopt failed on XSK candidate";
    return false;
  }
  if (domain == AF_XDP) {
    return validate_linux_xsk_fd(xsk_fd, expected_ifindex, expected_queue_id, err_out);
  }
  /* For unprivileged T2 daemon tests (--test-mock-netdev), accept AF_UNIX SOCK_SEQPACKET
   * as a synthetic XSK stand-in while strictly rejecting TCP, UNIX stream/dgram, pipe,
   * file, memfd, and unbound AF_XDP (SEC-WIRE-06). */
  if (domain == AF_UNIX && stype == SOCK_SEQPACKET) {
    return true;
  }
  if (err_out) *err_out = "file descriptor is not a bound AF_XDP socket";
  return false;
}

bool MockNetdevBackend::update_xsk_map(const std::string& if_name, unsigned int ifindex,
                                       uint32_t queue_id, int xsk_fd,
                                       std::string* err_out) {
  (void)ifindex;
  (void)xsk_fd;
  if (fail_update_xsk_map) {
    if (err_out) *err_out = "injected update_xsk_map failure";
    return false;
  }
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end()) {
    if (err_out) *err_out = "mock interface not found";
    return false;
  }
  it->second.xsk_queues.insert(queue_id);
  return true;
}

bool MockNetdevBackend::delete_xsk_map(const std::string& if_name, unsigned int ifindex,
                                       uint32_t queue_id, std::string* err_out) {
  (void)ifindex;
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end()) {
    if (err_out) *err_out = "mock interface not found";
    return false;
  }
  it->second.xsk_queues.erase(queue_id);
  return true;
}

bool MockNetdevBackend::add_udp_filter(const std::string& if_name, unsigned int ifindex,
                                       uint32_t queue_id, uint16_t dst_port,
                                       std::string* err_out) {
  (void)ifindex;
  if (fail_add_udp_filter) {
    if (err_out) *err_out = "injected add_udp_filter failure";
    return false;
  }
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end()) {
    if (err_out) *err_out = "mock interface not found";
    return false;
  }
  it->second.udp_filters.insert({queue_id, dst_port});
  return true;
}

bool MockNetdevBackend::delete_udp_filter(const std::string& if_name,
                                          unsigned int ifindex, uint32_t queue_id,
                                          uint16_t dst_port, std::string* err_out) {
  (void)ifindex;
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end()) {
    if (err_out) *err_out = "mock interface not found";
    return false;
  }
  it->second.udp_filters.erase({queue_id, dst_port});
  return true;
}

bool MockNetdevBackend::add_flow(const std::string& if_name, unsigned int ifindex,
                                 uint32_t queue_id, const FlowSpec& spec,
                                 uint32_t* out_rule_location, std::string* err_out) {
  (void)ifindex;
  if (fail_add_flow) {
    if (err_out) *err_out = "injected add_flow failure";
    return false;
  }
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end() || !out_rule_location) {
    if (err_out) *err_out = "mock interface not found";
    return false;
  }
  /* Downward slot allocation from max_flow_slots - 1 down to 1, preserving foreign rules
   * such as location 10 (RES-Q-02). */
  for (uint32_t loc = it->second.max_flow_slots; loc-- > 1;) {
    if (it->second.flow_rules.find(loc) == it->second.flow_rules.end()) {
      it->second.flow_rules[loc] =
          BackendFlowRule{loc, queue_id, spec, /*mtl_owned=*/true};
      *out_rule_location = loc;
      return true;
    }
  }
  if (err_out) *err_out = "mock flow rule table full";
  return false;
}

bool MockNetdevBackend::delete_flow(const std::string& if_name, unsigned int ifindex,
                                    uint32_t rule_location, std::string* err_out) {
  (void)ifindex;
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end()) {
    if (err_out) *err_out = "mock interface not found";
    return false;
  }
  auto rit = it->second.flow_rules.find(rule_location);
  if (rit == it->second.flow_rules.end()) return true;
  if (!rit->second.mtl_owned) {
    if (err_out) *err_out = "refusing to delete non-MTL foreign flow rule";
    return false;
  }
  it->second.flow_rules.erase(rit);
  return true;
}

void MockNetdevBackend::restore_xdp_state(const std::string& if_name,
                                          unsigned int ifindex, XdpAttachMode mode) {
  (void)ifindex;
  auto it = ifaces_.find(if_name);
  if (it != ifaces_.end()) it->second.xdp_mode = mode;
}

void MockNetdevBackend::restore_flow_state(const std::string& if_name, uint32_t queue_id,
                                           uint32_t rule_location, const FlowSpec& spec) {
  auto it = ifaces_.find(if_name);
  if (it == ifaces_.end()) return;
  auto existing = it->second.flow_rules.find(rule_location);
  if (existing != it->second.flow_rules.end() && !existing->second.mtl_owned) return;
  it->second.flow_rules[rule_location] =
      BackendFlowRule{rule_location, queue_id, spec, /*mtl_owned=*/true};
}

void MockNetdevBackend::restore_udp_filter_state(const std::string& if_name,
                                                 uint32_t queue_id, uint16_t dst_port) {
  auto it = ifaces_.find(if_name);
  if (it != ifaces_.end()) it->second.udp_filters.insert({queue_id, dst_port});
}

void MockNetdevBackend::restore_xsk_state(const std::string& if_name, uint32_t queue_id) {
  auto it = ifaces_.find(if_name);
  if (it != ifaces_.end()) it->second.xsk_queues.insert(queue_id);
}

std::vector<BackendInterfaceSnapshot> MockNetdevBackend::snapshot_state() const {
  std::vector<BackendInterfaceSnapshot> out;
  for (const auto& [name, iface] : ifaces_) {
    BackendInterfaceSnapshot s;
    s.if_name = iface.info.if_name;
    s.ifindex = iface.info.ifindex;
    s.max_queues = iface.info.max_queues;
    s.xdp_mode = iface.xdp_mode;
    s.xsk_queues = iface.xsk_queues;
    s.udp_filters = iface.udp_filters;
    for (const auto& [loc, rule] : iface.flow_rules) s.flow_rules.push_back(rule);
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace mtlm2
