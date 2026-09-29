#!/bin/bash
set -e

LOG_FILE=$1

make -B ARCH=$ARCH | tee $LOG_FILE

for i in $(seq 1 1);
do
	echo "run" $i | tee -a "$LOG_FILE"
	./main | tee -a "$LOG_FILE"
done
