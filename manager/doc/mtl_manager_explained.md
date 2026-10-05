# MTL Manager explained

This document explains MTL Manager for a reader who knows nothing about it. It
explains the purpose of the manager, and the purpose of each of its functions.
It also explains why the code is the way it is.

The document has eight parts:

1. **Background.** The Linux, network and C++ concepts that you must know to
   read the manager. Read this part first if XDP, AF_XDP, eBPF or `SCM_RIGHTS`
   are new to you.
2. **Purpose.** What the manager must do, as a set of rules, without code.
3. **Why.** The reason for each design decision, with its cost and an
   alternative.
4. **Function contracts.** For each function: its purpose, its input, its
   normal result, an example, and what it must do with wrong data.
5. **Function tables.** Each function with its caller, the state it reads and
   changes, its result and its errors.
6. **Examples.** Full sequences, from the start of a client to its end.
7. **Corrections.** Errors in the two other documents.
8. **AF_XDP guide.** A learning path, a lab, a quickstart, and the AF_XDP
   data path of MTL for ST 2110, with a comparison to DPDK.

The code is the branch `version-update-26.09-rel`. The other documents are
[functional_map.md](functional_map.md) and
[communication.md](communication.md). This document does not repeat the byte
layout of the records. Refer to `communication.md` for it.

In this document, these names have one meaning each:

| Name | Meaning |
| --- | --- |
| manager | The `MtlManager` process. |
| client | One MTL process that is connected to the manager. |
| library | The MTL library (`libmtl`) in the client. |
| interface | One Linux network interface, for example `ens785f0`. |
| queue | One RX queue of an interface. |
| flow rule | One ethtool ntuple rule in the NIC. |
| lcore | One logical CPU core, as DPDK numbers it. |
| request | A record from the client to the manager. |
| reply | A record from the manager to the client. |

---

## Part 1. Background

### 1.1 The problem, with an example

A host has one Intel E810 NIC port, `ens785f0`, and 32 CPU cores. Three MTL
processes run on the host:

| Process | Work |
| --- | --- |
| A | Sends one 1080p video stream. |
| B | Receives two video streams on UDP ports 20000 and 20002. |
| C | Receives one audio stream on UDP port 30000. |

Each process needs three things:

1. **CPU cores.** MTL runs a loop on each core, and the loop never stops. This
   loop is a "busy poll". A busy poll uses 100% of its core.
2. **RX queues.** The NIC puts received packets in queues. Each process needs
   its own queues.
3. **Flow rules.** The NIC must put the packets of process B in the queue of
   process B, and not in the queue of process C.

Without an agreement between the processes, these faults occur:

- Process A and process B both take core 2. Each loop gets only half of the
  core. The video packets of A leave late, and the receiver drops frames.
- Process B and process C both take queue 3. The packets for B go to C, or the
  packets for C go to B.
- Process C stops with a crash. Its flow rule stays in the NIC. The next
  process that uses port 30000 gets no packets, because the old rule sends them
  to a queue that nobody reads.

The manager prevents these faults. It is the one place on the host that gives
out cores, queues and flow rules. It records which process has each one. When
a process stops, the manager takes back everything that the process had.

A simple comparison is the reception desk of a hotel. The desk gives one key
for each room. It records which guest has which key. When a guest leaves, the
desk takes the key back. If a guest leaves without a word, the desk still
knows, because the guest is no longer in the building. For the manager, "the
guest is no longer in the building" means "the socket of the client closed".

### 1.2 Users, root and capabilities

Linux gives each process a user ID. The user ID 0 is **root**. Root can do all
privileged operations.

Linux also divides the power of root into **capabilities**. A process can have
one capability without the others. These capabilities are important for MTL:

| Capability | What it permits | Who needs it for MTL |
| --- | --- | --- |
| `CAP_NET_ADMIN` | Change the network configuration: add a flow rule, attach an XDP program. | The manager |
| `CAP_BPF` (or `CAP_SYS_ADMIN` on an old kernel) | Load an eBPF program and make an eBPF map. | The manager |
| `CAP_NET_RAW` | Make a raw socket, which includes an AF_XDP socket. | The client |

The MTL XDP guide (`doc/xdp.md`) tells you to give only `CAP_NET_RAW` to the
application:

```bash
sudo setcap 'cap_net_raw+ep' ./tests/tools/RxTxApp/build/RxTxApp
```

Thus the application can make AF_XDP sockets, but it cannot load an XDP program
or add a flow rule. The manager does these two operations for it. The manager
runs as root.

This design is **privilege separation**. A small program does the dangerous
operations. The large program, with codecs, parsers and plugins, runs with less
power. If the large program has a defect, the defect can do less damage. The
first commit of the manager has the title "manager: add daemon server for
privileged controls" (commit `5a564cb8`, November 2023).

### 1.3 CPU cores, lcores, pinning and NUMA

A modern CPU has many cores. With hyper-threading, each physical core shows as
two logical cores. Linux numbers each logical core, for example 0 to 31.

DPDK calls a logical core an **lcore**. DPDK gives an lcore ID to each core
that the process can use. Usually the lcore ID is the same as the Linux CPU
number.

**Pinning** means that a thread runs on one core only. The Linux scheduler
cannot move it. MTL pins each of its scheduler threads, because a move to a
different core makes the cache cold and gives a delay. A delay breaks the
packet timing of ST 2110.

A pinned busy poll takes all the time of its core. Two pinned busy polls on one
core share the core, and each one is late. Thus two processes must never pin a
busy poll to the same core. This is the first resource of the manager.

**NUMA** (non-uniform memory access) means that a large server has more than
one CPU socket. Each socket has its own memory and its own PCI devices. A core
reads the memory of its own socket faster. MTL tries to take a core on the same
socket as the NIC. The library does this selection, not the manager. The
manager only answers "free" or "used" for each lcore that the library asks
for.

Before the manager existed, MTL shared the lcore table in System V shared
memory, with a file lock (`doc/shm_lcore.md`). That design has a problem: when a
process crashes, its lcores stay "used" in the table. A person must clean the
table with the tool `LcoreMgr`. The manager does not have this problem, because
the kernel closes the socket of a crashed process, and the manager sees it.

### 1.4 The NIC, RX queues and RSS

A NIC receives packets from the cable and writes them to memory. It does not
write all packets to one place. It has many **RX queues**. Each queue is a ring
of buffers in memory. One CPU core reads one queue. Many queues let many cores
read at the same time.

Linux calls a pair of one RX queue and one TX queue a **combined channel**. You
can read the channel counts with this command:

```bash
ethtool -l ens785f0
```

The result has "Pre-set maximums" (`max_combined`) and "Current hardware
settings" (`combined_count`). The manager reads the current count with the
ioctl `ETHTOOL_GCHANNELS`. It then makes one entry for each queue.

The NIC selects a queue for each packet. When no rule applies, the NIC uses
**RSS** (receive side scaling). RSS calculates a hash of the addresses and
ports of the packet, and the hash selects a queue. Thus a normal TCP connection,
for example SSH, can go to any queue.

The kernel uses the queues too. The normal kernel network stack reads each
queue that no AF_XDP socket uses. Queue 0 is special in the manager: the
manager never gives it to a client. Part 3, section 3.18, explains why.

### 1.5 ethtool ntuple flow rules

RSS spreads packets by hash. MTL needs a different result: "all packets to UDP
port 20000 must go to queue 3". A **flow rule** does this. The E810 calls this
function "Flow Director". Linux calls it an **ntuple** rule.

This is the command form of a rule:

```bash
ethtool -N ens785f0 flow-type udp4 dst-ip 239.1.1.1 dst-port 20000 action 3 loc 1023
```

The rule means: "a UDP IPv4 packet to 239.1.1.1 port 20000 goes to queue 3". The
rule is at location 1023.

| Word | Meaning |
| --- | --- |
| `flow-type udp4` | The rule applies to UDP over IPv4. The ioctl value is `UDP_V4_FLOW` (2). |
| `dst-ip`, `src-ip`, `dst-port`, `src-port` | The fields to compare. A field that you do not give matches all values. |
| `action 3` | Put the packet in queue 3. The ioctl field is `ring_cookie`. |
| `loc 1023` | The location of the rule in the rule table. |

The NIC has a table of rules, with a fixed number of locations. Each rule has
one location. On many NICs, a rule at a lower location has a higher priority.
You can list the rules and delete a rule with these commands:

```bash
ethtool -n ens785f0                 # list the rules
ethtool -N ens785f0 delete 1023     # delete the rule at location 1023
```

The manager does not run the `ethtool` program. It sends the same commands to
the kernel with `ioctl(SIOCETHTOOL)`:

| ioctl command | Same as | What it does |
| --- | --- | --- |
| `ETHTOOL_GRXCLSRLCNT` | `ethtool -n` | Read the number of rules. |
| `ETHTOOL_GRXCLSRLALL` | `ethtool -n` | Read the location of each rule, and the table size. |
| `ETHTOOL_SRXCLSRLINS` | `ethtool -N ... loc N` | Insert a rule. |
| `ETHTOOL_SRXCLSRLDEL` | `ethtool -N ... delete N` | Delete a rule. |
| `ETHTOOL_GCHANNELS` | `ethtool -l` | Read the channel counts. |

A rule belongs to the NIC, not to a process. When the process that asked for
the rule stops, the rule stays. Only a delete removes it. This is why the
manager must delete the rules of a client that stops.

### 1.6 The kernel network stack and kernel bypass

Usually a packet goes through the **kernel network stack**. The driver makes a
kernel structure `sk_buff` for each packet. The stack checks the IP header and
the UDP header, finds the socket, and copies the data to the application. This
is safe and general. It is also slow for video: one 1080p stream at 60 frames
is about 270,000 packets each second.

**Kernel bypass** means that the application gets the packets without the
kernel stack. MTL has two ways to bypass the kernel:

| Way | How it works | Need for the manager |
| --- | --- | --- |
| DPDK | The NIC, or a VF of it, is taken from the kernel driver and given to `vfio-pci`. DPDK drives the NIC from user space. | Only for lcores |
| AF_XDP | The NIC stays with its kernel driver. The driver gives chosen packets to a socket in user space. | For lcores, queues, flow rules, XDP and the map descriptor |

AF_XDP is important for cloud and edge use, because the interface stays a
normal Linux interface. SSH, `ip` and other tools continue to work on it.

MTL has two AF_XDP drivers:

| Driver name in MTL | Port string example | What makes the AF_XDP sockets |
| --- | --- | --- |
| `native_af_xdp` | `native_af_xdp:ens785f0` | MTL itself, in `lib/src/dev/mt_af_xdp.c` |
| `net_af_xdp` (DPDK AF_XDP PMD) | `dpdk_af_xdp:ens785f0` | The DPDK AF_XDP driver |

### 1.7 AF_XDP in more detail

An **AF_XDP socket** (also "XSK") connects to one queue of one interface. The
application gives the kernel a block of memory, the **UMEM**. The UMEM is
divided into frames, for example 4 KiB each. Four rings move frame numbers
between the application and the driver:

| Ring | Direction | Meaning |
| --- | --- | --- |
| Fill | Application to driver | "These frames are free. Write received packets into them." |
| RX | Driver to application | "A packet is in this frame." |
| TX | Application to driver | "Send the packet in this frame." |
| Completion | Driver to application | "The packet in this frame is sent. The frame is free again." |

In **zero-copy** mode, the NIC writes the packet directly into the UMEM. In
**copy** mode, the driver copies each packet into the UMEM. Copy mode is slower
but works with each driver. The library tries zero-copy first, then copy mode
(`xdp_socket_init()`, `mt_af_xdp.c:422`).

An AF_XDP socket on queue 3 does not get all the packets of queue 3
automatically. A small program in the driver must send each packet to the
socket. That program is an XDP program. The next sections explain it.

### 1.8 eBPF and XDP

**eBPF** is a small virtual machine in the Linux kernel. You write a small
program in C, compile it with `clang -target bpf`, and load it into the kernel.
The kernel **verifier** checks the program before it runs. The verifier refuses
a program that can read outside a packet, loop without end, or crash the
kernel.

An eBPF program keeps data in an **eBPF map**. A map is a table in the kernel.
A program in the kernel and a process in user space can both read and write the
same map. User space reaches a map through a file descriptor (section 1.10).

| Map type | Key | Value | Use in MTL |
| --- | --- | --- | --- |
| `BPF_MAP_TYPE_HASH` | Any fixed size | Any fixed size | `udp4_dp_filter`: key is a UDP port, value is 1. |
| `BPF_MAP_TYPE_XSKMAP` | Queue number | An AF_XDP socket | `xsks_map`: which socket gets the packets of which queue. |

**XDP** (eXpress Data Path) is a place in the network driver where an eBPF
program can run. The program runs for each received packet, before the kernel
makes an `sk_buff`. This is the earliest and fastest point to decide what
happens to a packet. The program returns an **action**:

| Action | Result |
| --- | --- |
| `XDP_PASS` | The packet continues to the normal kernel network stack. |
| `XDP_DROP` | The packet is discarded. |
| `XDP_TX` | The packet goes back out of the same interface. |
| `XDP_REDIRECT` | The packet goes to a different place, for example an AF_XDP socket. |
| `XDP_ABORTED` | An error. The packet is discarded. |

An XDP program attaches in one of two modes:

| Mode | Where it runs | Speed | Needs |
| --- | --- | --- | --- |
| Native (driver) | In the driver, before the `sk_buff` | Fast | Driver support. `ice` has it. |
| SKB (generic) | After the kernel makes the `sk_buff` | Slow | Nothing. Each driver works. |

The manager tries native mode first, and then SKB mode.

### 1.9 One XDP slot, many programs: the libxdp dispatcher

The kernel lets **one** XDP program attach to an interface. MTL needs two
programs on the same interface:

1. `mtl_dp_filter`, from the manager. It selects the UDP ports of MTL.
2. `xsk_def_prog`, from libxdp. It sends a packet to the AF_XDP socket of its
   queue.

**libxdp** solves this with a **dispatcher**. The dispatcher is the one program
that the kernel attaches. It calls the real programs one after the other, in
the order of their **priority**. A lower number runs first.

Each program says which of its actions continue to the next program. This is
the **chain call action** list. In C, the program declares it with
`XDP_RUN_CONFIG`. This is the declaration in `manager/mtl.xdp.c:22`:

```c
struct {
  __uint(priority, 19);
  __uint(XDP_PASS, 0);   /* XDP_PASS: stop the chain, give the packet to the kernel */
  __uint(XDP_DROP, 1);   /* XDP_DROP: continue to the next program */
} XDP_RUN_CONFIG(mtl_dp_filter);
```

The result is a chain of two programs:

| Priority | Program | Action that continues the chain | Final action |
| --- | --- | --- | --- |
| 19 | `mtl_dp_filter` | `XDP_DROP` | `XDP_PASS` goes to the kernel stack. |
| 20 | `xsk_def_prog` | `XDP_PASS` | `XDP_REDIRECT` to the socket, or `XDP_PASS`. |

Thus in this chain, `XDP_DROP` from `mtl_dp_filter` does **not** discard the
packet. It means "this packet is for MTL, give it to the next program". This
is the most confusing part of the manager. Part 3, section 3.22, explains why
the code uses `XDP_DROP` for it.

You can see the chain with this command:

```bash
sudo xdp-loader status
```

### 1.10 The path of one packet

This example uses the interface `ens785f0`. Process B has an AF_XDP socket on
queue 3, and port 20000 is in `udp4_dp_filter`. A flow rule sends UDP port
20000 to queue 3.

```text
cable
  │
  ▼
NIC: flow rule "udp4 dst-port 20000 → queue 3"       (ethtool ntuple rule)
  │  packet in queue 3
  ▼
driver (ice): XDP hook
  │
  ▼
xdp_dispatcher
  ├─ prio 19  mtl_dp_filter
  │     IPv4? UDP? dst port 20000 in udp4_dp_filter?  yes → XDP_DROP → continue
  │                                                   no  → XDP_PASS → kernel stack
  └─ prio 20  xsk_def_prog
        bpf_redirect_map(xsks_map, rx_queue_index = 3, XDP_PASS)
        slot 3 has the socket of process B → XDP_REDIRECT → AF_XDP socket
        slot 3 is empty                     → XDP_PASS     → kernel stack
  │
  ▼
AF_XDP socket of process B: frame in the RX ring → MTL reads the video packet
```

These packets take different paths in the same chain:

| Packet | Queue | `mtl_dp_filter` | `xsk_def_prog` | Goes to |
| --- | --- | --- | --- | --- |
| UDP to port 20000 | 3 (flow rule) | `XDP_DROP` (continue) | Redirect to slot 3 | Process B |
| SSH (TCP port 22), RSS selects queue 3 | 3 | `XDP_PASS` | Not called | Kernel, then `sshd` |
| ARP | Any | `XDP_PASS` (not IPv4) | Not called | Kernel |
| UDP to port 20000, but no socket on the queue | 5 | `XDP_DROP` (continue) | Slot 5 empty, `XDP_PASS` | Kernel |

The second row shows why the filter is necessary. Without the filter, the SSH
packet on queue 3 would go to process B, and the SSH connection would stop.

`xsks_map` has 64 entries (`DEFAULT_QUEUE_IDS` in libxdp
`xsk_def_xdp_prog.c`). Thus only queues 0 to 63 can have a socket.

### 1.11 File descriptors, and how to give one to a different process

A **file descriptor** (fd) is a small integer that a process uses to refer to
a kernel object. The object can be a file, a socket, an eBPF map, or an eBPF
program. The number has a meaning only in its own process. Each process has its
own table of numbers.

Example: in the manager, fd 7 refers to `xsks_map` of `ens785f0`. In process B,
fd 7 can be a log file. If the manager sends the number 7 as data, process B
gets nothing useful.

The kernel gives one correct way to copy a descriptor to a different process
over a Unix socket: **`SCM_RIGHTS`**. The sender puts the fd in a "control
message" of `sendmsg()`. The kernel adds a new entry in the fd table of the
receiver, which refers to the same kernel object. The receiver gets its own
number from `recvmsg()`, for example 12.

```text
manager fd 7 ──┐
               ├──▶ kernel object: xsks_map of ens785f0
process B fd 12┘
```

A control message must travel with at least one byte of normal data. This is
why the manager sends one byte, a space, with the descriptor.

With the map descriptor, process B can call `bpf_map_update_elem()` on
`xsks_map`. No capability is necessary for this call, because the descriptor
itself is the permission. This is how a process with only `CAP_NET_RAW` puts
its socket in a map that root made.

### 1.12 Unix domain sockets

A **Unix domain socket** (`AF_UNIX`) connects two processes on the same host.
It uses a file path as its address, for example
`/var/run/imtl/mtl_manager.sock`. The data never goes to a network.

1. The server calls `socket()`, then `bind()` with the path. `bind()` makes a
   special file of type "socket" at the path.
2. The server calls `listen()`, then `accept()` for each client.
3. The client calls `socket()`, then `connect()` with the same path.
4. Each side then calls `send()` and `recv()`.

`ls -l` shows the socket file with the letter `s`:

```text
srwxrwxrwx 1 root root 0 Sep 29 10:00 /var/run/imtl/mtl_manager.sock
```

There are three socket types. The difference is important for the manager:

| Type | Record boundaries | Order | Use |
| --- | --- | --- | --- |
| `SOCK_STREAM` | **No.** The data is one stream of bytes. | Kept | The manager uses this type. |
| `SOCK_SEQPACKET` | Yes. One `recv()` gives one full record. | Kept | An alternative (section 3.4). |
| `SOCK_DGRAM` | Yes | Kept for `AF_UNIX` | No connection. The manager cannot see a client stop. |

"No record boundaries" means this: the client sends two records of 118 bytes.
The manager can get one `recv()` of 236 bytes. Or it can get one `recv()` of 100
bytes and one of 136 bytes. The stream gives the bytes in the correct order,
but it does not keep the size of each `send()`. A receiver must count the bytes
itself. This is **framing**.

When one side closes its socket, a `recv()` on the other side returns 0. This
is how the manager knows that a client has gone. The kernel closes each socket
of a process when the process stops for any reason, including `SIGKILL` and a
crash.

When one side sends to a socket that the other side has closed, `send()` fails
with `EPIPE`. The kernel also sends the signal `SIGPIPE` to the sender. The flag
`MSG_NOSIGNAL` on `send()` stops the signal.

### 1.13 File modes, umask and the socket file

Each file has a **mode**: three groups of three permission bits. The groups are
for the owner, the group of the file, and all other users. The bits are read
(r, 4), write (w, 2) and execute (x, 1). The mode is usually written as an
octal number.

| Octal | Text | Meaning |
| --- | --- | --- |
| `0777` | `rwxrwxrwx` | Each user can read, write and execute. |
| `0755` | `rwxr-xr-x` | The owner can write. The others can read and execute. |
| `0666` | `rw-rw-rw-` | Each user can read and write. |
| `0660` | `rw-rw----` | The owner and the group can read and write. The others have nothing. |
| `0600` | `rw-------` | Only the owner can read and write. |

For a socket file, only one bit has an effect on Linux: **write**. `connect()`
needs write permission on the socket file (see `man 7 unix`). Read and execute
have no effect. Thus `0777` and `0666` give the same result for a socket.

`connect()` also needs execute ("search") permission on each directory of the
path. For `/var/run/imtl/`, each user needs `x` on `/var`, `/var/run` and
`/var/run/imtl`.

The **umask** of a process removes bits from the mode of each new file. A
common umask is `022`. It removes write for the group and for the others.
`bind()` makes the socket file with mode `0777` minus the umask. With umask
`022`, the result is `0755`. With `0755`, only the owner (root) can write, and
only root can connect.

Part 3, section 3.1, uses these facts to explain the `0777` in the manager.

### 1.14 epoll

The manager waits for events on many descriptors at the same time: the listen
socket, the signal descriptor, and one socket for each client. **epoll** lets
one thread wait for all of them in one call.

1. `epoll_create1()` makes an epoll instance.
2. `epoll_ctl(EPOLL_CTL_ADD, fd)` adds a descriptor to the watch list.
3. `epoll_wait()` sleeps until one or more descriptors are ready. It returns a
   list of the ready descriptors.

The manager uses **level-triggered** mode, which is the default. In this mode,
`epoll_wait()` reports a descriptor again and again while it has data to read.
Thus a descriptor that is always "ready", for example because of an error,
makes the loop run without a stop.

Example: the manager watches fds 3 (signal), 4 (listen), 6 (process A), 8
(process B). Process B sends a request. `epoll_wait()` returns one event, for
fd 8. The manager reads from fd 8, and the other fds wait.

### 1.15 Signals

A **signal** is a short message from the kernel, or from a different process,
to a process. It has only a number. Each signal has a **default action**. The
most usual default action is "terminate the process".

A process can do one of four things with a signal:

| Choice | Result |
| --- | --- |
| Default | The default action occurs, usually "terminate". |
| Ignore (`SIG_IGN`) | The kernel discards the signal. |
| Catch (a handler) | The kernel stops the process wherever it is, and calls a handler function. |
| Block (`sigprocmask`) | The kernel keeps the signal "pending" and does not deliver it. |

Two signals cannot be caught, ignored or blocked: `SIGKILL` and `SIGSTOP`.

A handler function has a strong limit. It runs in the middle of any other code,
for example in the middle of a `malloc()` or in the middle of a change to a
C++ container. Thus a handler can call only "async-signal-safe" functions. It
must not free memory, write to a log with `std::string`, or run a C++
destructor.

A **signalfd** solves this problem. The process blocks the signal first. Then
it makes a signalfd for it. The kernel then puts each pending signal in the
signalfd as data. The process reads it at a safe time, in its normal loop, like
any other event. The code that runs after the read is normal code, and it can
do anything.

The block must come first. If the signal is not blocked, the kernel does the
default action before the signalfd gets the signal.

| Signal | Number | Default action | Usual source |
| --- | --- | --- | --- |
| `SIGHUP` | 1 | Terminate | The terminal closes. `nohup` sets it to "ignore". |
| `SIGINT` | 2 | Terminate | Ctrl+C in the terminal. `kill -INT`. |
| `SIGQUIT` | 3 | Terminate and write a core dump | Ctrl+\\ |
| `SIGKILL` | 9 | Terminate. Cannot be changed. | `kill -9`. The OOM killer. |
| `SIGPIPE` | 13 | Terminate | A `send()` to a closed socket. |
| `SIGTERM` | 15 | Terminate | `kill`, `pkill`, `killall`, `systemctl stop`, `docker stop`. |

`kill <pid>` sends `SIGTERM` when you give no signal. `pkill` and `killall` also
send `SIGTERM` by default. These are the most usual ways to stop a daemon.

When the kernel terminates a process on a signal, no code of the process runs.
No destructor runs, and no cleanup runs. The kernel closes the descriptors and
frees the memory, but it does not remove an XDP program or a flow rule. Those
belong to the interface, not to the process.

### 1.16 Byte order

A number of four bytes can go into memory in two orders. **Big-endian** puts
the most important byte first. **Little-endian** puts it last. x86 CPUs are
little-endian. Network protocols use big-endian, which is also called
**network byte order**.

| Function | Meaning |
| --- | --- |
| `htonl()` | Host to network, 4 bytes |
| `htons()` | Host to network, 2 bytes |
| `ntohl()` | Network to host, 4 bytes |
| `ntohs()` | Network to host, 2 bytes |

Example: the value `0x494D544C` in network order is the bytes `49 4D 54 4C`,
which is the ASCII text "IMTL". This is the magic value of each record.

### 1.17 C++ object lifetime

The manager is C++. It uses the C++ object lifetime to release resources.

| Tool | Meaning | Use in the manager |
| --- | --- | --- |
| Destructor | A function that runs when an object goes away. | `~mtl_instance()` gives back the resources of a client. |
| `std::unique_ptr` | One owner. When the owner goes, the object goes. | The list `clients` in `main()`. |
| `std::shared_ptr` | Many owners. The object goes when the last owner goes. | Each client that uses an interface owns it. |
| `std::weak_ptr` | A reference that does not own. It can tell if the object still exists. | The global list `g_interfaces`. |

This pattern is RAII (resource acquisition is initialization): each resource
belongs to an object, and the destructor of the object releases it. RAII works
only when the destructor runs. A signal that terminates the process stops RAII
completely (section 1.15).

### 1.18 Glossary

| Term | Meaning |
| --- | --- |
| AF_XDP | A socket family that gets packets from the XDP hook, with no kernel stack. |
| busy poll | A loop that reads a queue again and again, with no sleep. |
| chain call action | An XDP action that makes the dispatcher call the next program. |
| combined channel | One RX queue and one TX queue with one interrupt. |
| dispatcher | The libxdp program that calls many XDP programs in order. |
| eBPF | The virtual machine in the kernel that runs XDP programs. |
| fd | File descriptor. |
| flow rule | A NIC rule that sends matching packets to one queue. |
| ifindex | The number that Linux gives to an interface. `ip link` shows it. |
| lcore | A logical CPU core, as DPDK numbers it. |
| libbpf | The library that loads eBPF programs and reaches eBPF maps. |
| libxdp | The library that adds the dispatcher and the AF_XDP helpers to libbpf. |
| NUMA | A computer with more than one memory area, one for each CPU socket. |
| PMD | Poll mode driver. A DPDK driver. |
| RSS | Receive side scaling. The NIC spreads packets over queues by a hash. |
| `SCM_RIGHTS` | A control message that copies a descriptor to a different process. |
| signalfd | A descriptor that gives blocked signals as data. |
| ST 2110 | The SMPTE standard for professional video, audio and data over IP. |
| UMEM | The memory that an AF_XDP socket shares with the kernel. |
| verifier | The part of the kernel that checks an eBPF program before it runs. |
| VF | Virtual function. A part of a NIC that shows as its own PCI device. |
| XDP | The hook in the driver where an eBPF program sees each received packet. |
| XSK | An AF_XDP socket. |
| XSKMAP | An eBPF map from a queue number to an AF_XDP socket. |

---
## Part 2. The purpose of the manager, without code

### 2.1 The four duties

The manager has four duties. Each function in the manager serves one or more
of them.

| Duty | Short name | What it means | Example |
| --- | --- | --- | --- |
| D1 | Arbitrate | Give each shared resource to one client only. | Process A gets lcore 2. Process B asks for lcore 2 and gets "no". |
| D2 | Act with privilege | Do the root operations for clients that are not root. | The manager adds the flow rule for process B. |
| D3 | Clean up | Take back all resources of a client that stops, also after a crash. | Process C crashes. The manager deletes its flow rule and frees its queue. |
| D4 | Share kernel objects | Give each client access to the one XDP chain of an interface. | The manager sends the `xsks_map` descriptor to process B. |

The manager does **not** move packets. After the setup, the packets go from the
NIC to the client with no step through the manager. If the manager stops, the
sessions that already run continue to receive packets. Only new requests and
the cleanup stop.

### 2.2 The resources that the manager arbitrates

| Resource | Range | Scope | Table in the manager |
| --- | --- | --- | --- |
| lcore | 0 to 127 | The host | `std::bitset<128>` in `mtl_lcore` |
| queue | 1 to `combined_count - 1` | One interface | `std::vector<bool> queues` in `mtl_interface` |
| flow rule location | 1 to `rule_size - 1` | One interface | None. The NIC holds the table. The manager reads it each time. |
| UDP port in the filter | 0 to 65535, at most 256 at a time | One interface | `udp4_dp_refcnt` and the map `udp4_dp_filter` |

For each client, `mtl_instance` records what the client has:

| Record | Contents |
| --- | --- |
| `lcore_ids` | The lcores of the client. |
| `interfaces` | A `shared_ptr` to each interface that the client uses. |
| `if_queue_ids` | For each ifindex, the queues of the client. |
| `if_flow_ids` | For each ifindex, the flow rule locations of the client. |

The filter ports have no record for each client. Part 4 shows the result of
this gap.

### 2.3 The rules that must always be true

A correct manager keeps these rules true at all times. In this document, a
rule of this type is an **invariant**. The table shows which invariants the
current code enforces.

| ID | Invariant | Enforced now | The gap |
| --- | --- | --- | --- |
| I1 | An lcore has at most one owner. | Partly | `PUT_LCORE` does not check the owner. A client can free the lcore of a different client. |
| I2 | A queue has at most one owner. | Partly | `PUT_QUEUE` does not check the owner. |
| I3 | Queue 0 never has an owner. | No | `PUT_QUEUE` for queue 0 frees queue 0, and the next `GET_QUEUE` gives it out. |
| I4 | Only the client that added a flow rule can delete it. | No | `DEL_FLOW` deletes any location. |
| I5 | A port is in the filter while one or more clients use it. | Partly | A remove with no add makes the count too low. The port then leaves the map while a client still uses it. |
| I6 | An interface has the MTL XDP chain while one or more clients use it. | Yes | The `shared_ptr` count controls it. |
| I7 | When a client disconnects, all its resources return. | Partly | Filter ports do not return. A signal on the manager stops all cleanup. |
| I8 | The manager sends exactly one reply to each request. | No | Five cases send no reply. The client then waits with no end. |
| I9 | No input from a client can stop the manager or damage its memory. | No | A large `num_if` reads past the buffer. A closed client socket can give `SIGPIPE`. |
| I10 | A client can change only the interfaces of its own ports. | No | Any ifindex makes an interface. |

### 2.4 The trust model

The current code trusts each client. It expects that each client is the MTL
library, and that the library sends correct requests. This expectation is the
reason for most of the gaps in section 2.3.

The socket file lets each local user connect (section 3.1). Thus the trust
model and the access rule do not agree. The manager trusts each client, but
each local user can be a client. A correct design selects one of these two
changes:

1. Limit the access, so that only trusted users can connect.
2. Check each request, so that a bad client can damage only its own work.

PR 1741 does both. It limits the socket mode, and it adds checks for the owner
and for the input.

---

## Part 3. Why the code is the way it is

Each section has four parts. **Question** is the design decision. **Answer**
is the reason. **Cost** is what the decision breaks or risks. **Alternative**
is a different design.

### 3.1 Why does the manager set the socket file mode to 0777 with `fs::permissions()`?

This is the code, at `mtl_manager.cpp:95`:

```cpp
  ret = bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
  ...
  /* Allow all users to connect (which might be insecure) */
  fs::permissions(MTL_MANAGER_SOCK_PATH, fs::perms::all, fs::perm_options::replace);
```

**Question.** Why does the manager change the mode of the socket file after
`bind()`? Why is the mode `0777`?

**Answer.** The reason is the privilege separation of section 1.2. Follow the
steps:

1. The manager runs as root. The umask of root is usually `022`.
2. `bind()` makes the socket file with mode `0777 & ~022`, which is `0755`
   (`srwxr-xr-x`). The owner is root.
3. On Linux, `connect()` to a Unix socket needs write permission on the file.
4. With mode `0755`, only root has write permission. Thus only root can
   connect.
5. MTL wants the application to run as a normal user with `CAP_NET_RAW`. That
   user has no write permission, and its `connect()` fails with `EACCES`.
6. The library then thinks that no manager runs. It continues in single
   instance mode (section 3.34). A native AF_XDP port then fails, because it
   needs the manager.

The call `fs::permissions(path, fs::perms::all, fs::perm_options::replace)`
does the same as `chmod(path, 0777)`:

| Part | Meaning |
| --- | --- |
| `fs::perms::all` | The value `0777`: read, write and execute for owner, group and others. |
| `fs::perm_options::replace` | Set the mode to this value. Do not add to the old mode, and do not remove from it. |

After this call, each local user has write permission, and each user can
connect.

**Why `fs::permissions()` and not `chmod()`?** Both give the same result. The
code uses C++17 `std::filesystem` for the directory too (`fs::exists`,
`fs::create_directory`). Thus `fs::permissions` keeps one style in `main()`.
There is one difference. `fs::permissions()` throws the exception
`fs::filesystem_error` when it fails, and `chmod()` returns `-1`. The manager
does not catch the exception. Thus a failure of this call stops the manager
with `std::terminate()`. This failure is not probable, because root owns the
file.

**Why `0777` and not `0666`?** For a socket file, only the write bit has an
effect (section 1.13). The read and execute bits do nothing. `perms::all` is
the name that is easy to find, and it gives the necessary write bit. `0666`
gives the same access.

**Why after `bind()` and not before?** The socket file does not exist before
`bind()`. A different method is to set the umask to `0` before `bind()`. The
code does not do this. Thus there is a short time between `bind()` and
`fs::permissions()`. In this time, only root can connect. This does not cause
a problem, because no client can connect before `listen()` in any case.

**Why is it necessary in Docker?** The `Dockerfile` of the manager runs it as
the user `imtl` (uid 1001), not as root. The container shares
`/var/run/imtl` with the host (`-v /var/run/imtl:/var/run/imtl`). The
application containers can run with a different uid. Without `0777`, only uid
1001 could connect.

**The directory.** `fs::create_directory()` makes `/var/run/imtl` with mode
`0777 & ~umask`, which is `0755`. Each user has the execute bit on the
directory. Thus each user can reach the socket file in it. The manager does
not change the mode of the directory.

**Cost.** Each local user can connect and send requests. The original commit
(`5a564cb8`) knows this: its comment says "which might be insecure". The
manager does not check who the client is (section 2.4). Thus each local user
can do these things:

| Action of a bad local user | Result |
| --- | --- |
| Send `GET_LCORE` for lcores 0 to 127. | No MTL process can get an lcore. |
| Send `GET_QUEUE` until the reply is `-1`. | No MTL process can get a queue. |
| Send `DEL_FLOW` for locations 1 to 2047. | The flow rules of each client go away. The video streams stop. |
| Send `DEL_UDP_DP_FILTER` for a port. | The filter drops the port. Its packets go to the kernel, not to MTL. |
| Send `GET_QUEUE` with the ifindex of a different interface. | The manager clears all flow rules of that interface and attaches XDP to it. |
| Send `IF_XSK_MAP_FD`, then delete the map entries. | The AF_XDP sockets of other clients get no packets. |
| Connect, send a request, and close the socket before the reply. | The `send()` of the reply gives `SIGPIPE`, and the manager stops. |

Some of these actions need no bad intent. A test program with a defect can do
them too.

**Alternative.** Use a smaller mode and a group. For example:

1. Make a group `mtl`, and add the MTL users to it.
2. Set the socket file to owner root, group `mtl`, mode `0660`.
3. Only root and the members of `mtl` can connect.

PR 1741 uses `0666` for the system path, `0600` for a socket in a user
directory, and `0660` with the option `--socket-group`. The manager can also
read the uid of the peer with `getsockopt(SO_PEERCRED)`, and refuse or limit
each request by uid.

### 3.2 Why is the manager a separate process?

**Answer.** Three reasons:

1. **Privilege.** The application does not need root (section 1.2). Only the
   small manager needs it.
2. **Shared state.** Many processes need one table of lcores and queues. A
   separate process is one owner for the table.
3. **Cleanup.** A process that crashes cannot clean up after itself. A
   different process must do it. The manager sees the crash as a closed
   socket.

**Cost.** A daemon is one more part to install, start and monitor. If it is
not there, AF_XDP does not work.

**Alternative.** The older method is System V shared memory for the lcores
(`doc/shm_lcore.md`). It needs no daemon. But it needs a PID check to find
dead processes, and a person must run `LcoreMgr` to clean up. It also fails
between containers that do not share the IPC namespace. It cannot do D2 or
D4.

### 3.3 Why a Unix domain socket?

**Answer.**

| Property | Why the manager needs it |
| --- | --- |
| Local only | No network user can reach the manager. |
| File permissions | The file mode controls who can connect (section 3.1). |
| `SCM_RIGHTS` | Only a Unix socket can send a descriptor (section 1.11). |
| Close detection | The kernel closes the socket when the client stops. `recv()` returns `0`. This is duty D3. |
| Peer identity | `SO_PEERCRED` can give the pid, uid and gid of the peer. The code does not use it now. |
| Container support | A bind mount of the directory shares the socket between containers. |

**Alternative.** TCP on `127.0.0.1` cannot send a descriptor, and each local
user can connect. D-Bus is large and adds a dependency. Shared memory has no
close detection.

### 3.4 Why `SOCK_STREAM`, and what is the cost?

**Answer.** `SOCK_STREAM` is the most usual socket type, and each example
uses it. It gives a connection, order, and close detection.

**Cost.** A stream has no record boundaries (section 1.12). The manager
expects exactly one record for each `recv()`. This is true in practice,
because each client sends one request and then waits for the reply. But the
manager has no framing:

- If one `recv()` gets less than one record, the manager discards the bytes.
- If one `recv()` gets more than one record, the manager handles only the
  first.
- The rest of a split record is then the start of the next "record", with a
  wrong magic.

The library has the same problem when it reads a reply.

**Alternative.** `SOCK_SEQPACKET` keeps each `send()` as one record, and it
also gives a connection and close detection. It is a small change on both
sides. The other alternative is framing: read into a buffer, and take a record
only when all its bytes are there. PR 1741 adds framing with a `feed()`
function.

### 3.5 Why one fixed record size for each message?

**Answer.** `mtl_message_t` is a header and a `union` of all bodies. The size
is always the size of the largest body plus the header: 118 bytes. Each side
sends and reads the same size. This is simple, and each read uses one buffer
with no allocation.

**Cost.** The record has no version number. `body_len` is in the header, but
the manager never reads it. A small message, for example `GET_LCORE` with 2
bytes of data, still uses 118 bytes. This is not important for a control
channel.

**Alternative.** A variable size with `body_len` and a version field. PR 1741
adds a protocol version.

### 3.6 Why network byte order on a local socket?

**Answer.** Both sides are on the same host. Thus both sides have the same
byte order, and no conversion is necessary. The code uses `htonl()` and
`ntohl()` from habit, and in case the protocol goes to a network in the
future.

**Cost.** The conversion must be correct on both sides. In the library,
`REGISTER` (`mt_instance.c:211`) and `PUT_LCORE` (`mt_instance.c:41`) set
`body_len` with no `htonl()`. This causes no fault, because the manager does
not read `body_len`. It becomes a fault if a future manager reads it.

The IP addresses of `ADD_FLOW` go through two conversions. The library does
`htonl()` and the manager does `ntohl()`. Thus the address arrives in the
order that it had in the library, which is network order. The manager then
puts it in `ip4dst` with no change, and `ip4dst` needs network order. The
result is correct.

### 3.7 Why one thread?

**Answer.** One thread handles each event in order. Two requests can never
change the same table at the same time. Thus the tables need no lock. The
code is small and easy to read.

`mtl_lcore` has a `std::mutex` all the same. With one thread, the mutex
has no effect. It is a guard in case of a future second thread. It costs very
little.

**Cost.** Each request waits for the one before it. Most requests are fast.
Some are slow:

| Slow step | Why | Result |
| --- | --- | --- |
| The first request for an interface | `load_xdp()` loads and attaches the program. Some drivers reset the link. | All clients wait, possibly for more than one second. |
| An ethtool ioctl | The driver can sleep. | All clients wait. |
| `send()` to a client that does not read | The socket buffer is full. `send()` blocks. | The manager stops for all clients. |

**Alternative.** A thread for each client, with locks. Or one thread with a
non-blocking `send()` and a send timeout. PR 1741 sets `SO_SNDTIMEO` to
2000 ms.

### 3.8 Why epoll?

**Answer.** The manager waits for many descriptors at the same time (section
1.14). `epoll` gives this in one call. `select()` has a limit of 1024
descriptors, and `poll()` needs the full list for each call. For 10 clients,
all three work. `epoll` is the modern Linux choice.

The loop makes a new `events` vector for each wait, with `clients.size() + 2`
entries. The `+ 2` is for the listen socket and the signalfd. This allocation
is small.

### 3.9 Why a signalfd for SIGINT?

**Answer.** A normal signal handler can run in the middle of any code (section
1.15). The cleanup of the manager runs C++ destructors, which free memory and
call ioctls. These calls are not safe in a handler. The signalfd lets the loop
read `SIGINT` as a normal event. The loop then sets `is_running = false`,
leaves the `while`, and the destructors run in the normal way.

### 3.10 Why must the manager block SIGINT before it makes the signalfd?

**Answer.** A signalfd receives only a signal that is blocked. If the signal
is not blocked, the kernel does the default action first. For `SIGINT`, that
action terminates the process, and the signalfd never sees it.

The block also has an effect on child processes: a child gets the blocked
mask. The manager makes no child, so this has no effect.

### 3.11 Why does the manager handle only SIGINT?

**Answer.** The first use of the manager was in a terminal, where the operator
stops it with Ctrl+C. Ctrl+C sends `SIGINT`. The README also tells Docker users
to send `SIGINT`: `docker kill -s SIGINT mtl-manager`.

**Cost.** The usual tools send `SIGTERM`, not `SIGINT`:

| Tool | Signal | Result in the manager |
| --- | --- | --- |
| `kill <pid>`, `pkill`, `killall` | `SIGTERM` | The manager stops with no cleanup. |
| `systemctl stop` | `SIGTERM` | The manager stops with no cleanup. |
| `docker stop` | `SIGTERM`, then `SIGKILL` after 10 s | The manager stops with no cleanup. |
| Close of the terminal | `SIGHUP` | The manager stops with no cleanup. |
| A reply to a closed client | `SIGPIPE` | The manager stops with no cleanup. |

"No cleanup" means that the XDP chain stays on each interface, and the flow
rules stay in the NIC. The next manager clears the flow rules when it makes
the interface again (section 3.20).

**Alternative.** Add `SIGTERM`, `SIGHUP` and `SIGQUIT` to the same mask, and
set `SIGPIPE` to `SIG_IGN`, or send with `MSG_NOSIGNAL`. PR 1741 adds
`SIGTERM` and ignores `SIGPIPE`.

### 3.12 Why does the manager call `unlink()` before `bind()`?

**Answer.** `bind()` fails with `EADDRINUSE` if a file is at the path. A
manager that stopped with no cleanup leaves the old socket file. The
`unlink()` removes it, so that the new `bind()` works.

**Cost.** The manager does not check if a different manager is still alive.
If you start a second manager, it removes the socket file of the first.
The first manager continues to run, and it keeps its clients. New clients
connect to the second manager. Each manager has its own tables, so both
managers can give the same lcore to two clients.

**Alternative.** Before `unlink()`, try `connect()` to the path. If the
connect works, a manager is alive, and the new manager must stop. PR 1741 does
this and returns `-EADDRINUSE`.

### 3.13 Why `/var/run/imtl/`?

**Answer.**

- `/var/run` is a link to `/run`, which is a `tmpfs`. A reboot clears it.
  Thus a stale socket file does not stay after a reboot.
- `/var/run` is the standard place for the sockets of system daemons.
- A subdirectory lets a container share only this directory with a bind
  mount.

**Cost.** Only root can write in `/var/run`. A manager that is not root cannot
make the directory, and it stops with `-EIO`. The path is fixed at build time.
You cannot change it for a test.

### 3.14 Why does the manager not remove the socket file at exit?

**Answer.** There is no reason in the code. It is an omission.

**Cost.** After the manager stops, the file stays. A `connect()` to it gives
`ECONNREFUSED`. The library then goes to single instance mode, which is the
correct result. Thus the omission causes almost no fault. The next manager
removes the file with `unlink()` (section 3.12).

### 3.15 What does `MAX_CLIENTS = 10` limit?

**Answer.** It is the backlog of `listen()`. The backlog is the number of new
connections that can wait for `accept()`. It does **not** limit the number of
clients. The manager accepts each connection with no limit. A client that
opens many connections can use all descriptors of the manager.

### 3.16 Why is the receive buffer 256 bytes?

**Answer.** One record is 118 bytes. 256 bytes is more than one record, with
space left. There is no special meaning.

**Cost.** One `recv()` can get two records (236 bytes). The manager handles
only the first. The client of the second record then waits for a reply with
no end.

### 3.17 Why does the manager reserve queue 0?

**Answer.** The kernel uses queue 0 for its default traffic. RSS sends many
packets, for example SSH and ARP, to all queues, and queue 0 is always one of
them. Many drivers also use queue 0 for special traffic. If an AF_XDP socket
takes queue 0, the XDP chain still passes non-MTL packets to the kernel
(section 1.10). But the reserve is a simple extra guard, and it keeps one
queue always free for the kernel.

**Cost.** One queue less for MTL. The code sets `queues[0] = true` at the
start, but `put_queue(0)` can clear it (Part 4, section 4.20).

### 3.18 Why does `get_queue()` give the lowest free queue?

**Answer.** It is the simplest search. The result is predictable. The first
client gets queue 1, the next gets queue 2, and so on.

**Cost.** A freed queue is given again at once. If an old flow rule still
points to it, the new owner gets packets of the old stream.

### 3.19 Why does `add_flow()` search for a free location from the end?

**Answer.** On many NICs, a lower location has a higher priority. An admin
usually adds rules at low locations. The manager starts at the last location,
`rule_size - 1`, which has the lowest priority. Thus a rule of the admin wins
over a rule of MTL. The code comment says "start from lowest priority".

**Why is location 0 never used?** The loop stops at `free_loc > 0`, and a
result of `0` is an error. The reason is the reply: the handler records a
flow only if the result is greater than `0` (`if (ret > 0)`). A flow at
location 0 would look like "success with no flow". Thus location 0 cannot
be recorded, and the code never uses it.

**Cost.** The search reads the full rule table for each new flow. With 1000
rules this is fast enough. There is also an edge case: if the driver reports a
table size of `0`, `free_loc` becomes `-1`. Part 4, section 4.22, explains it.

### 3.20 Why does the manager delete all flow rules when it makes an interface?

**Answer.** A previous manager or a previous client can stop with no cleanup
(section 3.11). Its flow rules stay in the NIC and send packets to queues that
nobody reads. The manager cannot know which rules are old. Thus it deletes all
rules at the start. It deletes them again when the last client leaves the
interface.

**Cost.**

1. The manager also deletes the rules that an admin added by hand, or that a
   different program added. It gives no warning before it does this.
2. The loop has a defect. It uses the variable `cmd` for the count and also
   for each delete. The `memset(&cmd, 0, ...)` in the loop sets
   `cmd.rule_cnt` to `0`. The loop test `i < cmd.rule_cnt` then fails after
   the first delete. Thus the function deletes only the first rule.

Example of the defect: the NIC has rules at locations 1021, 1022 and 1023. The
loop deletes 1021, and `cmd.rule_cnt` becomes `0`. The loop stops. Rules 1022
and 1023 stay.

**Alternative.** Record the locations that the manager added in a file under
`/run`, and delete only those. Or give MTL a fixed range of locations. PR 1741
fixes the loop with a separate variable.

### 3.21 Why does `g_interfaces` hold a `weak_ptr`?

**Answer.** `g_interfaces` is the global list of interfaces. Each client holds
a `shared_ptr` to each interface that it uses. The global list holds a
`weak_ptr`, which does not count as an owner. Thus:

1. Client A makes interface 5. The count of owners is 1.
2. Client B uses interface 5. The count is 2.
3. Client A disconnects. The count is 1. The interface stays.
4. Client B disconnects. The count is 0. The destructor runs. It removes the
   XDP chain and the flow rules.

If the global list held a `shared_ptr`, the count would never reach 0, and
the XDP chain would stay until the manager stops.

**Cost.** The expired `weak_ptr` stays in the map after the interface goes. It
uses a small amount of memory. `get_interface()` sees that `lock()` fails, and
it makes a new interface.

### 3.22 Why does the manager load XDP at `REGISTER`?

**Answer.** The library creates its AF_XDP socket with
`XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD` (section 3.28). With this flag, libxdp
does not load a program. Thus the XDP chain must exist before the socket.
`REGISTER` is the first request of the client. It lists the AF_XDP
interfaces, and the manager makes each interface at once. Thus the chain is
ready before the library makes a socket.

**Cost.** A failure on one interface refuses the full registration. Then the
library goes on in single instance mode. The interfaces that the manager made
before the failure stay in the list of the client until it disconnects.

### 3.23 Why does `mtl_dp_filter` return `XDP_DROP` for an MTL packet?

**Answer.** The dispatcher looks only at the return value of each program
(section 1.9). The filter needs two different results:

| Result | Meaning | Action code |
| --- | --- | --- |
| "Not for MTL" | Give the packet to the kernel, and stop the chain. | `XDP_PASS` |
| "For MTL" | Continue to `xsk_def_prog`. | Some other code |

`XDP_PASS` must mean "to the kernel", because the final action of the chain is
also the action for the packet. Thus "continue" needs a different code. The
code uses `XDP_DROP`, and it sets `XDP_DROP` as a chain call action in
`XDP_RUN_CONFIG`.

**Cost.** The meaning depends on the dispatcher. If a different loader
attaches `mtl_dp_filter` alone, with no dispatcher, the kernel really drops
each MTL packet. The code comment says `/* go to next program: xsk_def_prog */`
to help the reader.

### 3.24 Why does the filter have a count for each port?

**Answer.** Two clients can receive on the same UDP port, for example two
receivers of one multicast stream on different queues. Both add the port. The
first add puts the port in the map. When the first client removes the port,
the second still needs it. The count keeps the port in the map until the last
remove.

**Cost.** The count belongs to the interface, not to the client. The manager
does not record which client added which port. Thus:

- When a client stops, its ports stay in the count. The port stays in the map
  until the interface goes.
- A remove with no add makes the count `-1`. The code writes `0` to the map
  and returns success. The count is now one less than the real number of
  users. Example: a stray remove (`-1`), add by A (`0`), add by B (`1`), remove
  by A (`0`). The last step removes the port, but B still uses it.
- A failed map update does not undo the count. Example: the 257th port fails
  with `E2BIG`, but its count is `1`. The next add of that port gets the count
  `2` and returns success, and the port is still not in the map.

### 3.25 Why is `udp4_dp_filter` a hash map?

**Answer.** Commit `002b38ec` changed it from an array to a hash. An array
of 65536 ports uses locked memory for each entry, and the map update failed
with `-7` (`E2BIG`) on some hosts. A hash map with 256 entries uses memory only
for the ports in it.

**Cost.** At most 256 ports on one interface. A hash lookup is a little slower
than an array lookup, but this is not important for XDP.

### 3.26 Why does the manager write the value `0` to remove a port?

**Answer.** The code calls `bpf_map_update_elem()` with the value `0`. It does
not call `bpf_map_delete_elem()`. `lookup_udp4_dp()` treats a value of `0` the
same as a missing key. Thus the result is correct.

**Cost.** The entry stays in the map. After 256 different ports, the map is
full, and the next new port fails with `E2BIG`, also when all old values are
`0`.

### 3.27 Why does the manager send the map with `SCM_RIGHTS`?

**Answer.** The client must put its AF_XDP socket in `xsks_map`. It needs a
descriptor of the map for this. `SCM_RIGHTS` is the only way to give a
descriptor to a different process with no shared file system object (section
1.11). Commit `e02ec974` moved this function from `tools/ebpf` into the
manager.

**Alternative.** Pin the map in the BPF file system, for example at
`/sys/fs/bpf/mtl/ens785f0/xsks_map`. The client then opens the path. This needs
a mounted `bpffs`, correct modes on the path, and a cleanup of the pin. A
pinned map also stays after all processes stop. The descriptor method has
none of these problems.

### 3.28 Why does the library use `XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD`?

**Answer.** Without this flag, `xsk_socket__create()` loads the default XDP
program itself. That needs `CAP_NET_ADMIN` and `CAP_BPF`. The application
does not have them, so the call fails with `EPERM`. The library then prints
"please run with mtl manager or root user" (`mt_af_xdp.c:434`). With the flag,
libxdp only makes the socket, and the manager supplies the program and the
map.

### 3.29 Why does `REGISTER` send the pid, uid and hostname?

**Answer.** The manager uses them only in its log. Each log line of a client
starts with `[Instance <hostname>:<pid>]`. This helps the operator to find the
process.

**Cost.** The values come from the client, and the manager does not check
them. In a container, the pid and the hostname are the values in the
namespace of the container. `SO_PEERCRED` gives the real pid and uid from the
kernel.

A second fault: the code makes the hostname with `std::string(hostname, 64)`.
This always makes a string of 64 characters, which includes the null bytes
after the name. The log then has invisible null bytes.

### 3.30 Why is `HEARTBEAT` in the protocol but not in the code?

**Answer.** The type exists since the first commit, for a future health check.
The close detection of the socket already finds a client that stops. Thus
nobody implemented it.

**Cost.** The close detection does not find a client that is alive but stuck,
for example in a deadlock. Such a client keeps its resources. A heartbeat can
find it. The manager treats a `HEARTBEAT` now as an unknown type, and it sends
no reply.

### 3.31 Why are there three reply types?

**Answer.** `RESPONSE`, `IF_QUEUE_ID` and `IF_FLOW_ID` all carry one signed
integer. The different type lets the client check that the reply is for its
request. The library helper checks the type, and it returns `-EIO` for a
wrong type.

**Cost.** When `ADD_FLOW` fails because the interface is bad, the manager
replies with the type `RESPONSE`, not `IF_FLOW_ID`. The library then reports
`-EIO`, not `-1`. The result is still an error, which is correct.

### 3.32 Why does the library add 1 to the queue for DPDK AF_XDP?

**Answer.** The DPDK AF_XDP driver (`net_af_xdp`) gets the arguments
`start_queue=1,queue_count=N` (`mt_dev.c:383`). DPDK queue 0 is then NIC queue
1. The flow rule needs the NIC queue, so the library adds
`MT_DPDK_AF_XDP_START_QUEUE` (1) in `mt_socket_add_flow()`.

**Cost.** DPDK AF_XDP selects its queues itself. It does not ask the manager
with `GET_QUEUE`. Thus the manager does not know these queues. Two DPDK AF_XDP
processes on one interface both use queue 1. A native AF_XDP process can also
get queue 1 from the manager at the same time.

### 3.33 Why 128 lcores?

**Answer.** `std::bitset<128>` is small and fast. 128 was enough for the
servers at the time.

**Cost.** DPDK can have `RTE_MAX_LCORE` above 128, and a large server has more
than 128 logical cores. The manager refuses each lcore of 128 or more with
`-1`. The library then tries the next lcore. If all remaining lcores are 128
or more, the library cannot start a scheduler.

The manager arbitrates the DPDK lcore ID, not the Linux CPU number. Two
processes with different `--lcores` maps can have the same lcore ID on
different CPUs. They get a conflict that is not real. Or they can have
different IDs on the same CPU, and the manager does not see the real conflict.

### 3.34 Why does the library continue when it cannot connect?

**Answer.** Many users run one MTL process with DPDK on a VF. They do not need
the manager. If the connect fails, `mt_instance_init()` prints a warning,
"connect to manager fail, assume single instance mode", and continues. The
library then uses its own lcore arbitration.

**Cost.** A wrong socket mode (section 3.1) also looks like "no manager". The
user sees only a warning. A native AF_XDP port then fails later with "AF_XDP
backend must run with MTL Manager!" (`mt_af_xdp.c:731`).

### 3.35 Why does the manager not make the AF_XDP sockets itself?

**Answer.** The UMEM and the rings of an AF_XDP socket are memory in the
process that uses them. The data path of MTL reads the RX ring directly, with
no system call. If the manager made the socket, the rings would be in the
memory of the manager. The client needs only `CAP_NET_RAW` to make the socket
itself.

### 3.36 Why does `load_xdp()` try native mode first?

**Answer.** Native mode is much faster (section 1.8). Not each driver supports
it, so SKB mode is the fallback.

**Cost.** The code sets `xdp_mode = XDP_MODE_NATIVE` after the fallback too
(`mtl_interface.hpp:430`). Thus after an SKB attach, `xdp_mode` is wrong.
`unload_xdp()` then calls `xdp_program__detach()` with native mode, and the
detach can fail. The program then stays on the interface.

### 3.37 Why is all the code in header files?

**Answer.** The manager has one source file, `mtl_manager.cpp`, which
includes all `.hpp` files. Thus the program is one translation unit. The
global `g_interfaces` and the static `logger::log_level_min` are defined in
headers, which is safe with one translation unit only. It is a simple method
for a small program.

**Cost.** A unit test that includes the headers in two files gets a
"multiple definition" link error.

### 3.38 Why does the manager clear the flow rules also when it removes an interface?

**Answer.** When the last client leaves, no MTL flow must stay. The destructor
of each client already deletes the flow rules of that client. The clear in
`~mtl_interface()` is a second guard, for example for a rule that a client
added directly with ethtool.

**Cost.** The same as section 3.20: it deletes admin rules too, and the loop
defect keeps all rules but the first.

---

## Part 8. Native AF_XDP: a learning guide and a quickstart

This part teaches XDP and AF_XDP through MTL. It tells you how to run MTL on
AF_XDP, where MTL turns the AF_XDP path on, what the XDP program does, how an
ST 2110 stream goes through it, and what you get and lose in comparison with
DPDK.

Read sections 1.6 to 1.10 first. This part uses their words and does not
repeat them. The line numbers in this part are from the branch `mtl-manager`,
commit `9dc7a85b`.

| Section | Use it when |
| --- | --- |
| 8.1 | XDP is new to you, and you want an order in which to learn it. |
| 8.2 | You want to see an XDP program work on your own computer, with no NIC. |
| 8.3 | You want MTL to send and receive ST 2110-20 on AF_XDP now. |
| 8.4 | You must find the code that selects and starts the AF_XDP path. |
| 8.5 | You must know what `mtl_dp_filter` does with each packet. |
| 8.6 | You must know how the library moves packets through the UMEM. |
| 8.7 | You must know how ST 2110 works on this path. |
| 8.8 | You must select AF_XDP or DPDK for a deployment. |
| 8.9 | You want numbers for your own host. |
| 8.10 | An AF_XDP run fails. |
| 8.11 | You want the open points that were found while this part was written. |

### 8.1 A learning path for XDP

Learn the concepts in this order. Each step needs the step before it.

| Step | Concept | One sentence | Where to read |
| --- | --- | --- | --- |
| 1 | The kernel path of a packet | The driver makes an `sk_buff`, and the stack finds the socket. | 1.6 |
| 2 | eBPF | A small program that the kernel verifies and then runs in the kernel. | 1.8 |
| 3 | eBPF map | A table in the kernel that eBPF and user space both read and write. | 1.8 |
| 4 | XDP hook and actions | An eBPF program in the driver that returns `PASS`, `DROP`, `TX`, `REDIRECT` or `ABORTED` for each RX packet. | 1.8 |
| 5 | Native and SKB modes | Native mode runs before the `sk_buff`. SKB mode runs after it and is slow. | 1.8 |
| 6 | AF_XDP socket, UMEM and the four rings | A socket on one queue that gets packets into memory of the application. | 1.7 |
| 7 | `XSKMAP` and `bpf_redirect_map()` | The XDP program sends a packet to the socket of its queue. | 1.8, 1.10 |
| 8 | The libxdp dispatcher | Many XDP programs on one interface, in order of priority. | 1.9 |
| 9 | Flow rules | The NIC puts a packet in the queue of the socket. | 1.4, 1.5 |
| 10 | Zero-copy and copy modes | In zero-copy mode, the NIC writes into the UMEM directly. | 1.7, 8.6 |

Three facts make the rest easy:

1. **XDP is an RX hook only.** It does not see TX packets. An AF_XDP socket
   sends from its TX ring with no eBPF program on the path.
2. **An AF_XDP socket gets nothing by itself.** A packet reaches the socket
   only if the NIC puts it in the queue of the socket **and** an XDP program
   on that interface redirects it. MTL needs a flow rule for the first part,
   and the XDP chain for the second part.
3. **The kernel keeps the interface.** Each packet that the XDP program does
   not redirect goes to the kernel stack as usual. This is the main
   difference from DPDK.

The external references, in the order to read them:

1. Kernel documentation, AF_XDP:
   <https://www.kernel.org/doc/HTML/latest/networking/af_xdp.html>
2. The XDP tutorial of the xdp-project, lessons `basic01` to `basic04` and
   `advanced03-AF_XDP`: <https://github.com/xdp-project/xdp-tutorial>
3. libxdp and the dispatcher protocol:
   <https://github.com/xdp-project/xdp-tools/blob/main/lib/libxdp/README.org>
4. MTL's own guide: [doc/xdp.md](../../doc/xdp.md).

### 8.2 A lab: the MTL filter on a veth pair

This lab loads `mtl.xdp.o` on a virtual interface. It needs no NIC and no
MTL process. It shows two facts: the map selects the ports, and `XDP_DROP` of
`mtl_dp_filter` has two different results (section 3.23).

> These commands were not run on a host while this part was written. Do the
> lab on a test computer. The commands change only the network namespace
> `xdplab`, and step 7 removes it.

You need `xdp-loader` and `bpftool` (xdp-tools and `linux-tools`), and
`mtl.xdp.o`. The manager installs `mtl.xdp.o` into `<prefix>/lib/bpf`, for
example `/usr/local/lib/bpf/mtl.xdp.o` (`manager/meson.build:72`).

**Step 1. Make a veth pair. One end goes into a namespace.**

```bash
sudo ip netns add xdplab
sudo ip link add veth0 type veth peer name veth1
sudo ip link set veth1 netns xdplab
sudo ip addr add 10.11.0.1/24 dev veth0
sudo ip link set veth0 up
sudo ip -n xdplab addr add 10.11.0.2/24 dev veth1
sudo ip -n xdplab link set veth1 up
```

**Step 2. Start a UDP receiver on ports 5000 and 5001 in the namespace.**

```bash
sudo ip netns exec xdplab python3 - <<'PY' &
import select, socket
socks = []
for port in (5000, 5001):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    socks.append(s)
while True:
    for s in select.select(socks, [], [])[0]:
        data, _ = s.recvfrom(2048)
        print("port", s.getsockname()[1], "got", data, flush=True)
PY
```

**Step 3. Send one packet to each port. Both arrive.**

```bash
for p in 5000 5001; do
  python3 -c "import socket; socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(b'hello', ('10.11.0.2', $p))"
done
```

**Step 4. Load the filter with libxdp, and put port 5000 in the map.**

```bash
sudo ip netns exec xdplab xdp-loader load -m skb veth1 /usr/local/lib/bpf/mtl.xdp.o
sudo ip netns exec xdplab xdp-loader status veth1
sudo bpftool map show name udp4_dp_filter
```

The key is a `__u16` in host byte order. On x86, 5000 = `0x1388` is the
bytes `0x88 0x13`. The value is one byte, `1`:

```bash
sudo bpftool map update name udp4_dp_filter key 0x88 0x13 value 0x01
sudo bpftool map dump name udp4_dp_filter
```

If `bpftool` says that more than one map has the name, the manager is also
running. Then use `bpftool map update id <id> ...` with the ID from
`bpftool map show`.

**Step 5. Send the two packets again. Both arrive.**

Port 5001 is not in the map, so `mtl_dp_filter` returns `XDP_PASS`. Port 5000
is in the map, so it returns `XDP_DROP`. But libxdp loaded the program through
a dispatcher, and `XDP_DROP` is a chain call action. The dispatcher goes to
the next program. No next program exists, so the dispatcher returns
`XDP_PASS`. This is the normal MTL case, but with no `xsk_def_prog`.

**Step 6. Load the same object with no dispatcher. Port 5000 stops.**

```bash
sudo ip netns exec xdplab xdp-loader unload veth1 --all
sudo ip netns exec xdplab ip link set dev veth1 xdpgeneric obj /usr/local/lib/bpf/mtl.xdp.o sec xdp
sudo bpftool map update name udp4_dp_filter key 0x88 0x13 value 0x01
```

Send the two packets again. Only port 5001 arrives. With no dispatcher, the
kernel does what `XDP_DROP` says. This is the risk of section 3.23. If your
`ip` cannot load the object, skip this step.

**Step 7. Remove the lab.**

```bash
sudo ip netns exec xdplab ip link set dev veth1 xdpgeneric off
kill %1
sudo ip link del veth0          # also removes veth1
sudo ip netns del xdplab
```

What you learned:

| Fact | Where MTL uses it |
| --- | --- |
| A map changes the behavior of a program with no reload. | The manager adds and removes ports at runtime (3.24 to 3.26). |
| The dispatcher gives an action a second meaning. | `XDP_DROP` means "continue" in the MTL chain (1.9). |
| SKB mode works on each interface. | The manager falls back to it (3.36). |

### 8.3 Quickstart: ST 2110-20 on native AF_XDP

This quickstart sends one 1080p59 stream from one port and receives it on a
second port of the same host. The two ports connect with a cable or through a
switch. The examples use `ens785f0` (TX) and `ens785f1` (RX). Use the names of
your interfaces.

**Step 1. Check the host.**

```bash
./script/build_ebpf_xdp.sh --check
```

The check finds the missing packages and the kernel options
`CONFIG_BPF_SYSCALL`, `CONFIG_XDP_SOCKETS` and `CONFIG_BPF_JIT`. Install the
packages that it names.

**Step 2. Build and install libxdp and libbpf.**

```bash
./script/build_ebpf_xdp.sh
pkg-config --modversion libxdp libbpf   # the versions in versions.env
```

**Step 3. Build MTL and the manager.**

```bash
./build.sh
```

In the output of `meson setup` for `lib/` and for `manager/`, find these
lines. If they are not there, the build has no AF_XDP path (section 8.4).

```text
Run-time dependency libxdp found: YES 1.6.3
Run-time dependency libbpf found: YES 1.5.0
```

Then make sure that the XDP object is installed:

```bash
ls /usr/local/lib/bpf/mtl.xdp.o
```

**Step 4. Prepare the two interfaces.**

```bash
for ifc in ens785f0 ens785f1; do
  sudo nmcli dev set "$ifc" managed no                    # if NetworkManager runs
  sudo ip link set "$ifc" up
  ethtool -k "$ifc" | grep ntuple                         # must be "on"
  ethtool -l "$ifc"                                       # "Combined" queues
  echo 2      | sudo tee /sys/class/net/$ifc/napi_defer_hard_irqs
  echo 200000 | sudo tee /sys/class/net/$ifc/gro_flush_timeout
done
sudo ip addr add 192.168.96.101/24 dev ens785f0
sudo ip addr add 192.168.96.102/24 dev ens785f1
sudo sysctl -w vm.nr_hugepages=2048
```

| Item | Why |
| --- | --- |
| An IPv4 address | The library reads the address of the interface. With no address, `mtl_init()` fails with `SIOCGIFADDR fail`. |
| `ntuple` on | The manager adds an ethtool flow rule for each RX session. It does not turn `ntuple` on. `ice` has it on by default. |
| Combined queues | The manager gives queues 1 to `combined_count - 1`. Queue 0 is reserved (3.17). For more queues: `sudo ethtool -L <if> combined <n>`. |
| `napi_defer_hard_irqs`, `gro_flush_timeout` | The driver does its work in NAPI. These values make it do less interrupts, and more work in each poll. |
| Hugepages | The UMEM is a DPDK mempool, and DPDK takes its memory from hugepages (8.6). AF_XDP does not remove this need. |

If two ports on one host are on the same subnet, the kernel can answer ARP for
the two addresses from the wrong port. If the RX side gets nothing, put the
ports on different subnets, or set `arp_ignore=1` and `arp_announce=2` on the
two interfaces.

**Step 5. Start the manager.**

```bash
sudo MtlManager
```

Keep it in its own terminal. Its log tells you when it loads the XDP
program.

**Step 6. Make a test file and a configuration.**

One frame of 1080p YUV 4:2:2 10-bit is 1920 × 1080 × 2.5 = 5,184,000 bytes.

```bash
cd tests/tools/RxTxApp
dd if=/dev/urandom of=test.yuv bs=5184000 count=10
cat > xdp_quickstart.json <<'JSON'
{
    "interfaces": [
        { "name": "native_af_xdp:ens785f0" },
        { "name": "native_af_xdp:ens785f1" }
    ],
    "tx_sessions": [
        {
            "dip": [ "local:1" ],
            "interface": [ 0 ],
            "video": [
                {
                    "replicas": 1,
                    "type": "frame",
                    "pacing": "narrow",
                    "packing": "BPM",
                    "start_port": 20000,
                    "payload_type": 112,
                    "tr_offset": "default",
                    "video_format": "i1080p59",
                    "pg_format": "YUV_422_10bit",
                    "video_url": "./test.yuv"
                }
            ]
        }
    ],
    "rx_sessions": [
        {
            "ip": [ "local:0" ],
            "interface": [ 1 ],
            "video": [
                {
                    "replicas": 1,
                    "type": "frame",
                    "pacing": "narrow",
                    "start_port": 20000,
                    "payload_type": 112,
                    "tr_offset": "default",
                    "video_format": "i1080p59",
                    "pg_format": "YUV_422_10bit",
                    "display": false,
                    "measure_latency": true
                }
            ]
        }
    ]
}
JSON
```

`local:1` means "the address of interface 1". More examples are in
`tests/tools/RxTxApp/script/native_af_xdp_json/`.

**Step 7. Run.**

As root:

```bash
sudo ./build/RxTxApp --config_file xdp_quickstart.json --test_time 30
```

As a user, with the capability only (`doc/xdp.md`):

```bash
sudo setcap 'cap_net_raw+ep' ./build/RxTxApp
./build/RxTxApp --config_file xdp_quickstart.json --test_time 30
```

**Step 8. Look at what MTL made, while the test runs.**

```bash
sudo xdp-loader status ens785f1        # dispatcher, mtl_dp_filter (19), xsk_def_prog (20)
ethtool -n ens785f1                    # one rule: UDP dst port 20000 -> the session queue
sudo bpftool map dump name udp4_dp_filter   # key 20 4e (= 20000), value 01
```

In the RxTxApp log, find these lines:

| Log text | Meaning |
| --- | --- |
| `xdp_parse_drv_name(...), if:ens785f1 drv:ice` | The library found the kernel driver. |
| `xdp_umem_init(...), umem ... buffer ... size ...` | One UMEM for each queue. |
| `xdp_parse_pacing_ice(...), rl feature no` | No hardware rate limit. The TX uses TSC pacing (8.7). |
| `xsk create with zero copy fail ..., try copy mode` | Only if the driver has no zero-copy. The run continues, slower. |
| `mt_rx_xdp_get(1,<q>), ip ... port 20000` | The RX session has queue `<q>`. |
| `xdp_queue_rx_stat(1,<q>), pkts ... bytes ... burst ...` | The periodic RX counters of the queue. |
| `xdp_queue_tx_stat(0,<q>), pkts ... submit ... free ... wakeup ...` | The periodic TX counters of the queue. |
| `xdp_queue_tx_stat(0,<q>), pkts copy ...` | The number of packets that `xdp_tx()` copied into the UMEM. It is equal to the TX packet count (8.6). |

**Step 9. Stop.**

Stop RxTxApp, then the manager (Ctrl-C). When the last client of an interface
goes, the manager removes the XDP program and the flow rules (2.1, 3.38).
Then this command shows no program:

```bash
sudo xdp-loader status
```

### 8.4 Where MTL turns on the AF_XDP path

Three gates must all open: the build, the port name, and the manager.

**Gate 1: the build.** `meson` looks for `libxdp` and `libbpf`. If it finds
both, it defines `MTL_HAS_XDP_BACKEND`.

| Component | File | Result with the libraries | Result without them |
| --- | --- | --- | --- |
| Library | `lib/meson.build:27` | `mt_af_xdp.c` is built. | The stubs in `mt_af_xdp.h` return `-ENOTSUP`, with the log `no xdp support for this build`. |
| Manager | `manager/meson.build:56` | `clang -target bpf` compiles `mtl.xdp.c`, `llvm-strip` strips it, and `meson install` puts `mtl.xdp.o` into `<prefix>/lib/bpf`. | The manager has no XDP code. |

**Gate 2: the port name.** The application selects the backend with the
prefix of the port name.

| Port string | PMD type | Owner of the AF_XDP sockets |
| --- | --- | --- |
| `native_af_xdp:ens785f0` | `MTL_PMD_NATIVE_AF_XDP` | The library, `lib/src/dev/mt_af_xdp.c` |
| `dpdk_af_xdp:ens785f0` | `MTL_PMD_DPDK_AF_XDP` | The DPDK `net_af_xdp` PMD (3.32) |
| `0000:af:01.0` | `MTL_PMD_DPDK_USER` | No AF_XDP. DPDK on a VF or PF. |

`mtl_pmd_by_port_name()` (`lib/src/mt_util.c:967`) reads the prefix.

**Gate 3: the manager.** `mt_dev_xdp_init()` stops with
`AF_XDP backend must run with MTL Manager!` if no manager is connected
(`mt_af_xdp.c:730`). Only the manager has `CAP_NET_ADMIN`, which loads an XDP
program and adds a flow rule.

The start sequence of one `native_af_xdp` port, in order:

| # | Where | What happens |
| --- | --- | --- |
| 1 | `mt_main.c:270` | Check that the port string has an interface name. |
| 2 | `mt_instance.c:219` | `REGISTER` lists the ifindex of each AF_XDP port. The manager makes an `mtl_interface` for each, and `load_xdp()` attaches the chain (3.22). |
| 3 | `mt_dev.c:355` | No EAL argument for the port. DPDK does not see the interface. |
| 4 | `mt_main.c:424` | The NUMA node comes from sysfs of the kernel interface. |
| 5 | `mt_dev.c:2264` | The driver entry `native_af_xdp` (`mt_dev.c:101`) gives the flags below. |
| 6 | `mt_dev.c:2320` | Queue count: `max(tx + 1, rx)`. The `+ 1` is for the system TX queue. |
| 7 | `mt_dev.c:1359` | One mempool for each RX queue, 2048 bytes of data room, no private area. This mempool becomes the UMEM. |
| 8 | `mt_dev.c:2523` → `mt_dev_xdp_init()` (`mt_af_xdp.c:720`) | For each queue: get a queue number from the manager, make the UMEM, fill the fill ring, make the socket, put the socket in `xsks_map`. |
| 9 | `mt_af_xdp.c:792` | If the driver is `ice`, detect the rate limit (8.7). |
| 10 | `mt_queue.c:71`, `mt_queue.c:170` | Each session that gets a queue gets `mt_rx_xdp_get()` or `mt_tx_xdp_get()`. |

The flags of the `native_af_xdp` driver entry (`mt_dev.c:101`):

| Flag | Effect on the AF_XDP port |
| --- | --- |
| `MT_DRV_F_NOT_DPDK_PMD` | No `rte_eth_dev`. MTL counts the statistics in software. |
| `MT_DRV_F_NO_CNI` | No MTL control-plane RX thread (`mt_cni.c:490`). The kernel gets ARP, IGMP and PTP. |
| `MT_DRV_F_USE_KERNEL_CTL` | ARP from the kernel neighbor table (`mt_arp.c:364`). Flow rules through the manager (`mt_flow.c:245`). |
| `MT_DRV_F_RX_POOL_COMMON` | The RX mbufs have no private area, so the mempool object fits a UMEM frame. |
| `MT_DRV_F_MCAST_IN_DP` | A multicast join is a kernel socket option in the session (`mt_af_xdp.c:993`). |
| `MT_DRV_F_KERNEL_BASED` | The kernel owns the interface. |

### 8.5 What the XDP program does

The program is `manager/mtl.xdp.c`, 64 lines. It has one map and one function.

```c
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 256); /* max 256 filters */
  __type(key, __u16);       /* udp port: 16bit */
  __type(value, __u8);      /* only 1 or 0 */
} udp4_dp_filter SEC(".maps");
```

The map holds at most 256 UDP ports for each interface. The key is in host
byte order. The manager writes `1` to add a port and `0` to remove it (3.26).

The function, step by step:

| Line | Test | If the test fails |
| --- | --- | --- |
| 46 | `parse_ethhdr()`: is the EtherType IPv4? The xdp-tools helper skips VLAN tags. | `XDP_PASS`. ARP, IPv6, LLDP and PTP over Ethernet go to the kernel. |
| 51 | `parse_iphdr()`: is the protocol UDP? | `XDP_PASS`. TCP (SSH) and ICMP go to the kernel. |
| 55 | `parse_udphdr()`: is the UDP header complete? | `XDP_PASS`. |
| 60 | Is the destination port in `udp4_dp_filter` with a value that is not 0? | `XDP_PASS`. PTP over UDP (ports 319, 320) goes to the kernel. |
| 63 | All tests pass. | Return `XDP_DROP`, which means "continue to `xsk_def_prog`" (1.9). |

Then `xsk_def_prog` of libxdp calls
`bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS)`. If the queue of
the packet has a socket, the packet goes to that socket. If not, it goes to
the kernel.

Four results of this design:

1. **The program never discards a packet.** Each packet that MTL does not
   want goes to the kernel.
2. **The program selects by port, not by queue.** The flow rule in the NIC
   selects the queue. The program only stops kernel packets that land on an
   MTL queue, for example by RSS (1.10).
3. **The filter knows only the destination port.** Two sessions on the same
   port and different addresses pass the filter both. The flow rules put them
   in their queues.
4. **The cost for a kernel packet is small.** For each packet, the dispatcher
   calls a program that parses three headers and does one hash lookup.

### 8.6 The library side: UMEM, RX and TX

**The UMEM is a DPDK mempool.** `xdp_umem_init()` (`mt_af_xdp.c:294`) gives
the memory of the RX mempool of the queue to `xsk_umem__create()`:

| UMEM setting | Value | Why |
| --- | --- | --- |
| Memory | The mempool memory, aligned down to the page | The kernel pins pages. |
| `frame_size` | The size of one mempool object | One UMEM frame = one `rte_mbuf`. |
| `frame_headroom` | Mempool header + `struct rte_mbuf` + private area + `RTE_PKTMBUF_HEADROOM` | The packet data lands where the mbuf expects its data. |
| `flags` | `XDP_UMEM_UNALIGNED_CHUNK_FLAG` | Mempool objects are not on power-of-two boundaries. |
| Fill ring | 2 × 2048 | |
| Completion ring | 2048 | |

Thus a received frame **is** an `rte_mbuf`. The rest of MTL uses it with no
change and no copy. The same trick lets DPDK APIs serve the AF_XDP path, which
is why `doc/build.md` says that DPDK is necessary for this backend too.

**The RX path, `xdp_rx()` (`mt_af_xdp.c:652`).** The session tasklet polls.
There is no system call.

```text
NIC (zero-copy) writes the packet into a UMEM frame from the fill ring
  → XDP chain redirects it to the socket of the queue
  → descriptor in the RX ring
xdp_rx():
  1. xsk_ring_cons__peek()        how many descriptors are ready
  2. rte_pktmbuf_alloc_bulk()     the same number of new mbufs, for the refill
  3. for each descriptor: address → rte_mbuf pointer, set data_off and length
  4. xdp_rx_check_pkt()           only if the port was not in the filter map
  5. xsk_ring_cons__release()
  6. xdp_rx_prod_reserve()        put the new mbufs in the fill ring
  → the mbufs go to the RX session, as from a DPDK queue
```

**The TX path, `xdp_tx()` (`mt_af_xdp.c:536`).** The library **copies** each
packet:

```text
xdp_tx(burst of mbufs from the session):
  1. xdp_tx_check_free()     free the mbufs that the completion ring returns
  2. if the TX ring is full: return 0, the session tries again later
  3. for each packet:
       rte_pktmbuf_alloc()    one new mbuf from the UMEM mempool
       xsk_ring_prod__reserve()
       mt_memcpy() each segment of the session mbuf into the UMEM frame
       rte_pktmbuf_free()     the session mbuf
  4. xsk_ring_prod__submit()
  5. xdp_tx_wakeup()          send() only if the kernel asks for a wakeup
```

The copy is necessary because the session builds its packets in the TX
mempool and in the frame buffer, and the NIC can read only the UMEM. For
1080p59 the copy is about 2.6 Gbit/s of data for each stream.

**Zero-copy and copy modes.** `xdp_socket_init()` (`mt_af_xdp.c:406`) tries
`XDP_ZEROCOPY` first, then copy mode. `MTL_FLAG_AF_XDP_ZC_DISABLE`
(`--afxdp_zc_disable` in RxTxApp) starts in copy mode.

| | Zero-copy mode | Copy mode |
| --- | --- | --- |
| RX | The NIC writes into the UMEM. | The driver writes into its own buffer, and the kernel copies into the UMEM. |
| TX | The NIC reads the UMEM. The library still does its own copy (above). | The kernel copies from the UMEM into an `sk_buff`. |
| Driver support | Needs `ndo_xsk_wakeup`. `ice` has it. | Each driver. |

### 8.7 How ST 2110 works on AF_XDP

The session code does not change. A video, audio or ancillary session calls
`mt_rxq_get()` and `mt_txq_get()`, and gets a burst function. With
`native_af_xdp`, the burst functions are `rx_xdp_burst` and `tx_xdp_burst`.
The rest of the session is the same as on DPDK: the slot reassembly and the
bitmap on RX, and the builder tasklet, the ring and the transmitter tasklet on
TX.

#### 8.7.1 RX of one ST 2110-20 stream

```text
mt_rx_xdp_get()                                     mt_af_xdp.c:918
  1. take a free queue of this port              (a queue from the manager)
  2. mt_rx_flow_create()                         → mt_socket_add_flow()
       → manager: ethtool ntuple "udp4 [src ip] [dst ip] dst-port P → queue q"
  3. mt_instance_update_udp_dp_filter(P, add)    → manager: udp4_dp_filter[P] = 1
       success → skip_all_check: the library does not check each packet again
  4. multicast address? mt_socket_get_multicast_fd()
       → kernel socket with IP_ADD_MEMBERSHIP
       → the kernel sends the IGMP report and adds the multicast MAC to the NIC
```

After this, each packet of the stream goes: NIC flow rule → queue `q` →
`mtl_dp_filter` → `xsk_def_prog` → the socket of queue `q` → `xdp_rx()` → the
RX video session. The session copies the payload into the frame buffer, as on
DPDK.

`mt_rx_xdp_put()` does the steps in reverse order. When the session closes
the multicast socket, the kernel leaves the group.

For a multicast stream, you can need
`sudo sysctl -w net.ipv4.conf.all.rp_filter=0`
(`doc/experimental/af_xdp.md`).

#### 8.7.2 TX of one ST 2110-20 stream and its pacing

`mt_tx_xdp_get()` (`mt_af_xdp.c:814`) takes a free TX queue. No flow rule and
no filter are necessary on TX. The source MAC is the MAC of the kernel
interface. The destination MAC comes from the kernel neighbor table.

ST 2110-21 says when each packet must be on the wire. MTL has two pacing
methods on this path:

| Pacing | How it works on AF_XDP | When MTL uses it |
| --- | --- | --- |
| TSC (software) | The transmitter tasklet reads the TSC, and puts each packet (or each small bulk) in the TX ring at its time. | Always, if the rate limit is not available. Also with a shared TX queue (`mt_dev.c:1461`). |
| RL (hardware rate limit) | `mt_tx_xdp_get()` writes the session rate to `/sys/class/net/<if>/queues/tx-<q>/tx_maxrate`, with bit 31 set to say "kbit/s" (`mt_af_xdp.c:868`). The NIC shapes the XDP TX ring of the queue. | Only if `xdp_parse_pacing_ice()` succeeds (`mt_af_xdp.c:277`). |

The rate limit needs the patch
`patches/ice_drv/1.12.7/xdp/0001-xdp-add-set-rate_kbps-support-for-the-tx_maxrate-sys.patch`.
The patch adds bit 31 to the `ice` `tx_maxrate` handler, and applies the rate
to the XDP ring of the queue. A stock `ice` refuses the large value, so the
detection fails and the log says `rl feature no`. `script/build_drivers.sh`
applies only `patches/ice_drv/<version>/*.patch`, and `versions.env` pins
`ICE_VER=2.6.7`. Thus a default build has **TSC pacing only** on AF_XDP.

With TSC pacing, MTL controls when a packet goes into the TX ring. It does
not control when the NIC sends it. The driver takes packets from the ring in
its NAPI poll. This adds a variable delay between the ring and the wire. A
narrow (type N) sender is the most sensitive to this delay. Measure the
compliance of your host (8.9) before you depend on narrow pacing.

There is no IPv4 checksum offload on this path, because the port has no
`rte_eth_dev_info`. The session calculates the IPv4 header checksum in
software.

#### 8.7.3 The control plane: ARP, IGMP and PTP

The kernel keeps the interface, so the kernel does the control plane:

| Function | DPDK PMD | Native AF_XDP |
| --- | --- | --- |
| ARP | MTL CNI answers and asks. | The kernel. MTL reads the neighbor table. |
| IGMP | MTL sends the reports. | The kernel, for the socket of the session. |
| PTP | MTL built-in PTP through the CNI, with NIC timestamps. | No MTL PTP: the CNI is off. MTL reads `CLOCK_REALTIME` (`ptp_from_real_time()`, `mt_dev.c:2289`). |
| SSH and other traffic | Not on a VF that DPDK owns. | Yes, on the same port. |

For a correct ST 2110 time on AF_XDP, synchronize `CLOCK_REALTIME` to the PTP
grandmaster with linuxptp on the same interface:

```bash
sudo ptp4l -i ens785f0 -m                         # NIC clock from the network
sudo phc2sys -s ens785f0 -c CLOCK_REALTIME -O 0 -m  # system clock from the NIC clock
```

PTP packets do not match `udp4_dp_filter` (ports 319 and 320, or Ethernet
type `0x88F7`), so `ptp4l` gets them from the kernel.

#### 8.7.4 ST 2022-7 and the other media types

* **ST 2022-7.** Give two ports, for example `native_af_xdp:ens785f0` and
  `native_af_xdp:ens785f1`. Each port has its own XDP chain and its own
  queues. The session merges the two streams, as on DPDK. If the two ports
  get different pacing, the session uses TSC on both
  (`st_tx_video_session.c:545`).
* **ST 2110-30, -40, -41.** They use the same queue path. They send 1 to 8
  packets for each frame and pace once for each frame, so the variable TX
  delay is less important for them.
* **ST 2110-22.** The same path. `pacing->vrx` is 0 for ST 22.

### 8.8 What you get and what you lose in comparison with DPDK

**Expect no speed gain.** AF_XDP does not send or receive faster than the DPDK
PMD in MTL. The upstream guide says that there is "a slight performance
discrepancy compared to the full DPDK user PMD"
(`doc/experimental/af_xdp.md`). MTL publishes no number. The gain is in
deployment.

| Property | DPDK PMD (`MTL_PMD_DPDK_USER`) | Native AF_XDP | Better |
| --- | --- | --- | --- |
| Owner of the port | DPDK, on a VF or PF bound to `vfio-pci` | The kernel driver | AF_XDP |
| Host setup | VFs, `vfio-pci`, IOMMU | An IP address, the manager | AF_XDP |
| Privilege of the application | Root, or access to the VFIO group | `CAP_NET_RAW`; the manager has root | AF_XDP |
| Other traffic on the same port | No | Yes: SSH, ARP, `ptp4l` | AF_XDP |
| Containers | VFIO device in the container | Host network plus `NET_RAW` (`docker/docker-compose.xdp.yml`) | AF_XDP |
| Many processes on one port | One VF for each process | Queues of one PF, through the manager | AF_XDP |
| Standard tools (`tcpdump`, `ip`, `ethtool -S`) | Do not see the VF | Work, except for redirected packets | AF_XDP |
| Driver | Any DPDK PMD | Any kernel driver; zero-copy needs XDP and XSK support | Equal |
| Hugepages | Yes | Yes (8.6) | Equal |
| RX copies | 0 | 0 in zero-copy mode, 1 in copy mode | Equal / DPDK |
| TX copies | 0 (the payload is a chained mbuf from the frame) | 1 for each packet (`xdp_tx()`) | DPDK |
| CPU outside MTL lcores | None: the PMD runs in the lcores | NAPI in softirq on the IRQ cores: RX descriptors, the XDP chain, TX completions | DPDK |
| TX pacing | Hardware RL on E810 through `rte_tm`, or TSC | TSC; RL only with the patched `ice` 1.12.7 | DPDK |
| PTP | Built-in, with NIC timestamps | `ptp4l` and `phc2sys` outside MTL | DPDK for simple setup; equal for accuracy if linuxptp is good |
| Queues | VF queues | `combined` channels, queue 0 reserved, queues 0 to 63 only (`xsks_map`) | Depends |
| Flow rules | `rte_flow` in MTL | ethtool ntuple through the manager | Equal |

Select **native AF_XDP** when:

* the port must stay a normal Linux interface, or you cannot use VFs or
  `vfio-pci` (cloud, edge, a locked kernel);
* the application must run without root, or in a container with no device
  passthrough;
* the density is small to medium, and wide pacing or the measured pacing of
  your host is sufficient.

Select **DPDK** when:

* you need narrow (type N) pacing with hardware rate limit at high density;
* you need the most streams for each core;
* you need MTL's built-in PTP.

### 8.9 How to measure the difference on your host

Run the same configuration twice, once on a VF and once on AF_XDP, with the
same streams and the same lcores. Change only the `name` of each interface.

| Measure | How | What it tells you |
| --- | --- | --- |
| CPU in MTL | The scheduler lines of the periodic MTL statistics (each 10 s), and `top -H -p <pid>` | The cost in the lcores. |
| CPU outside MTL | `mpstat -P ALL 1`, the `%soft` column of the IRQ cores of the interface | The cost of NAPI and the XDP chain. DPDK has no such cost. |
| Packet loss | The RX session statistics, and `xdp_queue_rx_stat` | The RX keeps up or not. |
| TX back pressure | `xdp_queue_tx_stat`: `tx prod full`, `mbuf alloc fail` | The TX ring or the UMEM pool is too small. |
| ST 2110-21 compliance | A pcap capture analyzed with EBU LIST | The pacing on the wire, which is the real result of 8.7.2. |
| Density | Increase `replicas` until the loss or the compliance fails | The number of streams for each core. |

Write down the kernel, the `ice` version, the zero-copy state from the log, and
the two NAPI values from step 4 of 8.3. They change the AF_XDP result.

### 8.10 Troubleshooting

| Log text | Cause | Fix |
| --- | --- | --- |
| `no xdp support for this build` | `meson` did not find libxdp or libbpf. | 8.3 step 2, then build again. |
| `AF_XDP backend must run with MTL Manager!` | No manager, or the client cannot open the manager socket. | Start `sudo MtlManager`. See 3.1 for the socket mode. |
| `connect to manager fail, assume single instance mode` | The same cause. | The same fix. |
| Manager: `Failed to load built-in xdp program.` | libxdp does not find `mtl.xdp.o`. | Install the manager with `meson install`, or set `LIBXDP_OBJECT_PATH` to the directory of `mtl.xdp.o` for the manager. |
| Manager: `Failed to attach XDP program with native mode, try skb mode.` | The driver has no native XDP. | The run continues slowly. See 3.36 for the detach risk. |
| `get xsks_map_fd fail` | The manager has no XDP chain on the interface. | Look at the manager log for the cause. |
| `no free queue found` | The manager has no free queue on the interface. | `sudo ethtool -L <if> combined <n>`. |
| `please add capability for the app: sudo setcap 'cap_net_raw+ep' <app>` | The UMEM create failed with `EPERM`. | `setcap`, or run as root. |
| `please run with mtl manager or root user` | The socket create failed with `EPERM`. | The same fix. |
| `umem create fail -105` (`ENOBUFS`) as a user | The locked-memory limit is too small for the UMEM. | Raise `ulimit -l`, or add `CAP_IPC_LOCK`. |
| `xsk create with zero copy fail ..., try copy mode` | The driver or the queue has no zero-copy. | Use `ice`, or accept copy mode. |
| `socket add flow fail for queue <q>` | `ntuple` is off, or the flow table is full. | `sudo ethtool -K <if> ntuple on`; `ethtool -n <if>`. |
| `SIOCGIFADDR fail` | The interface has no IPv4 address. | `sudo ip addr add ...`. |
| `rl feature no` | No patched `ice`. | Normal for the default build. TSC pacing. |
| RX gets nothing, no error | Wrong ARP answer, `rp_filter`, or a port on a queue above 63. | 8.3 step 4; `rp_filter=0`; check the queue in the log. |

### 8.11 Open points found while this part was written

These points come from reading the code. They were not checked on a host.

1. **The TX wakeup does not run.** `xdp_socket_init()` does not set
   `XDP_USE_NEED_WAKEUP` (the line is a comment, `mt_af_xdp.c:420`). Without
   this flag, the kernel never sets `XDP_RING_NEED_WAKEUP`. Thus
   `xsk_ring_prod__needs_wakeup()` is always false, and `xdp_tx_wakeup()`
   never calls `send()`. In zero-copy mode, the driver sends when its NAPI
   runs for another reason, for example an interrupt. In copy mode, the
   kernel sends only from `sendmsg()`. Check: the `wakeup` count in
   `xdp_queue_tx_stat` stays 0, and a run with `--afxdp_zc_disable` sends or
   does not send.
2. **The rate-limit patch is only for `ice` 1.12.7.** The pinned driver is
   2.6.7, and `build_drivers.sh` does not apply the `xdp/` subdirectory. Thus
   hardware pacing on AF_XDP is not available in a default build (8.7.2).
3. **`xdp_mode` is wrong after an SKB attach.** See 3.36.
4. **The fill ring mbufs are not freed at the end.**
   `xdp_queue_clean_mbuf()` is empty, with a `todo` (`mt_af_xdp.c:198`). The
   mempool is freed later, so the effect is small.
5. **TX and RX share one mempool for each queue.** The pool has
   `2048 + 1024` mbufs, and the RX fill takes 2048 at the start. The TX copy
   takes its mbufs from the remaining part. If completions are slow, the TX
   log shows `mbuf alloc fail`.
6. **The filter is IPv4 only.** An ST 2110 stream on IPv6 does not reach the
   socket.

---
