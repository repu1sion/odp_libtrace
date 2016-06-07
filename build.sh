#!/bin/bash

# colors
WHITE='\033[1;37m'
NC='\033[0m' # No Color


export RTE_SDK=/mnt/raw/gdwk/dpdk/dpdk/
export RTE_TARGET=x86_64-native-linuxapp-gcc
export LDFLAGS='-L/usr/local/lib/'

NUMCORES=$(nproc)
echo -e "${WHITE}number of cores detected: $NUMCORES ${NC}"

./bootstrap.sh

if [ $NUMCORES -le 2 ]; then
	echo -e "${WHITE}configuring for laptop ${NC}"
	./configure CFLAGS="-shared-libgcc -I/archive/repos/odp-dpdk/include -I/archive/repos/odp-dpdk/platform/linux-dpdk/include -I/archive/repos/odp-dpdk/platform/linux-dpdk/arch/x86"
else
	echo -e "${WHITE}configuring for big pc ${NC}"
	./configure CFLAGS="-shared-libgcc -I/mnt/raw/gdwk/odp/odp-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/arch/x86"
fi

#./configure --with-dpdk CFLAGS="-shared-libgcc"


make
sudo make install
sudo ldconfig
