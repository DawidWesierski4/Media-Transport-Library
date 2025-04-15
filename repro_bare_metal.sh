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
INPUT_FOLDER=/mnt/ramdisk_src
INPUT=/mnt/dev_shared/src//ParkJoy_8bit_1920x1080_50Hz_P420.y4m
#INPUT=${INPUT_FOLDER}/GSTREAMER_${WIDTH}_${HEIGHT}_${FORMAT}_${VIDEO_FPS}_${VIDEO_FPS_DIV}.raw
BLOCKSIZE=8294400
# GSTREAMER_PLUGINS_PATH=/home/mtl/gstreamer/
IP_MULTICAST=239.168.75.30
IP_MULTICAST2=239.168.75.32

DISNEY_BUFFER_AUDIO_SIZE=4806

VFIO_PORT_1=0000:${PF_1}.0
VFIO_PORT_2=0000:${PF_1}.1
VFIO_PORT_3=0000:${PF_1}.2
VFIO_PORT_4=0000:${PF_1}.3
VFIO_PORT_5=0000:${PF_1}.4
VFIO_PORT_6=0000:${PF_1}.5


VFIO_PORT_1_2=0000:${PF_2}.0
VFIO_PORT_2_2=0000:${PF_2}.1
VFIO_PORT_3_2=0000:${PF_2}.2
VFIO_PORT_4_2=0000:${PF_2}.3
VFIO_PORT_5_2=0000:${PF_2}.4
VFIO_PORT_6_2=0000:${PF_2}.5

IP_PORT_1=192.168.12.181
IP_PORT_2=192.168.12.182
IP_PORT_3=192.168.12.183
IP_PORT_4=192.168.12.184
IP_PORT_5=192.168.12.185
IP_PORT_6=192.168.12.186

AUDIO_FB_CNT=60


init_test() {
    if $(pgrep MtlManager > /dev/null); then
        echo "MtlManager is already running"
    else
        echo "Starting MtlManager"
        MtlManager &
        sleep 3
    fi

    SCRIPT_BASENAME=$(basename $0)
    SCRIPT_DIR=$(dirname $0)
    LOG_DIR=${SCRIPT_DIR}/logs_${SCRIPT_BASENAME}

    if [ ! -d $LOG_DIR ]; then
        mkdir $LOG_DIR
    fi

    REPO_NUMBER_FILE=${LOG_DIR}/${SCRIPT_BASENAME}_repo_number
    if [ -f $REPO_NUMBER_FILE ]; then
        REPO_NUMBER=$(($(cat $REPO_NUMBER_FILE) + 1))
    else
        REPO_NUMBER=0
    fi
    echo $REPO_NUMBER > $REPO_NUMBER_FILE

    LOG_FILE=${LOG_DIR}/repo_${REPO_NUMBER}.log
    cat $0 > $LOG_FILE
    exec > >(tee -a $LOG_FILE) 2>&1

    if [ -z "$PF_1" ] || [ -z "$PF_2" ]; then
        echo -e "\e[31mError: PF_1 (primary port value=\"$PF_1\") or PF_2 (redundant port value=\"$PF_2\") is not set. Please use dpdk-devbind.py -s to check the available devices. For example, if you see a device like 0000:b1:01.2, set PF_1=b1:01.\e[0m"
        echo -e "\e[31mARE YOU SURE YOU ARE USING sudo -E  ಠ_ಠ ? \e[0m"
        exit 1
    fi

    trap kill_ass EXIT
    trap kill_ass SIGINT
    echo "Initialized $SCRIPT_BASENAME test iteration $REPO_NUMBER"
    echo "Log directory: $LOG_FILE"
}

function_test_bare_metal() {
    echo "Starting test with $1 $2 $3 $4 $5"

    gst-launch-1.0 -v \
    filesrc location=/dev/zero blocksize=$BLOCKSIZE ! \
    video/x-raw,format=$FORMAT,width=$WIDTH,height=$HEIGHT,framerate=${VIDEO_FPS}/${VIDEO_FPS_DIV} ! \
    tee name=t ! \
    queue ! \
    mtl_st20p_tx payload-type=96 \
                 async=false \
                 sync=false \
                 port=$1 \
                 port-red=$3 \
                 tx-framebuff-num=${AUDIO_FB_CNT} \
                 ip=$IP_MULTICAST \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$VIDEO_UDP_PORT \
                 udp-port-red=$((VIDEO_UDP_PORT)) \
    t. ! \
    queue ! \
    mtl_st20p_tx payload-type=96 \
                 async=false \
                 sync=false \
                 port=$1 \
                 port-red=$3 \
                 tx-framebuff-num=${AUDIO_FB_CNT} \
                 ip=$IP_MULTICAST \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$VIDEO_UDP_PORT \
                 udp-port-red=$((VIDEO_UDP_PORT + 10)) \
    t. ! \
    queue ! \
    mtl_st20p_tx payload-type=96 \
                 async=false \
                 sync=false \
                 port=$1 \
                 port-red=$3 \
                 ip=$IP_MULTICAST \
                 tx-framebuff-num=${AUDIO_FB_CNT} \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$VIDEO_UDP_PORT \
                 udp-port-red=$((VIDEO_UDP_PORT + 10)) \
    audiotestsrc wave=sine is-live=${IS_LIVE_AUDIO} ! \
    "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=1,rate=48000,blocksize=$DISNEY_BUFFER_AUDIO_SIZE" ! \
    tee name=audio_tee ! \
    queue ! \
    mtl_st30p_tx payload-type=97 \
                 async=false \
                 sync=false \
                 tx-framebuff-num=${AUDIO_FB_CNT} \
                 port=$1 \
                 port-red=$3 \
                 ip=$IP_MULTICAST \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$AUDIO_UDP_PORT \
                 udp-port-red=$((AUDIO_UDP_PORT + 10)) \
    audio_tee. ! \
    queue ! \
    mtl_st30p_tx payload-type=98 \
                 async=false \
                 sync=false \
                 tx-framebuff-num=${AUDIO_FB_CNT} \
                 port=$1 \
                 port-red=$3 \
                 ip=$IP_MULTICAST \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$((AUDIO_UDP_PORT + 1)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 11)) \
    audio_tee. ! \
    queue ! \
    mtl_st30p_tx payload-type=99 \
                 async=false \
                 sync=false \
                 tx-framebuff-num=${AUDIO_FB_CNT} \
                 port=$1 \
                 port-red=$3 \
                 ip=$IP_MULTICAST \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$((AUDIO_UDP_PORT + 2)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 12)) \
    audio_tee. ! \
    queue ! \
    mtl_st30p_tx payload-type=99 \
                 async=false \
                 sync=false \
                 tx-framebuff-num=${AUDIO_FB_CNT} \
                 port=$1 \
                 port-red=$3 \
                 ip=$IP_MULTICAST \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$((AUDIO_UDP_PORT + 2)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 12)) \
    audio_tee. ! \
    queue ! \
    mtl_st30p_tx payload-type=99 \
                 async=false \
                 sync=false \
                 tx-framebuff-num=${AUDIO_FB_CNT} \
                 port=$1 \
                 port-red=$3 \
                 ip=$IP_MULTICAST \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$((AUDIO_UDP_PORT + 2)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 12)) \
    audio_tee. ! \
    queue ! \
    mtl_st30p_tx payload-type=99 \
                 async=false \
                 sync=false \
                 tx-framebuff-num=${AUDIO_FB_CNT} \
                 port=$1 \
                 port-red=$3 \
                 ip=$IP_MULTICAST \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$((AUDIO_UDP_PORT + 2)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 12)) \
    audio_tee. ! \
    queue ! \
    mtl_st30p_tx payload-type=99 \
                 async=false \
                 sync=false \
                 port=$1 \
                 port-red=$3 \
                 ip=$IP_MULTICAST \
                 ip-red=$IP_MULTICAST2 \
                 udp-port=$((AUDIO_UDP_PORT + 2)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 12)) \
    audio_tee. ! \
    queue ! \
    mtl_st30p_tx payload-type=100 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$((AUDIO_UDP_PORT + 3)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 13)) \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port=$1 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
                 enable-ptp=true 2>&1 | tee -a $5
}

set -x

function kill_ass()
{
    sudo kill -9 $(pgrep stress)
    sudo kill -9 $(pgrep gst)
    sudo kill -9 $(pgrep Svt)

    echo KILL EVERYTHING 
}


if [[ ${BASH_SOURCE} == ${0} ]]; then

    if [[ $EUID -ne 0 ]]; then
        echo "This script must be run as root" 
        exit 1
    fi

    IS_LIVE_AUDIO=false
    for arg in "$@"; do
        if [[ "$arg" == "is-live" ]]; then
            IS_LIVE_AUDIO=true
            echo "ENABLING IS LIVE FROM ARUGMNET" | tee -a $LOG_FILE
            break
        fi
    done

    init_test
    export GST_DEBUG=WARN
    export GST_PLUGIN_PATH=$GSTREAMER_PLUGINS_PATH
    function_test_bare_metal $VFIO_PORT_1   $IP_PORT_2 $VFIO_PORT_2_2 $IP_PORT_1 ${LOG_FILE}_1 &
    sleep 1
    function_test_bare_metal $VFIO_PORT_3   $IP_PORT_4 $VFIO_PORT_4_2 $IP_PORT_3 ${LOG_FILE}_2 &
    sleep 1
    function_test_bare_metal $VFIO_PORT_5   $IP_PORT_6 $VFIO_PORT_6_2 $IP_PORT_5 ${LOG_FILE}_3 &


    for i in {1..40}; do
        # echo "Starting instance $i on 8-27,66-83"
        #sudo taskset -c 6,64-83 /home/labrat/SVT-AV1/Bin/Release/SvtAv1EncApp -i $INPUT --qp 30 --preset 0 --lp 1 &
        sudo taskset -c 6-55,62-112 stress -c 1112 --vm 1  --vm-stride 256 &
    done
    wait
    #function_test_bare_metal $VFIO_PORT_1_2 $IP_PORT_2 $VFIO_PORT_2   $IP_PORT_1 ${LOG_FILE}_4 &
    #function_test_bare_metal $VFIO_PORT_3_2 $IP_PORT_4 $VFIO_PORT_4   $IP_PORT_3 ${LOG_FILE}_5 &
    #function_test_bare_metal $VFIO_PORT_5_2 $IP_PORT_6 $VFIO_PORT_6   $IP_PORT_5 ${LOG_FILE}_6 &




fi
