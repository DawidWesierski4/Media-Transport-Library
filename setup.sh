#!/bin/bash

sudo sysctl -w vm.nr_hugepages=32768

sudo ./script/nicctl.sh create_vf 0000:b1:00.0
sudo ./script/nicctl.sh create_vf 0000:b1:00.1
sudo ./script/nicctl.sh create_vf 0000:4b:00.0
sudo ./script/nicctl.sh create_vf 0000:4b:00.1
