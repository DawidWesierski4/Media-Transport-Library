/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * RTP-level TX with user pacing: st20_tx_put_mbuf_tai().
 *
 * The app sends the first packet of each frame with st20_tx_put_mbuf_tai() and a
 * TAI that is off the epoch grid on purpose, and the other packets with
 * st20_tx_put_mbuf(). A second port receives the stream in the same process, so
 * the TX and the RX side read one PTP clock, and the app prints when the first
 * packet of each frame arrived against the TAI that it asked for.
 *
 * Three frames take the other paths on purpose:
 *   frame 30: no TAI          -> default pacing, one warning, stat_error_user_timestamp
 *   frame 40: TAI in the past
 *   frame 50: TAI 1.5 s ahead
 *
 * --no_exact      drop ST20_TX_FLAG_EXACT_USER_PACING: the frame goes on the nearest
 * epoch
 * --offset <f>    where the app TAI sits inside a frame period, 0 to 1 (default 0.37)
 * --seconds <n>   run time (default 6)
 * The other options are those of the samples: --p_port (TX), --r_port (RX),
 * --pacing_way tsc, ...
 *
 * The arrival time is a software time: mtl_ptp_read_time() when the RX thread
 * dequeues the packet. It includes the RX poll and the ring, a few us.
 */

#include "../sample_util.h"

#define DEMO_MAX_FRAMES (4096)
#define DEMO_PRINT_FRAMES (8)
#define DEMO_FRAME_NO_TAI (30)
#define DEMO_FRAME_PAST (40)
#define DEMO_FRAME_AHEAD (50)
#define DEMO_PKTS_PER_FRAME (4320) /* 1080p: 4 packets of 1200 bytes per line */
#define DEMO_PAYLOAD (1200)

struct demo_frame {
  uint64_t target;  /* the TAI the app asked for, 0 for none */
  uint64_t put;     /* when the app put the first packet */
  uint64_t arrival; /* when the RX thread saw the first packet */
  const char* what;
};

struct demo_ctx {
  mtl_handle st;
  st20_tx_handle tx;
  st20_rx_handle rx;
  bool exact;
  double offset;
  uint64_t fps_mul, fps_den; /* frame period = 1e9 * den / mul ns */
  volatile bool stop;
  uint8_t payload_type;

  /* tx thread */
  uint32_t frame;
  uint32_t pkt_idx;
  uint32_t seq;
  uint64_t next_target;
  pthread_t tx_thread;
  pthread_mutex_t tx_mutex;
  pthread_cond_t tx_cond;

  /* rx thread */
  bool rx_have_ts;
  uint32_t rx_last_ts;
  uint32_t rx_frames;
  pthread_t rx_thread;

  struct demo_frame frames[DEMO_MAX_FRAMES];
};

static void demo_fps(enum st_fps fps, uint64_t* mul, uint64_t* den) {
  switch (fps) {
    case ST_FPS_P59_94:
      *mul = 60000, *den = 1001;
      break;
    case ST_FPS_P29_97:
      *mul = 30000, *den = 1001;
      break;
    default:
      *mul = (uint64_t)(st_frame_rate(fps) + 0.5), *den = 1;
      break;
  }
}

/* epoch k starts at k * TFRAME from the SMPTE Epoch, ST 2110-21:2022 6.2 (p6) */
static uint64_t demo_epoch(struct demo_ctx* d, uint64_t tai) {
  return (uint64_t)((unsigned __int128)tai * d->fps_mul / (d->fps_den * NS_PER_S));
}

static uint64_t demo_epoch_tai(struct demo_ctx* d, uint64_t epoch) {
  return (uint64_t)((unsigned __int128)epoch * d->fps_den * NS_PER_S / d->fps_mul);
}

static uint64_t demo_nearest_epoch_tai(struct demo_ctx* d, uint64_t tai) {
  uint64_t e = demo_epoch(d, tai);
  uint64_t lo = demo_epoch_tai(d, e), hi = demo_epoch_tai(d, e + 1);
  return (tai - lo < hi - tai) ? lo : hi;
}

static uint64_t demo_frame_ns(struct demo_ctx* d) {
  return d->fps_den * NS_PER_S / d->fps_mul;
}

/* the TAI for the next frame: one frame period after the last, off the grid */
static uint64_t demo_pick_tai(struct demo_ctx* d, uint64_t now, const char** what) {
  uint64_t tai;

  if (d->next_target < now + 2 * NS_PER_MS) {
    /* first frame, or the session fell behind: anchor 5 ms ahead */
    uint64_t e = demo_epoch(d, now + 5 * NS_PER_MS) + 1;
    d->next_target = demo_epoch_tai(d, e) + (uint64_t)(d->offset * demo_frame_ns(d));
  }
  tai = d->next_target;
  d->next_target += demo_frame_ns(d);
  *what = "regular";

  if (d->frame == DEMO_FRAME_NO_TAI) {
    tai = 0;
    *what = "no tai";
  } else if (d->frame == DEMO_FRAME_PAST) {
    tai = now - NS_PER_MS;
    *what = "tai 1 ms in the past";
  } else if (d->frame == DEMO_FRAME_AHEAD) {
    tai = now + NS_PER_S + NS_PER_S / 2;
    *what = "tai 1.5 s ahead";
  }
  return tai;
}

static int demo_tx_done(void* priv) {
  struct demo_ctx* d = priv;
  st_pthread_mutex_lock(&d->tx_mutex);
  st_pthread_cond_signal(&d->tx_cond);
  st_pthread_mutex_unlock(&d->tx_mutex);
  return 0;
}

static void* demo_tx_thread(void* arg) {
  struct demo_ctx* d = arg;
  void *mbuf, *usrptr;

  while (!d->stop) {
    mbuf = st20_tx_get_mbuf(d->tx, &usrptr);
    if (!mbuf) {
      st_pthread_mutex_lock(&d->tx_mutex);
      mbuf = st20_tx_get_mbuf(d->tx, &usrptr);
      if (!mbuf) {
        if (!d->stop) st_pthread_cond_wait(&d->tx_cond, &d->tx_mutex);
        st_pthread_mutex_unlock(&d->tx_mutex);
        continue;
      }
      st_pthread_mutex_unlock(&d->tx_mutex);
    }

    struct st20_rfc4175_rtp_hdr* rtp = usrptr;
    memset(rtp, 0, sizeof(*rtp));
    rtp->base.version = 2;
    rtp->base.payload_type = d->payload_type;
    /* ST20_TX_FLAG_USER_TIMESTAMP keeps it: the RX side reads the frame index here */
    rtp->base.tmstamp = htonl(d->frame);
    rtp->base.seq_number = htons((uint16_t)d->seq);
    rtp->seq_number_ext = htons((uint16_t)(d->seq >> 16));
    rtp->row_number = htons(d->pkt_idx / 4);
    rtp->row_offset = htons(480 * (d->pkt_idx % 4));
    rtp->row_length = htons(DEMO_PAYLOAD);
    memset((uint8_t*)rtp + sizeof(*rtp), 0, DEMO_PAYLOAD);
    uint16_t len = sizeof(*rtp) + DEMO_PAYLOAD;
    d->seq++;

    if (d->pkt_idx == 0) {
      /* the first packet of a frame: only its TAI is read */
      uint64_t now = mtl_ptp_read_time(d->st);
      const char* what;
      uint64_t tai = demo_pick_tai(d, now, &what);
      if (d->frame < DEMO_MAX_FRAMES) {
        d->frames[d->frame].target = tai;
        d->frames[d->frame].put = now;
        d->frames[d->frame].what = what;
      }
      st20_tx_put_mbuf_tai(d->tx, mbuf, len, tai);
    } else {
      if (d->pkt_idx == DEMO_PKTS_PER_FRAME - 1) rtp->base.marker = 1;
      st20_tx_put_mbuf(d->tx, mbuf, len);
    }

    d->pkt_idx++;
    if (d->pkt_idx >= DEMO_PKTS_PER_FRAME) {
      d->pkt_idx = 0;
      d->frame++;
    }
  }
  return NULL;
}

/* busy poll, so the arrival time does not include a wake-up */
static void* demo_rx_thread(void* arg) {
  struct demo_ctx* d = arg;
  void* usrptr;
  uint16_t len;

  while (!d->stop) {
    void* mbuf = st20_rx_get_mbuf(d->rx, &usrptr, &len);
    if (!mbuf) continue;
    uint64_t now = mtl_ptp_read_time(d->st);
    struct st20_rfc4175_rtp_hdr* rtp = usrptr;
    uint32_t ts = ntohl(rtp->base.tmstamp);
    if (!d->rx_have_ts || ts != d->rx_last_ts) {
      d->rx_have_ts = true;
      d->rx_last_ts = ts;
      d->rx_frames++;
      if (ts < DEMO_MAX_FRAMES && !d->frames[ts].arrival) d->frames[ts].arrival = now;
    }
    st20_rx_put_mbuf(d->rx, mbuf);
  }
  return NULL;
}

static int demo_rx_ready(void* priv) {
  MTL_MAY_UNUSED(priv);
  return 0;
}

static double demo_us(uint64_t a, uint64_t b) {
  return (double)(int64_t)(a - b) / 1000.0;
}

static int demo_cmp(const void* a, const void* b) {
  double x = *(const double*)a, y = *(const double*)b;
  return (x > y) - (x < y);
}

static void demo_print_row(struct demo_ctx* d, uint32_t i) {
  struct demo_frame* f = &d->frames[i];
  uint64_t ref = f->target ? f->target : f->put;
  uint64_t near = demo_nearest_epoch_tai(d, ref);

  printf("%5u  %-21s %20" PRIu64 " %+11.1f", i, f->what, f->target,
         f->target ? demo_us(f->target, near) : 0.0);
  if (!f->arrival) {
    printf("  %12s\n", "not received");
    return;
  }
  printf("  %+12.1f  %+12.1f\n", f->target ? demo_us(f->arrival, f->target) : 0.0,
         demo_us(f->arrival, near));
}

static void demo_report(struct demo_ctx* d) {
  uint32_t n = d->frame < DEMO_MAX_FRAMES ? d->frame : DEMO_MAX_FRAMES;
  static double v[DEMO_MAX_FRAMES];
  uint32_t cnt = 0, lost = 0;

  printf("\n=== %s, app TAI at %.2f of a frame period (%.1f us) ===\n",
         d->exact ? "EXACT_USER_PACING" : "USER_PACING, nearest epoch", d->offset,
         d->offset * demo_frame_ns(d) / 1000.0);
  printf("%5s  %-21s %20s %11s  %12s  %12s\n", "frame", "what", "app tai (ns)",
         "tai-epoch", "arr-tai", "arr-epoch");
  printf("%5s  %-21s %20s %11s  %12s  %12s\n", "", "", "", "(us)", "(us)", "(us)");
  for (uint32_t i = 0; i < n; i++) {
    struct demo_frame* f = &d->frames[i];
    bool special =
        i == DEMO_FRAME_NO_TAI || i == DEMO_FRAME_PAST || i == DEMO_FRAME_AHEAD;
    bool after = i == DEMO_FRAME_NO_TAI + 1 || i == DEMO_FRAME_PAST + 1 ||
                 i == DEMO_FRAME_AHEAD + 1;
    if (i < DEMO_PRINT_FRAMES || special || after) demo_print_row(d, i);
    /* frames 0 and 1 meet the warm-up of the session; the specials and the frame
     * after each one take another path */
    if (i < 2 || special || after || i + 1 >= n) continue;
    if (!f->arrival) {
      lost++;
      continue;
    }
    uint64_t ref = d->exact ? f->target : demo_nearest_epoch_tai(d, f->target);
    v[cnt++] = demo_us(f->arrival, ref);
  }

  if (cnt) {
    qsort(v, cnt, sizeof(v[0]), demo_cmp);
    printf(
        "\nregular frames %u: arrival - %s, min %+.1f us, median %+.1f us, max %+.1f us",
        cnt, d->exact ? "app tai" : "nearest epoch", v[0], v[cnt / 2], v[cnt - 1]);
    printf(", not received %u\n", lost);
  }

  struct st20_tx_user_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (st20_tx_get_session_stats(d->tx, &stats) >= 0)
    printf("stat_error_user_timestamp %" PRIu64 "\n",
           stats.common.stat_error_user_timestamp);
  printf("tx frames %u, rx frames %u\n", d->frame, d->rx_frames);
}

/* take the options of this sample out of argv, leave the rest for sample_util */
static int demo_parse_args(struct demo_ctx* d, int* seconds, int argc, char** argv) {
  int out = 1;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--no_exact")) {
      d->exact = false;
    } else if (!strcmp(argv[i], "--offset") && i + 1 < argc) {
      d->offset = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
      *seconds = atoi(argv[++i]);
    } else {
      argv[out++] = argv[i];
    }
  }
  if (d->offset < 0.0 || d->offset >= 1.0) {
    err("%s, --offset must be in [0, 1)\n", __func__);
    return -EINVAL;
  }
  return out;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  struct demo_ctx* d;
  int seconds = 6;
  int ret;

  d = calloc(1, sizeof(*d));
  if (!d) return -ENOMEM;
  d->exact = true;
  d->offset = 0.37;
  argc = demo_parse_args(d, &seconds, argc, argv);
  if (argc < 0) {
    free(d);
    return argc;
  }

  memset(&ctx, 0, sizeof(ctx));
  ret = fwd_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) goto out;
  if (ctx.param.num_ports < 2) {
    err("%s, give --p_port (TX) and --r_port (RX)\n", __func__);
    ret = -EINVAL;
    goto out;
  }

  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s, mtl_init fail\n", __func__);
    ret = -EIO;
    goto out;
  }
  d->st = ctx.st;
  d->payload_type = ctx.payload_type;
  demo_fps(ctx.fps, &d->fps_mul, &d->fps_den);
  st_pthread_mutex_init(&d->tx_mutex, NULL);
  st_pthread_cond_init(&d->tx_cond, NULL);

  struct st20_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "demo_rx";
  ops_rx.priv = d;
  ops_rx.num_port = 1;
  memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx.param.port[MTL_PORT_R]);
  ops_rx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
  ops_rx.type = ST20_TYPE_RTP_LEVEL;
  ops_rx.width = ctx.width;
  ops_rx.height = ctx.height;
  ops_rx.fps = ctx.fps;
  ops_rx.fmt = ctx.fmt;
  ops_rx.payload_type = ctx.payload_type;
  ops_rx.rtp_ring_size = 4096;
  ops_rx.notify_rtp_ready = demo_rx_ready;
  d->rx = st20_rx_create(ctx.st, &ops_rx);
  if (!d->rx) {
    err("%s, st20_rx_create fail\n", __func__);
    ret = -EIO;
    goto out;
  }

  struct st20_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "demo_tx";
  ops_tx.priv = d;
  ops_tx.num_port = 1;
  memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx.param.port[MTL_PORT_P]);
  ops_tx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
  ops_tx.pacing = ST21_PACING_NARROW;
  ops_tx.type = ST20_TYPE_RTP_LEVEL;
  ops_tx.width = ctx.width;
  ops_tx.height = ctx.height;
  ops_tx.fps = ctx.fps;
  ops_tx.fmt = ctx.fmt;
  ops_tx.payload_type = ctx.payload_type;
  ops_tx.rtp_ring_size = 1024;
  ops_tx.rtp_frame_total_pkts = DEMO_PKTS_PER_FRAME;
  ops_tx.rtp_pkt_size = DEMO_PAYLOAD + sizeof(struct st20_rfc4175_rtp_hdr);
  ops_tx.notify_rtp_done = demo_tx_done;
  ops_tx.flags = ST20_TX_FLAG_USER_PACING | ST20_TX_FLAG_USER_TIMESTAMP;
  if (d->exact) ops_tx.flags |= ST20_TX_FLAG_EXACT_USER_PACING;
  d->tx = st20_tx_create(ctx.st, &ops_tx);
  if (!d->tx) {
    err("%s, st20_tx_create fail\n", __func__);
    ret = -EIO;
    goto out;
  }

  info("%s, %s, app TAI at %.2f of a frame period, %d s\n", __func__,
       d->exact ? "EXACT_USER_PACING" : "nearest epoch", d->offset, seconds);
  pthread_create(&d->rx_thread, NULL, demo_rx_thread, d);
  pthread_create(&d->tx_thread, NULL, demo_tx_thread, d);

  for (int i = 0; i < seconds && !ctx.exit; i++) sleep(1);

  d->stop = true;
  st_pthread_mutex_lock(&d->tx_mutex);
  st_pthread_cond_signal(&d->tx_cond);
  st_pthread_mutex_unlock(&d->tx_mutex);
  pthread_join(d->tx_thread, NULL);
  pthread_join(d->rx_thread, NULL);

  demo_report(d);
  ret = d->rx_frames ? 0 : -EIO;

out:
  if (d->tx) st20_tx_free(d->tx);
  if (d->rx) st20_rx_free(d->rx);
  if (ctx.st) {
    st_pthread_mutex_destroy(&d->tx_mutex);
    st_pthread_cond_destroy(&d->tx_cond);
    mtl_uninit(ctx.st);
  }
  free(d);
  return ret;
}
