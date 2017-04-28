#!/bin/bash

OUTFILE="trace.erf.gz"

# colors
WHITE='\033[1;37m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}capturing from odp 03:00.0 iface into $OUTFILE ${NC}"
sleep 2
if [ -e $OUTFILE ]; then
	rm -f $OUTFILE
fi
sudo tracesplit odp:"03:00.0" erf:$OUTFILE
echo -e "${YELLOW}done. outfile with captured packets is : ${WHITE}`ls -la $OUTFILE`${NC}
${YELLOW}use tracepktdump $OUTFILE to show dump of packets in hex.${NC}"
