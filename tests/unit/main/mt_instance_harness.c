/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "main/mt_instance_harness.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#undef MTL_HAS_USDT
#include "mt_instance.h"
#include "mt_main.h"

struct ut_instance_ctx {
  struct mtl_main_impl impl;
};

ut_instance_ctx* ut_instance_create_orphan(void) {
  int fds[2];
  ut_instance_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0) {
    free(ctx);
    return NULL;
  }
  close(fds[1]);
  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.instance = mt_instance_create_from_fd(fds[0], "", "ut-orphan");
  if (!ctx->impl.instance) {
    close(fds[0]);
    free(ctx);
    return NULL;
  }
  return ctx;
}

int ut_instance_connect(ut_instance_ctx** out_ctx) {
  if (!out_ctx) return -EINVAL;
  *out_ctx = NULL;

  struct mt_instance_ctx* inst = NULL;
  int rc = mt_instance_connect(NULL, &inst);
  if (rc < 0) return rc;
  if (!inst) return 0; /* Standalone / shm fallback mode */

  ut_instance_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    mt_instance_disconnect(inst);
    return -ENOMEM;
  }
  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.instance = inst;
  *out_ctx = ctx;
  return 0;
}

void ut_instance_destroy(ut_instance_ctx* ctx) {
  if (!ctx) return;
  mt_instance_uinit(&ctx->impl);
  free(ctx);
}

int ut_instance_get_cpu(ut_instance_ctx* ctx, uint32_t phys_cpu, char lease_id_out[40]) {
  return mt_instance_get_cpu(&ctx->impl, phys_cpu, lease_id_out);
}

int ut_instance_release_lease(ut_instance_ctx* ctx, const char* lease_id) {
  return mt_instance_release_lease(&ctx->impl, lease_id);
}
