/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the MtlManager IPC unit tests: the instance socket is one end of a
 * socketpair whose peer has already closed, as after the manager exits.
 */

#ifndef TESTS_UNIT_MAIN_MT_INSTANCE_HARNESS_H
#define TESTS_UNIT_MAIN_MT_INSTANCE_HARNESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_instance_ctx ut_instance_ctx;

/* A main handle connected to a manager that has gone away; NULL on failure. */
ut_instance_ctx* ut_instance_create_orphan(void);
/* Connects to the configured manager socket (MTL_MANAGER_SOCK) via mt_instance_connect.
 * Returns 0 on success (with *out_ctx set or NULL in standalone mode), <0 on error. */
int ut_instance_connect(ut_instance_ctx** out_ctx);
void ut_instance_destroy(ut_instance_ctx* ctx);

int ut_instance_get_cpu(ut_instance_ctx* ctx, uint32_t phys_cpu, char lease_id_out[40]);
int ut_instance_release_lease(ut_instance_ctx* ctx, const char* lease_id);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_MAIN_MT_INSTANCE_HARNESS_H */
