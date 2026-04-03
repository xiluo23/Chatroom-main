#!/bin/bash

set -e 

if [ -d `pwd`/build ]; then
    rm -rf `pwd`/build
fi

mkdir build && cd `pwd`/build &&
    cmake ..&&
    make
cd ..
