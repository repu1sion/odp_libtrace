#!/bin/bash

export RTE_SDK=/root/dpdk/
export RTE_TARGET=x86_64-native-linuxapp-gcc

# loading dpdk kernel modules
cd $RTE_SDK/$RTE_TARGET/kmod
sudo modprobe uio
sudo rmmod igb_uio.ko &> /dev/null
sudo insmod ./igb_uio.ko
lsmod | grep -i uio

# bind network card to dpdk driver
cd $RTE_SDK/usertools/
sudo ./dpdk-devbind.py -b igb_uio 0000:03:00.0
sudo ./dpdk-devbind.py --status

# setup hugepages
sudo sysctl -w vm.nr_hugepages=1000
sudo sysctl vm.nr_hugepages
cat /proc/meminfo | grep -i huge
if [ ! -d /mnt/huge ]; then
	sudo mkdir /mnt/huge
fi
cat /proc/mounts | grep -i huge > /dev/null
if [ ! $? -eq 0 ]; then
	sudo mount -t hugetlbfs nodev /mnt/huge
fi
cat /proc/mounts | grep -i huge
