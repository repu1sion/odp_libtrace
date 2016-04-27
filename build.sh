#!/bin/bash

export RTE_SDK=/mnt/raw/gdwk/dpdk/dpdk/
export RTE_TARGET=x86_64-native-linuxapp-gcc

./bootstrap.sh
./configure --with-dpdk=yes CFLAGS="-shared-libgcc -I/mnt/raw/gdwk/odp/odp-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/arch/x86" LIBS="-lodp-dpdk"
#./configure --with-dpdk=yes CFLAGS="-shared-libgcc -I/mnt/raw/gdwk/odp/odp-dpdk/include"
make LIBS="-lodp-dpdk"
sudo make install
sudo ldconfig
