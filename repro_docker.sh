#!/bin/bash
set -e
cd $(dirname $0)
SVT_PATH=/home/labrat/SVT-AV1/Bin/Release
VIDEO_UDP_PORT=20000
AUDIO_UDP_PORT=30000
VIDEO_PAYLOAD_TYPE=112
AUDIO_PAYLOAD_TYPE=111
VIDEO_FPS_DIV=1001
WIDTH=1920
HEIGHT=1080
LOG_LEVEL=0
FORMAT=I422_10LE
VIDEO_FPS=60000
VIDEO_FPS_DIV=1001
#INPUT=${INPUT_FOLDER}/GSTREAMER_${WIDTH}_${HEIGHT}_${FORMAT}_${VIDEO_FPS}_${VIDEO_FPS_DIV}.raw
BLOCKSIZE=8294400
GSTREAMER_PLUGINS_PATH=/home/mtl/gstreamer/

IP_PORT_1=192.168.12.181
IP_PORT_2=192.168.12.182
IP_PORT_3=192.168.12.183
IP_PORT_4=192.168.12.184
IP_PORT_5=192.168.12.185
IP_PORT_6=192.168.12.186



function_test_docker() {
    docker run --rm \
        --ulimit memlock=-1:-1 \
        --device /dev/vfio:/dev/vfio \
        --volume /var/run/imtl:/var/run/imtl \
        --volume ${INPUT_FOLDER}:${INPUT_FOLDER} \
        --volume ./:/tmp/mtl \
        --volume /hugepages:/hugepages \
        --cap-add SYS_NICE \
        --cap-add IPC_LOCK \
        -e GSTREAMER_PLUGINS_PATH=$GSTREAMER_PLUGINS_PATH \
        -e GST_PLUGIN_PATH=$GSTREAMER_PLUGINS_PATH \
        -e INPUT=$INPUT \
        -e BLOCKSIZE=$BLOCKSIZE \
        -e FORMAT=$FORMAT \
        -e WIDTH=$WIDTH \
        -e HEIGHT=$HEIGHT \
        -e VIDEO_FPS=$VIDEO_FPS \
        -e VIDEO_FPS_DIV=$VIDEO_FPS_DIV \
        -e VIDEO_UDP_PORT=$VIDEO_UDP_PORT \
        -e IP_MULTICAST=$IP_MULTICAST \
        -e IP_MULTICAST2=$IP_MULTICAST2 \
        -e GST_DEBUG=WARNING \
        mtl_rockod_gstreamer:latest \
        ". /tmp/mtl/repro_bare_metal.sh && function_test_bare_metal $1  $2 $3 $4 /dev/null" 2>&1 | tee -a $5
}



if [[ ${BASH_SOURCE} == ${0} ]]; then

    if [[ $EUID -ne 0 ]]; then
        echo "This script must be run as root" 
        exit 1
    fi

    . repro_bare_metal.sh

    init_test

    function_test_docker $VFIO_PORT_1   $IP_PORT_2 $VFIO_PORT_2_2 $IP_PORT_1 ${LOG_FILE}_1 &
    function_test_docker $VFIO_PORT_3   $IP_PORT_4 $VFIO_PORT_4_2 $IP_PORT_3 ${LOG_FILE}_2 &
    # function_test_docker $VFIO_PORT_5   $IP_PORT_6 $VFIO_PORT_6_2 $IP_PORT_5 ${LOG_FILE}_3 &

    #function_test_bare_metal $VFIO_PORT_1_2 $IP_PORT_2 $VFIO_PORT_2   $IP_PORT_1 ${LOG_FILE}_4 &
    #function_test_bare_metal $VFIO_PORT_3_2 $IP_PORT_4 $VFIO_PORT_4   $IP_PORT_3 ${LOG_FILE}_5 &
    #function_test_bare_metal $VFIO_PORT_5_2 $IP_PORT_6 $VFIO_PORT_6   $IP_PORT_5 ${LOG_FILE}_6 &



    wait
fi
