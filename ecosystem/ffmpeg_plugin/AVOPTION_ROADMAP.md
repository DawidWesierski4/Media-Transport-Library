# FFmpeg Plugin AVOption Roadmap

## Scope

This is an inventory of MTL inputs that are relevant to the existing FFmpeg
`mtl_st20p`, `mtl_st22`, `mtl_st22p`, and `mtl_st30p` devices but are not
currently exposed as AVOptions. It deliberately excludes APIs which need a new
FFmpeg device or an application callback, such as ST40/ST41, external frames,
RTCP callbacks, user timestamps, custom PTP clocks, and MTL statistics
callbacks.

The current plugin exposes device BDF/IP pairs for `p` and `r`, queue counts,
DMA devices, and session destination IP/UDP port/payload type. It hard-codes
the MTL video migration flags, RX separate-lcore flag, NUMA binding, ST20P
block-get/DMA flags, ST20P transport format, and ST22P packetization.

## ST20P Transport-Format Boundary

`transport_fmt` is intentionally selected from FFmpeg's `-pix_fmt` in
`mtl_st20p_{tx,rx}.c`; it is not independently configurable. For example,
`-pix_fmt yuv422p10le` selects planar `ST_FRAME_FMT_YUV422PLANAR10LE` and MTL
transport `ST20_FMT_YUV_422_10BIT`.

That does not support an RFC4175 file as input. The RFC integrity assets use
`YUV422RFC4175PG2BE10`: packed wire-format bytes at 2.5 bytes/pixel. FFmpeg
has no corresponding rawvideo `AVPixelFormat`, whereas `yuv422p10le` is
planar and occupies 4 bytes/pixel. Giving the RFC file `-pix_fmt yuv422p10le`
would therefore corrupt the byte stream before it reaches MTL, despite both
formats using the same `ST20_FMT_YUV_422_10BIT` transport enum.

Consequently, the FFmpeg cases in
`tests/acceptance/tests/single/st20p/test_integrity.py` must remain limited to
`yuv_files_422p10le`; the explicit RFC skip is correct. Do not add a
`transport_format` AVOption to bypass this restriction: it would make the
declared FFmpeg input layout disagree with the input bytes. Supporting the RFC
assets requires an FFmpeg rawvideo pixel format or a conversion path, not an
MTL AVOption.

## Required Upstream Work

The following AVOptions are required because they are the subject of the two
referenced pull requests. They are low-risk device-init mappings shared by all
of the plugin's existing devices.

| AVOption(s) | MTL target | Direction | Source | Notes |
| --- | --- | --- | --- | --- |
| `p2_port`/`p2_sip` through `p7_port`/`p7_sip` | `mtl_init_params.port[]`, `sip_addr[]` | TX | [PR #1664](https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1664) | Exposes the remaining six MTL device ports. Include `p2_*` through `p7_*` exactly as proposed. |
| `p2_tx_queues`/`p2_rx_queues` through `p7_tx_queues`/`p7_rx_queues` | `mtl_init_params.tx_queues_cnt[]`, `rx_queues_cnt[]` | TX | [PR #1664](https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1664) follow-up patch | Necessary whenever the additional ports need non-default queue sizing. |
| `ptp_enable` | `MTL_FLAG_PTP_ENABLE`, `pacing = ST21_TX_PACING_WAY_PTP` | TX | [PR #1665](https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1665) | Also installs the plugin's PTP-sync logging callback. Requires a PF. |
| `ptp_pi` | `MTL_FLAG_PTP_PI` | TX | [PR #1665](https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1665) | Meaningful only with `ptp_enable`; PF-only. |
| `ptp_unicast` | `MTL_FLAG_PTP_UNICAST_ADDR` | TX | [PR #1665](https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1665) | Meaningful only with `ptp_enable`. |

`p2` through `p7` are device ports, not session members: the existing
`st_tx_port`/`st_rx_port` supports only primary and redundant members. The
extra BDFs allow one FFmpeg process to initialize more NICs for its separate
device instances; they do not turn a stream into seven-way redundancy.

## Recommended Next Batch

These options have a direct field or flag mapping, are useful in the existing
acceptance suite, and do not require a change to FFmpeg's media model.

| Priority | AVOption(s) | MTL target | Devices | Acceptance value | Implementation effort |
| --- | --- | --- | --- | --- | --- |
| P1 | `packing` (`bpm`, `gpm`, `gpm_sl`) | `st20p_tx_ops.transport_packing` | ST20P TX | Enables `tests/acceptance/tests/single/st20p/test_packing.py`; the plugin otherwise has no way to choose the packetizer. | One ST20P context field, AVOption, and enum parser. |
| P1 | `interlaced` | `st20p_tx_ops.interlaced`, `st20p_rx_ops.interlaced` | ST20P TX and RX | Enables the ST20P interlace acceptance case. FFmpeg carries field order, but the plugin currently never forwards it to MTL. | Boolean AVOption in each affected context; retain FFmpeg-derived default of false. |

Both options are session-specific and should land with focused ST20P tests.

## Other Directly Exposable Inputs

These fields are available in MTL and could be mapped without redesign, but
they are not selected for the first batch because they have less acceptance
coverage, are tuning/debug controls, or are less safe to advertise broadly.

| Area | Candidate AVOption(s) | MTL target | Reason to defer |
| --- | --- | --- | --- |
| Network setup | `netmask`, `gateway`, `dhcp` | `netmask[]`, `gateway[]`, `net_proto[]` | Useful for non-default networks, but the plugin's static DPDK deployment is the established path and per-port option naming needs a complete design. |
| Host scheduling | `lcores`, `main_lcore`, `allow_across_numa_core`, `tasklet_thread`, `tasklet_sleep`, `dedicated_sys_lcore` | `lcores`, `main_lcore`, corresponding `MTL_FLAG_*` values | Operational tuning with a large host-specific failure surface. |
| Queue/memory tuning | `nb_tx_desc`, `nb_rx_desc`, `rx_pool_data_size`, `memzone_max`, `tasklets_nb_per_sch`, `rss_sch_nb` | Matching `mtl_init_params` fields | Expert capacity tuning; defaults are preferred and no FFmpeg acceptance gap currently requires them. |
| RX behavior | `promiscuous`, `no_multicast`, `rx_udp_port_only`, `rx_burst_size`, `rx_hdr_split`, `rx_auto_detect`, `rx_receive_incomplete`, `rx_disable_migrate`, `rx_multi_threads` | `MTL_FLAG_*`, `st20p_rx_ops.flags`, `rx_burst_size` | Some need new output-format negotiation or incomplete-frame reporting in FFmpeg, so exposing only the flag would create unclear behavior. |
| TX behavior | `drop_when_late`, `start_vrx`, `pad_interval`, `rtp_timestamp_delta_us`, `static_pad`, `tx_no_chain`, `multi_src_port`, `random_src_port` | ST20P flags/fields and `MTL_FLAG_*` values | Useful diagnostics/performance controls, but not required to close a current FFmpeg test gap. `drop_when_late` is a good later candidate once FFmpeg reports dropped frames. |
| PTP tuning | `ptp_kp`, `ptp_ki`, `ptp_source_tsc`, `phc2sys` | `kp`, `ki`, `MTL_FLAG_PTP_SOURCE_TSC`, `MTL_FLAG_PHC2SYS_ENABLE` | Keep the first PTP surface aligned with PR #1665. These require documented operational semantics and privilege considerations. |

## Not A Plugin AVOption Gap

The following acceptance cases remain RxTxApp-only for reasons outside the
current FFmpeg plugin parameter surface:

| Acceptance area | Why it is not a small AVOption addition |
| --- | --- |
| ST20P RFC media-format conversion and RFC integrity assets | `YUV422RFC4175PG2BE10` is packed RFC4175 input, not an FFmpeg rawvideo pixel format. The plugin must keep FFmpeg integrity cases on the compatible planar format; see the transport-format boundary above. |
| ST20P resolution sweep | FFmpeg already derives width and height from its media stream; no missing MTL control is demonstrated. |
| ST 2022-7 redundancy | The plugin already has primary/redundant device and session address options; the FFmpeg acceptance adapter needs redundant command construction, not a new plugin AVOption. |
| ST40 ancillary and ST41 fast metadata | No FFmpeg device implementation exists for those MTL APIs. |
| User pacing/timestamps, RTCP, external frames, GPU frame ownership | These depend on callbacks or per-frame metadata that the current FFmpeg device path does not produce. |

## Suggested Landing Order

1. Apply PR #1664 and PR #1665, including their README documentation.
2. Keep FFmpeg ST20P integrity coverage to the compatible planar
   `YUV422PLANAR10LE` assets; retain the RFC4175 skip.
3. Add ST20P `packing` and `interlaced`, each with focused acceptance
   coverage.

This order exposes operationally important controls while preserving the
plugin's small, media-oriented interface instead of mirroring every MTL tuning
or callback API.