/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

TEST_F(NoCtxTest, st20p_default_timestamps) {
  initSt20pDefaultContext();

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pDefaultTimestamp(handler); });
  auto* frameTestStrategy = static_cast<St20pDefaultTimestamp*>(bundle.strategy);

  bundle.handler->startSession();
  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  sleepUntilFailure();
  bundle.handler->stopSession();

  ASSERT_GT(frameTestStrategy->idx_rx, 0u)
      << "st20p_user_pacing did not receive any frames";
}

TEST_F(NoCtxTest, st20p_user_pacing) {
  initSt20pDefaultContext();

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
      });

  auto* frameTestStrategy = static_cast<St20pUserTimestamp*>(bundle.strategy);

  bundle.handler->startSession();

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  ASSERT_EQ(frameTestStrategy->getPacingParameters(), 0);
  EXPECT_GT(frameTestStrategy->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(frameTestStrategy->pacing_trs_ns, 0.0);
  EXPECT_GT(frameTestStrategy->pacing_vrx_pkts, 0u);

  sleepUntilFailure();

  bundle.handler->stopSession();

  ASSERT_GT(frameTestStrategy->idx_tx, 0u)
      << "st20p_user_pacing did not transmit any frames";
  ASSERT_GT(frameTestStrategy->idx_rx, 0u)
      << "st20p_user_pacing did not receive any frames";
  ASSERT_EQ(frameTestStrategy->idx_tx, frameTestStrategy->idx_rx)
      << "TX/RX frame count mismatch";
}

TEST_F(NoCtxTest, st20p_user_pacing_offset_jitter) {
  initSt20pDefaultContext();

  std::vector<double> jitterMultipliers = {-0.00005, 0.0,      0.0000875, -0.00003,
                                           0.00012,  -0.00001, 0.0,       0.00006};
  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [jitterMultipliers](St20pHandler* handler) {
        return new St20pUserTimestamp(handler, jitterMultipliers);
      },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
      });
  auto* strategy = static_cast<St20pUserTimestamp*>(bundle.strategy);

  bundle.handler->startSession();

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  ASSERT_EQ(strategy->getPacingParameters(), 0);
  EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(strategy->pacing_trs_ns, 0.0);
  EXPECT_GT(strategy->pacing_vrx_pkts, 0u);

  sleepUntilFailure();

  bundle.handler->stopSession();

  ASSERT_GE(strategy->idx_tx, jitterMultipliers.size()) << "TX frames below expectation";
  ASSERT_GE(strategy->idx_rx, jitterMultipliers.size()) << "RX frames below expectation";
  ASSERT_EQ(strategy->idx_tx, strategy->idx_rx) << "TX/RX frame count mismatch";
}

TEST_F(NoCtxTest, st20p_user_pacing_drop_old) {
  initSt20pDefaultContext();

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_BLOCK_GET;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
        /* TODO: Enable */
        /* handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_DROP_WHEN_LATE; */
      });

  bundle.handler->startSessionRx();
  bundle.handler->startSessionTx(
      {[this, &bundle](std::atomic<bool>& stopFlag) {
        bundle.handler->st20TxSimulateUnderFlowFunction(stopFlag);
      }});

  /*
  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);
  sleepUntilFailure(130);
  mtl_stop(ctx->handle);

  st20_rx_user_stats stats;
  st20p_rx_get_session_stats(bundle.handler->sessionsHandleRx, &stats);
  st20_tx_user_stats statsTx;
  st20p_tx_get_session_stats(bundle.handler->sessionsHandleTx, &statsTx);
  uint64_t packetsSend = statsTx.common.port[0].packets;
  uint64_t packetsRecieved = stats.common.port[0].packets + stats.common.port[1].packets;

  ASSERT_NEAR(packetsSend, packetsRecieved, packetsSend / 100)
      << "Packet comparison against primary stream";
  ASSERT_LE(stats.common.stat_pkts_out_of_order, packetsRecieved / 1000)
      << "Out of order packets";
  ASSERT_NEAR(bundle.strategy->idx_tx, bundle.strategy->idx_rx, bundle.strategy->idx_tx / 100)
      << "Frame comparison against primary stream";
  */
  bundle.handler->stopSession();
}
