# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Unified pytest integration runner plugin for mtl_probe.

Emits one structured JSONL line per (case, profile, nic, host) record.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

_HOST_FAULT_STASH_KEY: pytest.StashKey[str | None] = pytest.StashKey()


@dataclass
class ProbeRunner:
    binary: Path
    nic: str
    host: str
    config: pytest.Config
    strict_hw: bool = False
    dry_run: bool = False

    def run(
        self,
        scenario: str,
        profile: str = "default",
        topology: str = "vf_pair",
        extra_args: list[str] | None = None,
        timeout_s: int = 120,
    ) -> dict[str, Any]:
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
            res_file = Path(tf.name)
        try:
            cmd = [
                str(self.binary),
                "--scenario",
                scenario,
                "--profile",
                profile,
                "--result",
                str(res_file),
            ]
            if self.dry_run:
                cmd.append("--dry-run")
            if extra_args:
                cmd.extend(extra_args)
            try:
                proc = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=timeout_s,
                    check=False,
                )
            except subprocess.TimeoutExpired:
                data = {
                    "scenario": scenario,
                    "profile": profile,
                    "result": "failed",
                    "exit_code": -1,
                    "message": f"probe timed out after {timeout_s} s",
                }
            except OSError as exc:
                data = {
                    "scenario": scenario,
                    "profile": profile,
                    "result": "failed",
                    "exit_code": -1,
                    "message": f"cannot execute probe binary: {exc}",
                }
            else:
                if res_file.exists() and res_file.stat().st_size:
                    try:
                        data = json.loads(res_file.read_text(encoding="utf-8"))
                    except json.JSONDecodeError as exc:
                        data = {
                            "scenario": scenario,
                            "profile": profile,
                            "result": "failed",
                            "exit_code": proc.returncode,
                            "message": f"corrupt probe JSON: {exc}",
                        }
                    else:
                        if proc.returncode != data.get("exit_code") or (
                            data.get("result") == "passed" and proc.returncode != 0
                        ):
                            stderr = proc.stderr.strip()
                            detail = f"process exited {proc.returncode}"
                            if stderr:
                                detail = f"{detail}: {stderr}"
                            prev = str(data.get("message", "")).strip()
                            data["result"] = "failed"
                            data["exit_code"] = proc.returncode
                            data["message"] = f"{prev} ({detail})" if prev else detail
                else:
                    data = {
                        "scenario": scenario,
                        "profile": profile,
                        "result": "failed",
                        "exit_code": proc.returncode,
                        "message": proc.stderr.strip()
                        or f"probe exited {proc.returncode}",
                    }
            data["nic"] = self.nic
            data["host"] = self.host
            data["topology"] = topology
            return data
        finally:
            res_file.unlink(missing_ok=True)

    def run_checked(
        self,
        scenario: str,
        profile: str = "default",
        topology: str = "vf_pair",
        extra_args: list[str] | None = None,
        timeout_s: int = 120,
    ) -> dict[str, Any]:
        """Run one scenario and enforce skip, strict-hw, and host-fault rules."""
        res = self.run(
            scenario,
            profile=profile,
            topology=topology,
            extra_args=extra_args,
            timeout_s=timeout_s,
        )
        if res["result"] == "skipped":
            if self.strict_hw:
                pytest.fail(f"Strict HW topology required: {res['message']}")
            pytest.skip(res["message"])
        if res["result"] == "host_fault":
            self.config.stash[_HOST_FAULT_STASH_KEY] = str(res["message"])
            pytest.fail(f"Host fault detected: {res['message']}")
        assert res["result"] == "passed", res["message"]
        return res


def pytest_addoption(parser: pytest.Parser) -> None:
    grp = parser.getgroup("mtl_probe")
    grp.addoption(
        "--probe-bin",
        action="store",
        default=os.environ.get("MTL_PROBE_BIN", ""),
        help="Path to mtl_probe binary",
    )
    grp.addoption(
        "--nic-label",
        action="store",
        default=os.environ.get("MTL_NIC_LABEL", "E810"),
        help="CI NIC label",
    )
    grp.addoption(
        "--jsonl-out",
        action="store",
        default="",
        help="Write structured JSONL results to this path",
    )
    grp.addoption(
        "--strict-hw",
        action="store_true",
        default=False,
        help="Turn missing hardware or topology skips into failures",
    )
    grp.addoption(
        "--dry-run",
        action="store_true",
        default=False,
        help="Pass --dry-run to stub mtl_probe binary",
    )


def pytest_configure(config: pytest.Config) -> None:
    config.stash[_HOST_FAULT_STASH_KEY] = None
    for mark in (
        "feature(name): Test plan feature ID covered by this test",
        "api(name): MTL API or direction covered by this test",
        "depth(level): Verification depth (run, stats, integrity, compliance)",
        "profile(name): mtl_init() profile name",
        "requires_topology(name): Required port topology "
        "(vf_pair, cross_pf_rx_ts, pf_pair, redundant_4p)",
        "requires_isolation: Requires exclusive CPU partition",
    ):
        config.addinivalue_line("markers", mark)


def pytest_sessionstart(session: pytest.Session) -> None:
    jsonl_opt = session.config.getoption("--jsonl-out")
    if jsonl_opt:
        path = Path(jsonl_opt)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("", encoding="utf-8")


@pytest.fixture(scope="session")
def probe_runner(request: pytest.FixtureRequest) -> ProbeRunner:
    strict_hw = bool(request.config.getoption("--strict-hw"))
    bin_opt = request.config.getoption("--probe-bin")
    if not bin_opt:
        msg = "Set --probe-bin or MTL_PROBE_BIN to run mtl_probe tests"
        if strict_hw:
            pytest.fail(msg)
        pytest.skip(msg)
    bin_path = Path(bin_opt)
    if not bin_path.is_file() or not os.access(bin_path, os.X_OK):
        msg = f"Probe binary is missing or not executable: {bin_path}"
        if strict_hw:
            pytest.fail(msg)
        pytest.skip(msg)
    return ProbeRunner(
        binary=bin_path,
        nic=request.config.getoption("--nic-label"),
        host=socket.gethostname(),
        config=request.config,
        strict_hw=strict_hw,
        dry_run=bool(request.config.getoption("--dry-run")),
    )


def _item_profile(item: pytest.Item) -> str:
    callspec = getattr(item, "callspec", None)
    if callspec and "profile" in callspec.params:
        return str(callspec.params["profile"])
    profiles = [m.args[0] for m in item.iter_markers("profile") if m.args]
    return str(profiles[0]) if profiles else "default"


def _write_jsonl_record(item: pytest.Item, result: str, duration_s: float) -> None:
    jsonl_opt = item.config.getoption("--jsonl-out")
    if not jsonl_opt:
        return
    features = [m.args[0] for m in item.iter_markers("feature") if m.args]
    apis = [m.args[0] for m in item.iter_markers("api") if m.args]
    depths = [m.args[0] for m in item.iter_markers("depth") if m.args]
    record = {
        "tier": "integration",
        "case": item.nodeid,
        "profile": _item_profile(item),
        "nic": item.config.getoption("--nic-label"),
        "host": socket.gethostname(),
        "result": result,
        "duration_ms": round(duration_s * 1000.0, 2),
        "features": features,
        "apis": apis,
        "depth": depths[0] if depths else "stats",
    }
    path = Path(jsonl_opt)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(record) + "\n")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item: pytest.Item, call: pytest.CallInfo):
    outcome = yield
    rep = outcome.get_result()
    # Record call outcomes and setup skips/failures so no test silently vanishes.
    if rep.when == "setup" and rep.outcome == "passed":
        return
    if rep.when == "teardown":
        return
    host_fault = item.config.stash.get(_HOST_FAULT_STASH_KEY, None)
    result = "host_fault" if host_fault is not None else rep.outcome
    _write_jsonl_record(item, result, rep.duration)


def pytest_runtest_teardown(item: pytest.Item, nextitem: pytest.Item | None) -> None:
    host_fault = item.config.stash.get(_HOST_FAULT_STASH_KEY, None)
    if host_fault is not None:
        item.config.stash[_HOST_FAULT_STASH_KEY] = None
        pytest.exit(f"Host fault detected: {host_fault}", returncode=3)
