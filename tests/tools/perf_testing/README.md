# perf_testing

`perf_testing` measures the scheduler cost of the st20p RX payload paths. It starts N
identical st20p RX sessions on one port, and with `--tx` also N TX sessions on a second
port, so one host can send traffic to itself. At each interval it reads
`mtl_sch_get_stats()` for every scheduler slot and `st20p_rx_get_session_stats()` /
`st20p_tx_get_session_stats()` for every session, and prints one record set. Run it once
per RX path (CPU copy, `--dma`, `--hdr_split`) and compare the `busy_pct` and `avg_ns` of
the RX video tasklet.

## What it measures

The library measures each tasklet handler call and each pass over all tasklets of a
scheduler. The counters move only while the measurement is enabled:

- `--time_measure` sets `MTL_FLAG_TASKLET_TIME_MEASURE` at `mtl_init()`.
- Without the flag, attach the USDT probe `sys:tasklet_time_measure` from
  `lib/src/mt_usdt_provider.d` to the running process. The counters then move while the
  probe stays attached:

  ```bash
  sudo bpftrace -e 'usdt:/path/to/libmtl.so:sys:tasklet_time_measure { }'
  ```

  Use the `libmtl.so` the tool loaded
  (`ldd tests/tools/perf_testing/build/perf_testing | grep mtl`). Without the
  flag and without the probe the tool still runs and reports `time_measure` as `0`
  (`false` in JSON) with all time counters at zero.

The API counters are cumulative. The tool keeps the previous sample per scheduler and
tasklet and prints the difference per interval. `mtl_sch_get_stats()` reports at most
`MTL_SCH_MAX_TASKLETS` (64) tasklets per scheduler; `nb_tasklets_total` tells when a
scheduler has more. `min_ns` and `max_ns` have no per-interval
form in the API, so they are the values since `mtl_init()` or the last reset. Pass
`--reset_each_interval` to call `mtl_sch_reset_stats()` after each sample; then `min_ns`
and `max_ns` are per interval too.

## Build

The tool is its own meson project and is not part of `build.sh`. Build the library first,
then point `pkg-config` at its `mtl.pc`:

```bash
MTL_INSTALL_PREFIX=$PWD/.local_install ./build.sh
export PKG_CONFIG_PATH=$(dirname "$(find .local_install -name mtl.pc)")
meson setup tests/tools/perf_testing/build tests/tools/perf_testing
ninja -C tests/tools/perf_testing/build
```

With a system install of the library (`./build.sh` as non-root installs to
`/usr/local`) the `PKG_CONFIG_PATH` line is not needed.

## Run

All runs need the hugepages and the VFs from `doc/run.md`. The TX port and the RX port
must be two different VFs on the same link, or two ports with a cable between them.

`--rx_ip` selects the addressing. A multicast group (`224.0.0.0/4`, the default) is the
destination of the TX sessions and the group the RX sessions join. A unicast address is
the sender IP the RX sessions filter on; with `--tx` the tool ignores it for the wiring and
runs each stream from `--t_sip` to `--p_sip`.

CPU copy, 4 sessions, loopback TX, 60 s:

```bash
sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:01.0 --p_sip 192.168.85.80 \
  --tx --t_port 0000:af:01.1 --t_sip 192.168.85.81 \
  --sessions 4 --time_measure --duration 60 --interval 5
```

DMA offload, same traffic, two DSA devices:

```bash
sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:01.0 --p_sip 192.168.85.80 \
  --tx --t_port 0000:af:01.1 --t_sip 192.168.85.81 \
  --sessions 4 --time_measure --duration 60 \
  --dma 0000:80:04.0 --dma 0000:80:04.1
```

Header split, JSON output:

```bash
sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:01.0 --p_sip 192.168.85.80 \
  --tx --t_port 0000:af:01.1 --t_sip 192.168.85.81 \
  --sessions 4 --time_measure --duration 60 --hdr_split --json
```

`--hdr_split` sets `ST20P_RX_FLAG_HDR_SPLIT` on each session and
`nb_rx_hdr_split_queues` to the session count. The build, the NIC and the driver must
support header split; when they do not, `st20p_rx_create()` fails and the tool exits with
status 1.

`--duration 0` runs until SIGINT. The tool stops the sessions, frees them, and calls
`mtl_stop()` and `mtl_uninit()` on SIGINT and at the end of the duration.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--p_port <pci>` | required | RX port |
| `--p_sip <ip>` | required | RX port IP |
| `--rx_ip <ip>` | `239.168.85.20` | Multicast group, or the sender IP of a unicast stream |
| `--udp_port <n>` | `20000` | UDP port of session 0; session i uses `n + i` |
| `--sessions <n>` | `1` | RX sessions, and TX sessions with `--tx` |
| `--width`, `--height` | `1920`, `1080` | Resolution |
| `--fps` | `p59` | `p59`, `p50`, `p29`, `p25`, `p60`, `p30` |
| `--fmt` | `YUV_422_10bit` | Transport format; the output format is the same, so no conversion runs |
| `--tx` | off | Also create TX sessions on `--t_port` / `--t_sip` |
| `--hdr_split` | off | Header split RX queues |
| `--dma <pci>` | none | DMA device, repeat for more; sets `ST20P_RX_FLAG_DMA_OFFLOAD` |
| `--time_measure` | off | Set `MTL_FLAG_TASKLET_TIME_MEASURE` |
| `--interval <s>` | `5` | Sample interval |
| `--duration <s>` | `30` | Runtime, `0` runs until SIGINT |
| `--json` | off | One JSON object per interval on one line |
| `--reset_each_interval` | off | Call `mtl_sch_reset_stats()` after each sample |
| `--lcores <list>` | none | `mtl_init_params.lcores` |
| `--log_level` | `warning` | `debug`, `info`, `notice`, `warning`, `error` |

## Output

### CSV

Two header lines start with `#`. Each later line is one record; the second column tells
the record kind.

Scheduler records (`kind` is `loop` or `tasklet`):

```text
ts,kind,sch_idx,sch_name,lcore,time_measure,tasklet_idx,tasklet_name,cnt,avg_ns,min_ns,max_ns,busy_pct,avg_ns_per_loop
```

| Column | Meaning |
| --- | --- |
| `ts` | Wall clock of the sample, seconds since the epoch |
| `sch_idx`, `sch_name` | Scheduler slot and name |
| `lcore` | Lcore of the scheduler, `-1` in thread mode |
| `time_measure` | `1` while the counters move |
| `tasklet_idx`, `tasklet_name` | Tasklet slot and name; `-1` and `loop` for the loop record |
| `cnt` | Handler calls (or loop passes) in the interval |
| `avg_ns` | `sum_ns / cnt` over the interval |
| `min_ns`, `max_ns` | Since `mtl_init()` or the last reset |
| `busy_pct` | `100 * sum_ns / interval_ns` |
| `avg_ns_per_loop` | The scheduler's own loop average, always available |

Session records (`kind` is `session`), cumulative since the session start:

```text
ts,kind,dir,idx,frames,pkts,lost,unrecovered
```

`frames` is the count of frames the tool got from `st20p_rx_get_frame()` or put with
`st20p_tx_put_frame()`. The tool never reads or writes frame data; the TX frames carry
the zero fill of the library. `pkts`, `lost` and `unrecovered` come from the session
stats; a TX record has `lost` and `unrecovered` at `0`.

### JSON

One object per interval:

```json
{"ts":1760000000.123,"interval_ns":5000000000,
 "sch":[{"idx":0,"name":"sch_0","lcore":4,"time_measure":true,"avg_ns_per_loop":812,
         "loop":{"cnt":6150000,"avg_ns":812,"min_ns":90,"max_ns":45120,"busy_pct":99.9},
         "tasklets":[{"idx":0,"name":"rx_video","time":{"cnt":6150000,"avg_ns":640,"min_ns":40,"max_ns":44000,"busy_pct":78.7}}]}],
 "sessions":[{"dir":"rx","idx":0,"frames":2996,"pkts":12500000,"lost":0,"unrecovered":0}]}
```
