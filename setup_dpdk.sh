#!/bin/bash

# loading dpdk kernel modules
cd $RTE_SDK/$RTE_TARGET/kmod
sudo modprobe uio
sudo insmod ./igb_uio.ko
lsmod | grep -i uio

# bind network card to dpdk driver
cd $RTE_SDK/tools/
sudo ./dpdk_nic_bind.py -b igb_uio 0000:03:00.0
sudo ./dpdk_nic_bind.py --status

# setup hugepages
sudo sysctl -w vm.nr_hugepages=500
sudo sysctl vm.nr_hugepages
cat /proc/meminfo | grep -i huge
