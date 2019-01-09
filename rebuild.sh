#!/bin/bash

make -j${NUMCORES} && sudo make install && sudo ldconfig && echo "built successfully"
