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

VFIO_PORT_1=0000:4b:01.0
VFIO_PORT_2=0000:4b:01.1
VFIO_PORT_3=0000:4b:01.2
VFIO_PORT_4=0000:4b:01.3
VFIO_PORT_5=0000:4b:01.4
VFIO_PORT_6=0000:4b:01.5


VFIO_PORT_1_2=0000:4b:11.0
VFIO_PORT_2_2=0000:4b:11.1
VFIO_PORT_3_2=0000:4b:11.2
VFIO_PORT_4_2=0000:4b:11.3
VFIO_PORT_5_2=0000:4b:11.4
VFIO_PORT_6_2=0000:4b:11.5

IP_PORT_1=192.168.12.181
IP_PORT_2=192.168.12.182
IP_PORT_3=192.168.12.183
IP_PORT_4=192.168.12.184
IP_PORT_5=192.168.12.185
IP_PORT_6=192.168.12.186
NUMA_NODE=1

set -xe
init_test() {
    # if [[ ! -d ${INPUT_FOLDER} ]]; then
    #     mkdir -p ${INPUT_FOLDER}
    # fi

    # if ! mount | grep -q "${INPUT_FOLDER}.*tmpfs"; then
    #     echo "${INPUT_FOLDER} is not a ramdisk. Please mount it as a tmpfs."
    #     mount -t tmpfs -o size=22G tmpfs ${INPUT_FOLDER}
    # fi

    # if [[ ! -f ${INPUT} ]]; then

    #     echo "Creating input file ${INPUT}"
    #     dd if=/dev/zero of=${INPUT} bs=${BLOCKSIZE} count=$((VIDEO_FPS * 25 / VIDEO_FPS_DIV))
    # fi


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

    echo "Initialized $SCRIPT_BASENAME test iteration $REPO_NUMBER"
    echo "Log directory: $LOG_FILE"
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
        "GST_PLUGIN_PATH=$GSTREAMER_PLUGINS_PATH gst-launch-1.0 -v \
    filesrc location=/dev/zero blocksize=$BLOCKSIZE ! \
    video/x-raw,format=$FORMAT,width=$WIDTH,height=$HEIGHT,framerate=${VIDEO_FPS}/${VIDEO_FPS_DIV} ! \
    tee name=t t. ! \
    queue ! \
    mtl_st20p_tx payload-type=96 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$VIDEO_UDP_PORT \
                 udp-port-red=$((VIDEO_UDP_PORT + 10)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
                 enable-ptp=true \
    filesrc location=/dev/zero ! \
    \"audio/x-raw,layout=(string)interleaved,format=S24LE,channels=2,rate=48000,channel-mask=(bitmask)0x63\" ! \
    queue ! \
    mtl_st30p_tx payload-type=97 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$AUDIO_UDP_PORT \
                 udp-port-red=$((AUDIO_UDP_PORT + 10)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
                 enable-ptp=true \
    filesrc location=/dev/zero ! \
    \"audio/x-raw,layout=(string)interleaved,format=S24LE,channels=2,rate=48000,channel-mask=(bitmask)0x63\" ! \
    queue ! \
    mtl_st30p_tx payload-type=98 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$((AUDIO_UDP_PORT + 1)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 11)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
                 enable-ptp=true \
    filesrc location=/dev/zero ! \
    \"audio/x-raw,layout=(string)interleaved,format=S24LE,channels=2,rate=48000,channel-mask=(bitmask)0x63\" ! \
    queue ! \
    mtl_st30p_tx payload-type=99 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$((AUDIO_UDP_PORT + 2)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 12)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
                 enable-ptp=true \
    filesrc location=/dev/zero ! \
    \"audio/x-raw,layout=(string)interleaved,format=S24LE,channels=2,rate=48000,channel-mask=(bitmask)0x63\" ! \
    mtl_st30p_tx payload-type=100 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$((AUDIO_UDP_PORT + 3)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 13)) \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 dev-port=$1 \
                 ip-red=$IP_MULTICAST2 \
                 dev-ip=$2 \
                 enable-ptp=true" 2>&1 | tee -a $5
}

function_test_bare_metal() {
    GST_PLUGIN_PATH=$GSTREAMER_PLUGINS_PATH numactl --membind 0 gst-launch-1.0 -v \
    filesrc location=/dev/zero blocksize=$BLOCKSIZE ! \
    video/x-raw,format=$FORMAT,width=$WIDTH,height=$HEIGHT,framerate=${VIDEO_FPS}/${VIDEO_FPS_DIV} ! \
    tee name=t t. ! \
    queue ! \
    mtl_st20p_tx payload-type=96 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$VIDEO_UDP_PORT \
                 udp-port-red=$((VIDEO_UDP_PORT)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
                 enable-ptp=true \
    t. ! \
    queue ! \
    mtl_st20p_tx payload-type=96 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$VIDEO_UDP_PORT \
                 udp-port-red=$((VIDEO_UDP_PORT + 1)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
                 enable-ptp=true \
    queue ! \
    mtl_st20p_tx payload-type=96 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$VIDEO_UDP_PORT \
                 udp-port-red=$((VIDEO_UDP_PORT + 2)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
                 enable-ptp=true \
    filesrc location=/dev/zero ! \
    "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=2,rate=48000,channel-mask=(bitmask)0x63" ! \
    queue ! \
    mtl_st30p_tx payload-type=97 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$AUDIO_UDP_PORT \
                 udp-port-red=$((AUDIO_UDP_PORT + 10)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
    filesrc location=/dev/zero ! \
    "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=2,rate=48000,channel-mask=(bitmask)0x63" ! \
    queue ! \
    mtl_st30p_tx payload-type=98 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$((AUDIO_UDP_PORT + 1)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 11)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
    filesrc location=/dev/zero ! \
    "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=2,rate=48000,channel-mask=(bitmask)0x63" ! \
    queue ! \
    mtl_st30p_tx payload-type=99 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$((AUDIO_UDP_PORT + 2)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 12)) \
                 dev-port=$1 \
                 dev-ip=$2 \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 ip-red=$IP_MULTICAST2 \
    filesrc location=/dev/zero ! \
    "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=2,rate=48000,channel-mask=(bitmask)0x63" ! \
    mtl_st30p_tx payload-type=100 \
                 async=false \
                 sync=false \
                 ip=$IP_MULTICAST \
                 udp-port=$((AUDIO_UDP_PORT + 3)) \
                 udp-port-red=$((AUDIO_UDP_PORT + 13)) \
                 dev-ip-red=$4 \
                 dev-port-red=$3 \
                 dev-port=$1 \
                 ip-red=$IP_MULTICAST2 \
                 dev-ip=$2 2>&1 | tee -a $5
}

# function_test_numa_force() {
#     numactl --cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE} docker run --rm \
#         --ulimit memlock=-1:-1 \
#         --device /dev/vfio:/dev/vfio \
#         --volume /var/run/imtl:/var/run/imtl \
#         --volume ${INPUT_FOLDER}:${INPUT_FOLDER} \
#         --volume /hugepages:/hugepages \
#         --cap-add SYS_NICE \
#         --cap-add IPC_LOCK \
#         --cpuset-cpus=28-55 \
#         -e GSTREAMER_PLUGINS_PATH=$GSTREAMER_PLUGINS_PATH \
#         -e INPUT=$INPUT \
#         -e BLOCKSIZE=$BLOCKSIZE \
#         -e FORMAT=$FORMAT \
#         -e WIDTH=$WIDTH \
#         -e HEIGHT=$HEIGHT \
#         -e VIDEO_FPS=$VIDEO_FPS \
#         -e VIDEO_FPS_DIV=$VIDEO_FPS_DIV \
#         -e VIDEO_UDP_PORT=$VIDEO_UDP_PORT \
#         -e IP_MULTICAST=$IP_MULTICAST \
#         -e IP_MULTICAST2=$IP_MULTICAST2 \
#         -e GST_DEBUG=WARNING \
#         mtl_rockod_gstreamer:latest \
#         "GST_PLUGIN_PATH=$GSTREAMER_PLUGINS_PATH gst-launch-1.0 -v videotestsrc blocksize=$BLOCKSIZE ! \
#         video/x-raw,format=$FORMAT,width=$WIDTH,height=$HEIGHT,framerate=${VIDEO_FPS}/${VIDEO_FPS_DIV} ! \
#         tee name=t t. ! \
#         queue ! \
#         mtl_st20p_tx payload-type=96 \
#                     async=false \
#                     sync=false \
#                     ip=$IP_MULTICAST \
#                     udp-port=$VIDEO_UDP_PORT \
#                     dev-port=$1 \
#                     dev-ip=$2  "
# }


kill_ass()
{
    sudo kill -9 $(pgrep stress)
    sudo kill -9 $(pgrep gst)
}
# unused $VFIO_PORT_2 $IP_PORT_1
# unused $VFIO_PORT_4 $IP_PORT_3
# unused $VFIO_PORT_6 $IP_PORT_5
if [[ ${BASH_SOURCE} == ${0} ]]; then
    init_test

    if [[ $EUID -ne 0 ]]; then
        echo "This script must be run as root" 
        exit 1
    fi


    
    #/home/labrat/SVT-AV1/Bin/Release/SvtAv1EncApp -i $INPUT --qp 31 --preset 0 --lp 1 &
    #/home/labrat/SVT-AV1/Bin/Release/SvtAv1EncApp -i $INPUT --qp 32 --preset 0 --lp 1 &
    #/home/labrat/SVT-AV1/Bin/Release/SvtAv1EncApp -i $INPUT --qp 33 --preset 0 --lp 1 &

    #trap SIGINT kill_ass

    function_test_bare_metal $VFIO_PORT_1 $IP_PORT_2 $VFIO_PORT_2_2 $IP_PORT_1 ${LOG_FILE}_1 "1,2" &
    sleep 15
    function_test_bare_metal $VFIO_PORT_3 $IP_PORT_4 $VFIO_PORT_4_2 $IP_PORT_3 ${LOG_FILE}_2 "3,4" &
    sleep 15
    function_test_bare_metal $VFIO_PORT_5 $IP_PORT_6 $VFIO_PORT_6_2 $IP_PORT_5 ${LOG_FILE}_3 "4,5" &
    sleep 15

    for i in {1..40}; do
        # echo "Starting instance $i on 8-27,66-83"
        #sudo taskset -c 8-27,64-83 /home/labrat/SVT-AV1/Bin/Release/SvtAv1EncApp -i $INPUT --qp 30 --preset 0 --lp 1 &
        sudo taskset -c 10-55,66-111 stress -c 10 --vm 1  --vm-stride 256 &
    done
    # function_test2 $VFIO_PORT_1_2 $IP_PORT_2 $VFIO_PORT_2 $IP_PORT_1 ${LOG_FILE}_4 "1,2" &

    # function_test2 $VFIO_PORT_3_2 $IP_PORT_4 $VFIO_PORT_4 $IP_PORT_3 ${LOG_FILE}_5 "3,4" &

    # function_test2 $VFIO_PORT_5_2 $IP_PORT_6 $VFIO_PORT_6 $IP_PORT_5 ${LOG_FILE}_6 "4,5" &

    sleep 10



    wait



    # function_test2 $VFIO_PORT_2 $IP_PORT_1 &
    # function_test2 $VFIO_PORT_4 $IP_PORT_3 &
    # function_test2 $VFIO_PORT_6 $IP_PORT_5 &

    #numactl --cpunodebind=0 --membind=0
    # function_test_numa_force $VFIO_PORT_1 $IP_PORT_2 &
    # function_test_numa_force $VFIO_PORT_3 $IP_PORT_4 &
    # function_test_numa_force $VFIO_PORT_5 $IP_PORT_6 &
    # function_test_numa_force $VFIO_PORT_2 $IP_PORT_1 &
    # function_test_numa_force $VFIO_PORT_4 $IP_PORT_3 &
    # function_test_numa_force $VFIO_PORT_6 $IP_PORT_5 &

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
    kill -9 $PID1
    kill -9 $PID2
    kill -9 $PID3
fi
