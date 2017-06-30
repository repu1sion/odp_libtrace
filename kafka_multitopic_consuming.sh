#!/bin/bash

export KAFKA_CONSUME_TOPIC1="capt"
export KAFKA_CONSUME_TOPIC2="capt2"
#printenv

USERID=`id -u`
echo "userid is : $USERID"
if [ ! $USERID -eq 0 ]; then
	# we need -E to keep env variables from common user
	echo "not root. running with sudo -E"
	sudo -E tracesplit kafka:k erf:trace.erf.gz
else
	echo "executing as root"
	tracesplit kafka:k erf:trace.erf.gz
fi
