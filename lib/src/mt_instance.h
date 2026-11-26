/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023-2026 Intel Corporation
 */

#ifndef _MT_LIB_INSTANCE_HEAD_H_
#define _MT_LIB_INSTANCE_HEAD_H_

#include "mt_main.h"

struct mt_instance_ctx;

int mt_instance_connect(struct mtl_init_params* p, struct mt_instance_ctx** out_ctx);
void mt_instance_disconnect(struct mt_instance_ctx* ctx);
int mt_instance_uinit(struct mtl_main_impl* impl);

/* Unit-test helper: wraps an existing socket descriptor in a manager context. */
struct mt_instance_ctx* mt_instance_create_from_fd(int sock_fd, const char* sock_path,
                                                   const char* client_name);

int mt_instance_get_cpu(struct mtl_main_impl* impl, uint32_t phys_cpu,
                        char lease_id_out[40]);
int mt_instance_release_lease(struct mtl_main_impl* impl, const char* lease_id);

int mt_instance_get_queue(struct mtl_main_impl* impl, const char* if_name,
                          uint16_t* out_queue_id, char out_lease[40]);
int mt_instance_attach_xsk(struct mtl_main_impl* impl, const char* queue_lease,
                           int xsk_fd, char out_lease[40]);
int mt_instance_add_flow(struct mtl_main_impl* impl, const char* queue_lease,
                         uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                         uint16_t dst_port, char out_lease[40], int* out_rule_loc);
int mt_instance_add_udp_filter(struct mtl_main_impl* impl, const char* queue_lease,
                               uint16_t dst_port, char out_lease[40]);

int mt_instance_ping(struct mtl_main_impl* impl, int64_t seq);
int mt_instance_heartbeat(struct mtl_main_impl* impl, const char* client_state);

#endif
