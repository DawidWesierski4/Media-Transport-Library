# MtlManager 2 Documentation

## Overview

MtlManager 2 is the root daemon that manages shared host resources for Media Transport Library (MTL) processes.
The daemon replaces the legacy binary protocol (`mtl_mproto.h`) with Varlink JSON IPC over Unix domain sockets.

MtlManager 2 controls these host assets:

- Physical Linux CPU leases with per-UID quotas and quarantine on live disconnect or revocation.
- Multi-NIC AF_XDP receive queues with queue 0 permanently protected.
- Reversed `SCM_RIGHTS` AF_XDP socket registration (`AttachXsk`) so tenants never receive writable BPF map descriptors.
- MTL-owned IPv4 UDP ethtool flow rules (`udp4` only) and queue-scoped XDP UDP port filters (`{rx_queue_index, dst_port}`).

## Build

Install the build dependencies on Linux:

```bash
sudo apt-get update
sudo apt-get install -y clang llvm libjson-c-dev libxdp-dev libbpf-dev
```

Build and install `MtlManager`, the `mtlm2d` symlink, and `mtl.xdp.o`:

```bash
meson setup build manager
meson compile -C build
sudo meson install -C build
```

Run the unit and daemon test suites under ASan and UBSan:

```bash
meson setup build_test manager -Denable_tests=true -Db_sanitize=address,undefined
meson compile -C build_test
meson test -C build_test --num-processes 1
```

## Run and Policy Modes

MtlManager 2 listens on two Unix domain sockets:

- Client socket: `/run/imtl/manager2.sock` for tenant Varlink RPCs (`OpenSession`, `ClaimSession`, `CloseSession`, `AcquireCpu`, `AcquireQueue`, `AttachXsk`, `AddFlow`, `AddUdpFilter`, `ReleaseLease`).
- Admin socket: `/run/imtl/admin.sock` (mode `0600`) for host-only admin RPCs (`GetStatus`, `RevokeLease`, `RevokeSession`, `KillQuarantinedOwner`).

### Zero-Argument Single-Host Mode

Run `sudo MtlManager` with no arguments for single-host development or CI tests:

```bash
sudo MtlManager
```

Zero-argument mode enables a single-host policy:

- Client socket `/run/imtl/manager2.sock` uses mode `0666`.
- All online CPUs, all network interfaces, and UDP ports `1..65535` are allowed.
- Admin RPCs remain restricted to `/run/imtl/admin.sock` (mode `0600`).

### Policy File and systemd Service

Pass `--policy-file` for multi-tenant hosts.
The systemd unit `mtlm2.service` always starts `MtlManager` with `--policy-file /etc/mtl/manager2-policy.json` and client socket mode `0660` (group `mtl`).
Policy files deny access by default and enforce `allowed_cpus`, `allowed_interfaces`, `allowed_udp_ports`, and aggregated per-UID quotas (`max_cpus`, `max_queues`, `max_flows`).

### Cgroup v2 `verify_only` Isolation Mode

Pass `--cgroup-mode=verify_only` (or set `"cgroup_verify": { "mode": "verify_only" }` in the policy file) when an external orchestrator assigns tenant cpusets, such as Docker (`--cpuset-cpus`), Kubernetes, or `systemd-run --scope -p AllowedCPUs=...`.

- MtlManager 2 operates strictly read-only against `--cgroup-mount` (default `/sys/fs/cgroup`) and never writes to cgroupfs.
- `mtlm2.service` sets `ProtectControlGroups=yes`.
- On `OpenSession`, MtlManager 2 reads the peer unified cgroup v2 path from `/proc/<pid>/cgroup`, checks the optional `expected_cgroup_pattern` glob, and rejects empty `cpuset.cpus.effective` or `cpuset.mems.effective` with `CgroupVerificationFailed`.
- On every `AcquireCpu` and `GetStatus`, MtlManager 2 re-reads `cpuset.cpus.effective` and `cpuset.mems.effective` live, rejects CPUs outside the effective set with `CpuOutsideEffectiveSet` or `NumaNodeNotAllowed`, and intersects automatic CPU selection with the live effective sets.
- `GetStatus` reports top-level `cgroupVerifyMode` and per-session `cgroupPath`, `effectiveCpus`, and `effectiveMems`.

## Non-Destructive Stop and Restart

MtlManager 2 stores active leases and kernel objects in `/run/imtl/state/journal.json` (mode `0600`).
Normal stop (`SIGTERM`, `SIGINT`, `SIGQUIT`, or `systemctl restart mtlm2`) preserves `journal.json`, active CPU leases, XDP programs, `xsks_map` entries, UDP filters, and ethtool flow rules for live tenants.

When MtlManager 2 starts again after a normal stop or crash:

1. The daemon loads `/run/imtl/state/journal.json`.
2. The daemon puts leases of live processes into `RECOVERING` state and monitors each owner `pidfd` with no timeout.
3. Live AF_XDP receive streams continue with zero packet loss and reclaim their leases via `ClaimSession` on the next RPC.
4. The daemon frees leases and deletes recorded MTL-owned flow rules for owners that already exited.
5. Foreign (non-MTL) ethtool flow rules remain untouched.

Pass `--teardown-on-exit` only in tests that require full teardown on normal exit.

## Legacy SysV Shared Memory Coexistence

MtlManager 2 compares the host CPU count (`sysconf(_SC_NPROCESSORS_CONF)`) with the legacy SysV shared memory slot count (`ftok("/dev/null", 21)`, lock `/tmp/kahawai_lcore.lock`):

- **Case A (`host_cpus <= shm_slot_count`)**: Coexistence is enabled by default.
  The daemon skips CPUs held by live legacy processes and writes each tenant PID into the legacy table so older clients do not steal live tenant CPUs during a daemon restart.
  Pass `--fail-on-legacy-shm` to refuse startup when live legacy clients exist.
- **Case B (`host_cpus > shm_slot_count`)**: Shared memory writes are disabled because MTL remaps EAL lcore IDs.
  If live legacy shared memory clients exist at startup, MtlManager 2 refuses to start and prints the conflicting PIDs.
  Stop the legacy clients or rebuild DPDK with `DPDK_MAX_LCORES` set to at least the host CPU count.

## Compatibility and Upgrade Notes

- Do not run legacy v1 `MtlManager` and MtlManager 2 on the same host.
  MtlManager 2 refuses to start if `/var/run/imtl/mtl_manager.sock` accepts connections.
  New `libmtl` refuses to fall back to SysV shared memory if only a v1 manager listens.
- `MTL_PMD_DPDK_AF_XDP` returns `-ENOTSUP` when MtlManager 2 is active.
  Migrate configurations (including `tests/tools/RxTxApp/script/dpdk_af_xdp_json/`) to `MTL_PMD_NATIVE_AF_XDP` or run without MtlManager 2.
- Legacy DPDK VF applications built against older `libmtl` releases stay safe on Case A hosts through SysV shared memory coexistence.
- Older AF_XDP applications built against v1 `libmtl` stop working once `/var/run/imtl/mtl_manager.sock` is removed.
  Rebuild all AF_XDP applications against this MTL release before you upgrade to MtlManager 2.
- Downgrading from MtlManager 2 to v1 while tenants run is not supported.
  Stop all MTL applications before you change major manager versions.

## Environment Variables for `libmtl`

- `MTL_MANAGER_SOCK`: Override the default client socket path `/run/imtl/manager2.sock`.
- `MTL_REQUIRE_MANAGER=1`: Fail `mtl_init()` if MtlManager 2 is absent or rejects the session.
- `MTL_ABORT_ON_MANAGER_LOSS=1`: Call `mtl_abort()` if `ClaimSession` fails after a daemon restart.
