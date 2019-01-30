run tracesplit in spdk read format:
-----
1. from spdk root setup spdk:
# ./scripts/setup.sh

2. run tracesplit
# tracesplit spdk:s erf:trace.erf.gz

tracesplit with max priority:
# nice -n -20 tracesplit spdk:s erf:trace.erf.gz

or saving spdk to pcap:
# tracesplit spdk:s pcap:trace.pcap


build libtrace:
-----
# ./clean.sh
# time ./build.sh > /dev/null && echo ok


test harddisk/ssd write speed with dd (without caches):
-----
# dd if=/dev/zero of=here bs=1G count=1 oflag=direct


perf top(see how code consumes cpu)
-----
perf top -t 1780896
