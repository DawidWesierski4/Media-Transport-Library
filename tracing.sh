#!/bin/bash

if [ -z "$1" ]; then
  process_name="gstreamer"
else
  process_name="$1"
fi
regular_expression_numbers='^[0-9]+$'

if ! pgrep "${process_name}" > /dev/null ; then
  echo "NO ${process_name} WORKING PROCESS FOUND"
  exit 1
fi

sudo BPFTRACE_MAX_STRLEN=235 bpftrace -e '
usdt::sys:sessions_time_measure {
  printf("%s", strftime("%H:%M:%S", nsecs));
}
usdt::sys:tasklet_time_measure
{
  printf("%s", strftime("%H:%M:%S", nsecs));
}
usdt::sys:tasklet_time_out_of_bounds_measure {
    printf("%s", strftime("%H:%M:%S", nsecs));
}
usdt::sys:log_msg
{
  if (arg0 > 2) {
    printf("%s [%d ns] l%d: %s", strftime("%H:%M:%S", nsecs), nsecs, arg0, str(arg1));
  }
}
' -p $(pgrep ${process_name})  | tee -a /tmp/log