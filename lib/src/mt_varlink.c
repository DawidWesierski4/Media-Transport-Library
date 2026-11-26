/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mt_varlink.h"

#ifndef WINDOWSENV

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void close_fd_array(const int* fds, size_t count) {
  if (!fds) return;
  for (size_t i = 0; i < count; ++i) {
    if (fds[i] >= 0) close(fds[i]);
  }
}

static void free_out_msg(mt_varlink_out_msg_t* msg) {
  if (!msg) return;
  for (size_t i = 0; i < msg->fd_count && i < MT_VARLINK_MAX_FDS; ++i) {
    if (msg->fds[i] >= 0) {
      close(msg->fds[i]);
      msg->fds[i] = -1;
    }
  }
  free(msg->data);
  free(msg);
}

static void copy_str_trunc(char* dst, size_t dst_sz, const char* src) {
  if (!dst || dst_sz == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strnlen(src, dst_sz - 1u);
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static bool is_valid_utf8(const uint8_t* s, size_t len) {
  size_t i = 0;
  while (i < len) {
    uint8_t b0 = s[i];
    if (b0 <= 0x7Fu) {
      if (b0 == 0x00u) return false;
      i += 1u;
    } else if (b0 >= 0xC2u && b0 <= 0xDFu) {
      if (i + 1u >= len) return false;
      if ((s[i + 1u] & 0xC0u) != 0x80u) return false;
      i += 2u;
    } else if (b0 == 0xE0u) {
      if (i + 2u >= len) return false;
      if (s[i + 1u] < 0xA0u || s[i + 1u] > 0xBFu || (s[i + 2u] & 0xC0u) != 0x80u)
        return false;
      i += 3u;
    } else if ((b0 >= 0xE1u && b0 <= 0xECu) || (b0 >= 0xEEu && b0 <= 0xEFu)) {
      if (i + 2u >= len) return false;
      if ((s[i + 1u] & 0xC0u) != 0x80u || (s[i + 2u] & 0xC0u) != 0x80u) return false;
      i += 3u;
    } else if (b0 == 0xEDu) {
      if (i + 2u >= len) return false;
      if (s[i + 1u] < 0x80u || s[i + 1u] > 0x9Fu || (s[i + 2u] & 0xC0u) != 0x80u)
        return false;
      i += 3u;
    } else if (b0 == 0xF0u) {
      if (i + 3u >= len) return false;
      if (s[i + 1u] < 0x90u || s[i + 1u] > 0xBFu || (s[i + 2u] & 0xC0u) != 0x80u ||
          (s[i + 3u] & 0xC0u) != 0x80u)
        return false;
      i += 4u;
    } else if (b0 >= 0xF1u && b0 <= 0xF3u) {
      if (i + 3u >= len) return false;
      if ((s[i + 1u] & 0xC0u) != 0x80u || (s[i + 2u] & 0xC0u) != 0x80u ||
          (s[i + 3u] & 0xC0u) != 0x80u)
        return false;
      i += 4u;
    } else if (b0 == 0xF4u) {
      if (i + 3u >= len) return false;
      if (s[i + 1u] < 0x80u || s[i + 1u] > 0x8Fu || (s[i + 2u] & 0xC0u) != 0x80u ||
          (s[i + 3u] & 0xC0u) != 0x80u)
        return false;
      i += 4u;
    } else {
      return false;
    }
  }
  return true;
}

static bool check_json_depth(struct json_object* obj, int depth) {
  if (!obj) return true;
  enum json_type type = json_object_get_type(obj);
  if (type == json_type_object) {
    if (depth > MT_VARLINK_MAX_JSON_DEPTH) return false;
    json_object_object_foreach(obj, key, val) {
      (void)key;
      if (val) {
        enum json_type ct = json_object_get_type(val);
        if ((ct == json_type_object || ct == json_type_array) &&
            !check_json_depth(val, depth + 1))
          return false;
      }
    }
  } else if (type == json_type_array) {
    if (depth > MT_VARLINK_MAX_JSON_DEPTH) return false;
    size_t n = json_object_array_length(obj);
    for (size_t i = 0; i < n; ++i) {
      struct json_object* elem = json_object_array_get_idx(obj, i);
      if (elem) {
        enum json_type ct = json_object_get_type(elem);
        if ((ct == json_type_object || ct == json_type_array) &&
            !check_json_depth(elem, depth + 1))
          return false;
      }
    }
  }
  return true;
}

static size_t reader_max_bytes(const mt_varlink_reader_t* r) {
  return (r->max_frame_bytes > 0u) ? r->max_frame_bytes : MT_VARLINK_MAX_FRAME_BYTES;
}

static uint8_t* reader_buf(mt_varlink_reader_t* r) {
  return r->dyn_buf ? r->dyn_buf : r->buf;
}

static size_t reader_cap(const mt_varlink_reader_t* r) {
  return r->dyn_buf ? r->dyn_cap : MT_VARLINK_MAX_FRAME_BYTES;
}

static void reader_release_dyn_if_empty(mt_varlink_reader_t* r) {
  if (r->len == 0u && r->dyn_buf != NULL) {
    free(r->dyn_buf);
    r->dyn_buf = NULL;
    r->dyn_cap = 0u;
  }
}

static int reader_ensure_cap(mt_varlink_reader_t* r, size_t needed) {
  size_t limit = reader_max_bytes(r);
  if (needed > limit) return -EMSGSIZE;
  if (needed <= reader_cap(r)) return 0;

  size_t new_cap = r->dyn_buf ? r->dyn_cap : MT_VARLINK_MAX_FRAME_BYTES;
  while (new_cap < needed) {
    if (new_cap > limit / 2u) {
      new_cap = limit;
      break;
    }
    new_cap *= 2u;
  }
  if (new_cap > limit) new_cap = limit;

  if (!r->dyn_buf) {
    uint8_t* nb = (uint8_t*)malloc(new_cap);
    if (!nb) return -ENOMEM;
    if (r->len > 0u) memcpy(nb, r->buf, r->len);
    r->dyn_buf = nb;
    r->dyn_cap = new_cap;
  } else {
    uint8_t* nb = (uint8_t*)realloc(r->dyn_buf, new_cap);
    if (!nb) return -ENOMEM;
    r->dyn_buf = nb;
    r->dyn_cap = new_cap;
  }
  return 0;
}

static int reader_attach_fds(mt_varlink_reader_t* r, size_t append_offset, const int* fds,
                             size_t fd_count) {
  if (fd_count == 0u) return 0;
  if (!fds || fd_count > MT_VARLINK_MAX_FDS) {
    close_fd_array(fds, fd_count);
    return -EPROTO;
  }
  const uint8_t* buf = reader_buf(r);
  if (append_offset != 0u && buf[append_offset - 1u] != '\0') {
    close_fd_array(fds, fd_count);
    return -EPROTO;
  }
  if (r->fd_attach_count >= MT_VARLINK_MAX_FD_ATTACHES) {
    close_fd_array(fds, fd_count);
    return -EPROTO;
  }

  mt_varlink_fd_attach_t* slot = &r->fd_attaches[r->fd_attach_count++];
  slot->byte_offset = append_offset;
  slot->fd_count = fd_count;
  for (size_t i = 0; i < MT_VARLINK_MAX_FDS; ++i)
    slot->fds[i] = (i < fd_count) ? fds[i] : -1;
  return 0;
}

static void reader_discard_prefix(mt_varlink_reader_t* r, size_t consumed) {
  size_t out_idx = 0;
  for (size_t i = 0; i < r->fd_attach_count && i < MT_VARLINK_MAX_FD_ATTACHES; ++i) {
    mt_varlink_fd_attach_t* att = &r->fd_attaches[i];
    if (att->byte_offset < consumed) {
      close_fd_array(att->fds, att->fd_count);
      att->fd_count = 0;
      for (size_t j = 0; j < MT_VARLINK_MAX_FDS; ++j) att->fds[j] = -1;
    } else {
      att->byte_offset -= consumed;
      if (out_idx != i) r->fd_attaches[out_idx] = *att;
      ++out_idx;
    }
  }
  r->fd_attach_count = out_idx;

  uint8_t* buf = reader_buf(r);
  size_t remaining = r->len - consumed;
  if (remaining > 0u) memmove(buf, buf + consumed, remaining);
  r->len = remaining;
  reader_release_dyn_if_empty(r);
}

void mt_varlink_frame_init(mt_varlink_frame_t* frame) {
  if (!frame) return;
  memset(frame, 0, sizeof(*frame));
  for (size_t i = 0; i < MT_VARLINK_MAX_FDS; ++i) frame->fds[i] = -1;
}

void mt_varlink_frame_clear(mt_varlink_frame_t* frame) {
  if (!frame) return;
  for (size_t i = 0; i < frame->fd_count && i < MT_VARLINK_MAX_FDS; ++i) {
    if (frame->fds[i] >= 0) close(frame->fds[i]);
  }
  for (size_t i = 0; i < MT_VARLINK_MAX_FDS; ++i) frame->fds[i] = -1;
  frame->fd_count = 0;
  free(frame->json_text);
  frame->json_text = NULL;
  frame->json_len = 0;
}

void mt_varlink_reader_init(mt_varlink_reader_t* r) {
  if (!r) return;
  memset(r, 0, sizeof(*r));
  r->max_frame_bytes = MT_VARLINK_MAX_FRAME_BYTES;
  for (size_t i = 0; i < MT_VARLINK_MAX_FD_ATTACHES; ++i) {
    for (size_t j = 0; j < MT_VARLINK_MAX_FDS; ++j) r->fd_attaches[i].fds[j] = -1;
  }
}

void mt_varlink_reader_set_max_frame(mt_varlink_reader_t* r, size_t max_bytes) {
  if (!r) return;
  if (max_bytes == 0u) max_bytes = MT_VARLINK_MAX_FRAME_BYTES;
  if (max_bytes > MT_VARLINK_MAX_LARGE_FRAME_BYTES)
    max_bytes = MT_VARLINK_MAX_LARGE_FRAME_BYTES;
  r->max_frame_bytes = max_bytes;
}

void mt_varlink_reader_reset(mt_varlink_reader_t* r) {
  if (!r) return;
  for (size_t i = 0; i < r->fd_attach_count && i < MT_VARLINK_MAX_FD_ATTACHES; ++i) {
    for (size_t j = 0; j < r->fd_attaches[i].fd_count && j < MT_VARLINK_MAX_FDS; ++j) {
      if (r->fd_attaches[i].fds[j] >= 0) {
        close(r->fd_attaches[i].fds[j]);
        r->fd_attaches[i].fds[j] = -1;
      }
    }
    r->fd_attaches[i].fd_count = 0;
    r->fd_attaches[i].byte_offset = 0;
  }
  free(r->dyn_buf);
  r->dyn_buf = NULL;
  r->dyn_cap = 0u;
  r->len = 0;
  r->fd_attach_count = 0;
}

int mt_varlink_reader_feed(mt_varlink_reader_t* r, const uint8_t* data, size_t len,
                           const int* fds, size_t fd_count) {
  if (!r || (len > 0u && !data) || (fd_count > 0u && !fds)) {
    close_fd_array(fds, fd_count);
    return -EINVAL;
  }
  if (fd_count > 0u && len == 0u) {
    close_fd_array(fds, fd_count);
    return -EPROTO;
  }
  if (fd_count > 0u && len > 1u && memchr(data, '\0', len - 1u) != NULL) {
    close_fd_array(fds, fd_count);
    return -EPROTO;
  }
  size_t limit = reader_max_bytes(r);
  if (r->len > limit || len > limit - r->len) {
    close_fd_array(fds, fd_count);
    return -EMSGSIZE;
  }
  int cap_rc = reader_ensure_cap(r, r->len + len);
  if (cap_rc < 0) {
    close_fd_array(fds, fd_count);
    return cap_rc;
  }
  if (fd_count > 0u) {
    int rc = reader_attach_fds(r, r->len, fds, fd_count);
    if (rc < 0) return rc;
  }
  if (len > 0u) {
    uint8_t* buf = reader_buf(r);
    memcpy(buf + r->len, data, len);
    r->len += len;
  }
  return (int)len;
}

int mt_varlink_reader_recv(mt_varlink_reader_t* r, int sock_fd) {
  if (!r || sock_fd < 0) return -EINVAL;

  size_t limit = reader_max_bytes(r);
  uint8_t* buf = reader_buf(r);
  if (r->len >= limit) {
    if (memchr(buf, '\0', r->len) == NULL) return -EMSGSIZE;
    return -EAGAIN;
  }

  if (r->len >= reader_cap(r)) {
    size_t target = (r->len <= limit - MT_VARLINK_MAX_FRAME_BYTES)
                        ? (r->len + MT_VARLINK_MAX_FRAME_BYTES)
                        : limit;
    int cap_rc = reader_ensure_cap(r, target);
    if (cap_rc < 0) return cap_rc;
    buf = reader_buf(r);
  }

  size_t cap = reader_cap(r);
  if (cap > limit) cap = limit;
  size_t avail = cap - r->len;

  struct iovec iov;
  iov.iov_base = buf + r->len;
  iov.iov_len = avail;

  union {
    struct cmsghdr align;
    unsigned char buf[CMSG_SPACE(sizeof(int) * 16u)];
  } cmsg_u;
  memset(&cmsg_u, 0, sizeof(cmsg_u));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_u.buf;
  msg.msg_controllen = sizeof(cmsg_u.buf);

  ssize_t n;
  do {
    n = recvmsg(sock_fd, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
  } while (n < 0 && errno == EINTR);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -EAGAIN;
    return -errno;
  }

  int recvd_fds[16];
  size_t recvd_count = 0;
  bool too_many_fds = (msg.msg_flags & MSG_CTRUNC) != 0;

  for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      if (cmsg->cmsg_len < CMSG_LEN(0)) continue;
      size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
      const unsigned char* ptr = CMSG_DATA(cmsg);
      for (size_t off = 0; off + sizeof(int) <= data_len; off += sizeof(int)) {
        int fd = -1;
        memcpy(&fd, ptr + off, sizeof(int));
        if (fd < 0) continue;
        if (recvd_count < (sizeof(recvd_fds) / sizeof(recvd_fds[0]))) {
          recvd_fds[recvd_count++] = fd;
        } else {
          close(fd);
          too_many_fds = true;
        }
      }
    }
  }

  if (too_many_fds || recvd_count > MT_VARLINK_MAX_FDS) {
    close_fd_array(recvd_fds, recvd_count);
    return -EPROTO;
  }

  if (n == 0) {
    if (recvd_count > 0u) {
      close_fd_array(recvd_fds, recvd_count);
      return -EPROTO;
    }
    return 0;
  }

  if (recvd_count > 0u) {
    const uint8_t* new_slice = buf + r->len;
    /* Reject mid-frame FD attachment and NUL-coalesced SCM_RIGHTS frames (ISSUE-17). */
    if ((r->len > 0u && buf[r->len - 1u] != '\0') ||
        (n > 1 && memchr(new_slice, '\0', (size_t)n - 1u) != NULL)) {
      close_fd_array(recvd_fds, recvd_count);
      return -EPROTO;
    }
    int rc = reader_attach_fds(r, r->len, recvd_fds, recvd_count);
    if (rc < 0) return rc;
  }

  r->len += (size_t)n;
  return (int)n;
}

int mt_varlink_reader_pop(mt_varlink_reader_t* r, mt_varlink_frame_t* out) {
  if (!r || !out) return -EINVAL;
  if (r->len == 0u) return 0;

  uint8_t* buf = reader_buf(r);
  const uint8_t* nul = (const uint8_t*)memchr(buf, '\0', r->len);
  if (!nul) {
    if (r->len >= reader_max_bytes(r)) return -EMSGSIZE;
    return 0;
  }

  size_t frame_len = (size_t)(nul - buf);
  size_t consumed = frame_len + 1u;
  if (frame_len == 0u) {
    reader_discard_prefix(r, consumed);
    return -EPROTO;
  }

  char* copy = (char*)malloc(frame_len + 1u);
  if (!copy) return -ENOMEM;
  memcpy(copy, buf, frame_len);
  copy[frame_len] = '\0';

  mt_varlink_frame_init(out);
  out->json_text = copy;
  out->json_len = frame_len;

  bool proto_err = false;
  size_t out_idx = 0;
  for (size_t i = 0; i < r->fd_attach_count && i < MT_VARLINK_MAX_FD_ATTACHES; ++i) {
    mt_varlink_fd_attach_t* att = &r->fd_attaches[i];
    if (att->byte_offset == 0u) {
      if (out->fd_count == 0u && att->fd_count <= MT_VARLINK_MAX_FDS) {
        for (size_t j = 0; j < att->fd_count; ++j) {
          out->fds[j] = att->fds[j];
          att->fds[j] = -1;
        }
        out->fd_count = att->fd_count;
        att->fd_count = 0;
      } else {
        close_fd_array(att->fds, att->fd_count);
        att->fd_count = 0;
        proto_err = true;
      }
    } else if (att->byte_offset < consumed) {
      close_fd_array(att->fds, att->fd_count);
      att->fd_count = 0;
      proto_err = true;
    } else {
      att->byte_offset -= consumed;
      if (out_idx != i) r->fd_attaches[out_idx] = *att;
      ++out_idx;
    }
  }
  r->fd_attach_count = out_idx;

  size_t remaining = r->len - consumed;
  if (remaining > 0u) memmove(buf, buf + consumed, remaining);
  r->len = remaining;
  reader_release_dyn_if_empty(r);

  if (proto_err) {
    mt_varlink_frame_clear(out);
    return -EPROTO;
  }
  return 1;
}

void mt_varlink_writer_init(mt_varlink_writer_t* w, size_t max_bytes,
                            size_t max_messages) {
  if (!w) return;
  memset(w, 0, sizeof(*w));
  w->max_bytes = max_bytes;
  w->max_messages = max_messages;
}

void mt_varlink_writer_clear(mt_varlink_writer_t* w) {
  if (!w) return;
  mt_varlink_out_msg_t* cur = w->head;
  while (cur != NULL) {
    mt_varlink_out_msg_t* next = cur->next;
    free_out_msg(cur);
    cur = next;
  }
  w->head = NULL;
  w->tail = NULL;
  w->queued_bytes = 0;
  w->queued_messages = 0;
}

static int writer_enqueue_internal(mt_varlink_writer_t* w, const char* json_str,
                                   size_t json_len, const int* fds, size_t fd_count,
                                   size_t max_frame_bytes, bool allow_large) {
  if (!w || !json_str || json_len == 0u || (fd_count > 0u && !fds)) return -EINVAL;
  if (json_len >= max_frame_bytes) return -EMSGSIZE;
  if (fd_count > MT_VARLINK_MAX_FDS) return -EINVAL;
  if (memchr(json_str, '\0', json_len) != NULL) return -EINVAL;

  size_t total_len = json_len + 1u;
  size_t eff_max_bytes = w->max_bytes;
  if (allow_large && eff_max_bytes > 0u && eff_max_bytes < max_frame_bytes)
    eff_max_bytes = max_frame_bytes;
  if (eff_max_bytes > 0u &&
      (total_len > eff_max_bytes || w->queued_bytes > eff_max_bytes - total_len))
    return -ENOBUFS;
  if (w->max_messages > 0u && w->queued_messages >= w->max_messages) return -ENOBUFS;

  mt_varlink_out_msg_t* msg = (mt_varlink_out_msg_t*)calloc(1u, sizeof(*msg));
  if (!msg) return -ENOMEM;
  for (size_t i = 0; i < MT_VARLINK_MAX_FDS; ++i) msg->fds[i] = -1;

  msg->data = (uint8_t*)malloc(total_len);
  if (!msg->data) {
    free(msg);
    return -ENOMEM;
  }
  memcpy(msg->data, json_str, json_len);
  msg->data[json_len] = '\0';
  msg->len = total_len;

  for (size_t i = 0; i < fd_count; ++i) {
    if (fds[i] < 0) {
      free_out_msg(msg);
      return -EBADF;
    }
    int dup_fd = fcntl(fds[i], F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
      int err = errno;
      free_out_msg(msg);
      return -err;
    }
    msg->fds[i] = dup_fd;
    msg->fd_count = i + 1u;
  }

  if (w->tail) {
    w->tail->next = msg;
    w->tail = msg;
  } else {
    w->head = msg;
    w->tail = msg;
  }
  w->queued_bytes += total_len;
  w->queued_messages += 1u;
  return 0;
}

int mt_varlink_writer_enqueue(mt_varlink_writer_t* w, const char* json_str,
                              size_t json_len, const int* fds, size_t fd_count) {
  return writer_enqueue_internal(w, json_str, json_len, fds, fd_count,
                                 MT_VARLINK_MAX_FRAME_BYTES, false);
}

int mt_varlink_writer_enqueue_large(mt_varlink_writer_t* w, const char* json_str,
                                    size_t json_len) {
  return writer_enqueue_internal(w, json_str, json_len, NULL, 0,
                                 MT_VARLINK_MAX_LARGE_FRAME_BYTES, true);
}

int mt_varlink_writer_flush(mt_varlink_writer_t* w, int sock_fd) {
  if (!w || sock_fd < 0) return -EINVAL;
  if (!w->head) return 0;

  size_t total_sent = 0;
  while (w->head != NULL) {
    mt_varlink_out_msg_t* msg = w->head;
    struct iovec iov;
    iov.iov_base = msg->data + msg->sent;
    iov.iov_len = msg->len - msg->sent;

    union {
      struct cmsghdr align;
      unsigned char buf[CMSG_SPACE(sizeof(int) * MT_VARLINK_MAX_FDS)];
    } cmsg_u;
    memset(&cmsg_u, 0, sizeof(cmsg_u));

    struct msghdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;

    bool send_fds = (msg->fd_count > 0u && !msg->fds_sent);
    if (send_fds) {
      size_t fds_bytes = sizeof(int) * msg->fd_count;
      hdr.msg_control = cmsg_u.buf;
      hdr.msg_controllen = CMSG_SPACE(fds_bytes);
      struct cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(fds_bytes);
      memcpy(CMSG_DATA(cmsg), msg->fds, fds_bytes);
    }

    ssize_t n;
    do {
      n = sendmsg(sock_fd, &hdr, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return (total_sent > 0u) ? (int)total_sent : -EAGAIN;
      return -errno;
    }
    if (n == 0) return (total_sent > 0u) ? (int)total_sent : -EAGAIN;

    if (send_fds) {
      for (size_t i = 0; i < msg->fd_count && i < MT_VARLINK_MAX_FDS; ++i) {
        if (msg->fds[i] >= 0) {
          close(msg->fds[i]);
          msg->fds[i] = -1;
        }
      }
      msg->fd_count = 0;
      msg->fds_sent = true;
    }

    size_t sent_now = (size_t)n;
    msg->sent += sent_now;
    total_sent += sent_now;
    w->queued_bytes = (w->queued_bytes >= sent_now) ? (w->queued_bytes - sent_now) : 0u;

    if (msg->sent >= msg->len) {
      w->head = msg->next;
      if (!w->head) w->tail = NULL;
      if (w->queued_messages > 0u) w->queued_messages -= 1u;
      free_out_msg(msg);
    }
    if (total_sent >= (size_t)(INT_MAX / 2)) break;
  }
  return (int)total_sent;
}

bool mt_varlink_writer_has_pending(const mt_varlink_writer_t* w) {
  return w != NULL && w->head != NULL;
}

/* Strict RFC 8259 pre-parse scanner rejecting \u0000, unescaped control bytes,
 * unpaired UTF-16 surrogates, and duplicate keys within the same object. */
typedef struct {
  const uint8_t* s;
  size_t len;
  size_t pos;
} json_scanner_t;

typedef struct {
  uint8_t inline_buf[256];
  uint8_t* data;
  size_t len;
  size_t cap;
} key_buf_t;

static void key_buf_init(key_buf_t* kb) {
  kb->data = kb->inline_buf;
  kb->len = 0u;
  kb->cap = sizeof(kb->inline_buf);
}

static void key_buf_free(key_buf_t* kb) {
  if (kb->data != kb->inline_buf) free(kb->data);
  kb->data = kb->inline_buf;
  kb->len = 0u;
  kb->cap = sizeof(kb->inline_buf);
}

static int key_buf_push(key_buf_t* kb, uint8_t b) {
  if (kb->len >= kb->cap) {
    if (kb->cap > SIZE_MAX / 2u) return -ENOMEM;
    size_t new_cap = kb->cap * 2u;
    uint8_t* nb = NULL;
    if (kb->data == kb->inline_buf) {
      nb = (uint8_t*)malloc(new_cap);
      if (!nb) return -ENOMEM;
      memcpy(nb, kb->inline_buf, kb->len);
    } else {
      nb = (uint8_t*)realloc(kb->data, new_cap);
      if (!nb) return -ENOMEM;
    }
    kb->data = nb;
    kb->cap = new_cap;
  }
  kb->data[kb->len++] = b;
  return 0;
}

static int key_buf_push_utf8(key_buf_t* kb, uint32_t cp) {
  if (cp <= 0x7Fu) return key_buf_push(kb, (uint8_t)cp);
  if (cp <= 0x7FFu) {
    if (key_buf_push(kb, (uint8_t)(0xC0u | (cp >> 6))) != 0 ||
        key_buf_push(kb, (uint8_t)(0x80u | (cp & 0x3Fu))) != 0)
      return -ENOMEM;
    return 0;
  }
  if (cp <= 0xFFFFu) {
    if (key_buf_push(kb, (uint8_t)(0xE0u | (cp >> 12))) != 0 ||
        key_buf_push(kb, (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu))) != 0 ||
        key_buf_push(kb, (uint8_t)(0x80u | (cp & 0x3Fu))) != 0)
      return -ENOMEM;
    return 0;
  }
  if (key_buf_push(kb, (uint8_t)(0xF0u | (cp >> 18))) != 0 ||
      key_buf_push(kb, (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu))) != 0 ||
      key_buf_push(kb, (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu))) != 0 ||
      key_buf_push(kb, (uint8_t)(0x80u | (cp & 0x3Fu))) != 0)
    return -ENOMEM;
  return 0;
}

#define KEY_SET_INLINE_SLOTS 8u

typedef struct {
  uint64_t hash;
  size_t off;
  size_t len;
} key_entry_t;

typedef struct {
  uint8_t inline_arena[512];
  uint8_t* arena;
  size_t arena_len;
  size_t arena_cap;
  key_entry_t inline_entries[KEY_SET_INLINE_SLOTS];
  key_entry_t* table;
  size_t table_cap;
  size_t count;
} key_set_t;

static void key_set_init(key_set_t* ks) {
  ks->arena = ks->inline_arena;
  ks->arena_len = 0u;
  ks->arena_cap = sizeof(ks->inline_arena);
  ks->table = NULL;
  ks->table_cap = 0u;
  ks->count = 0u;
}

static void key_set_free(key_set_t* ks) {
  if (ks->arena != ks->inline_arena) free(ks->arena);
  free(ks->table);
  ks->arena = ks->inline_arena;
  ks->table = NULL;
}

static uint64_t fnv1a_hash(const uint8_t* data, size_t len) {
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= (uint64_t)data[i];
    h *= 1099511628211ULL;
  }
  return (h == 0ULL) ? 1ULL : h;
}

static bool key_set_entry_eq(const key_set_t* ks, const key_entry_t* ent,
                             const uint8_t* bytes, size_t len) {
  if (ent->len != len) return false;
  if (len == 0u) return true;
  return memcmp(ks->arena + ent->off, bytes, len) == 0;
}

static int key_set_grow_table(key_set_t* ks, size_t new_cap) {
  key_entry_t* nt = (key_entry_t*)calloc(new_cap, sizeof(*nt));
  if (!nt) return -ENOMEM;
  size_t mask = new_cap - 1u;
  if (ks->table == NULL) {
    for (size_t i = 0; i < ks->count; ++i) {
      size_t idx = (size_t)(ks->inline_entries[i].hash & (uint64_t)mask);
      while (nt[idx].hash != 0ULL) idx = (idx + 1u) & mask;
      nt[idx] = ks->inline_entries[i];
    }
  } else {
    for (size_t i = 0; i < ks->table_cap; ++i) {
      if (ks->table[i].hash == 0ULL) continue;
      size_t idx = (size_t)(ks->table[i].hash & (uint64_t)mask);
      while (nt[idx].hash != 0ULL) idx = (idx + 1u) & mask;
      nt[idx] = ks->table[i];
    }
    free(ks->table);
  }
  ks->table = nt;
  ks->table_cap = new_cap;
  return 0;
}

static int key_set_insert(key_set_t* ks, const uint8_t* bytes, size_t len) {
  uint64_t h = fnv1a_hash(bytes, len);
  if (ks->table == NULL) {
    for (size_t i = 0; i < ks->count; ++i) {
      if (ks->inline_entries[i].hash == h &&
          key_set_entry_eq(ks, &ks->inline_entries[i], bytes, len))
        return -EPROTO;
    }
    if (ks->count >= KEY_SET_INLINE_SLOTS) {
      int grc = key_set_grow_table(ks, 32u);
      if (grc != 0) return grc;
    }
  }

  size_t insert_idx = 0u;
  if (ks->table != NULL) {
    if ((ks->count + 1u) * 2u > ks->table_cap) {
      if (ks->table_cap > SIZE_MAX / 2u) return -ENOMEM;
      int grc = key_set_grow_table(ks, ks->table_cap * 2u);
      if (grc != 0) return grc;
    }
    size_t mask = ks->table_cap - 1u;
    size_t idx = (size_t)(h & (uint64_t)mask);
    while (ks->table[idx].hash != 0ULL) {
      if (ks->table[idx].hash == h && key_set_entry_eq(ks, &ks->table[idx], bytes, len))
        return -EPROTO;
      idx = (idx + 1u) & mask;
    }
    insert_idx = idx;
  }

  if (len > SIZE_MAX - ks->arena_len) return -ENOMEM;
  size_t needed = ks->arena_len + len;
  if (needed > ks->arena_cap) {
    size_t new_cap = ks->arena_cap;
    while (new_cap < needed) {
      if (new_cap > SIZE_MAX / 2u) return -ENOMEM;
      new_cap *= 2u;
    }
    uint8_t* na = NULL;
    if (ks->arena == ks->inline_arena) {
      na = (uint8_t*)malloc(new_cap);
      if (!na) return -ENOMEM;
      if (ks->arena_len > 0u) memcpy(na, ks->inline_arena, ks->arena_len);
    } else {
      na = (uint8_t*)realloc(ks->arena, new_cap);
      if (!na) return -ENOMEM;
    }
    ks->arena = na;
    ks->arena_cap = new_cap;
  }

  size_t off = ks->arena_len;
  if (len > 0u) {
    memcpy(ks->arena + off, bytes, len);
    ks->arena_len += len;
  }

  key_entry_t ent = {h, off, len};
  if (ks->table == NULL) {
    ks->inline_entries[ks->count++] = ent;
  } else {
    ks->table[insert_idx] = ent;
    ks->count++;
  }
  return 0;
}

static void skip_json_ws(json_scanner_t* sc) {
  while (sc->pos < sc->len) {
    uint8_t c = sc->s[sc->pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      sc->pos++;
    else
      break;
  }
}

static int hex_val(uint8_t c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
  return -1;
}

static int scan_hex4(json_scanner_t* sc, uint32_t* out_val) {
  if (sc->pos + 4u > sc->len) return -EPROTO;
  uint32_t v = 0u;
  for (size_t i = 0; i < 4u; ++i) {
    int hv = hex_val(sc->s[sc->pos++]);
    if (hv < 0) return -EPROTO;
    v = (v << 4) | (uint32_t)hv;
  }
  *out_val = v;
  return 0;
}

static int scan_json_string(json_scanner_t* sc, key_buf_t* out_kb) {
  if (sc->pos >= sc->len || sc->s[sc->pos] != '"') return -EPROTO;
  sc->pos++;

  while (sc->pos < sc->len) {
    uint8_t c = sc->s[sc->pos];
    if (c < 0x20u) return -EPROTO; /* Reject raw control bytes 0x00..0x1F (ISSUE-49). */
    if (c == '"') {
      sc->pos++;
      return 0;
    }
    if (c == '\\') {
      sc->pos++;
      if (sc->pos >= sc->len) return -EPROTO;
      uint8_t esc = sc->s[sc->pos++];
      uint8_t decoded = 0u;
      bool simple = true;
      switch (esc) {
        case '"':
          decoded = '"';
          break;
        case '\\':
          decoded = '\\';
          break;
        case '/':
          decoded = '/';
          break;
        case 'b':
          decoded = '\b';
          break;
        case 'f':
          decoded = '\f';
          break;
        case 'n':
          decoded = '\n';
          break;
        case 'r':
          decoded = '\r';
          break;
        case 't':
          decoded = '\t';
          break;
        case 'u':
          simple = false;
          break;
        default:
          return -EPROTO;
      }
      if (simple) {
        if (out_kb && key_buf_push(out_kb, decoded) != 0) return -ENOMEM;
      } else {
        uint32_t u1 = 0u;
        if (scan_hex4(sc, &u1) != 0) return -EPROTO;
        if (u1 == 0x0000u) return -EPROTO; /* Reject \u0000 (ISSUE-15/16). */
        if (u1 >= 0xDC00u && u1 <= 0xDFFFu) return -EPROTO; /* Lone low surrogate. */
        if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
          if (sc->pos + 6u > sc->len || sc->s[sc->pos] != '\\' ||
              sc->s[sc->pos + 1u] != 'u')
            return -EPROTO;
          sc->pos += 2u;
          uint32_t u2 = 0u;
          if (scan_hex4(sc, &u2) != 0 || u2 < 0xDC00u || u2 > 0xDFFFu) return -EPROTO;
          uint32_t cp = 0x10000u + (((u1 - 0xD800u) << 10) | (u2 - 0xDC00u));
          if (out_kb && key_buf_push_utf8(out_kb, cp) != 0) return -ENOMEM;
        } else {
          if (out_kb && key_buf_push_utf8(out_kb, u1) != 0) return -ENOMEM;
        }
      }
    } else {
      sc->pos++;
      if (out_kb && key_buf_push(out_kb, c) != 0) return -ENOMEM;
    }
  }
  return -EPROTO;
}

static int scan_json_number(json_scanner_t* sc) {
  if (sc->pos < sc->len && sc->s[sc->pos] == '-') sc->pos++;
  if (sc->pos >= sc->len) return -EPROTO;
  if (sc->s[sc->pos] == '0') {
    sc->pos++;
    if (sc->pos < sc->len && sc->s[sc->pos] >= '0' && sc->s[sc->pos] <= '9')
      return -EPROTO;
  } else if (sc->s[sc->pos] >= '1' && sc->s[sc->pos] <= '9') {
    sc->pos++;
    while (sc->pos < sc->len && sc->s[sc->pos] >= '0' && sc->s[sc->pos] <= '9') sc->pos++;
  } else {
    return -EPROTO;
  }
  if (sc->pos < sc->len && sc->s[sc->pos] == '.') {
    sc->pos++;
    if (sc->pos >= sc->len || sc->s[sc->pos] < '0' || sc->s[sc->pos] > '9')
      return -EPROTO;
    while (sc->pos < sc->len && sc->s[sc->pos] >= '0' && sc->s[sc->pos] <= '9') sc->pos++;
  }
  if (sc->pos < sc->len && (sc->s[sc->pos] == 'e' || sc->s[sc->pos] == 'E')) {
    sc->pos++;
    if (sc->pos < sc->len && (sc->s[sc->pos] == '+' || sc->s[sc->pos] == '-')) sc->pos++;
    if (sc->pos >= sc->len || sc->s[sc->pos] < '0' || sc->s[sc->pos] > '9')
      return -EPROTO;
    while (sc->pos < sc->len && sc->s[sc->pos] >= '0' && sc->s[sc->pos] <= '9') sc->pos++;
  }
  return 0;
}

static int scan_json_value(json_scanner_t* sc, int depth);

static int scan_json_object(json_scanner_t* sc, int depth) {
  if (depth > MT_VARLINK_MAX_JSON_DEPTH) return -EPROTO;
  if (sc->pos >= sc->len || sc->s[sc->pos] != '{') return -EPROTO;
  sc->pos++;
  skip_json_ws(sc);
  if (sc->pos < sc->len && sc->s[sc->pos] == '}') {
    sc->pos++;
    return 0;
  }

  key_set_t ks;
  key_buf_t kb;
  key_set_init(&ks);
  key_buf_init(&kb);

  int rc = 0;
  for (;;) {
    kb.len = 0u;
    rc = scan_json_string(sc, &kb);
    if (rc != 0) break;
    /* Reject duplicate keys within the same object scope (ISSUE-79). */
    rc = key_set_insert(&ks, kb.data, kb.len);
    if (rc != 0) break;
    skip_json_ws(sc);
    if (sc->pos >= sc->len || sc->s[sc->pos] != ':') {
      rc = -EPROTO;
      break;
    }
    sc->pos++;
    rc = scan_json_value(sc, depth);
    if (rc != 0) break;
    skip_json_ws(sc);
    if (sc->pos >= sc->len) {
      rc = -EPROTO;
      break;
    }
    if (sc->s[sc->pos] == ',') {
      sc->pos++;
      skip_json_ws(sc);
      continue;
    }
    if (sc->s[sc->pos] == '}') {
      sc->pos++;
      rc = 0;
      break;
    }
    rc = -EPROTO;
    break;
  }

  key_buf_free(&kb);
  key_set_free(&ks);
  return rc;
}

static int scan_json_array(json_scanner_t* sc, int depth) {
  if (depth > MT_VARLINK_MAX_JSON_DEPTH) return -EPROTO;
  if (sc->pos >= sc->len || sc->s[sc->pos] != '[') return -EPROTO;
  sc->pos++;
  skip_json_ws(sc);
  if (sc->pos < sc->len && sc->s[sc->pos] == ']') {
    sc->pos++;
    return 0;
  }

  for (;;) {
    int rc = scan_json_value(sc, depth);
    if (rc != 0) return rc;
    skip_json_ws(sc);
    if (sc->pos >= sc->len) return -EPROTO;
    if (sc->s[sc->pos] == ',') {
      sc->pos++;
      skip_json_ws(sc);
      continue;
    }
    if (sc->s[sc->pos] == ']') {
      sc->pos++;
      return 0;
    }
    return -EPROTO;
  }
}

static int scan_json_value(json_scanner_t* sc, int depth) {
  skip_json_ws(sc);
  if (sc->pos >= sc->len) return -EPROTO;
  uint8_t c = sc->s[sc->pos];
  if (c == '{') return scan_json_object(sc, depth + 1);
  if (c == '[') return scan_json_array(sc, depth + 1);
  if (c == '"') return scan_json_string(sc, NULL);
  if (c == 't') {
    if (sc->pos + 4u <= sc->len && memcmp(sc->s + sc->pos, "true", 4u) == 0) {
      sc->pos += 4u;
      return 0;
    }
    return -EPROTO;
  }
  if (c == 'f') {
    if (sc->pos + 5u <= sc->len && memcmp(sc->s + sc->pos, "false", 5u) == 0) {
      sc->pos += 5u;
      return 0;
    }
    return -EPROTO;
  }
  if (c == 'n') {
    if (sc->pos + 4u <= sc->len && memcmp(sc->s + sc->pos, "null", 4u) == 0) {
      sc->pos += 4u;
      return 0;
    }
    return -EPROTO;
  }
  if (c == '-' || (c >= '0' && c <= '9')) return scan_json_number(sc);
  return -EPROTO;
}

static int validate_json_object_strict(const char* json_text, size_t len) {
  json_scanner_t sc = {(const uint8_t*)json_text, len, 0u};
  skip_json_ws(&sc);
  if (sc.pos >= sc.len || sc.s[sc.pos] != '{') return -EPROTO;
  int rc = scan_json_object(&sc, 1);
  if (rc != 0) return rc;
  skip_json_ws(&sc);
  return (sc.pos == sc.len) ? 0 : -EPROTO;
}

int mt_varlink_parse_json_object(const char* json_text, size_t len,
                                 struct json_object** out_obj) {
  if (out_obj) *out_obj = NULL;
  if (!json_text || !out_obj || len == 0u || len > MT_VARLINK_MAX_LARGE_FRAME_BYTES)
    return -EINVAL;
  if (!is_valid_utf8((const uint8_t*)json_text, len)) return -EPROTO;
  int vrc = validate_json_object_strict(json_text, len);
  if (vrc != 0) return vrc;

  struct json_tokener* tok = json_tokener_new_ex(MT_VARLINK_MAX_JSON_DEPTH + 2);
  if (!tok) return -ENOMEM;

  int flags = JSON_TOKENER_STRICT;
#ifdef JSON_TOKENER_VALIDATE_UTF8
  flags |= JSON_TOKENER_VALIDATE_UTF8;
#endif
  json_tokener_set_flags(tok, flags);

  struct json_object* obj = json_tokener_parse_ex(tok, json_text, (int)len);
  enum json_tokener_error jerr = json_tokener_get_error(tok);
  size_t parse_end = json_tokener_get_parse_end(tok);
  json_tokener_free(tok);

  if (jerr != json_tokener_success || !obj) {
    if (obj) json_object_put(obj);
    return -EPROTO;
  }

  while (parse_end < len) {
    char c = json_text[parse_end];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++parse_end;
    } else {
      json_object_put(obj);
      return -EPROTO;
    }
  }

  if (!json_object_is_type(obj, json_type_object) || !check_json_depth(obj, 1)) {
    json_object_put(obj);
    return -EPROTO;
  }

  *out_obj = obj;
  return 0;
}

int mt_varlink_validate_request_envelope(struct json_object* obj, const char** out_method,
                                         struct json_object** out_params, bool* out_more,
                                         bool* out_oneway, bool* out_upgrade) {
  if (out_method) *out_method = NULL;
  if (out_params) *out_params = NULL;
  if (out_more) *out_more = false;
  if (out_oneway) *out_oneway = false;
  if (out_upgrade) *out_upgrade = false;
  if (!obj || !json_object_is_type(obj, json_type_object)) return -EINVAL;

  const char* method = NULL;
  struct json_object* params = NULL;
  bool more = false;
  bool oneway = false;
  bool upgrade = false;

  json_object_object_foreach(obj, key, val) {
    if (strcmp(key, "method") == 0) {
      if (!val || !json_object_is_type(val, json_type_string)) return -EINVAL;
      const char* m = json_object_get_string(val);
      int mlen = json_object_get_string_len(val);
      if (!m || mlen <= 0 || (size_t)mlen > MT_VARLINK_MAX_METHOD_LEN ||
          memchr(m, '\0', (size_t)mlen) != NULL)
        return -EINVAL;
      method = m;
    } else if (strcmp(key, "parameters") == 0) {
      if (!val || !json_object_is_type(val, json_type_object)) return -EINVAL;
      params = val;
    } else if (strcmp(key, "more") == 0) {
      if (!val || !json_object_is_type(val, json_type_boolean)) return -EINVAL;
      more = json_object_get_boolean(val) != 0;
    } else if (strcmp(key, "oneway") == 0) {
      if (!val || !json_object_is_type(val, json_type_boolean)) return -EINVAL;
      oneway = json_object_get_boolean(val) != 0;
    } else if (strcmp(key, "upgrade") == 0) {
      if (!val || !json_object_is_type(val, json_type_boolean)) return -EINVAL;
      upgrade = json_object_get_boolean(val) != 0;
    } else {
      return -EPROTO;
    }
  }

  if (!method || (more && oneway)) return -EINVAL;

  if (out_method) *out_method = method;
  if (out_params) *out_params = params;
  if (out_more) *out_more = more;
  if (out_oneway) *out_oneway = oneway;
  if (out_upgrade) *out_upgrade = upgrade;
  return 0;
}

int64_t mt_varlink_mono_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

int64_t mt_varlink_deadline_after_ms(int64_t timeout_ms) {
  int64_t now_ns = mt_varlink_mono_ns();
  if (timeout_ms <= 0) return now_ns;
  const int64_t max_ms = INT64_MAX / 1000000LL;
  if (timeout_ms >= max_ms) return INT64_MAX;
  int64_t delta_ns = timeout_ms * 1000000LL;
  if (now_ns > INT64_MAX - delta_ns) return INT64_MAX;
  return now_ns + delta_ns;
}

static int ns_to_poll_ms(int64_t rem_ns) {
  if (rem_ns <= 0) return 0;
  if (rem_ns >= (int64_t)INT_MAX * 1000000LL) return INT_MAX;
  int64_t ms = (rem_ns + 999999LL) / 1000000LL;
  if (ms <= 0) return 1;
  if (ms > (int64_t)INT_MAX) return INT_MAX;
  return (int)ms;
}

int mt_varlink_connect_unix(const char* sock_path, int64_t timeout_ms) {
  if (!sock_path || sock_path[0] == '\0') return -EINVAL;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  size_t plen = strlen(sock_path);
  if (plen >= sizeof(addr.sun_path)) return -ENAMETOOLONG;

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;

  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, sock_path, plen + 1u);

  int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
  if (rc == 0) return fd;
  if (errno != EINPROGRESS && errno != EAGAIN) {
    int err = errno;
    close(fd);
    return -err;
  }

  int64_t deadline = mt_varlink_deadline_after_ms(timeout_ms);
  for (;;) {
    int64_t rem = deadline - mt_varlink_mono_ns();
    if (rem <= 0) {
      close(fd);
      return -ETIMEDOUT;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int prc = poll(&pfd, 1, ns_to_poll_ms(rem));
    if (prc < 0) {
      if (errno == EINTR) continue;
      int err = errno;
      close(fd);
      return -err;
    }
    if (prc == 0) {
      close(fd);
      return -ETIMEDOUT;
    }
    int so_err = 0;
    socklen_t slen = sizeof(so_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) != 0) {
      int err = errno;
      close(fd);
      return -err;
    }
    if (so_err != 0) {
      close(fd);
      return -so_err;
    }
    return fd;
  }
}

static int map_varlink_error_to_errno(const char* err_name) {
  if (!err_name) return -EREMOTEIO;
  if (strstr(err_name, "ResourceBusy") != NULL) return -EBUSY;
  if (strstr(err_name, "AccessDenied") != NULL ||
      strstr(err_name, "PermissionDenied") != NULL)
    return -EACCES;
  if (strstr(err_name, "InvalidParameter") != NULL) return -EINVAL;
  if (strstr(err_name, "LeaseNotFound") != NULL ||
      strstr(err_name, "LeaseNotOwned") != NULL ||
      strstr(err_name, "SessionNotFound") != NULL)
    return -ENOENT;
  if (strstr(err_name, "MethodNotFound") != NULL ||
      strstr(err_name, "MethodNotImplemented") != NULL)
    return -ENOTSUP;
  return -EREMOTEIO;
}

int mt_varlink_call(int sock_fd, const char* method, struct json_object* params,
                    const int* fds, size_t fd_count, int64_t timeout_ms,
                    struct json_object** out_reply, char* err_name_out,
                    size_t err_name_sz, char* err_msg_out, size_t err_msg_sz) {
  if (out_reply) *out_reply = NULL;
  if (err_name_out && err_name_sz > 0u) err_name_out[0] = '\0';
  if (err_msg_out && err_msg_sz > 0u) err_msg_out[0] = '\0';
  if (sock_fd < 0 || !method || method[0] == '\0') return -EINVAL;

  struct json_object* req = json_object_new_object();
  if (!req) return -ENOMEM;
  struct json_object* jmethod = json_object_new_string(method);
  if (!jmethod || json_object_object_add(req, "method", jmethod) != 0) {
    if (jmethod) json_object_put(jmethod);
    json_object_put(req);
    return -ENOMEM;
  }
  struct json_object* jparams =
      params ? json_object_get(params) : json_object_new_object();
  if (!jparams || json_object_object_add(req, "parameters", jparams) != 0) {
    if (jparams) json_object_put(jparams);
    json_object_put(req);
    return -ENOMEM;
  }

  const char* serialized = json_object_to_json_string_ext(
      req, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
  if (!serialized) {
    json_object_put(req);
    return -ENOMEM;
  }
  size_t slen = strlen(serialized);

  mt_varlink_writer_t writer;
  mt_varlink_writer_init(&writer, MT_VARLINK_MAX_FRAME_BYTES * 2u, 4u);
  int rc = mt_varlink_writer_enqueue(&writer, serialized, slen, fds, fd_count);
  json_object_put(req);
  if (rc < 0) {
    mt_varlink_writer_clear(&writer);
    return rc;
  }

  int64_t deadline = mt_varlink_deadline_after_ms(timeout_ms);

  /* Flush request frame. */
  while (mt_varlink_writer_has_pending(&writer)) {
    int frc = mt_varlink_writer_flush(&writer, sock_fd);
    if (frc < 0) {
      if (frc != -EAGAIN) {
        mt_varlink_writer_clear(&writer);
        return frc;
      }
      int64_t rem = deadline - mt_varlink_mono_ns();
      if (rem <= 0) {
        mt_varlink_writer_clear(&writer);
        return -ETIMEDOUT;
      }
      struct pollfd pfd = {sock_fd, POLLOUT, 0};
      int prc = poll(&pfd, 1, ns_to_poll_ms(rem));
      if (prc < 0) {
        if (errno == EINTR) continue;
        int err = errno;
        mt_varlink_writer_clear(&writer);
        return -err;
      }
      if (prc == 0) {
        mt_varlink_writer_clear(&writer);
        return -ETIMEDOUT;
      }
    }
  }
  mt_varlink_writer_clear(&writer);

  /* Read reply frame up to 4 MiB (ISSUE-31). */
  mt_varlink_reader_t* reader = (mt_varlink_reader_t*)calloc(1u, sizeof(*reader));
  if (!reader) return -ENOMEM;
  mt_varlink_reader_init(reader);
  mt_varlink_reader_set_max_frame(reader, MT_VARLINK_MAX_LARGE_FRAME_BYTES);

  mt_varlink_frame_t frame;
  mt_varlink_frame_init(&frame);

  for (;;) {
    int pr = mt_varlink_reader_pop(reader, &frame);
    if (pr == 1) break;
    if (pr < 0) {
      mt_varlink_reader_reset(reader);
      free(reader);
      return pr;
    }

    int rrc = mt_varlink_reader_recv(reader, sock_fd);
    if (rrc == 0) {
      mt_varlink_reader_reset(reader);
      free(reader);
      return -EPIPE;
    }
    if (rrc < 0) {
      if (rrc != -EAGAIN) {
        mt_varlink_reader_reset(reader);
        free(reader);
        return rrc;
      }
      int64_t rem = deadline - mt_varlink_mono_ns();
      if (rem <= 0) {
        mt_varlink_reader_reset(reader);
        free(reader);
        return -ETIMEDOUT;
      }
      struct pollfd pfd = {sock_fd, POLLIN, 0};
      int prc = poll(&pfd, 1, ns_to_poll_ms(rem));
      if (prc < 0) {
        if (errno == EINTR) continue;
        int err = errno;
        mt_varlink_reader_reset(reader);
        free(reader);
        return -err;
      }
      if (prc == 0) {
        mt_varlink_reader_reset(reader);
        free(reader);
        return -ETIMEDOUT;
      }
    }
  }

  mt_varlink_reader_reset(reader);
  free(reader);

  struct json_object* reply = NULL;
  rc = mt_varlink_parse_json_object(frame.json_text, frame.json_len, &reply);
  mt_varlink_frame_clear(&frame);
  if (rc < 0) return rc;

  struct json_object* err_obj = NULL;
  if (json_object_object_get_ex(reply, "error", &err_obj) && err_obj &&
      json_object_is_type(err_obj, json_type_string)) {
    const char* err_name = json_object_get_string(err_obj);
    copy_str_trunc(err_name_out, err_name_sz, err_name);

    struct json_object* ep = NULL;
    if (json_object_object_get_ex(reply, "parameters", &ep) && ep &&
        json_object_is_type(ep, json_type_object)) {
      struct json_object* detail = NULL;
      if ((json_object_object_get_ex(ep, "message", &detail) ||
           json_object_object_get_ex(ep, "detail", &detail) ||
           json_object_object_get_ex(ep, "parameter", &detail)) &&
          detail && json_object_is_type(detail, json_type_string)) {
        copy_str_trunc(err_msg_out, err_msg_sz, json_object_get_string(detail));
      }
    }

    int mapped = map_varlink_error_to_errno(err_name);
    json_object_put(reply);
    return mapped;
  }

  if (out_reply)
    *out_reply = reply;
  else
    json_object_put(reply);
  return 0;
}

#endif /* !WINDOWSENV */
