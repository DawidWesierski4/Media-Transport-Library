# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.
import os

import pytest
import tests.Engine.RxTxApp as rxtxapp
from tests.Engine.media_files import anc_files, audio_files, yuv_files


@pytest.mark.parametrize("test_mode", ["unicast", "multicast"])
@pytest.mark.parametrize(
    "video_format",
    ["i1080p30", "i1080p50", "i1080p59", "i2160p30", "i2160p50", "i2160p59"],
)
def test_ptp_mixed_format(
    build, media, nic_port_list, test_time, test_mode, video_format
):
    video_file = yuv_files[video_format]
    audio_file = audio_files["PCM24"]
    ancillary_file = anc_files["text_p50"]

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_video_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode=test_mode,
        type_="frame",
        video_format=video_format,
        pg_format=video_file["format"],
        video_url=os.path.join(media, video_file["filename"]),
    )
    config = rxtxapp.add_audio_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode=test_mode,
        type_="frame",
        audio_format="PCM24",
        audio_channel=["U02"],
        audio_sampling="48kHz",
        audio_ptime="1",
        audio_url=os.path.join(media, audio_file["filename"]),
    )
    config = rxtxapp.add_ancillary_sessions(
        config=config,
        nic_port_list=nic_port_list,
        test_mode=test_mode,
        type_="frame",
        ancillary_format="closed_caption",
        ancillary_fps=ancillary_file["fps"],
        ancillary_url=os.path.join(media, ancillary_file["filename"]),
    )

    rxtxapp.execute_test(config=config, build=build, test_time=test_time, ptp=True)
