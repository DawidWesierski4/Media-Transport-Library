/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "sch/mt_sch_harness.h"

/* A process of a DPDK with a lower RTE_MAX_LCORE that died without uinit leaves its
 * smaller table, and shmget() of the larger one fails with EINVAL. */
TEST(MtSchLcoreShm, ReplacesUnusedSmallerTable) {
  size_t size = 0;
  ASSERT_EQ(ut_sch_lcore_shm_init_over_smaller(false, true, &size), 0);
  EXPECT_EQ(size, ut_sch_lcore_shm_size());
}

/* An attached smaller table belongs to a running process of the other build. */
TEST(MtSchLcoreShm, KeepsUsedSmallerTable) {
  size_t size = 0;
  EXPECT_LT(ut_sch_lcore_shm_init_over_smaller(true, true, &size), 0);
  EXPECT_EQ(size, ut_sch_lcore_shm_size() / 2);
}

/* LcoreMgr takes no lock file, so it leaves the table to mtl_init(). */
TEST(MtSchLcoreShm, LcoreMgrKeepsUnusedSmallerTable) {
  size_t size = 0;
  EXPECT_LT(ut_sch_lcore_shm_init_over_smaller(false, false, &size), 0);
  EXPECT_EQ(size, ut_sch_lcore_shm_size() / 2);
}

/* LcoreMgr --clean_pid_auto_check logs "delete dead lcore" for an entry of a dead PID,
 * but lcore_shm_clean_auto_pid() only counts it: the entry stays active and used keeps
 * it. A live process keeps the table attached, so the table is not removed either, and
 * the lcore stays taken for each other user until a process of the same user and host
 * asks for that lcore. Found on PR #1909 review, older than the PR. */
TEST(MtSchLcoreShm, AutoCleanFreesTheEntryOfADeadPid) {
  int cleaned = 0, used = -1;
  bool still_active = true;

  ASSERT_EQ(ut_sch_lcore_shm_auto_clean_dead(&cleaned, &still_active, &used), 0);

  EXPECT_EQ(cleaned, 1);
  EXPECT_FALSE(still_active) << "the tool logged the delete, the entry is still active";
  EXPECT_EQ(used, 0);
}
