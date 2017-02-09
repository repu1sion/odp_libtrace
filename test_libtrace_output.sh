#!/bin/bash

red='\033[0;31m'
white='\033[1;37m'
NC='\033[0m' # No Color

TIMESTART=$(date +%s)
TIMEEND=$(($TIMESTART+20))

OUTFILE=erf:trace.gz

echo "time to start: $TIMESTART, time to end: $TIMEEND"

#sudo tracesplit -s $TIMESTART -e $TIMEEND int:eth0 erf:trace.erf.gz &
sudo tracesplit -z 1 -Z gzip -s $TIMESTART -e $TIMEEND odp:"03:00.0" $OUTFILE
echo -e "${white}collecting is over. now time is : $(date +%s) output: $OUTFILE ${NC}"
echo "done"

#tracepktdump trace.erf.gz
