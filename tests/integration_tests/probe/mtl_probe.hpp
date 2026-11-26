/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mtl_probe: Plain C++ hardware integration probe with zero gtest and zero
 * global context.
 *
 * Pytest drives this binary. It passes scenario and profile CLI arguments,
 * runs one clean mtl_init()/mtl_uninit() lifecycle, and reads one JSON result
 * object.
 *
 * Exit codes:
 *   0 = passed
 *   1 = assertion failed (MTL defect)
 *   2 = unsupported topology or hardware prerequisite missing
 *   3 = host fault (driver wedged, VF missing, hugepage error)
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mtl_probe {

enum class ExitCode : int {
  Passed = 0,
  Failed = 1,
  Unsupported = 2,
  HostFault = 3,
};

struct ScenarioSpec {
  std::string scenario;
  std::string profile = "default";
};

struct ScenarioResult {
  std::string scenario;
  std::string profile;
  ExitCode code = ExitCode::Failed;
  std::string message;
  uint64_t frames_tx = 0;
  uint64_t frames_rx = 0;
  uint64_t digest_errors = 0;
  int64_t worst_early_ns = 0;
  int64_t worst_late_ns = 0;
  double duration_ms = 0.0;
};

using ScenarioFn = std::function<ScenarioResult(const ScenarioSpec&)>;

}  // namespace mtl_probe
