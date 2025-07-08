#!/bin/bash

if [ -z "$1" ]; then
  process_name="gstreamer"
else
  process_name="$1"
fi

if ! $(pgrep ${process_name}) ; then
  echo "NO ${process_name} WORKING PROCESS FOUND"
  exit 1
fi

sudo BPFTRACE_MAX_STRLEN=235 bpftrace -e '
usdt::sys:sessions_time_measure {
  printf("%s", strftime("%H:%M:%S", nsecs));
}
usdt::sys:sessions_time_measure
{
  printf("%s", strftime("%H:%M:%S", nsecs));
}
usdt::sys:tasklet_time_out_of_bounds_measure {
    printf("%s", strftime("%H:%M:%S", nsecs));
}
' -p $(pgrep ${process_name})