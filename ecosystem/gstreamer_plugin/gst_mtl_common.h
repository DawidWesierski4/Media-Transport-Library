/*
 * Copyright (C) 2024 Intel Corporation
*/

#ifndef __GST_MTL_COMMON_H__
#define __GST_MTL_COMMON_H__

#include <arpa/inet.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <mtl/mtl_api.h>
#include <mtl/st_pipeline_api.h>

typedef struct StDevArgs {
  gchar port[MTL_PORT_MAX_LEN];
  gchar local_ip_string[MTL_PORT_MAX_LEN];
  gint tx_queues_cnt[MTL_PORT_MAX];
  gint rx_queues_cnt[MTL_PORT_MAX];
  gchar dma_dev[MTL_PORT_MAX_LEN];
} StDevArgs;

typedef struct SessionPortArgs {
  gchar session_ip_string[MTL_PORT_MAX_LEN];
  gchar port[MTL_PORT_MAX_LEN];
  gint udp_port;
  gint payload_type;
} SessionPortArgs;

gboolean gst_mtl_common_parse_input_finfo(const GstVideoFormatInfo *finfo, enum st_frame_fmt* fmt);
gboolean gst_mtl_common_parse_fps(GstVideoInfo* info, enum st_fps* fps);
gboolean gst_mtl_common_parse_fps_code(gint fps_code, enum st_fps* fps);
gboolean gst_mtl_common_parse_pixel_format(const char* format, enum st_frame_fmt* fmt);

#endif /* __GST_MTL_COMMON_H__ */