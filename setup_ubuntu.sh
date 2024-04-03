#!/bin/bash

cd "$(dirname "$0")" || exit 2
export imtl_source_code=${PWD}

build_json-c_from_source() {
    cd "$imtl_source_code/../"

    if [ -d "json-c" ] && [[ $1 == "f" ]]; then
      rm -drf json-c
    fi
    git clone https://github.com/json-c/json-c.git -b json-c-0.16
    cd json-c/
    mkdir build
    cd build
    cmake ../
    make
    sudo make install
}

build_googletest_from_source() {
    cd "$imtl_source_code/../"

    if [ -d "googletest" ] && [[ $1 == "f" ]]; then
      rm -drf googletest
    fi

    git clone https://github.com/google/googletest.git -b v1.13.x
    cd googletest/
    mkdir build
    cd build/
    cmake ../
    make
    sudo make install
}

build_libpcap_from_source() {
    cd "$imtl_source_code/../"

    if [ -d "libpcap" ] && [[ $1 == "f" ]]; then
      rm -drf libpcap
    fi

    git clone https://github.com/the-tcpdump-group/libpcap.git -b libpcap-1.9
    cd libpcap/
    ./configure
    make
    sudo make install
    cd ..

}

build_initialize_dpdk_from_source() {
  cd "$imtl_source_code/../"

  if [ -d "dpdk" ] && [[ $1 == "f" ]]; then
    rm -drf dpdk
  fi

  git clone https://github.com/DPDK/dpdk.git
  cd dpdk
  git checkout v23.11
  git switch -c v23.11

  git am "$imtl_source_code"/patches/dpdk/23.11/*.patch

  meson setup build | tee /tmp/dpdk_build.log
  ninja -C build
  sudo ninja install -C build
  cd ..

  sleep 3

  if ! grep -q "Library numa found: YES" /tmp/dpdk_build.log; then
      echo -e "\033[31m[ERROR] Library numa not found\033[0m"
      exit 2
  fi
}


initialize_driver_e810_nic() {
  cd "$imtl_source_code/../"

  if [ ! -e "ice-1.13.7.tar.gz" ]; then
    if [[ $1 == "f" ]]; then
      rm -drf ice-1.13.7*
      wget https://downloadmirror.intel.com/812404/ice-1.13.7.tar.gz
    elif [ -d "ice-1.13.7" ]; then
      rm -drf "ice-1.13.7"
    fi
  else
    wget https://downloadmirror.intel.com/812404/ice-1.13.7.tar.gz
  fi

  tar xvzf ice-1.13.7.tar.gz
  cd ice-1.13.7

  git init
  git add .
  set +e
  git commit -m "init version 1.13.7"
  set -e
  git am "$imtl_source_code"/patches/ice_drv/1.13.7/*.patch


  cd src
  make
  sudo make install
  sudo rmmod ice

  sleep 3 # idk if needed

  sudo modprobe ice

  sleep 3 # idk if needed

  if ! sudo dmesg | grep -q "Intel(R) Ethernet Connection E800 Series Linux Driver - version Kahawai"; then
      echo -e "\033[31m[ERROR] Ice driver didn't initialize correctly \033[0m"
      exit 2
  elif ! sudo dmesg | grep -q "The DDP package was successfully loaded: ICE OS Default Package version 1.3.35.0"; then
      echo -e "\033[31m[ERROR] Ice driver DDP package version 1.3.35.0 didn't initialize, or initialized wrong version (check dmseg for more details) \033[0m"
      exit 2
  fi

}

update_firmware_e810_nic() {
  cd "$imtl_source_code/../"

  wget https://downloadmirror.intel.com/816410/Release_29.0.zip
  unzip Release_29.0.zip
  cd NVMUpdatePackage/E810

  tar xvf E810_NVMUpdatePackage_v4_40_Linux.tar.gz 
  cd E810/Linux_x64/

  ./nvmupdate64e
}


DPDK_PMD_setup_e810_nic() {
  if ! groups | grep -q vfio ; then
    getent group 2110 || sudo groupadd -g 2110 vfio
    sudo usermod -aG vfio "${USER}"
  fi

  if [ ! -f "/etc/udev/rules.d/10-vfio.rules" ] || ! grep -q 'SUBSYSTEM=="vfio", GROUP="vfio", MODE="0660"' /etc/udev/rules.d/10-vfio.rules; then
    sudo echo 'SUBSYSTEM=="vfio", GROUP="vfio", MODE="0660"' | sudo tee -a /etc/udev/rules.d/10-vfio.rules
    sudo udevadm control --reload-rules
    sudo udevadm trigger
  fi

  NIC_810_PCI="$(lshw -c net -businfo | grep E810 | head -1 | awk '{print $1}' | sed 's/pci@//g')"
  export NIC_810_PCI=$NIC_810_PCI

  if [ -z "$NIC_810_PCI" ]; then
     echo -e "\033[31m[ERROR] DPDK_PMD_setup 810 pci address not found \033[0m"
  fi

  cd "$imtl_source_code"
  sudo ./script/nicctl.sh create_vf "$NIC_810_PCI"
  if [ -z "$(ls -lg /dev/vfio/*)" ]; then
    echo -e "\033[31m[ERROR] newly created VFIO device is NOT correctly assigned to the vfio group \033[0m"
  fi

  sudo ./script/nicctl.sh bind_pmd "$NIC_810_PCI"
  sudo sysctl -w vm.nr_hugepages=2048
}

check_if_iommu_enabled() {
  # Read the GRUB_CMDLINE_LINUX_DEFAULT line
  CMDLINE=$(grep "GRUB_CMDLINE_LINUX_DEFAULT=" "/etc/default/grub")

  # Check for the presence of "intel_iommu=on" and "iommu=pt"
  if [[ "$CMDLINE" != *"intel_iommu=on"* ]] || [[ "$CMDLINE" != *"iommu=pt"* ]]; then
      echo -e "\033[31m[ERROR] One or both of 'intel_iommu=on' and 'iommu=pt' are missing in GRUB_CMDLINE_LINUX_DEFAULT. \033[0m"
      exit 2
  fi

  # Check if CPU flags has vmx feature
  if ! lscpu | grep -q vmx; then
      echo -e "\033[31m[ERROR] CPU flags vmx feature not enabled \033[0m"
      exit 2
  fi

  if [ -z "$(ls /sys/kernel/iommu_groups/)" ]; then
     echo -e "\033[31m[ERROR] IOMMU not enabled\033[0m"
  fi
}

check_secure_path_for_root_user() {
  # Read the secure_path line
  CMDLINE=$(grep "secure_path=" "/etc/sudoers")

  # Check for the presence of "intel_iommu=on" and "iommu=pt"
  if [[ "$CMDLINE" != *"/usr/local/bin"* ]]; then
      echo -e "\033[31m[ERROR] From file /etc/sudoers, find secure_path doesn't include /usr/local/bin \033[0m"
      exit 2
  fi
}

build_setup_for_ubuntu() {
  ### https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/doc/build.md

  cd "$imtl_source_code"

  if [ -z "$http_proxy" ]; then
      export http_proxy=http://proxy-dmz.intel.com:912
      export https_proxy=http://proxy-dmz.intel.com:912
  fi

  sudo sysctl -w vm.nr_hugepages=2048 # from RUN.md
  sudo apt-get install -y python3
  sudo apt-get install -y python3-pip
  sudo apt-get install -y pkg-config
  sudo apt-get install -y libsdl2-dev
  sudo apt-get install -y libsdl2-ttf-dev
  sudo apt-get install -y libnuma-dev
  sudo apt-get install -y libssl-dev
  sudo apt-get install -y meson
  sudo apt-get install -y cmake

  # These dependencies are needed by the gtest / pcap / json-c
  sudo apt-get install -y gcc-12 flex bison

  sudo pip install pyelftools
  sudo pip install ninja # This package doesn't want to be downloaded with proxy

  build_json-c_from_source f
  build_googletest_from_source f
  build_libpcap_from_source f

  check_secure_path_for_root_user
  build_initialize_dpdk_from_source f

  cd "$imtl_source_code"
  if [ -d "./build" ]; then
    rm -drf build
  fi

  ./build.sh "$1"
}

run_E810_setup_for_ubuntu() {
  ### https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/doc/run.md

  cd "$imtl_source_code"

  check_if_iommu_enabled

  ### https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/doc/e810.md
    initialize_driver_e810_nic f
    update_firmware_e810_nic &
  ###

  sudo sysctl -w vm.nr_hugepages=2048 
  DPDK_PMD_setup_e810_nic
}

show_help() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -u      [arg]  Configure IMTL for Ubuntu [arg] == build / release for iMTL "
    echo "  -p             Only setup DPDK_PMD for 810 NIC."
    echo "  -h             Display this help message and exit."
}

while getopts "u:ph" opt; do
    case ${opt} in
        u )
            set -x -e
            # build_setup_for_ubuntu "$OPTARG"
            run_E810_setup_for_ubuntu
            set +x +e
            ;;
        p )
            set -x
            DPDK_PMD_setup_e810_nic
            set +x
            ;;
        h )
            show_help
            exit 0
            ;;
        \? )
            echo "Invalid option: $OPTARG" 1>&2
            show_help
            exit 1
            ;;
        : )
            echo "Invalid option: $OPTARG requires an argument" 1>&2
            exit 1
            ;;
    esac
done

sudo newgrp vfio
