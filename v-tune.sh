#!/bin/bash

# Ensure necessary environment variables are set
if [ -z "$IP_MULTICAST" ] || [ -z "$IP_PORT_T" ] || [ -z "$VFIO_PORT_T" ]; then
  echo "Please set IP_MULTICAST, IP_PORT_T, and VFIO_PORT_T environment variables."
  exit 1
fi

# Run the GStreamer pipeline
/usr/bin/gst-launch-1.0 \
  audiotestsrc is-live=true wave=sine freq=770 ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  --gst-plugin-path "$(dirname $0)/ecosystem/gstreamer_plugin/build/"

# Run the GStreamer pipeline
/usr/bin/gst-launch-1.0 -v \
  filesrc location=/dev/zero ! \
  queue ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  queue ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  queue ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  queue ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  queue ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  queue ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  queue ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  queue ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T" \
  filesrc location=/dev/zero ! \
  queue ! \
  "audio/x-raw,layout=(string)interleaved,format=S24LE,channels=8,rate=48000,channel-mask=(bitmask)0xFF" ! \
  mtl_st30p_tx payload-type=99 async=false sync=false udp-port=30000 \
  ip="$IP_MULTICAST" dev-ip="$IP_PORT_T" dev-port="$VFIO_PORT_T"  --gst-plugin-path "$(dirname $0)/ecosystem/gstreamer_plugin/build/"