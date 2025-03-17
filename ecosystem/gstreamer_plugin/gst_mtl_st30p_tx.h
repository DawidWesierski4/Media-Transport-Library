/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2020 Niels De Graef <niels.degraef@gmail.com>
 * Copyright (C) 2024 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GST_MTL_ST30P_TX_H__
#define __GST_MTL_ST30P_TX_H__

#include "gst_mtl_common.h"

G_BEGIN_DECLS

#define GST_TYPE_MTL_ST30P_TX (gst_mtl_st30p_tx_get_type())
G_DECLARE_FINAL_TYPE(Gst_Mtl_St30p_Tx, gst_mtl_st30p_tx, GST, MTL_ST30P_TX, GstAudioSink)

struct _Gst_Mtl_St30p_Tx {
  GstAudioSink element;
  mtl_handle mtl_lib_handle;
  st30p_tx_handle tx_handle;
  guint frame_size;

  gboolean session_ready;
  pthread_mutex_t session_mutex;
  pthread_t session_thread;

  /*
   * Handles incomplete frame buffers when their size does not match the expected size.
   */
  struct st30_frame* cur_frame;
  guint cur_frame_available_size;

  /* arguments */
  guint retry_frame;
  guint log_level;
  GeneralArgs generalArgs;  /* imtl initialization device */
  SessionPortArgs portArgs; /* imtl tx session */
  guint framebuffer_num;
  gchar ptime[MTL_PORT_MAX_LEN];
  gboolean async_session_create;
};

G_END_DECLS

#endif /* __GST_MTL_ST30P_TX_H__ */
