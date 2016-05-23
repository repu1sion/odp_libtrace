#!/bin/bash

export RTE_SDK=/mnt/raw/gdwk/dpdk/dpdk/
export RTE_TARGET=x86_64-native-linuxapp-gcc
export LDFLAGS='-L/usr/local/lib/'

./bootstrap.sh
./configure CFLAGS="-shared-libgcc -I/mnt/raw/gdwk/odp/odp-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/arch/x86"
#./configure --with-dpdk CFLAGS="-shared-libgcc"
make
sudo make install
sudo ldconfig
