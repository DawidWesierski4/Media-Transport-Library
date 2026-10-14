/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

class MtDevHdrSplitTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevHdrSplitTest, GetsAPayloadPoolQueueOnAPortThatCanSplit) {
  EXPECT_TRUE(ut_dev_get_hdr_split_rx_queue(ctx_, true, true));
}

TEST_F(MtDevHdrSplitTest, GetFailsOnAPortWithoutTheFeature) {
  EXPECT_FALSE(ut_dev_get_hdr_split_rx_queue(ctx_, false, false));
}

TEST_F(MtDevHdrSplitTest, GetFailsWithoutTheFeatureEvenIfAQueueHasAPayloadPool) {
  EXPECT_FALSE(ut_dev_get_hdr_split_rx_queue(ctx_, false, true));
}
