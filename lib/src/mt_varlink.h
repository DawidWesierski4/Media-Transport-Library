/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _MT_LIB_VARLINK_HEAD_H_
#define _MT_LIB_VARLINK_HEAD_H_

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_VARLINK_MAX_FRAME_BYTES 65536u
#define MT_VARLINK_MAX_LARGE_FRAME_BYTES (4u * 1024u * 1024u)
#define MT_VARLINK_MAX_JSON_DEPTH 16
#define MT_VARLINK_MAX_METHOD_LEN 256u
#define MT_VARLINK_MAX_FDS 1u
#define MT_VARLINK_MAX_FD_ATTACHES 16u
#define MT_VARLINK_ID_STR_LEN 40u

#define MT_VARLINK_DEFAULT_SOCK_PATH "/run/imtl/manager2.sock"
#define MT_VARLINK_DEFAULT_ADMIN_SOCK_PATH "/run/imtl/admin.sock"
#define MT_VARLINK_SERVICE_INTERFACE "org.varlink.service"
#define MT_VARLINK_MANAGER_INTERFACE "org.openvisualcloud.mtl.Manager"

#ifndef WINDOWSENV

typedef struct mt_varlink_frame {
  char* json_text; /* NUL-terminated UTF-8 JSON object */
  size_t json_len;
  int fds[MT_VARLINK_MAX_FDS];
  size_t fd_count;
} mt_varlink_frame_t;

typedef struct mt_varlink_fd_attach {
  size_t byte_offset; /* Exact byte-0 offset of owning frame inside reader buffer */
  int fds[MT_VARLINK_MAX_FDS];
  size_t fd_count;
} mt_varlink_fd_attach_t;

typedef struct mt_varlink_reader {
  uint8_t buf[MT_VARLINK_MAX_FRAME_BYTES];
  uint8_t* dyn_buf;
  size_t dyn_cap;
  size_t max_frame_bytes;
  size_t len;
  mt_varlink_fd_attach_t fd_attaches[MT_VARLINK_MAX_FD_ATTACHES];
  size_t fd_attach_count;
} mt_varlink_reader_t;

typedef struct mt_varlink_out_msg {
  uint8_t* data; /* Heap buffer including trailing NUL */
  size_t len;
  size_t sent;
  int fds[MT_VARLINK_MAX_FDS];
  size_t fd_count;
  bool fds_sent;
  struct mt_varlink_out_msg* next;
} mt_varlink_out_msg_t;

typedef struct mt_varlink_writer {
  mt_varlink_out_msg_t* head;
  mt_varlink_out_msg_t* tail;
  size_t queued_bytes;
  size_t queued_messages;
  size_t max_bytes;
  size_t max_messages;
} mt_varlink_writer_t;

void mt_varlink_frame_init(mt_varlink_frame_t* frame);
void mt_varlink_frame_clear(mt_varlink_frame_t* frame);

void mt_varlink_reader_init(mt_varlink_reader_t* r);
void mt_varlink_reader_set_max_frame(mt_varlink_reader_t* r, size_t max_bytes);
void mt_varlink_reader_reset(mt_varlink_reader_t* r);

/* Non-blocking recvmsg(MSG_CMSG_CLOEXEC | MSG_DONTWAIT).
 * Returns >0 bytes read, 0 on EOF, -EAGAIN on would-block, -EMSGSIZE if frame
 * exceeds cap, -EPROTO on mid-frame, zero-byte, or coalesced SCM_RIGHTS. */
int mt_varlink_reader_recv(mt_varlink_reader_t* r, int sock_fd);

/* In-memory feed for deterministic unit tests. Takes ownership of fds on both
 * success and error. */
int mt_varlink_reader_feed(mt_varlink_reader_t* r, const uint8_t* data, size_t len,
                           const int* fds, size_t fd_count);

/* Extracts next NUL-delimited frame and transfers ownership of out->fds.
 * Returns 1 if extracted, 0 if incomplete, <0 on error. */
int mt_varlink_reader_pop(mt_varlink_reader_t* r, mt_varlink_frame_t* out);

void mt_varlink_writer_init(mt_varlink_writer_t* w, size_t max_bytes,
                            size_t max_messages);
void mt_varlink_writer_clear(mt_varlink_writer_t* w);

/* Duplicates passed FDs with F_DUPFD_CLOEXEC. Caller retains ownership of fds. */
int mt_varlink_writer_enqueue(mt_varlink_writer_t* w, const char* json_str,
                              size_t json_len, const int* fds, size_t fd_count);

/* Enqueues a large outbound reply (0 FDs) up to MT_VARLINK_MAX_LARGE_FRAME_BYTES. */
int mt_varlink_writer_enqueue_large(mt_varlink_writer_t* w, const char* json_str,
                                    size_t json_len);

/* Non-blocking sendmsg(MSG_NOSIGNAL | MSG_DONTWAIT). */
int mt_varlink_writer_flush(mt_varlink_writer_t* w, int sock_fd);
bool mt_varlink_writer_has_pending(const mt_varlink_writer_t* w);

/* Strict UTF-8 + pre-parse JSON validator + json-c depth <= 16 object parser.
 * Rejects \u0000, raw ASCII control bytes 0x01..0x1F, unpaired UTF-16 surrogates,
 * duplicate keys in the same object, and trailing garbage. */
int mt_varlink_parse_json_object(const char* json_text, size_t len,
                                 struct json_object** out_obj);

/* Strict Varlink request envelope check. */
int mt_varlink_validate_request_envelope(struct json_object* obj, const char** out_method,
                                         struct json_object** out_params, bool* out_more,
                                         bool* out_oneway, bool* out_upgrade);

/* Monotonic clock and saturating deadline helper (ISSUE-35). */
int64_t mt_varlink_mono_ns(void);
int64_t mt_varlink_deadline_after_ms(int64_t timeout_ms);

/* Connects a non-blocking CLOEXEC AF_UNIX stream socket with bounded timeout.
 * Returns socket fd >= 0 on success, or negative errno on failure. */
int mt_varlink_connect_unix(const char* sock_path, int64_t timeout_ms);

/* Sends a single Varlink request and receives its reply within timeout_ms.
 * Returns 0 on success with *out_reply owning the parsed JSON reply object.
 * Returns -EPIPE / -ECONNRESET / -ENOTCONN / -ETIMEDOUT / -EPROTO on transport
 * or framing error. Returns -EBUSY / -EACCES / -ENOENT / -EINVAL / -EREMOTEIO
 * on Varlink error reply (filling err_name_out / err_msg_out if provided). */
int mt_varlink_call(int sock_fd, const char* method, struct json_object* params,
                    const int* fds, size_t fd_count, int64_t timeout_ms,
                    struct json_object** out_reply, char* err_name_out,
                    size_t err_name_sz, char* err_msg_out, size_t err_msg_sz);

#endif /* !WINDOWSENV */

#ifdef __cplusplus
}
#endif

#endif /* _MT_LIB_VARLINK_HEAD_H_ */
