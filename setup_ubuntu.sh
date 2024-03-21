#/bin/bash

echo "1.1 Install the build dependency from OS software store"

if [ -z "$http_proxy" ]; then
    export http_proxy=http://proxy-dmz.intel.com:912
    export https_proxy=http://proxy-dmz.intel.com:912
fi

sudo apt-get update
sudo apt-get install git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libssl-dev -y
sudo pip install pyelftools

export http_proxy=""
sudo pip install ninja #This package dosn't want to be downloaded with proxy
export http_proxy=http://proxy-dmz.intel.com:912

echo "1.2 Build dependency from source code"

git clone https://github.com/json-c/json-c.git -b json-c-0.16
cd json-c/
mkdir build
cd build
cmake ../
make
sudo make install
cd ../../

git clone https://github.com/the-tcpdump-group/libpcap.git -b libpcap-1.9
cd libpcap/
./configure
make
sudo make install
cd ..

git clone https://github.com/google/googletest.git -b v1.13.x
cd googletest/
mkdir build
cd build/
cmake ../
make
sudo make install
cd ../../../

export imtl_source_code=${PWD}/Media-Transport-Library

git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v23.11
git switch -c v23.11
cd ..

cd dpdk
git am $imtl_source_code/patches/dpdk/23.11/*.patch

meson setup build
ninja -C build
sudo ninja install -C build
cd ..

meson setup build | grep numa 
