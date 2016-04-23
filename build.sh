#!/bin/bash

export RTE_SDK=/mnt/raw/gdwk/dpdk/dpdk/
export RTE_TARGET=x86_64-native-linuxapp-gcc

./bootstrap.sh
./configure --with-dpdk=yes CFLAGS=-shared-libgcc
make
sudo make install
