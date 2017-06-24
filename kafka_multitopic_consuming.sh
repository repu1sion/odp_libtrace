#!/bin/bash

export KAFKA_CONSUME_TOPIC1="capt"
export KAFKA_CONSUME_TOPIC2="capt2"
#printenv

# we need -E to keep env variables from common user
sudo -E tracesplit kafka:k erf:trace.erf.gz
