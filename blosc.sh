#!/bin/bash

sudo tracesplit -z 5 -Z blosc:zlib odp:"03:00.0" erf:trace.gz
#valid types are: blosclz, lz4hc, lz4, snappy, zlib, zstd

