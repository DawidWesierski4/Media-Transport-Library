/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023-2026 Intel Corporation
 */

#include "mt_instance.h"

#ifndef WINDOWSENV

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mt_log.h"
#include "mt_varlink.h"

#define MT_INSTANCE_RPC_TIMEOUT_MS 5000LL
#define MT_INSTANCE_ALIVE_TIMEOUT_MS 500LL
#define MT_INSTANCE_CLOSE_TIMEOUT_MS 1000LL
#define MT_V1_DEFAULT_SOCK_PATH "/var/run/imtl/mtl_manager.sock"

struct mt_instance_ctx {
  pthread_mutex_t mutex;
  int sock_fd;
  char sock_path[256];
  char client_name[128];
  char session_id[MT_VARLINK_ID_STR_LEN];
  char** leases;
  size_t lease_count;
  size_t lease_cap;
  int64_t rpc_timeout_ms;
  int64_t hb_seq;
  bool abort_on_loss;
};

static const char* instance_resolve_sock_path(void) {
  const char* env_path = getenv("MTL_MANAGER_SOCK");
  if (env_path && env_path[0] != '\0') return env_path;
  return MT_VARLINK_DEFAULT_SOCK_PATH;
}

static const char* instance_resolve_v1_sock_path(void) {
  const char* env_path = getenv("MTL_V1_MANAGER_SOCK");
  if (env_path != NULL) return env_path;
  return MT_V1_DEFAULT_SOCK_PATH;
}

static bool env_flag_enabled(const char* name) {
  const char* v = getenv(name);
  return (v && strcmp(v, "1") == 0);
}

static int instance_verify_server_peercred(int fd, const char* sock_path) {
  struct ucred peer;
  socklen_t len = sizeof(peer);
  memset(&peer, 0, sizeof(peer));
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) != 0 || len != sizeof(peer) ||
      peer.pid <= 0) {
    err("%s, failed to query SO_PEERCRED on %s\n", __func__, sock_path);
    return -EACCES;
  }
  if (env_flag_enabled("MTL_REQUIRE_ROOT_MANAGER")) {
    if (peer.uid != 0) {
      err("%s, untrusted manager uid %u on %s (MTL_REQUIRE_ROOT_MANAGER=1 requires uid "
          "0)\n",
          __func__, (unsigned)peer.uid, sock_path);
      return -EACCES;
    }
  } else if (peer.uid != 0 && peer.uid != geteuid()) {
    err("%s, untrusted manager uid %u on %s (expected 0 or %u)\n", __func__,
        (unsigned)peer.uid, sock_path, (unsigned)geteuid());
    return -EACCES;
  }
  return 0;
}

static int instance_query_and_log_get_info(int fd, const char* sock_path,
                                           int64_t timeout_ms) {
  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = mt_varlink_call(fd, MT_VARLINK_SERVICE_INTERFACE ".GetInfo", NULL, NULL, 0,
                           timeout_ms, &reply, err_name, sizeof(err_name), err_msg,
                           sizeof(err_msg));
  if (rc < 0) {
    err("%s, GetInfo on %s fail %d (%s: %s)\n", __func__, sock_path, rc, err_name,
        err_msg);
    return rc;
  }

  const char* version = "unknown";
  char ifaces_buf[512];
  ifaces_buf[0] = '\0';

  struct json_object* rparams = NULL;
  if (reply && json_object_object_get_ex(reply, "parameters", &rparams) && rparams &&
      json_object_is_type(rparams, json_type_object)) {
    struct json_object* jver = NULL;
    if (json_object_object_get_ex(rparams, "version", &jver) && jver &&
        json_object_is_type(jver, json_type_string)) {
      version = json_object_get_string(jver);
    }
    struct json_object* jifs = NULL;
    if (json_object_object_get_ex(rparams, "interfaces", &jifs) && jifs &&
        json_object_is_type(jifs, json_type_array)) {
      size_t n = json_object_array_length(jifs);
      size_t off = 0;
      for (size_t i = 0; i < n && off + 1u < sizeof(ifaces_buf); ++i) {
        struct json_object* item = json_object_array_get_idx(jifs, i);
        if (!item || !json_object_is_type(item, json_type_string)) continue;
        const char* s = json_object_get_string(item);
        if (!s) continue;
        int w = snprintf(ifaces_buf + off, sizeof(ifaces_buf) - off, "%s%s",
                         (off > 0u) ? ", " : "", s);
        if (w < 0) break;
        if ((size_t)w >= sizeof(ifaces_buf) - off) {
          off = sizeof(ifaces_buf) - 1u;
          break;
        }
        off += (size_t)w;
      }
    }
  }

  info("%s, MtlManager 2 at %s version=%s interfaces=[%s]\n", __func__, sock_path,
       version, ifaces_buf);
  if (reply) json_object_put(reply);
  return 0;
}

static void instance_build_client_name(char* out, size_t out_sz) {
  char comm[64];
  memset(comm, 0, sizeof(comm));
  int fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, comm, sizeof(comm) - 1u);
    close(fd);
    if (n > 0) {
      comm[n] = '\0';
      for (ssize_t i = 0; i < n; ++i) {
        if (comm[i] == '\n' || comm[i] == '\r') {
          comm[i] = '\0';
          break;
        }
        unsigned char c = (unsigned char)comm[i];
        if (c < 0x20u || c > 0x7Eu) comm[i] = '_';
      }
    }
  }
  if (comm[0] == '\0') snprintf(comm, sizeof(comm), "mtl");
  snprintf(out, out_sz, "mtl-%s-%ld", comm, (long)getpid());
}

static bool is_transport_disconnect(int rc) {
  return rc == -EPIPE || rc == -ECONNRESET || rc == -ECONNABORTED || rc == -ENOTCONN;
}

static int instance_track_lease_locked(struct mt_instance_ctx* ctx,
                                       const char* lease_id) {
  if (!lease_id || lease_id[0] == '\0') return -EINVAL;
  for (size_t i = 0; i < ctx->lease_count; ++i) {
    if (strcmp(ctx->leases[i], lease_id) == 0) return 0;
  }
  if (ctx->lease_count >= ctx->lease_cap) {
    size_t new_cap = (ctx->lease_cap == 0u) ? 16u : (ctx->lease_cap * 2u);
    char** nl = (char**)realloc(ctx->leases, new_cap * sizeof(char*));
    if (!nl) return -ENOMEM;
    ctx->leases = nl;
    ctx->lease_cap = new_cap;
  }
  char* copy = strdup(lease_id);
  if (!copy) return -ENOMEM;
  ctx->leases[ctx->lease_count++] = copy;
  return 0;
}

static void instance_untrack_lease_locked(struct mt_instance_ctx* ctx,
                                          const char* lease_id) {
  if (!lease_id) return;
  for (size_t i = 0; i < ctx->lease_count; ++i) {
    if (strcmp(ctx->leases[i], lease_id) == 0) {
      free(ctx->leases[i]);
      ctx->leases[i] = ctx->leases[ctx->lease_count - 1u];
      ctx->leases[ctx->lease_count - 1u] = NULL;
      ctx->lease_count--;
      return;
    }
  }
}

static void instance_copy_str(char* dst, size_t dst_sz, const char* src) {
  if (!dst || dst_sz == 0u) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strnlen(src, dst_sz - 1u);
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static int instance_reconnect_and_claim_locked(struct mtl_main_impl* impl,
                                               struct mt_instance_ctx* ctx) {
  if (ctx->sock_fd >= 0) {
    close(ctx->sock_fd);
    ctx->sock_fd = -1;
  }
  if (ctx->sock_path[0] == '\0') return -EIO;

  int fd = mt_varlink_connect_unix(ctx->sock_path, ctx->rpc_timeout_ms);
  if (fd < 0) {
    err("%s, reconnect to %s fail %d\n", __func__, ctx->sock_path, fd);
    goto fail;
  }
  if (instance_verify_server_peercred(fd, ctx->sock_path) < 0) {
    close(fd);
    goto fail;
  }

  struct json_object* params = json_object_new_object();
  struct json_object* arr = json_object_new_array();
  if (!params || !arr) {
    if (params) json_object_put(params);
    if (arr) json_object_put(arr);
    close(fd);
    goto fail;
  }
  json_object_object_add(params, "clientName", json_object_new_string(ctx->client_name));
  for (size_t i = 0; i < ctx->lease_count; ++i) {
    json_object_array_add(arr, json_object_new_string(ctx->leases[i]));
  }
  json_object_object_add(params, "leaseIds", arr);

  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = mt_varlink_call(fd, MT_VARLINK_MANAGER_INTERFACE ".ClaimSession", params, NULL,
                           0, ctx->rpc_timeout_ms, &reply, err_name, sizeof(err_name),
                           err_msg, sizeof(err_msg));
  json_object_put(params);
  if (rc < 0) {
    err("%s, ClaimSession fail %d (%s: %s)\n", __func__, rc, err_name, err_msg);
    close(fd);
    goto fail;
  }

  struct json_object* rparams = NULL;
  struct json_object* jsid = NULL;
  if (json_object_object_get_ex(reply, "parameters", &rparams) && rparams &&
      json_object_object_get_ex(rparams, "sessionId", &jsid) && jsid &&
      json_object_is_type(jsid, json_type_string)) {
    instance_copy_str(ctx->session_id, sizeof(ctx->session_id),
                      json_object_get_string(jsid));
  }
  json_object_put(reply);

  ctx->sock_fd = fd;
  info("%s, reclaimed manager session %s (%zu leases)\n", __func__, ctx->session_id,
       ctx->lease_count);
  return 0;

fail:
  if (impl && (ctx->abort_on_loss || env_flag_enabled("MTL_ABORT_ON_MANAGER_LOSS"))) {
    err("%s, MTL_ABORT_ON_MANAGER_LOSS=1 set, aborting instance\n", __func__);
    mtl_abort(impl);
  }
  return -EIO;
}

static int instance_call_locked(struct mtl_main_impl* impl, struct mt_instance_ctx* ctx,
                                const char* method, struct json_object* params,
                                const int* fds, size_t fd_count,
                                struct json_object** out_reply, char* err_name,
                                size_t err_name_sz, char* err_msg, size_t err_msg_sz) {
  int rc = -EPIPE;
  if (ctx->sock_fd >= 0) {
    rc = mt_varlink_call(ctx->sock_fd, method, params, fds, fd_count, ctx->rpc_timeout_ms,
                         out_reply, err_name, err_name_sz, err_msg, err_msg_sz);
  }
  if (!is_transport_disconnect(rc)) return rc;

  warn("%s, manager connection lost (%d) during %s, attempting ClaimSession\n", __func__,
       rc, method);
  int rrc = instance_reconnect_and_claim_locked(impl, ctx);
  if (rrc < 0) return rrc;

  return mt_varlink_call(ctx->sock_fd, method, params, fds, fd_count, ctx->rpc_timeout_ms,
                         out_reply, err_name, err_name_sz, err_msg, err_msg_sz);
}

struct mt_instance_ctx* mt_instance_create_from_fd(int sock_fd, const char* sock_path,
                                                   const char* client_name) {
  if (sock_fd < 0) return NULL;
  struct mt_instance_ctx* ctx = (struct mt_instance_ctx*)calloc(1u, sizeof(*ctx));
  if (!ctx) return NULL;
  if (pthread_mutex_init(&ctx->mutex, NULL) != 0) {
    free(ctx);
    return NULL;
  }
  ctx->sock_fd = sock_fd;
  ctx->rpc_timeout_ms = MT_INSTANCE_RPC_TIMEOUT_MS;
  ctx->abort_on_loss = env_flag_enabled("MTL_ABORT_ON_MANAGER_LOSS");
  if (sock_path) instance_copy_str(ctx->sock_path, sizeof(ctx->sock_path), sock_path);
  if (client_name && client_name[0] != '\0')
    instance_copy_str(ctx->client_name, sizeof(ctx->client_name), client_name);
  else
    instance_build_client_name(ctx->client_name, sizeof(ctx->client_name));
  return ctx;
}

int mt_instance_connect(struct mtl_init_params* p, struct mt_instance_ctx** out_ctx) {
  if (!out_ctx) return -EINVAL;
  *out_ctx = NULL;

  const char* sock_path = instance_resolve_sock_path();
  bool require_manager = env_flag_enabled("MTL_REQUIRE_MANAGER");
  bool has_native_af_xdp = false;
  bool has_dpdk_af_xdp = false;

  if (p) {
    int num_ports = (p->num_ports > MTL_PORT_MAX) ? MTL_PORT_MAX : p->num_ports;
    for (int i = 0; i < num_ports; ++i) {
      if (p->pmd[i] == MTL_PMD_NATIVE_AF_XDP) has_native_af_xdp = true;
      if (p->pmd[i] == MTL_PMD_DPDK_AF_XDP) has_dpdk_af_xdp = true;
    }
  }

  struct stat st;
  bool sock_exists = (lstat(sock_path, &st) == 0);
  int fd = mt_varlink_connect_unix(sock_path, MT_INSTANCE_RPC_TIMEOUT_MS);
  if (fd < 0) {
    if (fd == -ENOENT && !sock_exists) {
      /* D4 (VER-M): if v1 MtlManager is listening, refuse shm fallback cleanly. */
      const char* v1_sock_path = instance_resolve_v1_sock_path();
      if (v1_sock_path && v1_sock_path[0] != '\0') {
        int v1_fd = mt_varlink_connect_unix(v1_sock_path, MT_INSTANCE_ALIVE_TIMEOUT_MS);
        if (v1_fd >= 0) {
          close(v1_fd);
          err("%s, v1 MtlManager detected; upgrade MtlManager to v2 (socket %s)\n",
              __func__, v1_sock_path);
          return -EPROTONOSUPPORT;
        }
      }
      if (require_manager) {
        err("%s, MTL_REQUIRE_MANAGER=1 set but %s is missing; check 'systemctl status "
            "mtlm2.service' and 'journalctl -u mtlm2.service'\n",
            __func__, sock_path);
        return -ENOENT;
      }
      if (has_native_af_xdp) {
        err("%s, MTL_PMD_NATIVE_AF_XDP requires MtlManager 2 at %s; check 'systemctl "
            "status mtlm2.service' and 'journalctl -u mtlm2.service'\n",
            __func__, sock_path);
        return -ENOENT;
      }
      info("%s, manager socket %s absent, using standalone/shm mode\n", __func__,
           sock_path);
      return 0;
    }
    err("%s, connect to %s fail %d (%s); check 'systemctl status mtlm2.service' and "
        "'journalctl -u mtlm2.service', or stop legacy shm MTL processes on hosts with "
        ">RTE_MAX_LCORE CPUs\n",
        __func__, sock_path, fd, strerror(-fd));
    return fd;
  }

  /* D5 (SEC-AC-14): verify server SO_PEERCRED before sending any RPC. */
  int peer_rc = instance_verify_server_peercred(fd, sock_path);
  if (peer_rc < 0) {
    close(fd);
    return peer_rc;
  }

  if (has_dpdk_af_xdp) {
    close(fd);
    err("%s, MTL_PMD_DPDK_AF_XDP is not supported with MtlManager 2; migrate to "
        "MTL_PMD_NATIVE_AF_XDP or run without MtlManager 2\n",
        __func__);
    return -ENOTSUP;
  }

  struct mt_instance_ctx* ctx = mt_instance_create_from_fd(fd, sock_path, NULL);
  if (!ctx) {
    close(fd);
    return -ENOMEM;
  }

  /* VER-PROTO-03: query org.varlink.service.GetInfo at connect and log once. */
  int info_rc =
      instance_query_and_log_get_info(ctx->sock_fd, sock_path, ctx->rpc_timeout_ms);
  if (info_rc < 0) {
    close(ctx->sock_fd);
    ctx->sock_fd = -1;
    mt_instance_disconnect(ctx);
    return info_rc;
  }

  struct json_object* params = json_object_new_object();
  if (!params) {
    mt_instance_disconnect(ctx);
    return -ENOMEM;
  }
  json_object_object_add(params, "clientName", json_object_new_string(ctx->client_name));

  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = mt_varlink_call(ctx->sock_fd, MT_VARLINK_MANAGER_INTERFACE ".OpenSession",
                           params, NULL, 0, ctx->rpc_timeout_ms, &reply, err_name,
                           sizeof(err_name), err_msg, sizeof(err_msg));
  json_object_put(params);
  if (rc < 0) {
    err("%s, OpenSession on %s fail %d (%s: %s); check 'systemctl status mtlm2.service' "
        "and 'journalctl -u mtlm2.service', or stop legacy shm MTL processes on hosts "
        "with >RTE_MAX_LCORE CPUs\n",
        __func__, sock_path, rc, err_name, err_msg);
    close(ctx->sock_fd);
    ctx->sock_fd = -1;
    mt_instance_disconnect(ctx);
    return rc;
  }

  struct json_object* rparams = NULL;
  struct json_object* jsid = NULL;
  if (!json_object_object_get_ex(reply, "parameters", &rparams) || !rparams ||
      !json_object_object_get_ex(rparams, "sessionId", &jsid) || !jsid ||
      !json_object_is_type(jsid, json_type_string)) {
    json_object_put(reply);
    err("%s, OpenSession reply missing sessionId\n", __func__);
    close(ctx->sock_fd);
    ctx->sock_fd = -1;
    mt_instance_disconnect(ctx);
    return -EPROTO;
  }
  instance_copy_str(ctx->session_id, sizeof(ctx->session_id),
                    json_object_get_string(jsid));
  json_object_put(reply);

  info("%s, connected to MtlManager 2 at %s session %s\n", __func__, sock_path,
       ctx->session_id);
  *out_ctx = ctx;
  return 0;
}

void mt_instance_disconnect(struct mt_instance_ctx* ctx) {
  if (!ctx) return;
  pthread_mutex_lock(&ctx->mutex);
  if (ctx->sock_fd >= 0) {
    if (ctx->session_id[0] != '\0') {
      struct json_object* reply = NULL;
      (void)mt_varlink_call(ctx->sock_fd, MT_VARLINK_MANAGER_INTERFACE ".CloseSession",
                            NULL, NULL, 0, MT_INSTANCE_CLOSE_TIMEOUT_MS, &reply, NULL, 0,
                            NULL, 0);
      if (reply) json_object_put(reply);
    }
    close(ctx->sock_fd);
    ctx->sock_fd = -1;
  }
  for (size_t i = 0; i < ctx->lease_count; ++i) free(ctx->leases[i]);
  free(ctx->leases);
  ctx->leases = NULL;
  ctx->lease_count = 0;
  ctx->lease_cap = 0;
  pthread_mutex_unlock(&ctx->mutex);
  pthread_mutex_destroy(&ctx->mutex);
  free(ctx);
}

int mt_instance_uinit(struct mtl_main_impl* impl) {
  if (!impl || !impl->instance) return 0;
  mt_instance_disconnect(impl->instance);
  impl->instance = NULL;
  return 0;
}

int mt_instance_get_cpu(struct mtl_main_impl* impl, uint32_t phys_cpu,
                        char lease_id_out[40]) {
  if (!impl || !impl->instance || !lease_id_out) return -EINVAL;
  lease_id_out[0] = '\0';

  struct mt_instance_ctx* ctx = impl->instance;
  struct json_object* params = json_object_new_object();
  if (!params) return -ENOMEM;
  json_object_object_add(params, "preferred", json_object_new_int64((int64_t)phys_cpu));
  json_object_object_add(params, "allowFallback", json_object_new_boolean(0));

  pthread_mutex_lock(&ctx->mutex);
  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = instance_call_locked(impl, ctx, MT_VARLINK_MANAGER_INTERFACE ".AcquireCpu",
                                params, NULL, 0, &reply, err_name, sizeof(err_name),
                                err_msg, sizeof(err_msg));
  json_object_put(params);
  if (rc < 0) {
    pthread_mutex_unlock(&ctx->mutex);
    if (rc != -EBUSY) {
      err("%s(cpu %u), AcquireCpu fail %d (%s: %s)\n", __func__, phys_cpu, rc, err_name,
          err_msg);
    }
    return rc;
  }

  struct json_object* rparams = NULL;
  struct json_object* jlid = NULL;
  if (!json_object_object_get_ex(reply, "parameters", &rparams) || !rparams ||
      !json_object_object_get_ex(rparams, "leaseId", &jlid) || !jlid ||
      !json_object_is_type(jlid, json_type_string)) {
    json_object_put(reply);
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(cpu %u), AcquireCpu reply missing leaseId\n", __func__, phys_cpu);
    return -EPROTO;
  }

  const char* lid = json_object_get_string(jlid);
  instance_copy_str(lease_id_out, MT_VARLINK_ID_STR_LEN, lid);
  int trc = instance_track_lease_locked(ctx, lease_id_out);
  json_object_put(reply);
  pthread_mutex_unlock(&ctx->mutex);
  return trc;
}

int mt_instance_release_lease(struct mtl_main_impl* impl, const char* lease_id) {
  if (!impl || !impl->instance || !lease_id || lease_id[0] == '\0') return -EINVAL;

  struct mt_instance_ctx* ctx = impl->instance;
  struct json_object* params = json_object_new_object();
  if (!params) return -ENOMEM;
  json_object_object_add(params, "leaseId", json_object_new_string(lease_id));

  pthread_mutex_lock(&ctx->mutex);
  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = instance_call_locked(impl, ctx, MT_VARLINK_MANAGER_INTERFACE ".ReleaseLease",
                                params, NULL, 0, &reply, err_name, sizeof(err_name),
                                err_msg, sizeof(err_msg));
  json_object_put(params);
  if (reply) json_object_put(reply);
  if (rc == 0 || rc == -ENOENT) instance_untrack_lease_locked(ctx, lease_id);
  pthread_mutex_unlock(&ctx->mutex);

  if (rc < 0) {
    err("%s(%s), ReleaseLease fail %d (%s: %s)\n", __func__, lease_id, rc, err_name,
        err_msg);
  }
  return rc;
}

int mt_instance_get_queue(struct mtl_main_impl* impl, const char* if_name,
                          uint16_t* out_queue_id, char out_lease[40]) {
  if (!impl || !impl->instance || !if_name || if_name[0] == '\0' || !out_queue_id ||
      !out_lease)
    return -EINVAL;
  out_lease[0] = '\0';
  *out_queue_id = 0;

  struct mt_instance_ctx* ctx = impl->instance;
  struct json_object* params = json_object_new_object();
  if (!params) return -ENOMEM;
  json_object_object_add(params, "ifName", json_object_new_string(if_name));

  pthread_mutex_lock(&ctx->mutex);
  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = instance_call_locked(impl, ctx, MT_VARLINK_MANAGER_INTERFACE ".AcquireQueue",
                                params, NULL, 0, &reply, err_name, sizeof(err_name),
                                err_msg, sizeof(err_msg));
  json_object_put(params);
  if (rc < 0) {
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%s), AcquireQueue fail %d (%s: %s)\n", __func__, if_name, rc, err_name,
        err_msg);
    return rc;
  }

  struct json_object* rparams = NULL;
  struct json_object* jlid = NULL;
  struct json_object* jqid = NULL;
  if (!json_object_object_get_ex(reply, "parameters", &rparams) || !rparams ||
      !json_object_object_get_ex(rparams, "leaseId", &jlid) || !jlid ||
      !json_object_is_type(jlid, json_type_string) ||
      !json_object_object_get_ex(rparams, "queueId", &jqid) || !jqid ||
      !json_object_is_type(jqid, json_type_int)) {
    json_object_put(reply);
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%s), AcquireQueue reply missing leaseId or queueId\n", __func__, if_name);
    return -EPROTO;
  }

  int64_t qv = json_object_get_int64(jqid);
  if (qv <= 0 || qv > 65535) {
    json_object_put(reply);
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%s), AcquireQueue returned invalid queueId %" PRId64 "\n", __func__, if_name,
        qv);
    return -EPROTO;
  }

  instance_copy_str(out_lease, MT_VARLINK_ID_STR_LEN, json_object_get_string(jlid));
  *out_queue_id = (uint16_t)qv;
  int trc = instance_track_lease_locked(ctx, out_lease);
  json_object_put(reply);
  pthread_mutex_unlock(&ctx->mutex);
  return trc;
}

int mt_instance_attach_xsk(struct mtl_main_impl* impl, const char* queue_lease,
                           int xsk_fd, char out_lease[40]) {
  if (!impl || !impl->instance || !queue_lease || queue_lease[0] == '\0' || xsk_fd < 0 ||
      !out_lease)
    return -EINVAL;
  out_lease[0] = '\0';

  if (fcntl(xsk_fd, F_GETFD) < 0) {
    err("%s(%s), invalid xsk_fd %d (%s)\n", __func__, queue_lease, xsk_fd,
        strerror(errno));
    return -EBADF;
  }

  struct mt_instance_ctx* ctx = impl->instance;
  struct json_object* params = json_object_new_object();
  if (!params) return -ENOMEM;
  json_object_object_add(params, "queueLease", json_object_new_string(queue_lease));

  pthread_mutex_lock(&ctx->mutex);
  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = instance_call_locked(impl, ctx, MT_VARLINK_MANAGER_INTERFACE ".AttachXsk",
                                params, &xsk_fd, 1u, &reply, err_name, sizeof(err_name),
                                err_msg, sizeof(err_msg));
  json_object_put(params);
  if (rc < 0) {
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%s), AttachXsk fail %d (%s: %s)\n", __func__, queue_lease, rc, err_name,
        err_msg);
    return rc;
  }

  struct json_object* rparams = NULL;
  struct json_object* jlid = NULL;
  if (!json_object_object_get_ex(reply, "parameters", &rparams) || !rparams ||
      !json_object_object_get_ex(rparams, "leaseId", &jlid) || !jlid ||
      !json_object_is_type(jlid, json_type_string)) {
    json_object_put(reply);
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%s), AttachXsk reply missing leaseId\n", __func__, queue_lease);
    return -EPROTO;
  }

  instance_copy_str(out_lease, MT_VARLINK_ID_STR_LEN, json_object_get_string(jlid));
  int trc = instance_track_lease_locked(ctx, out_lease);
  json_object_put(reply);
  pthread_mutex_unlock(&ctx->mutex);
  return trc;
}

int mt_instance_add_flow(struct mtl_main_impl* impl, const char* queue_lease,
                         uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                         uint16_t dst_port, char out_lease[40], int* out_rule_loc) {
  if (!impl || !impl->instance || !queue_lease || queue_lease[0] == '\0' ||
      dst_port == 0 || !out_lease)
    return -EINVAL;
  out_lease[0] = '\0';
  if (out_rule_loc) *out_rule_loc = -1;

  struct json_object* params = json_object_new_object();
  if (!params) return -ENOMEM;
  json_object_object_add(params, "queueLease", json_object_new_string(queue_lease));
  json_object_object_add(params, "protocol", json_object_new_string("udp4"));

  char ip_buf[INET_ADDRSTRLEN];
  if (src_ip != 0) {
    struct in_addr sa;
    sa.s_addr = src_ip;
    if (!inet_ntop(AF_INET, &sa, ip_buf, sizeof(ip_buf))) {
      json_object_put(params);
      return -EINVAL;
    }
    json_object_object_add(params, "srcIp", json_object_new_string(ip_buf));
  }

  {
    struct in_addr da;
    da.s_addr = dst_ip;
    if (!inet_ntop(AF_INET, &da, ip_buf, sizeof(ip_buf))) {
      json_object_put(params);
      return -EINVAL;
    }
    json_object_object_add(params, "dstIp", json_object_new_string(ip_buf));
  }

  if (src_port != 0) {
    json_object_object_add(params, "srcPort", json_object_new_int64((int64_t)src_port));
  }
  json_object_object_add(params, "dstPort", json_object_new_int64((int64_t)dst_port));

  struct mt_instance_ctx* ctx = impl->instance;
  pthread_mutex_lock(&ctx->mutex);
  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = instance_call_locked(impl, ctx, MT_VARLINK_MANAGER_INTERFACE ".AddFlow",
                                params, NULL, 0, &reply, err_name, sizeof(err_name),
                                err_msg, sizeof(err_msg));
  json_object_put(params);
  if (rc < 0) {
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%s), AddFlow fail %d (%s: %s)\n", __func__, queue_lease, rc, err_name,
        err_msg);
    return rc;
  }

  struct json_object* rparams = NULL;
  struct json_object* jlid = NULL;
  struct json_object* jloc = NULL;
  if (!json_object_object_get_ex(reply, "parameters", &rparams) || !rparams ||
      !json_object_object_get_ex(rparams, "leaseId", &jlid) || !jlid ||
      !json_object_is_type(jlid, json_type_string) ||
      !json_object_object_get_ex(rparams, "ruleLocation", &jloc) || !jloc ||
      !json_object_is_type(jloc, json_type_int)) {
    json_object_put(reply);
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%s), AddFlow reply missing leaseId or ruleLocation\n", __func__, queue_lease);
    return -EPROTO;
  }

  instance_copy_str(out_lease, MT_VARLINK_ID_STR_LEN, json_object_get_string(jlid));
  if (out_rule_loc) *out_rule_loc = (int)json_object_get_int64(jloc);
  int trc = instance_track_lease_locked(ctx, out_lease);
  json_object_put(reply);
  pthread_mutex_unlock(&ctx->mutex);
  return trc;
}

int mt_instance_add_udp_filter(struct mtl_main_impl* impl, const char* queue_lease,
                               uint16_t dst_port, char out_lease[40]) {
  if (!impl || !impl->instance || !queue_lease || queue_lease[0] == '\0' ||
      dst_port == 0 || !out_lease)
    return -EINVAL;
  out_lease[0] = '\0';

  struct json_object* params = json_object_new_object();
  if (!params) return -ENOMEM;
  json_object_object_add(params, "queueLease", json_object_new_string(queue_lease));
  json_object_object_add(params, "destinationPort",
                         json_object_new_int64((int64_t)dst_port));

  struct mt_instance_ctx* ctx = impl->instance;
  pthread_mutex_lock(&ctx->mutex);
  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = instance_call_locked(impl, ctx, MT_VARLINK_MANAGER_INTERFACE ".AddUdpFilter",
                                params, NULL, 0, &reply, err_name, sizeof(err_name),
                                err_msg, sizeof(err_msg));
  json_object_put(params);
  if (rc < 0) {
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%s, port %u), AddUdpFilter fail %d (%s: %s)\n", __func__, queue_lease,
        dst_port, rc, err_name, err_msg);
    return rc;
  }

  struct json_object* rparams = NULL;
  struct json_object* jlid = NULL;
  if (!json_object_object_get_ex(reply, "parameters", &rparams) || !rparams ||
      !json_object_object_get_ex(rparams, "leaseId", &jlid) || !jlid ||
      !json_object_is_type(jlid, json_type_string)) {
    json_object_put(reply);
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%s), AddUdpFilter reply missing leaseId\n", __func__, queue_lease);
    return -EPROTO;
  }

  instance_copy_str(out_lease, MT_VARLINK_ID_STR_LEN, json_object_get_string(jlid));
  int trc = instance_track_lease_locked(ctx, out_lease);
  json_object_put(reply);
  pthread_mutex_unlock(&ctx->mutex);
  return trc;
}

int mt_instance_ping(struct mtl_main_impl* impl, int64_t seq) {
  if (!impl || !impl->instance || seq < 0) return -EINVAL;

  struct mt_instance_ctx* ctx = impl->instance;
  struct json_object* params = json_object_new_object();
  if (!params) return -ENOMEM;
  json_object_object_add(params, "sequence", json_object_new_int64(seq));

  pthread_mutex_lock(&ctx->mutex);
  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = instance_call_locked(impl, ctx, MT_VARLINK_MANAGER_INTERFACE ".Ping", params,
                                NULL, 0, &reply, err_name, sizeof(err_name), err_msg,
                                sizeof(err_msg));
  json_object_put(params);
  if (rc < 0) {
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%" PRId64 "), Ping fail %d (%s: %s)\n", __func__, seq, rc, err_name, err_msg);
    return rc;
  }

  struct json_object* rparams = NULL;
  struct json_object* jseq = NULL;
  struct json_object* jmono = NULL;
  if (!json_object_object_get_ex(reply, "parameters", &rparams) || !rparams ||
      !json_object_object_get_ex(rparams, "sequence", &jseq) || !jseq ||
      !json_object_is_type(jseq, json_type_int) || json_object_get_int64(jseq) != seq ||
      !json_object_object_get_ex(rparams, "serverMonotonicUsec", &jmono) || !jmono ||
      !json_object_is_type(jmono, json_type_int)) {
    json_object_put(reply);
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(%" PRId64 "), invalid Ping reply\n", __func__, seq);
    return -EPROTO;
  }

  json_object_put(reply);
  pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

int mt_instance_heartbeat(struct mtl_main_impl* impl, const char* client_state) {
  if (!impl || !impl->instance) return -EINVAL;

  struct mt_instance_ctx* ctx = impl->instance;
  pthread_mutex_lock(&ctx->mutex);
  int64_t seq = ++ctx->hb_seq;

  struct json_object* params = json_object_new_object();
  if (!params) {
    pthread_mutex_unlock(&ctx->mutex);
    return -ENOMEM;
  }
  json_object_object_add(params, "sequence", json_object_new_int64(seq));
  if (client_state && client_state[0] != '\0') {
    json_object_object_add(params, "clientState", json_object_new_string(client_state));
  }

  struct json_object* reply = NULL;
  char err_name[128];
  char err_msg[256];
  int rc = instance_call_locked(impl, ctx, MT_VARLINK_MANAGER_INTERFACE ".Heartbeat",
                                params, NULL, 0, &reply, err_name, sizeof(err_name),
                                err_msg, sizeof(err_msg));
  json_object_put(params);
  if (rc < 0) {
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(seq %" PRId64 "), Heartbeat fail %d (%s: %s)\n", __func__, seq, rc, err_name,
        err_msg);
    return rc;
  }

  struct json_object* rparams = NULL;
  struct json_object* jseq = NULL;
  struct json_object* jstatus = NULL;
  struct json_object* jdl = NULL;
  if (!json_object_object_get_ex(reply, "parameters", &rparams) || !rparams ||
      !json_object_object_get_ex(rparams, "sequence", &jseq) || !jseq ||
      !json_object_is_type(jseq, json_type_int) || json_object_get_int64(jseq) != seq ||
      !json_object_object_get_ex(rparams, "status", &jstatus) || !jstatus ||
      !json_object_is_type(jstatus, json_type_string) ||
      !json_object_object_get_ex(rparams, "nextDeadlineMs", &jdl) || !jdl ||
      !json_object_is_type(jdl, json_type_int)) {
    json_object_put(reply);
    pthread_mutex_unlock(&ctx->mutex);
    err("%s(seq %" PRId64 "), invalid Heartbeat reply\n", __func__, seq);
    return -EPROTO;
  }

  json_object_put(reply);
  pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

bool mtl_is_manager_alive(void) {
  const char* sock_path = instance_resolve_sock_path();
  int fd = mt_varlink_connect_unix(sock_path, MT_INSTANCE_ALIVE_TIMEOUT_MS);
  if (fd < 0) return false;
  if (instance_verify_server_peercred(fd, sock_path) < 0) {
    close(fd);
    return false;
  }

  struct json_object* reply = NULL;
  int rc = mt_varlink_call(fd, MT_VARLINK_SERVICE_INTERFACE ".GetInfo", NULL, NULL, 0,
                           MT_INSTANCE_ALIVE_TIMEOUT_MS, &reply, NULL, 0, NULL, 0);
  if (reply) json_object_put(reply);
  close(fd);
  return rc == 0;
}

#else /* Windows stubs */

int mt_instance_connect(struct mtl_init_params* p, struct mt_instance_ctx** out_ctx) {
  MTL_MAY_UNUSED(p);
  if (out_ctx) *out_ctx = NULL;
  return 0;
}

void mt_instance_disconnect(struct mt_instance_ctx* ctx) {
  MTL_MAY_UNUSED(ctx);
}

int mt_instance_uinit(struct mtl_main_impl* impl) {
  MTL_MAY_UNUSED(impl);
  return 0;
}

struct mt_instance_ctx* mt_instance_create_from_fd(int sock_fd, const char* sock_path,
                                                   const char* client_name) {
  MTL_MAY_UNUSED(sock_fd);
  MTL_MAY_UNUSED(sock_path);
  MTL_MAY_UNUSED(client_name);
  return NULL;
}

int mt_instance_get_cpu(struct mtl_main_impl* impl, uint32_t phys_cpu,
                        char lease_id_out[40]) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(phys_cpu);
  MTL_MAY_UNUSED(lease_id_out);
  return -ENOTSUP;
}

int mt_instance_release_lease(struct mtl_main_impl* impl, const char* lease_id) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(lease_id);
  return -ENOTSUP;
}

int mt_instance_get_queue(struct mtl_main_impl* impl, const char* if_name,
                          uint16_t* out_queue_id, char out_lease[40]) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(if_name);
  MTL_MAY_UNUSED(out_queue_id);
  MTL_MAY_UNUSED(out_lease);
  return -ENOTSUP;
}

int mt_instance_attach_xsk(struct mtl_main_impl* impl, const char* queue_lease,
                           int xsk_fd, char out_lease[40]) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(queue_lease);
  MTL_MAY_UNUSED(xsk_fd);
  MTL_MAY_UNUSED(out_lease);
  return -ENOTSUP;
}

int mt_instance_add_flow(struct mtl_main_impl* impl, const char* queue_lease,
                         uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                         uint16_t dst_port, char out_lease[40], int* out_rule_loc) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(queue_lease);
  MTL_MAY_UNUSED(src_ip);
  MTL_MAY_UNUSED(dst_ip);
  MTL_MAY_UNUSED(src_port);
  MTL_MAY_UNUSED(dst_port);
  MTL_MAY_UNUSED(out_lease);
  MTL_MAY_UNUSED(out_rule_loc);
  return -ENOTSUP;
}

int mt_instance_add_udp_filter(struct mtl_main_impl* impl, const char* queue_lease,
                               uint16_t dst_port, char out_lease[40]) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(queue_lease);
  MTL_MAY_UNUSED(dst_port);
  MTL_MAY_UNUSED(out_lease);
  return -ENOTSUP;
}

int mt_instance_ping(struct mtl_main_impl* impl, int64_t seq) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(seq);
  return -ENOTSUP;
}

int mt_instance_heartbeat(struct mtl_main_impl* impl, const char* client_state) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(client_state);
  return -ENOTSUP;
}

bool mtl_is_manager_alive(void) {
  return false;
}

#endif
