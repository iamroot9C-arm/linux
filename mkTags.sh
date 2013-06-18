#!/bin/sh

export ARCH=arm
#export CROSS_COMPILE=arm-linux-gnueabihf-

make tags cscope -e KBUILD_SRC="$PWD"
