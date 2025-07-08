#!/bin/bash

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
' -p $(pgrep gst)