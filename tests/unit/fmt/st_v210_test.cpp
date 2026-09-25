/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the v210 line layout of Apple TN2162: 6 pixels in 16 bytes, each line padded
 * to a 48-pixel (128-byte) boundary. The linesize table is the byte count per line
 * that the FFmpeg v210 encoder and GStreamer 1.24 videotestsrc both produce.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='*StV210*'
 */

#include <gtest/gtest.h>
#include <stdint.h>

#include <vector>

extern "C" {
#include <mtl_api.h>
#include <st_pipeline_api.h>

#include "pipeline/st20p_harness.h"
#include "pipeline/st20p_tx_harness.h"
}

static const struct {
  uint32_t width;
  size_t linesize;
} v210_lines[] = {
    {720, 1920}, {1280, 3456}, {1920, 5120}, {2048, 5504}, {3840, 10240}, {4096, 11008},
};

TEST(StV210Size, LeastLinesizeIsPaddedTo48Pixels) {
  for (auto& c : v210_lines)
    EXPECT_EQ(st_frame_least_linesize(ST_FRAME_FMT_V210, c.width, 0), c.linesize)
        << "width " << c.width;
}

TEST(StV210Size, FrameSizeIsLinesizeTimesHeight) {
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_V210, 1280, 720, false), 2488320u);
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_V210, 1920, 1080, false), 5529600u);
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_V210, 2048, 1080, false), 5944320u);
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_V210, 4096, 2160, false), 23777280u);
}

TEST(StV210Size, InterlacedFrameSizeIsHalf) {
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_V210, 1280, 720, true), 1244160u);
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_V210, 1920, 1080, true), 2764800u);
}

TEST(StV210Size, OddWidthIsRejected) {
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_V210, 1279, 720, false), 0u);
}

/* 4:2:2 samples of one line: cb/cr per pixel pair, y per pixel. */
struct line_samples {
  std::vector<uint16_t> cb, y, cr;
};

static line_samples make_line(uint32_t width, uint32_t line) {
  line_samples s;
  for (uint32_t i = 0; i < width / 2; i++) {
    s.cb.push_back((line * 331 + i * 7 + 1) & 0x3FF);
    s.cr.push_back((line * 331 + i * 11 + 2) & 0x3FF);
  }
  for (uint32_t i = 0; i < width; i++) s.y.push_back((line * 331 + i * 13 + 3) & 0x3FF);
  return s;
}

static void pack_be10_line(const line_samples& s, uint8_t* be) {
  for (size_t p = 0; p < s.cb.size(); p++, be += 5) {
    uint16_t cb = s.cb[p], y0 = s.y[2 * p], cr = s.cr[p], y1 = s.y[2 * p + 1];
    be[0] = cb >> 2;
    be[1] = ((cb & 0x3) << 6) | (y0 >> 4);
    be[2] = ((y0 & 0xF) << 4) | (cr >> 6);
    be[3] = ((cr & 0x3F) << 2) | (y1 >> 8);
    be[4] = y1 & 0xFF;
  }
}

/* TN2162 word order; samples past the line end are zero, as FFmpeg writes them. */
static void pack_v210_line(const line_samples& s, uint8_t* v210) {
  uint32_t width = s.y.size();
  uint32_t groups = (width + 5) / 6;
  auto cb = [&](uint32_t i) -> uint32_t { return i < s.cb.size() ? s.cb[i] : 0; };
  auto cr = [&](uint32_t i) -> uint32_t { return i < s.cr.size() ? s.cr[i] : 0; };
  auto y = [&](uint32_t i) -> uint32_t { return i < width ? s.y[i] : 0; };
  for (uint32_t g = 0; g < groups; g++) {
    uint32_t c = g * 3, l = g * 6;
    uint32_t w[4] = {
        cb(c) | y(l) << 10 | cr(c) << 20,
        y(l + 1) | cb(c + 1) << 10 | y(l + 2) << 20,
        cr(c + 1) | y(l + 3) << 10 | cb(c + 2) << 20,
        y(l + 4) | cr(c + 2) << 10 | y(l + 5) << 20,
    };
    for (int k = 0; k < 4; k++)
      for (int b = 0; b < 4; b++) v210[g * 16 + k * 4 + b] = (w[k] >> (8 * b)) & 0xFF;
  }
}

static const uint8_t untouched = 0xA5;

static void init_frame(struct st_frame* f, enum st_frame_fmt fmt, uint32_t width,
                       uint32_t height, std::vector<uint8_t>& buf) {
  f->fmt = fmt;
  f->width = width;
  f->height = height;
  f->interlaced = false;
  f->linesize[0] = st_frame_least_linesize(fmt, width, 0);
  f->buffer_size = f->linesize[0] * height;
  f->data_size = f->buffer_size;
  buf.assign(f->buffer_size, untouched);
  f->addr[0] = buf.data();
}

class StV210Convert : public ::testing::TestWithParam<uint32_t> {};

TEST_P(StV210Convert, Be10ToV210MatchesTn2162Layout) {
  const uint32_t width = GetParam(), height = 2;
  const size_t be_line = width / 2 * 5;
  const size_t stride = st_frame_least_linesize(ST_FRAME_FMT_V210, width, 0);
  ASSERT_GT(stride, 0u);

  struct st_frame src = {}, dst = {};
  std::vector<uint8_t> src_buf, dst_buf;
  init_frame(&src, ST_FRAME_FMT_YUV422RFC4175PG2BE10, width, height, src_buf);
  init_frame(&dst, ST_FRAME_FMT_V210, width, height, dst_buf);
  ASSERT_EQ(src.linesize[0], be_line);

  std::vector<uint8_t> expect(stride * height, untouched);
  for (uint32_t line = 0; line < height; line++) {
    line_samples s = make_line(width, line);
    pack_be10_line(s, src_buf.data() + be_line * line);
    pack_v210_line(s, expect.data() + stride * line);
  }

  ASSERT_EQ(st_frame_convert(&src, &dst), 0);
  EXPECT_EQ(dst_buf, expect) << "width " << width;
}

TEST_P(StV210Convert, V210ToBe10ReadsPaddedLines) {
  const uint32_t width = GetParam(), height = 2;
  const size_t be_line = width / 2 * 5;
  const size_t stride = st_frame_least_linesize(ST_FRAME_FMT_V210, width, 0);
  ASSERT_GT(stride, 0u);

  struct st_frame src = {}, dst = {};
  std::vector<uint8_t> src_buf, dst_buf;
  init_frame(&src, ST_FRAME_FMT_V210, width, height, src_buf);
  init_frame(&dst, ST_FRAME_FMT_YUV422RFC4175PG2BE10, width, height, dst_buf);

  std::vector<uint8_t> expect(be_line * height);
  for (uint32_t line = 0; line < height; line++) {
    line_samples s = make_line(width, line);
    pack_v210_line(s, src_buf.data() + stride * line);
    pack_be10_line(s, expect.data() + be_line * line);
  }

  ASSERT_EQ(st_frame_convert(&src, &dst), 0);
  EXPECT_EQ(dst_buf, expect) << "width " << width;
}

/* 1920: lines contiguous; 1080: padded, full groups; 1280/4096: 2/4-pixel last group. */
INSTANTIATE_TEST_SUITE_P(Widths, StV210Convert,
                         ::testing::Values(1920u, 1080u, 1280u, 2048u, 4096u));

/* The pipeline frame uses the v210 stride, so the session stride must match it; 0 leaves
 * the session packed, where the v210 stride already equals it. */
class StV210SessionStride : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut20p_tx_init(), 0);
    ASSERT_EQ(ut20p_init(), 0);
  }
};

TEST_P(StV210SessionStride, TxSessionUsesPaddedStride) {
  const bool derive = GetParam();
  EXPECT_EQ(ut20p_tx_transport_linesize(ST20_FMT_V210, 1280, 0, derive), 3456u);
  EXPECT_EQ(ut20p_tx_transport_linesize(ST20_FMT_V210, 1080, 0, derive), 2944u);
  EXPECT_EQ(ut20p_tx_transport_linesize(ST20_FMT_V210, 1280, 4096, derive), 4096u);
  EXPECT_EQ(ut20p_tx_transport_linesize(ST20_FMT_V210, 1920, 0, derive), 0u);
  EXPECT_EQ(ut20p_tx_transport_linesize(ST20_FMT_YUV_422_10BIT, 1280, 0, derive), 0u);
}

TEST_P(StV210SessionStride, RxSessionUsesPaddedStride) {
  const bool derive = GetParam();
  EXPECT_EQ(ut20p_rx_transport_linesize(ST20_FMT_V210, 1280, 0, derive), 3456u);
  EXPECT_EQ(ut20p_rx_transport_linesize(ST20_FMT_V210, 1080, 0, derive), 2944u);
  EXPECT_EQ(ut20p_rx_transport_linesize(ST20_FMT_V210, 1280, 4096, derive), 4096u);
  EXPECT_EQ(ut20p_rx_transport_linesize(ST20_FMT_V210, 1920, 0, derive), 0u);
  EXPECT_EQ(ut20p_rx_transport_linesize(ST20_FMT_YUV_422_10BIT, 1280, 0, derive), 0u);
}

INSTANTIATE_TEST_SUITE_P(Derive, StV210SessionStride, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& i) {
                           return i.param ? "Derive" : "Convert";
                         });
