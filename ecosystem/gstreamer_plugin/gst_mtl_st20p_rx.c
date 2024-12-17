/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
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

/**
 * SECTION:element-mtl_rx_src
 *
 * The mtl_rx_src element is a GStreamer src plugin designed to interface with
 * the Media Transport Library (MTL).
 * MTL is a software-based solution optimized for high-throughput, low-latency
 * transmission and reception of media data.
 *
 * It features an efficient user-space LibOS UDP stack specifically crafted for
 * media transport and includes a built-in  SMPTE ST 2110-compliant
 * implementation for Professional Media over Managed IP Networks.
 *
 * This element allows GStreamer pipelines to send media data using the MTL
 * framework, ensuring efficient and reliable media transport over IP networks.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>

#include "gst_mtl_st20p_rx.h"

GST_DEBUG_CATEGORY_STATIC(gst_mtl_st20p_rx_debug);
#define GST_CAT_DEFAULT gst_mtl_st20p_rx_debug
#ifndef GST_LICENSE
#define GST_LICENSE "LGPL"
#endif
#ifndef GST_API_VERSION
#define GST_API_VERSION "1.0"
#endif
#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "Media Transport Library st2110 st20 rx plugin"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#endif
#ifndef PACKAGE
#define PACKAGE "gst-mtl-rx-st20"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.19.0.1"
#endif

enum {
  PROP_0,
  PROP_SILENT,
  PROP_RX_DEV_ARGS_PORT,
  PROP_RX_DEV_ARGS_SIP,
  PROP_RX_DEV_ARGS_DMA_DEV,
  PROP_RX_PORT_PORT,
  PROP_RX_PORT_IP,
  PROP_RX_PORT_UDP_PORT,
  PROP_RX_PORT_PAYLOAD_TYPE,
  PROP_RX_PORT_RX_QUEUES,
  PROP_RX_FRAMERATE,
  PROP_RX_FRAMEBUFF_NUM,
  PROP_MAX
};

/* pad template */
static GstStaticPadTemplate gst_mtl_st20p_rx_src_pad_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-raw, "
                                            "format = (string) {v210, I422_10LE},"
                                            "width = (int) [64, 16384], "
                                            "height = (int) [64, 8704], "
                                            "framerate = (fraction) [0, MAX]"));

#define gst_mtl_st20p_rx_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(Gst_Mtl_St20p_Rx, gst_mtl_st20p_rx, GST_TYPE_BASE_SRC,
                        GST_DEBUG_CATEGORY_INIT(gst_mtl_st20p_rx_debug, "mtl_st20p_rx", 0,
                                                "MTL St2110 st20 transmission src"));

GST_ELEMENT_REGISTER_DEFINE(mtl_st20p_rx, "mtl_st20p_rx", GST_RANK_NONE, GST_TYPE_MTL_ST20P_RX);

static void gst_mtl_st20p_rx_set_property(GObject* object, guint prop_id,
                                       const GValue* value, GParamSpec* pspec);
static void gst_mtl_st20p_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                       GParamSpec* pspec);
static void gst_mtl_st20p_rx_finalize(GObject* object);

static gboolean gst_mtl_st20p_rx_src_event(GstPad* pad, GstObject* parent, GstEvent* event);
static GstFlowReturn gst_mtl_st20p_rx_chain(GstPad* pad, GstObject* parent, GstBuffer* buf);

static gboolean gst_mtl_st20p_rx_start(GstBaseSrc* basesrc);
static gboolean gst_mtl_st20p_rx_stop(GstBaseSrc* basesrc);


static void gst_mtl_st20p_rx_class_init(Gst_Mtl_St20p_RxClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstVideoSinkClass* gstvideosrcelement_class;

  gobject_class = G_OBJECT_CLASS(klass);
  gstelement_class = GST_ELEMENT_CLASS(klass);
  gstvideosrcelement_class = GST_VIDEO_SINK_CLASS(klass);

  gst_element_class_set_metadata(
      gstelement_class, "MtlRxSt20Sink", "Sink/Video",
      "MTL transmission plugin for SMPTE ST 2110-20 standard (uncompressed video)",
      "Dawid Wesierski <dawid.wesierski@intel.com>");

  gst_element_class_add_static_pad_template(gstelement_class,
                                            &gst_mtl_st20p_rx_src_pad_template);

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_finalize);
  gstvideosrcelement_class->parent_class.start = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_start);

  g_object_class_install_property(
      gobject_class, PROP_SILENT,
      g_param_spec_boolean("silent", "Silent", "Turn on silent mode.", FALSE,
                           G_PARAM_READWRITE));

  g_object_class_install_property(
      gobject_class, PROP_RX_DEV_ARGS_PORT,
      g_param_spec_string("dev-port", "DPDK device port",
                          "DPDK port for synchronous ST 2110-20 uncompressed"
                          "video transmission, bound to the VFIO DPDK driver. ",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_DEV_ARGS_SIP,
      g_param_spec_string("dev-ip", "Local device IP",
                          "Local IP address that the port will be "
                          "identified by. This is the address from which ARP "
                          "responses will be sent.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_DEV_ARGS_DMA_DEV,
      g_param_spec_string("dma-dev", "DPDK DMA port",
                          "DPDK port for the MTL direct memory functionality.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_PORT,
      g_param_spec_string("rx-port", "Transmission Device Port",
                          "DPDK device port initialized for the transmission.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_IP,
      g_param_spec_string("rx-ip", "Receiving node's IP",
                          "Receiving MTL node IP address.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_UDP_PORT,
      g_param_spec_uint("rx-udp-port", "Receiver's UDP port",
                        "Receiving MTL node UDP port.", 0, G_MAXUINT, 20000,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_PAYLOAD_TYPE,
      g_param_spec_uint("rx-payload-type", "ST 2110 payload type",
                        "SMPTE ST 2110 payload type.", 0, G_MAXUINT, 112,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_RX_QUEUES,
      g_param_spec_uint("rx-queues", "Number of RX queues",
                        "Number of RX queues to initialize in DPDK backend.", 0,
                        G_MAXUINT, 16, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_FRAMERATE,
      g_param_spec_uint("rx-fps", "Video framerate", "Framerate of the video.", 0,
                        G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_FRAMEBUFF_NUM,
      g_param_spec_uint("rx-framebuff-num", "Number of framebuffers",
                        "Number of framebuffers to be used for transmission.", 0,
                        G_MAXUINT, 3, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean gst_mtl_st20p_rx_start(GstBaseSrc* basesrc) {
  struct mtl_init_params mtl_init_params = {0};
  gint ret;

  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(basesrc);

  GST_DEBUG_OBJECT(src, "start");
  GST_DEBUG("Media Transport Initialization start");
  gst_base_src_set_async_enabled(basesrc, FALSE);

  /* mtl is already initialzied */
  if (src->mtl_lib_handle) {
    GST_INFO("Mtl already initialized");
    if (mtl_start(src->mtl_lib_handle)) {
      GST_ERROR("Failed to start MTL");
      return FALSE;
    }
    return TRUE;
  }

  strncpy(mtl_init_params.port[MTL_PORT_P], src->devArgs.port, MTL_PORT_MAX_LEN);

  ret = inet_pton(AF_INET, src->devArgs.local_ip_string,
                  mtl_init_params.sip_addr[MTL_PORT_P]);
  if (ret != 1) {
    GST_ERROR("%s, sip %s is not valid ip address\n", __func__,
              src->devArgs.local_ip_string);
    return false;
  }

  if (src->devArgs.rx_queues_cnt[MTL_PORT_P]) {
    mtl_init_params.rx_queues_cnt[MTL_PORT_P] = src->devArgs.rx_queues_cnt[MTL_PORT_P];
  } else {
    mtl_init_params.rx_queues_cnt[MTL_PORT_P] = 16;
  }

  mtl_init_params.rx_queues_cnt[MTL_PORT_P] = 0;
  mtl_init_params.num_ports++;

  mtl_init_params.flags |= MTL_FLAG_BIND_NUMA;
  if (src->silent) {
    mtl_init_params.log_level = MTL_LOG_LEVEL_ERROR;
  } else {
    mtl_init_params.log_level = MTL_LOG_LEVEL_INFO;
  }

  src->retry_frame = 10;

  mtl_init_params.flags |= MTL_FLAG_BIND_NUMA;

  if (src->devArgs.dma_dev) {
    strncpy(mtl_init_params.dma_dev_port[0], src->devArgs.dma_dev, MTL_PORT_MAX_LEN);
  }

  if (src->mtl_lib_handle) {
    GST_ERROR("MTL already initialized");
    return false;
  }

  src->mtl_lib_handle = mtl_init(&mtl_init_params);
  if (!src->mtl_lib_handle) {
    GST_ERROR("Could not initialize MTL");
    return false;
  }

  return true;
}

static void gst_mtl_st20p_rx_init(Gst_Mtl_St20p_Rx* src) {
  GstElement* element = GST_ELEMENT(src);
  GstPad* srcpad;

  srcpad = gst_element_get_static_pad(element, "src");
  if (!srcpad) {
    GST_ERROR_OBJECT(src, "Failed to get src pad from child element");
    return;
  }

  gst_pad_set_event_function(srcpad, GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_src_event));

  gst_pad_set_chain_function(srcpad, GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_chain));
}

static void gst_mtl_st20p_rx_set_property(GObject* object, guint prop_id,
                                       const GValue* value, GParamSpec* pspec) {
  Gst_Mtl_St20p_Rx* self = GST_MTL_ST20P_RX(object);

  switch (prop_id) {
    case PROP_SILENT:
      self->silent = g_value_get_boolean(value);
      break;
    case PROP_RX_DEV_ARGS_PORT:
      strncpy(self->devArgs.port, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_DEV_ARGS_SIP:
      strncpy(self->devArgs.local_ip_string, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_DEV_ARGS_DMA_DEV:
      strncpy(self->devArgs.dma_dev, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_PORT_PORT:
      strncpy(self->portArgs.port, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_PORT_IP:
      strncpy(self->portArgs.session_ip_string, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_PORT_UDP_PORT:
      self->portArgs.udp_port = g_value_get_uint(value);
      break;
    case PROP_RX_PORT_PAYLOAD_TYPE:
      self->portArgs.payload_type = g_value_get_uint(value);
      break;
    case PROP_RX_PORT_RX_QUEUES:
      self->devArgs.rx_queues_cnt[MTL_PORT_P] = g_value_get_uint(value);
      break;
    case PROP_RX_FRAMERATE:
      self->framerate = g_value_get_uint(value);
      break;
    case PROP_RX_FRAMEBUFF_NUM:
      self->framebuffer_num = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_mtl_st20p_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                       GParamSpec* pspec) {
  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean(value, src->silent);
      break;
    case PROP_RX_DEV_ARGS_PORT:
      g_value_set_string(value, src->devArgs.port);
      break;
    case PROP_RX_DEV_ARGS_SIP:
      g_value_set_string(value, src->devArgs.local_ip_string);
      break;
    case PROP_RX_DEV_ARGS_DMA_DEV:
      g_value_set_string(value, src->devArgs.dma_dev);
      break;
    case PROP_RX_PORT_PORT:
      g_value_set_string(value, src->portArgs.port);
      break;
    case PROP_RX_PORT_IP:
      g_value_set_string(value, src->portArgs.rx_ip_string);
      break;
    case PROP_RX_PORT_UDP_PORT:
      g_value_set_uint(value, src->portArgs.udp_port);
      break;
    case PROP_RX_PORT_PAYLOAD_TYPE:
      g_value_set_uint(value, src->portArgs.payload_type);
      break;
    case PROP_RX_PORT_RX_QUEUES:
      g_value_set_uint(value, src->devArgs.rx_queues_cnt[MTL_PORT_P]);
      break;
    case PROP_RX_FRAMERATE:
      g_value_set_uint(value, src->framerate);
      break;
    case PROP_RX_FRAMEBUFF_NUM:
      g_value_set_uint(value, src->framebuffer_num);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/*
 * Create MTL session rx handle and initialize the session with the parameters
 * from caps negotiated by the pipeline.
 */
static gboolean gst_mtl_st20p_rx_session_create(Gst_Mtl_St20p_Rx* src, GstCaps* caps) {
  GstVideoInfo* info;
  struct st20p_rx_ops ops_rx = {0};
  gint ret;

  if (!src->mtl_lib_handle) {
    GST_ERROR("MTL library not initialized");
    return FALSE;
  }

  info = gst_video_info_new_from_caps(caps);
  ops_rx.name = "st20src";
  ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_rx.width = info->width;
  ops_rx.height = info->height;
  ops_rx.transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops_rx.port.num_port = 1;
  ops_rx.flags |= ST20P_RX_FLAG_BLOCK_GET;

  if (src->framebuffer_num) {
    ops_rx.framebuff_cnt = src->framebuffer_num;
  } else {
    ops_rx.framebuff_cnt = 3;
  }

  if (info->interlace_mode == GST_VIDEO_INTERLACE_MODE_INTERLEAVED) {
    ops_rx.interlaced = true;
  } else if (info->interlace_mode) {
    GST_ERROR("Unsupported interlace mode");
    return FALSE;
  }

  if (!gst_mtl_common_parse_input_fmt(info, &ops_rx.input_fmt)) {
    GST_ERROR("Failed to parse input format");
    return FALSE;
  }

  if (src->framerate) {
    if (!gst_mtl_st20p_rx_parse_fps_code(src->framerate, &ops_rx.fps)) {
      GST_ERROR("Failed to parse custom ops_rx fps code %d", src->framerate);
      return FALSE;
    }
  } else if (!gst_mtl_st20p_rx_parse_fps(info, &ops_rx.fps)) {
    GST_ERROR("Failed to parse fps");
    return FALSE;
  }

  if (inet_pton(AF_INET, src->portArgs.rx_ip_string, ops_rx.port.dip_addr[MTL_PORT_P]) !=
      1) {
    GST_ERROR("Invalid destination IP address: %s", src->portArgs.rx_ip_string);
    return FALSE;
  }

  if (strlen(src->portArgs.port) == 0) {
    strncpy(ops_rx.port.port[MTL_PORT_P], src->devArgs.port, MTL_PORT_MAX_LEN);
  } else {
    strncpy(ops_rx.port.port[MTL_PORT_P], src->portArgs.port, MTL_PORT_MAX_LEN);
  }

  if ((src->portArgs.udp_port < 0) || (src->portArgs.udp_port > 0xFFFF)) {
    GST_ERROR("%s, invalid UDP port: %d\n", __func__, src->portArgs.udp_port);
    return FALSE;
  }

  ops_rx.port.udp_port[0] = src->portArgs.udp_port;

  if ((src->portArgs.payload_type < 0) || (src->portArgs.payload_type > 0x7F)) {
    GST_ERROR("%s, invalid payload_type: %d\n", __func__, src->portArgs.payload_type);
    return FALSE;
  }

  ops_rx.port.payload_type = src->portArgs.payload_type;
  gst_video_info_free(info);

  ret = mtl_start(src->mtl_lib_handle);
  if (ret < 0) {
    GST_ERROR("Failed to start MTL library");
    return FALSE;
  }

  src->rx_handle = st20p_rx_create(src->mtl_lib_handle, &ops_rx);
  if (!src->rx_handle) {
    GST_ERROR("Failed to create st20p rx handle");
    return FALSE;
  }

  src->frame_size = st20p_rx_frame_size(src->rx_handle);
  return TRUE;
}

static gboolean gst_mtl_st20p_rx_src_event(GstPad* pad, GstObject* parent,
                                         GstEvent* event) {
  Gst_Mtl_St20p_Rx* src;
  GstCaps* caps;
  gint ret;

  src = GST_MTL_ST20P_RX(parent);

  GST_LOG_OBJECT(src, "Received %s event: %" GST_PTR_FORMAT, GST_EVENT_TYPE_NAME(event),
                 event);

  ret = GST_EVENT_TYPE(event);

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_SEGMENT:
      if (!src->rx_handle) {
        GST_ERROR("Rx handle not initialized");
        return FALSE;
      }
      ret = gst_pad_event_default(pad, parent, event);
      break;
    case GST_EVENT_CAPS:
      gst_event_parse_caps(event, &caps);
      ret = gst_mtl_st20p_rx_session_create(src, caps);
      if (!ret) {
        GST_ERROR("Failed to create RX session");
        return FALSE;
      }
      ret = gst_pad_event_default(pad, parent, event);
      break;
    case GST_EVENT_EOS:
      gst_mtl_st20p_rx_stop(GST_BASE_SINK(src));
      ret = gst_pad_event_default(pad, parent, event);
      gst_element_post_message(GST_ELEMENT(src), gst_message_new_eos(GST_OBJECT(src)));
      break;
    default:
      ret = gst_pad_event_default(pad, parent, event);
      break;
  }

  return ret;
}

/*
 * Takes the buffer from the source pad and sends it to the mtl library via
 * frame buffers, supports incomplete frames. But buffers needs to add up to the
 * actual frame size.
 */
static GstFlowReturn gst_mtl_st20p_rx_chain(GstPad* pad, GstObject* parent, GstBuffer* buf) {
  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(parent);
  gint buffer_size = gst_buffer_get_size(buf);
  gint buffer_n = gst_buffer_n_memory(buf);
  struct st_frame* frame = NULL;
  gint frame_size = src->frame_size;
  GstMemory* gst_buffer_memory;
  GstMapInfo map_info;

  if (!src->rx_handle) {
    GST_ERROR("Rx handle not initialized");
    return GST_FLOW_ERROR;
  }

  if (buffer_size != frame_size) {
    GST_ERROR("Buffer size %d does not match frame size %d", buffer_size, frame_size);
    return GST_FLOW_ERROR;
  }

  for (int i = 0; i < buffer_n; i++) {
    gst_buffer_memory = gst_buffer_peek_memory(buf, i);

    if (!gst_memory_map(gst_buffer_memory, &map_info, GST_MAP_READ)) {
      GST_ERROR("Failed to map memory");
      return GST_FLOW_ERROR;
    }

    frame = st20p_rx_get_frame(src->rx_handle);
    if (!frame) {
      GST_ERROR("Failed to get frame");
      return GST_FLOW_ERROR;
    }

    mtl_memcpy(frame->addr[0], map_info.data, buffer_size);
    gst_memory_unmap(gst_buffer_memory, &map_info);
    st20p_rx_put_frame(src->rx_handle, frame);
  }
  gst_buffer_unref(buf);
  return GST_FLOW_OK;
}

static void gst_mtl_st20p_rx_finalize(GObject* object) {
  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(object);

  if (src->rx_handle) {
    if (st20p_rx_free(src->rx_handle)) {
      GST_ERROR("Failed to free rx handle");
      return;
    }
  }

  if (src->mtl_lib_handle) {
    if (mtl_stop(src->mtl_lib_handle) || mtl_uninit(src->mtl_lib_handle)) {
      GST_ERROR("Failed to uninitialize MTL library");
      return;
    }
  }
}

static gboolean gst_mtl_st20p_rx_stop(GstBaseSrc* basesrc) {
  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(basesrc);

  if (src->mtl_lib_handle) {
    mtl_stop(src->mtl_lib_handle);
  }

  return true;
}

static gboolean plugin_init(GstPlugin* mtl_st20p_rx) {
  return gst_element_register(mtl_st20p_rx, "mtl_st20p_rx", GST_RANK_SECONDARY,
                              GST_TYPE_MTL_ST20P_RX);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mtl_st20p_rx,
                  "software-based solution designed for high-throughput transmission",
                  plugin_init, PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN)
