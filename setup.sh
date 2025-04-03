#!/bin/bash

sudo sysctl -w vm.nr_hugepages=4096

sudo ./script/nicctl.sh create_vf 0000:b1:00.1
sudo ./script/nicctl.sh create_vf 0000:4b:00.1
