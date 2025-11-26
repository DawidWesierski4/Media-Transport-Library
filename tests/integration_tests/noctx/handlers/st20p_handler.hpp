/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "core/constants.hpp"
#include "core/handler_base.hpp"

class St20pHandler : public Handlers {
 public:
  uint64_t nsFrameTime;

  St20pHandler(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy,
               st20p_tx_ops ops_tx = {}, st20p_rx_ops ops_rx = {}, bool create = true,
               bool start = true);
  explicit St20pHandler(st_tests_context* ctx, st20p_tx_ops ops_tx = {},
                        st20p_rx_ops ops_rx = {});
  ~St20pHandler();

  struct st20p_tx_ops sessionsOpsTx;
  struct st20p_rx_ops sessionsOpsRx;
  st20p_tx_handle sessionsHandleTx = nullptr;
  st20p_rx_handle sessionsHandleRx = nullptr;

  void fillSt20Ops(uint transmissionPort = 20000, uint framebufferQueueSize = 3,
                   enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT, uint width = 1920,
                   uint height = 1080, uint payloadType = 112,
                   enum st_fps fps = ST_FPS_P25, bool interlaced = false,
                   enum st20_packing packing = ST20_PACKING_BPM);

  void setFrameTestStrategy(FrameTestStrategy* frameTestStrategy);

  void createSession(st20p_tx_ops ops_tx, st20p_rx_ops ops_rx, bool start = true);
  void createSession(bool start = true);
  void createSessionTx();
  void createSessionRx();

  void startSession(std::vector<std::function<void(std::atomic<bool>&)>> threadFunctions,
                    bool isRx);
  void startSession();
  void startSessionTx();
  void startSessionTx(std::function<void(std::atomic<bool>&)> threadFunction);
  void startSessionRx();
  void startSessionRx(std::function<void(std::atomic<bool>&)> threadFunction);
  void st20TxDefaultFunction(std::atomic<bool>& stopFlag);
  void st20RxDefaultFunction(std::atomic<bool>& stopFlag);

    /**
     * @brief Configure how often the TX underflow simulation fires.
     *
     * A non-zero value triggers an artificial delay every N-th frame pushed by
     * the TX loop. Defaults to 500 to keep the test lightweight.
     */
    uint16_t underflowEveryNFrames = 500;

    /**
     * @brief Duration of the simulated underflow in microseconds.
     *
     * During an underflow event the TX thread sleeps for this long to mimic a
     * stalled producer.
     */
    uint32_t underflowDurationUs = 200000;

    /**
     * @brief TX worker that injects periodic underflow delays for robustness tests.
     *
     * Grabs frames from the TX session, sleeps every
     * `underflowEveryNFrames` for `underflowDurationUs`, optionally mutates the
     * payload, and hands the frame back to the SDK until `stopFlag` becomes true.
     */
    void st20TxSimulateUnderFlowFunction(std::atomic<bool>& stopFlag);


  /**
   * @brief Set the session port names for TX and RX, including redundant ports if
   * specified.
   *
   * This function updates the port names in sessionsOpsTx and sessionsOpsRx based on the
   * provided indices. If an index is SESSION_SKIP_PORT, that port is not set. If both
   * primary and redundant ports are set, num_port is set to 2, otherwise to 1.
   *
   * @param txPortIdx Index for the primary TX port in ctx->para.port, or
   * SESSION_SKIP_PORT to skip.
   * @param rxPortIdx Index for the primary RX port in ctx->para.port, or
   * SESSION_SKIP_PORT to skip.
   * @param txPortRedundantIdx Index for the redundant TX port in ctx->para.port, or
   * SESSION_SKIP_PORT to skip.
   * @param rxPortRedundantIdx Index for the redundant RX port in ctx->para.port, or
   * SESSION_SKIP_PORT to skip.
   */
  void setSessionPorts(int txPortIdx = SESSION_SKIP_PORT,
                       int rxPortIdx = SESSION_SKIP_PORT,
                       int txPortRedundantIdx = SESSION_SKIP_PORT,
                       int rxPortRedundantIdx = SESSION_SKIP_PORT);
};
