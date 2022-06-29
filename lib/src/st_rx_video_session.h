/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#ifndef _ST_LIB_RX_VIDEO_SESSION_HEAD_H_
#define _ST_LIB_RX_VIDEO_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_VIDEO_BURTS_SIZE (128)

#define ST_RX_VIDEO_DMA_MIN_SIZE (1024)

#define ST_RV_EBU_TSC_SYNC_MS (100) /* sync tsc with ptp period(ms) */
#define ST_RV_EBU_TSC_SYNC_NS (ST_RV_EBU_TSC_SYNC_MS * 1000 * 1000)

int st_rx_video_sessions_sch_init(struct st_main_impl* impl, struct st_sch_impl* sch);

int st_rx_video_sessions_sch_uinit(struct st_main_impl* impl, struct st_sch_impl* sch);

struct st_rx_video_session_impl* st_rx_video_sessions_mgr_attach(
    struct st_rx_video_sessions_mgr* mgr, struct st20_rx_ops* ops,
    struct st22_rx_ops* st22_ops);
int st_rx_video_sessions_mgr_detach(struct st_rx_video_sessions_mgr* mgr,
                                    struct st_rx_video_session_impl* s);

void st_rx_video_sessions_stat(struct st_main_impl* impl);

int st_rx_video_session_put_frame(struct st_rx_video_session_impl* s, void* frame);

int st_rx_video_sessions_mgr_update_src(struct st_rx_video_sessions_mgr* mgr,
                                        struct st_rx_video_session_impl* s,
                                        struct st_rx_source_info* src);

int st_rx_video_sessions_mgr_update(struct st_rx_video_sessions_mgr* mgr);

int st_rx_video_session_start_pcapng(struct st_main_impl* impl,
                                     struct st_rx_video_session_impl* s,
                                     uint32_t max_dump_packets, bool sync,
                                     struct st_pcap_dump_meta* meta);

/* call rx_video_session_put always if get successfully */
static inline struct st_rx_video_session_impl* rx_video_session_get(
    struct st_rx_video_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_video_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_video_session_put always if get successfully */
static inline struct st_rx_video_session_impl* rx_video_session_try_get(
    struct st_rx_video_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_rx_video_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_video_session_put always if get successfully */
static inline bool rx_video_session_get_empty(struct st_rx_video_sessions_mgr* mgr,
                                              int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_video_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void rx_video_session_put(struct st_rx_video_sessions_mgr* mgr, int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

void rx_video_session_cal_cpu_busy(struct st_rx_video_session_impl* s);
void rx_video_session_clear_cpu_busy(struct st_rx_video_session_impl* s);

static inline bool rx_video_session_is_cpu_busy(struct st_rx_video_session_impl* s) {
  if (s->dma_dev && (s->dma_busy_score > 90)) return true;

  if (s->cpu_busy_score > 95.0) return true;

  return false;
}

static inline float rx_video_session_get_cpu_busy(struct st_rx_video_session_impl* s) {
  return s->cpu_busy_score;
}

int st_rx_video_session_migrate(struct st_main_impl* impl,
                                struct st_rx_video_sessions_mgr* mgr,
                                struct st_rx_video_session_impl* s, int idx);

#endif