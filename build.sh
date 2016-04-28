#!/bin/bash

export RTE_SDK=/mnt/raw/gdwk/dpdk/dpdk/
export RTE_TARGET=x86_64-native-linuxapp-gcc
export LDFLAGS='-L/usr/local/lib/'
#export LIBS='-lodp-dpdk'

./bootstrap.sh
./configure --with-dpdk=yes CFLAGS="-shared-libgcc -I/mnt/raw/gdwk/odp/odp-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/arch/x86"
# make LIBS="-lodp-dpdk"
make
sudo make install
sudo ldconfig
