/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Standalone C++ probe runner with zero gtest dependency.
 * Pass --dry-run to execute offline contract stubs so a stub binary never
 * reports a false pass on hardware CI.
 */

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "mtl_probe.hpp"

namespace {

std::string json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (unsigned char c : in) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\b') {
      out += "\\b";
    } else if (c == '\f') {
      out += "\\f";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::string to_json(const mtl_probe::ScenarioResult& r) {
  const char* status = "failed";
  if (r.code == mtl_probe::ExitCode::Passed)
    status = "passed";
  else if (r.code == mtl_probe::ExitCode::Unsupported)
    status = "skipped";
  else if (r.code == mtl_probe::ExitCode::HostFault)
    status = "host_fault";

  std::ostringstream oss;
  oss << "{"
      << "\"scenario\":\"" << json_escape(r.scenario) << "\","
      << "\"profile\":\"" << json_escape(r.profile) << "\","
      << "\"result\":\"" << status << "\","
      << "\"exit_code\":" << static_cast<int>(r.code) << ","
      << "\"message\":\"" << json_escape(r.message) << "\","
      << "\"frames_tx\":" << r.frames_tx << ","
      << "\"frames_rx\":" << r.frames_rx << ","
      << "\"digest_errors\":" << r.digest_errors << ","
      << "\"worst_early_ns\":" << r.worst_early_ns << ","
      << "\"worst_late_ns\":" << r.worst_late_ns << ","
      << "\"duration_ms\":" << r.duration_ms << "}";
  return oss.str();
}

mtl_probe::ScenarioResult run_st20p_user_pacing_stub(
    const mtl_probe::ScenarioSpec& spec) {
  mtl_probe::ScenarioResult r;
  r.scenario = spec.scenario;
  r.profile = spec.profile;
  r.frames_tx = 600;
  r.frames_rx = 600;
  r.digest_errors = 0;
  r.worst_early_ns = 0;
  r.worst_late_ns = 4200;
  r.duration_ms = 1240.0;
  r.code = mtl_probe::ExitCode::Passed;
  r.message = "Dry-run stub: all 600 frames within 0/+10 us HW RX timestamp oracle";
  return r;
}

mtl_probe::ScenarioResult run_queue_scale_stub(const mtl_probe::ScenarioSpec& spec) {
  mtl_probe::ScenarioResult r;
  r.scenario = spec.scenario;
  r.profile = spec.profile;
  r.frames_tx = 0;
  r.frames_rx = 0;
  r.duration_ms = 2100.0;
  r.code = mtl_probe::ExitCode::Passed;
  r.message = "Dry-run stub: initialized and tore down requested TX/RX queues";
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  const std::map<std::string, mtl_probe::ScenarioFn> registry = {
      {"queue_scale", run_queue_scale_stub},
      {"st20p_default_timestamps", run_st20p_user_pacing_stub},
      {"st20p_user_pacing", run_st20p_user_pacing_stub},
  };

  std::string scenario;
  std::string profile = "default";
  std::string result_file;
  bool dry_run = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--scenario" && i + 1 < argc) {
      scenario = argv[++i];
    } else if (arg == "--profile" && i + 1 < argc) {
      profile = argv[++i];
    } else if (arg == "--result" && i + 1 < argc) {
      result_file = argv[++i];
    } else if (arg == "--dry-run") {
      dry_run = true;
    } else if (arg == "--list") {
      std::cout << "[";
      bool first = true;
      for (const auto& entry : registry) {
        if (!first) std::cout << ",";
        std::cout << "\"" << json_escape(entry.first) << "\"";
        first = false;
      }
      std::cout << "]\n";
      return 0;
    } else {
      std::cerr << "mtl_probe: unknown or incomplete option: " << arg << "\n";
      return static_cast<int>(mtl_probe::ExitCode::Failed);
    }
  }

  mtl_probe::ScenarioSpec spec;
  spec.scenario = scenario;
  spec.profile = profile;

  mtl_probe::ScenarioResult res;
  auto it = registry.find(scenario);
  if (it == registry.end()) {
    res.scenario = scenario;
    res.profile = profile;
    res.code = mtl_probe::ExitCode::Failed;
    res.message = "Unknown scenario: " + scenario;
  } else if (!dry_run) {
    res.scenario = scenario;
    res.profile = profile;
    res.code = mtl_probe::ExitCode::Unsupported;
    res.message =
        "Built without libmtl hardware backend. Pass --dry-run for contract tests.";
  } else {
    res = it->second(spec);
  }

  std::string payload = to_json(res);
  if (!result_file.empty()) {
    std::ofstream ofs(result_file);
    ofs << payload << "\n";
    ofs.flush();
    if (!ofs) {
      std::cerr << "mtl_probe: cannot write result file: " << result_file << "\n";
      return static_cast<int>(mtl_probe::ExitCode::Failed);
    }
  } else {
    std::cout << payload << "\n";
  }
  return static_cast<int>(res.code);
}
