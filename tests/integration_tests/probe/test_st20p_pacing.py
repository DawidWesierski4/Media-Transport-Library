# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Pytest integration tests that drive mtl_probe with zero gtest dependency."""

from __future__ import annotations

import pytest


@pytest.mark.feature("USER_PACING")
@pytest.mark.api("st20p_tx")
@pytest.mark.api("st20p_rx")
@pytest.mark.depth("integrity")
@pytest.mark.requires_topology("cross_pf_rx_ts")
@pytest.mark.requires_isolation
@pytest.mark.parametrize("profile", ["default", "ptp", "tsc_pacing"])
def test_st20p_user_pacing_hw_oracle(probe_runner, profile: str) -> None:
    res = probe_runner.run_checked(
        "st20p_user_pacing",
        profile=profile,
        topology="cross_pf_rx_ts",
    )
    assert res["frames_rx"] > 0, "Zero RX frames received"
    assert res["digest_errors"] == 0
    assert res["worst_late_ns"] <= 10_000


@pytest.mark.feature("ST20_OFFLOADS")
@pytest.mark.api("instance")
@pytest.mark.depth("stats")
@pytest.mark.profile("default")
@pytest.mark.requires_topology("vf_pair")
def test_queue_scale_lifecycle(probe_runner) -> None:
    probe_runner.run_checked("queue_scale", profile="default", topology="vf_pair")
