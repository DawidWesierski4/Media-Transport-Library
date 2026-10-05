# MTL Manager functional map

This document maps each function of MTL Manager, from the process level down
to each function. It describes the code on the branch `version-update-26.09-rel`.
Each section also gives the change that PR 1741 (`manager-public-api`) makes.

The messages, the socket and the signals have a separate document:
[communication.md](communication.md).

## 1. What the manager does

MTL Manager is one daemon for each host. The MTL processes on the host share
some resources, and the manager gives each resource to one process at a time.

| Resource | Where the manager keeps it | Why a process needs the manager |
| --- | --- | --- |
| lcore | One `std::bitset<128>` for all processes | Two processes must not pin a thread to the same core. |
| RX queue of an interface | One `std::vector<bool>` for each interface | One queue goes to one process. Queue 0 stays with the kernel. |
| ethtool ntuple flow rule | The NIC rule table | The rule sends a UDP flow to the queue of the process. |
| XDP program | The libxdp dispatcher on the interface | Only one loader can own the program chain of an interface. |
| `udp4_dp_filter` BPF map | The map of `mtl.xdp.o` | The map selects the UDP ports that go to AF_XDP. |
| `xsks_map` descriptor | libxdp, for each interface | The process needs the map to add its AF_XDP socket. |

The manager keeps all this state in memory only. It writes no state file.

When a process disconnects, the manager releases each resource that the process
held. This is the "instance monitor" function.

## 2. Process structure

The manager is one process with one thread. There is no worker thread.

```text
main()                              mtl_manager.cpp
 ├─ signalfd (SIGINT)               the stop signal
 ├─ listen socket                   /var/run/imtl/mtl_manager.sock
 └─ epoll loop
     ├─ listen event  → accept() → new mtl_instance
     ├─ signal event  → stop the loop
     └─ client event  → recv() → mtl_instance::handle_message()
                                   ├─ mtl_lcore (singleton)
                                   └─ mtl_interface (one for each ifindex, shared)
                                       ├─ ethtool ioctl (queues, flow rules)
                                       └─ libxdp / libbpf (mtl.xdp.o, maps)
```

### 2.1 Source files

| File | Lines | Contents |
| --- | --- | --- |
| `mtl_manager.cpp` | 215 | `main()`, `mtlm_version()`, the socket and the epoll loop. |
| `mtl_instance.hpp` | 357 | Class `mtl_instance`: the state of one client and the message handlers. |
| `mtl_interface.hpp` | 461 | Class `mtl_interface`: queues, flow rules and XDP of one interface. Global `g_interfaces`. |
| `mtl_lcore.hpp` | 56 | Class `mtl_lcore`: the lcore bitset. |
| `mtl_mproto.h` | 106 | The wire records and the constants. The library includes this file too. |
| `logging.hpp` | 58 | Class `logger`. |
| `mtl.xdp.c` | 64 | The BPF program `mtl_dp_filter` and the map `udp4_dp_filter`. |
| `meson.build`, `meson_options.txt` | | The build. |
| `Dockerfile` | | The container image. |

All the logic is in header files, and `mtl_manager.cpp` is the only
translation unit.

## 3. `main()` — `mtl_manager.cpp:35`

### 3.1 Startup

1. Set the log level to `INFO`. Log the version string of `mtlm_version()`.
2. Make the directory `/var/run/imtl/` if it does not exist. On failure, return
   `-EIO`.
3. Block `SIGINT` with `sigprocmask()`. Make a `signalfd` for `SIGINT`.
4. Make an `AF_UNIX` `SOCK_STREAM` socket.
5. Remove the old socket file with `unlink()`. Then call `bind()`.
6. Set the mode of the socket file to `0777` with `fs::permissions()`.
7. Call `listen()` with a backlog of `MAX_CLIENTS` (10).
8. Make an epoll instance. Add the signal descriptor and the listen descriptor.
9. Log "MTL Manager is running". If the build has no XDP, log a warning.

`MAX_CLIENTS` sets only the `listen()` backlog. It does not limit the number of
clients.

### 3.2 The loop — `mtl_manager.cpp:136`

Each pass calls `epoll_wait()` with no timeout. Then it examines each event.

| Event | Action |
| --- | --- |
| Listen descriptor | `accept()`, add the client to epoll, make an `mtl_instance`, and put it in `clients`. |
| Signal descriptor | Read one `signalfd_siginfo`. If it is `SIGINT`, stop the loop. |
| Client descriptor, `recv() > 0` | Call `handle_message(buf, len)`. The buffer is 256 bytes. |
| Client descriptor, `recv() == 0` | Remove the client from epoll. Erase the instance. The destructor releases the resources. |
| Client descriptor, `recv() < 0` | Log an error. Keep the client. |
| `epoll_wait()` fails | Log an error and start the next pass. |

The communication document gives the result of each event in detail.

### 3.3 Exit

1. Log "MTL Manager exited".
2. Close the signal descriptor, the epoll descriptor and the listen descriptor.
3. Return from `main()`. The `clients` vector goes out of scope, and each
   `mtl_instance` destructor runs.

The manager does not remove the socket file at exit. The next start removes it.

**PR 1741.** `main()` parses the command-line and calls `mtlm_server`. The
server removes the socket file at exit, but only if this process made it. See
section 9.

## 4. Class `mtl_instance` — `mtl_instance.hpp:23`

One `mtl_instance` holds the state of one client connection.

### 4.1 Data

| Member | Type | Meaning |
| --- | --- | --- |
| `conn_fd` | `const int` | The client socket. |
| `is_registered` | `bool` | True after a good `REGISTER`. |
| `pid`, `uid`, `hostname` | | From `REGISTER`. Used in the log prefix only. |
| `lcore_ids` | `unordered_set<uint16_t>` | The lcores of this client. |
| `interfaces` | `unordered_map<ifindex, shared_ptr<mtl_interface>>` | The interfaces that this client uses. |
| `if_queue_ids` | `map<ifindex, set<uint16_t>>` | The queues of this client, for each interface. |
| `if_flow_ids` | `map<ifindex, set<unsigned>>` | The flow rule locations of this client, for each interface. |

### 4.2 Functions

| Function | Line | What it does |
| --- | --- | --- |
| Constructor | 63 | Stores `conn_fd`. Sets `pid` and `uid` to -1 and `hostname` to "unknown". |
| Destructor | 66 | Puts back each lcore, each queue and each flow rule. Closes `conn_fd`. Then the `interfaces` map goes, which can destroy an interface. |
| `send_response()` | 52 | Sends one full `mtl_message_t` with a type and an `int` result. |
| `handle_message()` | 106 | Checks the length and the magic. Calls the handler for the type. |
| `handle_message_register()` | 191 | Stores `pid`, `uid`, `hostname`. Gets each interface. Sets `is_registered`. |
| `get_interface()` | 215 | Finds or makes the `mtl_interface` for an `ifindex`. |
| `handle_message_get_lcore()` | 151 | Takes an lcore from `mtl_lcore`. Adds it to `lcore_ids`. |
| `handle_message_put_lcore()` | 171 | Gives an lcore back to `mtl_lcore`. Removes it from `lcore_ids`. |
| `handle_message_if_xsk_map_fd()` | 244 | Sends the `xsks_map` descriptor with `SCM_RIGHTS`. |
| `handle_message_udp_dp_filter()` | 272 | Adds or removes a UDP port in `udp4_dp_filter`. |
| `handle_message_if_get_queue()` | 289 | Takes a free queue. Adds it to `if_queue_ids`. |
| `handle_message_if_put_queue()` | 306 | Gives a queue back. Removes it from `if_queue_ids`. |
| `handle_message_if_add_flow()` | 323 | Adds an ethtool flow rule. Adds the location to `if_flow_ids`. |
| `handle_message_if_del_flow()` | 341 | Deletes an ethtool flow rule. Removes the location from `if_flow_ids`. |

### 4.3 How `get_interface()` finds an interface

1. Look in the `interfaces` map of this client. If the interface is there,
   return it.
2. Look in the global `g_interfaces` map of `weak_ptr`. If a live interface is
   there, add it to the client map and return it.
3. Make a new `mtl_interface`. Put it in both maps.
4. If the constructor throws, log the error and return `nullptr`.

Each handler that takes an `ifindex` calls `get_interface()`. Thus a request
for an interface that the client did not register makes a new interface. The
constructor of a new interface deletes the flow rules of the NIC.

### 4.4 Checks that each handler does

| Handler | Registration check | Owner check | Reply when the check fails |
| --- | --- | --- | --- |
| get_lcore | Yes | Not applicable | **No reply** |
| put_lcore | Yes | **No** | **No reply** |
| register | Not applicable | Not applicable | `-1` |
| xsk_map_fd | **No** | **No** | Not applicable |
| udp_dp_filter | **No** | **No** | Not applicable |
| get_queue | **No** | Not applicable | Not applicable |
| put_queue | **No** | **No** | Not applicable |
| add_flow | **No** | Not applicable | Not applicable |
| del_flow | **No** | **No** | Not applicable |

"No owner check" means that one client can release a resource of a different
client. The resource then stays in the set of the first client. When the first
client goes, its destructor releases the resource again.

**PR 1741.** `require_registered()` replies `-EPERM` for each handler except
register. Each put and delete handler replies `-EINVAL` if the client does not
own the resource.

### 4.5 Destructor order

1. Log "Remove client".
2. For each lcore in `lcore_ids`, call `mtl_lcore::put_lcore()`.
3. For each queue in `if_queue_ids`, call `put_queue()` on the interface.
4. For each location in `if_flow_ids`, call `del_flow()` on the interface.
5. Close `conn_fd`.
6. Release the `interfaces` map. If this client holds the last reference, the
   `mtl_interface` destructor runs (section 5.3).

The destructor does not remove the UDP ports that the client added to
`udp4_dp_filter`. The port stays in the map until the interface goes.

**PR 1741.** The instance counts its filter ports, and the destructor removes
them.

## 5. Class `mtl_interface` — `mtl_interface.hpp:29`

One `mtl_interface` holds the state of one network interface. All clients that
use the interface share one object through `shared_ptr`.

### 5.1 Data

| Member | Meaning |
| --- | --- |
| `ifindex` | The kernel interface index. |
| `max_combined`, `combined_count` | From `ETHTOOL_GCHANNELS`. |
| `queues` | `vector<bool>` with `combined_count` entries. `true` means "in use". |
| `xdp_prog` | The `mtl.xdp.o` program. XDP builds only. |
| `xsks_map_fd` | The descriptor of the libxdp `xsks_map`. XDP builds only. |
| `udp4_dp_filter_fd` | The descriptor of the `udp4_dp_filter` map. XDP builds only. |
| `xdp_mode` | The attach mode. XDP builds only. |
| `udp4_dp_refcnt` | `map<port, int>`: the number of adds for each port. XDP builds only. |

The global `g_interfaces` (`mtl_interface.hpp:460`) maps `ifindex` to
`weak_ptr<mtl_interface>`.

### 5.2 Constructor — line 73

1. Call `clear_flow_rules()`.
2. Call `load_xdp()`. If it fails, throw. If the build has no XDP, throw.
3. Call `parse_combined_info()`. If it fails, throw.
4. Resize `queues` to `combined_count`. Mark queue 0 as in use.

Thus a build without XDP cannot make an interface. Each request that names an
interface then fails.

### 5.3 Destructor — line 93

1. Call `unload_xdp()`.
2. Call `clear_flow_rules()`.

### 5.4 Functions

| Function | Line | What it does | Result |
| --- | --- | --- | --- |
| `get_xsks_map_fd()` | 58 | Returns `xsks_map_fd`. | The descriptor, or -1 |
| `update_udp_dp_filter()` | 102 | Changes the count of the port. Only the first add and the last remove write the map. | 0 or -1 |
| `get_queue()` | 144 | Finds the first free queue and marks it. | The queue, or -1 |
| `put_queue()` | 157 | Marks a queue as free. | 0 or -1 |
| `clear_flow_rules()` | 168 | Reads each rule location, and deletes each rule. | 0 or negative |
| `parse_combined_info()` | 235 | Reads the channel counts. | 0 or -1 |
| `add_flow()` | 267 | Finds a free rule location and inserts a UDP4 rule. | The location, or negative |
| `del_flow()` | 376 | Deletes the rule at a location. | 0 or negative |
| `load_xdp()` | 412 | Loads and attaches `mtl.xdp.o`. Gets the two map descriptors. | 0 or -1 |
| `unload_xdp()` | 452 | Detaches and closes `mtl.xdp.o`. | None |

### 5.5 `add_flow()` in detail

1. Read the rule count with `ETHTOOL_GRXCLSRLCNT`.
2. Read all rule locations with `ETHTOOL_GRXCLSRLALL`. The field `data` gives
   the table size.
3. Start at the last location of the table. Go down until a location is free.
   Location 0 is never used.
4. Fill an `ethtool_rx_flow_spec`. Set a mask only for a field that is not zero.
   Set `ring_cookie` to the queue.
5. Insert the rule with `ETHTOOL_SRXCLSRLINS` at the free location.
6. Return the location. The client uses the location as the flow ID.

The code does not check the result of `calloc()` in step 2.

### 5.6 `load_xdp()` in detail

1. Find `mtl.xdp.o` with `xdp_program__find_file()`. libxdp looks in its search
   path.
2. Attach in native mode. If that fails, attach in SKB mode.
3. Call `xsk_setup_xdp_prog()`. This adds the libxdp `xsk_def_prog` to the
   dispatcher and gives `xsks_map_fd`.
4. Find the map `udp4_dp_filter` and store its descriptor.

The dispatcher chain on the interface is then:

| Priority | Program | Chain action |
| --- | --- | --- |
| 19 | `mtl_dp_filter` | `XDP_DROP` goes to the next program |
| 20 | `xsk_def_prog` | `XDP_PASS` |

`unload_xdp()` detaches `mtl_dp_filter` only. The dispatcher and
`xsk_def_prog` stay on the interface.

**PR 1741.** `detach()` also calls `remove_dispatcher()`, which removes the full
chain.

### 5.7 The BPF program — `mtl.xdp.c`

1. If the packet is not IPv4 UDP, return `XDP_PASS`.
2. Look up the UDP destination port in `udp4_dp_filter`.
3. If the port is in the map, return `XDP_DROP`. The dispatcher then runs
   `xsk_def_prog`, which sends the packet to the AF_XDP socket.
4. If not, return `XDP_PASS`. The packet goes to the kernel stack.

The map is a hash of 256 entries. The key is a `u16` port and the value is a
`u8`.

### 5.8 Defects in `mtl_interface`

| Location | Defect | Result |
| --- | --- | --- |
| `clear_flow_rules()`, line 217 | `memset(&cmd, 0, ...)` in the loop sets `cmd.rule_cnt`, the loop bound, to 0. | The function deletes only the first rule. |
| `load_xdp()`, line 430 | The code sets `xdp_mode` to native after the SKB branch too. | A program in SKB mode gets a detach in the wrong mode. |
| `update_udp_dp_filter()`, line 114 | A remove with no add makes the count negative. | The next add does not write the map. |
| `put_queue()`, line 157 | The function accepts queue 0. | A client can free queue 0. `get_queue()` can then give queue 0. |
| `add_flow()`, line 298 | No check of `calloc()`. | A null pointer write on low memory. |

PR 1741 corrects each of these defects.

## 6. Class `mtl_lcore` — `mtl_lcore.hpp`

`mtl_lcore` is a singleton with a `bitset<128>` and a mutex.

| Function | What it does | Result |
| --- | --- | --- |
| `get_instance()` | Returns the one object. | Reference |
| `get_lcore(id)` | If `id` is below 128 and free, marks it. | 0, or -1 if used or out of range |
| `put_lcore(id)` | If `id` is below 128 and used, frees it. | 0, or -1 if free or out of range |

The mutex is not necessary, because the manager has one thread. The class
refuses an lcore ID above 127.

**PR 1741.** The functions return `-EINVAL` and `-EBUSY`, and add `is_used()`
and `used_count()`.

## 7. Class `logger` — `logging.hpp`

| Function | What it does |
| --- | --- |
| `set_log_level()` | Sets the minimum level. The default is `DEBUG`. `main()` sets `INFO`. |
| `log()` | Writes the time, the level and the text to stdout. |

The levels are `DEBUG`, `INFO`, `WARNING` and `ERROR`. There is no option to
change the level at runtime.

**PR 1741.** `WARNING` and `ERROR` go to stderr. `--log-level` sets the level.

## 8. Build, package and start

### 8.1 Build

| Item | Value |
| --- | --- |
| Build file | `manager/meson.build` |
| Option | `enable_asan` |
| Output | `MtlManager` and `mtl.xdp.o` |
| XDP | `MTL_HAS_XDP_BACKEND` is set when libxdp and libbpf are found. |
| Install | `ninja install`. `./build.sh` builds the manager after the library. |

### 8.2 Container

The `Dockerfile` makes an image. Run it with `--privileged` and `--net=host`,
and mount `/var/run/imtl` and `/sys/fs/bpf`. The README says to stop it with
`docker kill -s SIGINT`.

### 8.3 Who starts and stops the manager

| Tool | Start | Stop | Stop signal |
| --- | --- | --- | --- |
| `.github/mcp/mtl_mcp_server.py` | `sudo nohup build/manager/MtlManager` | `sudo pkill -x MtlManager` | `SIGTERM` |
| `tests/acceptance/common/mtl_manager/mtlManager.py` | `sudo MtlManager` | `kill`, then `sudo pkill MtlManager` | `SIGKILL`, then `SIGTERM` |
| `.github/scripts/ci/cleanup.sh` | Not applicable | `killall -SIGINT MtlManager` | `SIGINT` |
| README, container | Manual | `docker kill -s SIGINT` | `SIGINT` |

Only `SIGINT` gives a clean stop in the current code. The MCP server and the
acceptance helper send signals that kill the process without a cleanup. The
communication document, section 6, gives the result.

## 9. PR 1741: new structure

PR 1741 splits the manager into classes that a test can replace.

| New file | Contents |
| --- | --- |
| `include/mtl_mproto.h` | The wire records, moved to the public API. Protocol version 1.1. |
| `include/mtlm_api.h` | The public client API. |
| `manager/mtlm_client.c` | The client library `libmtlm_client`. |
| `manager/mtlm_sock_path.c` | The socket path search. |
| `manager/mtlm_server.{hpp,cpp}` | The socket, the signals and the epoll loop. |
| `manager/mtl_instance.cpp` | The handlers, with the stream framing in `feed()`. |
| `manager/mtl_interface.cpp` | The interface, with `mtl_interface_registry`. |
| `manager/mtlm_netdev.hpp`, `mtlm_netdev_linux.cpp` | The ethtool calls behind `mtlm_netdev_ops`. |
| `manager/mtlm_xdp.cpp` | The XDP calls behind `mtlm_xdp_ops`. |
| `manager/mtl_lcore.cpp`, `logging.cpp` | The code that was in the headers. |
| `manager/service/` | systemd units, a tmpfiles rule and `install_service.sh`. |
| `manager/tests/`, `manager/tools/` | Unit tests, API tests and `MtlManagerProbe`. |

### 9.1 command-line

| Option | Function |
| --- | --- |
| `--sock-path PATH` | Bind `PATH`. |
| `--socket-mode OCTAL` | The mode of the socket file. |
| `--socket-group NAME` | The group of the socket file. Mode `0660`. |
| `--max-clients N` | The maximum number of clients. Default 64, range 1 to 4096. |
| `--log-level LEVEL` | `debug`, `info`, `warning` or `error`. |
| `--print-sock-path` | Print the path, then exit. |
| `--version` | Print the version and the protocol version, then exit. |
| `--help` | Print the options. |

### 9.2 Interface without XDP

In the PR, an interface works without XDP unless the caller needs XDP. Queue and
flow requests then work on a build or a host without libxdp.
`mtl_interface_registry` refuses an interface without XDP only for a caller that
needs XDP.

### 9.3 systemd units

| Unit | Main settings |
| --- | --- |
| `mtl-manager.service` | `RuntimeDirectory=imtl`, `KillSignal=SIGTERM`, `TimeoutStopSec=10`, `Restart=on-failure`, `EnvironmentFile=/etc/mtl/manager.env` |
| `mtl-manager-user.service` | The socket is below `XDG_RUNTIME_DIR`. No XDP. |
| `mtl-manager.conf` | tmpfiles rule `d /run/imtl 0755`. |

### 9.4 Tests

| Suite | Cases | Needs |
| --- | --- | --- |
| `manager/tests` (`MtlManagerTest`) | 98 | No NIC, no root |
| `tests/integration_tests/manager` (`MtlManagerApiTest`) | 31 | No NIC, no root |
| `MtlManagerProbe` | 188 | Root and an interface for the last four groups |
| Acceptance, `-m mtlmanager` | | The full acceptance host |

## 10. Library side

The library talks to the manager through `lib/src/mt_instance.c`. The
communication document, section 8, maps each library function to its message.

| Library function | File and line | When it runs |
| --- | --- | --- |
| `mt_instance_init()` | `mt_main.c:484` | At `mtl_init()`, after the privilege check. The caller ignores the result. |
| `mt_instance_uinit()` | `mt_main.c:652` | At `mtl_uninit()`. |
| `mt_is_manager_connected()` | `mt_main.h:1268` | Tests `instance_fd > 0`. |
| `mt_sch_get_lcore()` | `mt_sch.c:713` | Each scheduler start. Without the manager, the library uses the shared memory lcore path. |
| `mt_sch_put_lcore()` | `mt_sch.c:797` | Each scheduler stop. |
| `mt_socket_add_flow()` | `mt_socket.c:356` | RX flow create on a kernel control driver. |
| `mt_socket_remove_flow()` | `mt_socket.c:411` | RX flow free on a kernel control driver. |
| `xdp_socket_update_xskmap()` | `dev/mt_af_xdp.c:390` | Native AF_XDP socket create. |
| Queue get and put | `dev/mt_af_xdp.c:757`, `:237` | Native AF_XDP init and free. |
| Filter add and remove | `dev/mt_af_xdp.c:985`, `:1025` | Native AF_XDP RX queue get and put. |
| `mtl_is_manager_alive()` | `include/mtl_api.h:1657` | Public API. Connects and disconnects. |

Native AF_XDP cannot run without the manager (`dev/mt_af_xdp.c:730`).

On Windows, each `mt_instance_*` function is a stub that returns `-ENOTSUP`.
The stub of `mt_instance_update_udp_dp_filter()` is missing.

**PR 1741.** `impl->instance_client` replaces `instance_fd`. `mt_instance.c`
becomes a set of short wrappers over `mtlm_*`.
