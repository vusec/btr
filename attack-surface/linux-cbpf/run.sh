#!/bin/bash
set -e


OWN_DIR=`dirname $0`

cd ${OWN_DIR}/user

make -B ARCH=$ARCH


time taskset -c 0 sudo ./main $1 $2
sudo pkill main -9
