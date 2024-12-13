/*
 * Copyright (C) 2024 Intel Corporation
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gst_mtl_common.h"

static gboolean gst_mtl_parse_input_fmt(GstVideoInfo* info,
                                              enum st_frame_fmt* fmt) {
  GstVideoFormatInfo* finfo = info->finfo;

  if (finfo->format == GST_VIDEO_FORMAT_v210) {
    *fmt = ST_FRAME_FMT_V210;
  } else if (finfo->format == GST_VIDEO_FORMAT_I420_10LE) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else {
    return FALSE;
  }

  return TRUE;
}

static gboolean gst_mtl_parse_fps_code(gint fps_code, enum st_fps* fps) {
  if (!fps) {
    GST_ERROR("Invalid fps pointer");
    return FALSE;
  }

  switch (fps_code) {
    case 120:
      *fps = ST_FPS_P120;
      break;
    case 11988:
      *fps = ST_FPS_P119_88;
      break;
    case 100:
      *fps = ST_FPS_P100;
      break;
    case 60:
      *fps = ST_FPS_P60;
      break;
    case 5994:
      *fps = ST_FPS_P59_94;
      break;
    case 50:
      *fps = ST_FPS_P50;
      break;
    case 30:
      *fps = ST_FPS_P30;
      break;
    case 2997:
      *fps = ST_FPS_P29_97;
      break;
    case 25:
      *fps = ST_FPS_P25;
      break;
    case 24:
      *fps = ST_FPS_P24;
      break;
    case 2398:
      *fps = ST_FPS_P23_98;
      break;
    default:
      return FALSE;
  }
  return TRUE;
}
