# FFmpeg AVOption Acceptance Test Plan

## Scope

This plan covers only:

- PR [#1664](https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1664):
  initialize additional MTL ports and queue counts from FFmpeg;
- PR [#1665](https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1665):
  synchronize FFmpeg's built-in MTL PTP implementation to a grandmaster;
- ST20P `packing` and `interlaced` AVOptions.

RSS, ST30P, ST22, pacing-way, shaping, and RFC4175 input support are out of
scope. FFmpeg integrity tests must continue using planar
`YUV422PLANAR10LE`; packed `YUV422RFC4175PG2BE10` is not an FFmpeg rawvideo
pixel format.

## Test Principles

Each test must prove behavior, not merely successful process exit:

1. Multi-port tests prove every requested BDF was initialized, each session
   resolved to its requested MTL port, and packets traversed that physical
   port.
2. PTP tests prove both the capture-side `ptp4l` instance and the application
   under test synchronized to the real grandmaster.
3. Packing and interlace tests use the existing compliance and media oracles;
   no framework-specific duplicate test is added.
4. Command-generation tests cover every AVOption independently of hardware.

## 1. Multi-Port Session Tests

### Required Engine Changes

Add a framework-neutral per-session interface selector, for example
`nic_port`, inside each entry passed through `sessions=[...]`.

For RxTxApp, build one `interfaces` entry per unique BDF and one
`tx_sessions` or `rx_sessions` group per selected interface. Each group must
set `interface: [index]`; sessions using the same interface may remain in the
same group. The current engine appends every session into group zero, so it
cannot yet express per-session port selection even though RxTxApp JSON can.

For FFmpeg, add adapter-only device initialization parameters for `p2` through
`p7`, including source IP and RX/TX queue counts. The first MTL output context
must carry the complete initialization list:

```text
-p_port <BDF0> -p_sip <IP0>
-p2_port <BDF1> -p2_sip <IP1>
```

Each output context then selects its session port with the existing
`-p_port <BDF>` AVOption. Because all contexts in one FFmpeg process share the
MTL handle, the second session may select `<BDF1>` only after the first context
initialized it through `p2_port`.

The FFmpeg acceptance adapter therefore needs a true multi-output ST20P
command builder. Starting one FFmpeg process per stream does not test PR #1664
because each process creates an independent one-port MTL handle.

### Test A: Two Ports, Two Sessions

Parametrize the application as `rxtxapp` and `ffmpeg`. Allocate two TX PFs or
VFs and two RX interfaces. Use two planar input files or the same file with
different UDP ports and multicast groups.

| Session | Selected TX port | UDP port | Destination |
| --- | --- | --- | --- |
| 0 | initialized port 0 | 20000 | multicast group 0 |
| 1 | initialized port 1 | 20002 | multicast group 1 |

Run two corresponding RX sessions and require both outputs to pass media
integrity. Reverse the session-to-port assignment in a second parameter case;
this catches builders which always use interface zero.

### Test B: Additional-Port Queue Initialization

Run the FFmpeg case with deliberately distinct, valid queue counts, for
example port 0 TX/RX `4/4` and port 1 TX/RX `6/5`. Assert the MTL logs contain
the requested values for each numeric MTL port:

```text
mtl_init(0), socket_id ... port <BDF0>
mtl_init(1), socket_id ... port <BDF1>
mt_dev_init(0), user request queues tx 4 rx 4
mt_dev_init(1), user request queues tx 6 rx 5
```

Use counts supported by the allocated PF/VF and large enough for the tested
sessions. This test proves the queue AVOptions reached `mtl_init_params`; the
stream test separately proves they did not prevent operation.

### Port-Selection Oracle

Add an application-level assertion such as
`assert_session_ports(expected: list[int])`. It should parse a stable MTL
session-creation log that contains the physical port returned by
`mt_port_by_name()`. If the current ST20P pipeline logs do not expose that
value, add one notice-level attach log in the owning session code rather than
inferring selection from command text.

Also collect a traffic-side oracle:

- preferred: capture each TX PF separately and assert the expected UDP flow is
  present only on its selected PF;
- acceptable when separate capture is unavailable: snapshot per-PF packet
  counters before and after the run and require the selected port's TX counter
  to increase by the stream-sized minimum while the unselected port does not
  carry that flow.

Process success, rendered JSON, or presence of the BDF in initialization logs
alone is insufficient to prove session selection.

### Scaling Beyond Two Ports

After the two-port behavior is stable, add a nightly parameter for the largest
available host topology, up to `p7`. It only needs one session per initialized
port. Capability-gate by the number of allocatable interfaces; do not silently
reduce the requested count. The two-port case remains the required regression
test because it is practical on more hosts.

## 2. PTP Grandmaster Tests

### Current Harness Behavior

The function-scoped `ptp_sync` fixture already starts capture-side slave-only
PTP for tests marked `@pytest.mark.ptp`:

```text
ptp4l -i <capture PF> -s -m -2
```

It also suppresses capture-side `phc2sys`, preventing two daemons from driving
the same PHC. Keep this ownership model and continue reaping `ptp4l` by process
name in teardown.

The current `st20_interfaces_mix` test is not a sufficient regression test:
it is blanket `xfail`, includes a VF-only case, and checks media transfer
without asserting that capture `ptp4l` or built-in MTL PTP locked to the
grandmaster.

### Required Harness Changes

Make `ptp_sync` return a small session object containing the capture interface
and log path. Before yielding to traffic, wait for `ptp4l` to report a valid
grandmaster and a slave state. Fail with the last log lines if it remains
`LISTENING`, reports no foreign master, or times out. This distinguishes a
missing grandmaster from an MTL application failure.

The test topology must use real PFs for every PHC whose implementation is
under test. At minimum:

- application TX: PF connected to the PTP-aware fabric;
- application RX: VF is acceptable for media reception when only TX PTP is
  being tested;
- capture: a distinct kernel-bound PF, outside the application's IOMMU group,
  running `ptp4l` through the fixture.

If both TX and RX built-in PTP instances are to be validated in one case, use
distinct application PFs and ensure both are visible to the same grandmaster.

### Test C: RxTxApp Built-In PTP on PF

Replace the current blanket-xfail coverage with a PF-only RxTxApp case using
`enable_ptp=True`. Require:

1. capture `ptp4l` selected a grandmaster and entered slave state;
2. RxTxApp/MTL PTP statistics show received syncs and a bounded clock delta;
3. media transfer passes integrity;
4. optional ST 2110-21 compliance capture passes after the synchronization
   warm-up.

Do not treat a VF-only pass as evidence for PF PTP behavior.

### Test D: FFmpeg Built-In PTP on PF

Run the same topology and media case through FFmpeg with `-ptp_enable 1`.
Assert the PR #1665 callback log appears after a real delay response:

```text
PTP sync: master_utc_offset=<value> delta=<value>ns
```

Require multiple callback samples and a final bounded absolute delta, not just
one callback. Then require media integrity and the same optional compliance
oracle as the RxTxApp case.

Parameterize `ptp_pi` and `ptp_unicast` as separate nightly cases after the
base synchronization case is reliable. Each case must assert the startup log
confirms the selected flags and still satisfy the synchronization oracle.

### Grandmaster Capability Handling

Grandmaster availability is an explicit environment capability. Skip before
starting the application only when configuration says the test fabric has no
grandmaster or the fixture cannot discover one. Once a configured grandmaster
is expected, failure to reach slave/locked state is a test failure, not an
`xfail` or late skip.

## 3. Packing and Interlace Tests

Extend the existing functionality-first tests instead of adding FFmpeg-only
copies:

- add `ffmpeg` to `test_st20p_packing` after the adapter emits the new
  `packing` AVOption; use planar input assets for all FFmpeg cases and retain
  the existing compliance verdict for `BPM`, `GPM`, and `GPM_SL`;
- add `ffmpeg` to `test_st20p_interlace` after TX and RX both expose the
  boolean `interlaced` AVOption; require interlaced source assets, successful
  transfer, and compliance evidence distinguishing fields.

Add command-builder tests proving each value appears on the correct FFmpeg
device context and before `-i` for input-device options.

## 4. Validation Order

1. Unit-test RxTxApp JSON grouping and FFmpeg command generation, including
   reversed session-to-port assignments and all PR AVOptions.
2. Build each supported FFmpeg plugin version and inspect device help for the
   new options.
3. Run two-port/two-session RxTxApp acceptance first to validate the topology
   and port-selection oracle.
4. Run the equivalent single-process FFmpeg test, then queue initialization
   and larger nightly port counts.
5. Run capture-side `ptp4l` discovery alone, then RxTxApp PF PTP, then FFmpeg
   PF PTP, and finally PI/unicast variants.
6. Run existing ST20P packing and interlace matrices for both applications.

## Acceptance Criteria

- Two sessions can be assigned to two different initialized ports in both
  RxTxApp and one FFmpeg process.
- Initialization logs identify every requested BDF and queue count.
- A session-level and traffic-level oracle agree on the selected physical
  port.
- Capture-side `ptp4l` and application-side MTL PTP independently demonstrate
  synchronization to the real grandmaster on PF hardware.
- Packing and interlace behavior passes existing compliance/media checks.
- No RSS, ST30P, ST22, pacing, or RFC4175 input changes enter the patch.