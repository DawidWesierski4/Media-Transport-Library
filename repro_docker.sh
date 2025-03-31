#!/bin/bash

cd $(dirname $0)

VIDEO_UDP_PORT=20000
AUDIO_UDP_PORT=30000
VIDEO_PAYLOAD_TYPE=112
AUDIO_PAYLOAD_TYPE=111
VIDEO_FPS=60000
VIDEO_FPS_DIV=1001
WIDTH=1080
HEIGHT=720
LOG_LEVEL=0
FORMAT=I422_10LE
INPUT_FOLDER=/mnt/ramdisk_src
INPUT=${INPUT_FOLDER}/GSTREAMER_${WIDTH}_${HEIGHT}_${FORMAT}_${VIDEO_FPS}_${VIDEO_FPS_DIV}.raw
BLOCKSIZE=33177600
GSTREAMER_PLUGINS_PATH=/home/mtl/gstreamer/

VFIO_PORT_1=0000:b1:11.0
VFIO_PORT_2=0000:b1:11.1
VFIO_PORT_3=0000:b1:11.2
VFIO_PORT_4=0000:b1:11.3
VFIO_PORT_5=0000:b1:11.4
VFIO_PORT_6=0000:b1:11.5

IP_PORT_1=192.168.12.181
IP_PORT_2=192.168.12.182
IP_PORT_3=192.168.12.183
IP_PORT_4=192.168.12.184
IP_PORT_5=192.168.12.185
IP_PORT_6=192.168.12.186

set -xe
init_test() {
    if [[ ! -d ${INPUT_FOLDER} ]]; then
        mkdir -p ${INPUT_FOLDER}
    fi

    if ! mount | grep -q "${INPUT_FOLDER}.*tmpfs"; then
        echo "${INPUT_FOLDER} is not a ramdisk. Please mount it as a tmpfs."
        mount -t tmpfs -o size=22G tmpfs ${INPUT_FOLDER}
    fi

    if [[ ! -f ${INPUT} ]]; then

        echo "Creating input file ${INPUT}"
        dd if=/dev/zero of=${INPUT} bs=${BLOCKSIZE} count=$((VIDEO_FPS * 15 / VIDEO_FPS_DIV))
    fi


    if $(pgrep MtlManager > /dev/null); then
        echo "MtlManager is already running"
    else
        echo "Starting MtlManager"
        MtlManager &
        sleep 3
    fi
}

function_test() {
    docker run --rm \
        --ulimit memlock=-1:-1 \
        --device /dev/vfio:/dev/vfio \
        --volume /var/run/imtl:/var/run/imtl \
        --volume ${INPUT_FOLDER}:${INPUT_FOLDER} \
        --volume /hugepages:/hugepages \
        --cap-add SYS_NICE \
        --cap-add IPC_LOCK \
        -e GSTREAMER_PLUGINS_PATH=$GSTREAMER_PLUGINS_PATH \
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
        "GST_PLUGIN_PATH=$GSTREAMER_PLUGINS_PATH gst-launch-1.0 -v filesrc location=$INPUT blocksize=$BLOCKSIZE ! \
        video/x-raw,format=$FORMAT,width=$WIDTH,height=$HEIGHT,framerate=${VIDEO_FPS}/${VIDEO_FPS_DIV} ! \
        tee name=t t. ! \
        queue ! \
        mtl_st20p_tx payload-type=96 \
                    async=false \
                    sync=false \
                    log-level=$LOG_LEVEL \
                    ip=$IP_MULTICAST \
                    udp-port=$VIDEO_UDP_PORT \
                    dev-port=$1 \
                    dev-ip=$2 t. ! \
        queue ! \
        mtl_st20p_tx payload-type=96 \
                    async=false \
                    sync=false \
                    ip=$IP_MULTICAST \
                    log-level=$LOG_LEVEL \
                    udp-port=$VIDEO_UDP_PORT \
                    dev-port=$1 \
                    dev-ip=$2 t. ! \
        queue ! \
        mtl_st20p_tx payload-type=96 \
                    async=false \
                    sync=false \
                    log-level=$LOG_LEVEL \
                    ip=$IP_MULTICAST \
                    udp-port=$VIDEO_UDP_PORT \
                    dev-port=$1 \
                    dev-ip=$2  "
}

function_test2() {
    docker run --rm \
        --ulimit memlock=-1:-1 \
        --device /dev/vfio:/dev/vfio \
        --volume /var/run/imtl:/var/run/imtl \
        --volume ${INPUT_FOLDER}:${INPUT_FOLDER} \
        --volume /hugepages:/hugepages \
        --cap-add SYS_NICE \
        --cap-add IPC_LOCK \
        --cpuset-cpus=28-55 \
        -e GSTREAMER_PLUGINS_PATH=$GSTREAMER_PLUGINS_PATH \
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
        "GST_PLUGIN_PATH=$GSTREAMER_PLUGINS_PATH gst-launch-1.0 -v videotestsrc blocksize=$BLOCKSIZE ! \
        video/x-raw,format=$FORMAT,width=$WIDTH,height=$HEIGHT,framerate=${VIDEO_FPS}/${VIDEO_FPS_DIV} ! \
        tee name=t t. ! \
        queue ! \
        mtl_st20p_tx payload-type=96 \
                    async=false \
                    sync=false \
                    ip=$IP_MULTICAST \
                    udp-port=$VIDEO_UDP_PORT \
                    dev-port=$1 \
                    dev-ip=$2  "
}

# unused $VFIO_PORT_2 $IP_PORT_1
# unused $VFIO_PORT_4 $IP_PORT_3
# unused $VFIO_PORT_6 $IP_PORT_5
# only when not sourced :3 <3 ( ͡° ͜ʖ ͡°)
if [[ ${BASH_SOURCE} == ${0} ]]; then
    init_test

    if [[ $EUID -ne 0 ]]; then
        echo "This script must be run as root" 
        exit 1
    fi
    function_test2 $VFIO_PORT_1 $IP_PORT_2 &
    function_test2 $VFIO_PORT_3 $IP_PORT_4 &
    function_test2 $VFIO_PORT_5 $IP_PORT_6 &
    function_test2 $VFIO_PORT_2 $IP_PORT_1 &
    function_test2 $VFIO_PORT_4 $IP_PORT_3 &
    function_test2 $VFIO_PORT_6 $IP_PORT_5 &
    # funciton_local $VFIO_PORT_3 $IP_PORT_4 &
    # funciton_local $VFIO_PORT_5 $IP_PORT_6 &



    # function_test $VFIO_PORT_1 $IP_PORT_2 &
    # function_test $VFIO_PORT_3 $IP_PORT_4 &
    # function_test $VFIO_PORT_5 $IP_PORT_6 &
    # function_test $VFIO_PORT_2 $IP_PORT_1 &
    # function_test $VFIO_PORT_4 $IP_PORT_3 &
    # function_test $VFIO_PORT_6 $IP_PORT_5 &

    # function_test_red $VFIO_PORT_1 $IP_PORT_2 $VFIO_PORT_2 $IP_PORT_1 &
    # function_test_red $VFIO_PORT_3 $IP_PORT_4 $VFIO_PORT_4 $IP_PORT_3 &
    # function_test_red $VFIO_PORT_5 $IP_PORT_6 $VFIO_PORT_6 $IP_PORT_5 &

    # function_test_3 $VFIO_PORT_1 $IP_PORT_2
    # function_test_3 $VFIO_PORT_3 $IP_PORT_4
    # function_test_3 $VFIO_PORT_5 $IP_PORT_6
    # function_test_3 $VFIO_PORT_2 $IP_PORT_1
    # function_test_3 $VFIO_PORT_4 $IP_PORT_3
    # function_test_3 $VFIO_PORT_6 $IP_PORT_5

    wait
fi
