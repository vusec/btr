#!/bin/bash
set -e

OWN_DIR=`dirname $0`
mkdir -p ${OWN_DIR}/results/
LOG_FILE=${OWN_DIR}/results/log_${ARCH}.txt

echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

sudo ./do_experiment.sh "$LOG_FILE"
sudo chown -R $(whoami):$(whoami) ${OWN_DIR}/results/

python3 aggregate.py ${LOG_FILE} ${OWN_DIR}/results/agg_${ARCH}.txt | tee ${OWN_DIR}/results/summary_$ARCH.txt
