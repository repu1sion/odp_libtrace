#!/bin/bash

# headers needed:
# -----
#1. odp-dpdk/include
#2. odp-dpdk/platform/linux-dpdk
#3. odp-dpdk/platform/linux-dpdk/arch/x86


#export RTE_SDK=/mnt/raw/gdwk/dpdk/dpdk/
#export RTE_TARGET=x86_64-native-linuxapp-gcc
#export LDFLAGS='-L/usr/local/lib/'

INSTALL_WANDIO=0

# colors
WHITE='\033[1;37m'
RED='\033[0;31m'
NC='\033[0m' # No Color

HEADER1="include"
HEADER2="platform/linux-dpdk/include"
HEADER3="platform/linux-dpdk/arch/x86"

ODP_PATH=""
SEARCH_PATH="/"
NUMCORES=$(nproc)

# parsing args
if [ $# -eq 1 ]; then
	SEARCH_PATH=$1
fi

# search for odp-dpdk
echo -e "${WHITE}searching for odp in $SEARCH_PATH ${NC}"
ODP_PATH=`dirname $(find $SEARCH_PATH -mount -type d -name 'odp' -exec sh -c "find '{}' -maxdepth 1 -name README" \;)`
#ODP_PATH=`find $SEARCH_PATH -mount -type d -name 'odp' | awk '{if (NR==1) print $1}'`
if [ ! -z $ODP_PATH ]; then
	echo -e "${WHITE}odp found successfully. path is : $ODP_PATH ${NC}"
else
	echo -e "${RED}odp was not found. please try to pass its path as an argument to that script ${NC}"
	exit 1
fi


# dl and install wandio
if [ $INSTALL_WANDIO -eq 1 ]; then
	echo "installing wandio"
	wget http://research.wand.net.nz/software/wandio/wandio-1.0.4.tar.gz
	tar zxvf wandio-1.0.4.tar.gz
	cd wandio-1.0.4/
	./configure 
	make
	make install
	ldconfig
fi


# configuring
./bootstrap.sh

./configure CFLAGS="-g -shared-libgcc -I${ODP_PATH}/${HEADER1} -I${ODP_PATH}/${HEADER2} -I${ODP_PATH}/${HEADER3}"

#if [ $NUMCORES -le 2 ]; then
#	echo -e "${WHITE}configuring for laptop ${NC}"
#	./configure CFLAGS="-shared-libgcc -I/archive/repos/odp-dpdk/include -I/archive/repos/odp-dpdk/platform/linux-dpdk/include -I/archive/repos/odp-dpdk/platform/linux-dpdk/arch/x86"
#else
#	echo -e "${WHITE}configuring for big pc ${NC}"
#	./configure CFLAGS="-shared-libgcc -I/mnt/raw/gdwk/odp/odp-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/include -I/mnt/raw/gdwk/odp/odp-dpdk/platform/linux-dpdk/arch/x86"
#fi

#./configure --with-dpdk CFLAGS="-shared-libgcc"

# building
echo -e "${WHITE}number of cores detected: $NUMCORES ${NC}"
make -j${NUMCORES} && sudo make install && sudo ldconfig
