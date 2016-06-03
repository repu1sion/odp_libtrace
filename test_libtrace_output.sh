#!/bin/bash

TIMESTART=$(date +%s)
TIMEEND=$(($TIMESTART+10))

echo "time to start: $TIMESTART, time to end: $TIMEEND"

#sudo tracesplit -s $TIMESTART -e $TIMEEND int:eth0 erf:trace.erf.gz &
sudo tracesplit -s $TIMESTART -e $TIMEEND odp:"03:00.0" erf:trace.erf.gz &
echo "collecting is over. now time is : $(date +%s)"

#tracepktdump trace.erf.gz
