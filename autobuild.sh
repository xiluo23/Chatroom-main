#!/bin/bash

set -e 

mkdir build && cd `pwd`/build &&
    cmake ..&&
    make
cd ..
