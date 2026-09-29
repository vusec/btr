#!/bin/bash
set -e

if [[ $EUID -ne 0 ]]; then
   echo "Please run as root"
   exit 1
fi

make

rmmod btr_help_module || true
dmesg -C
insmod btr_help_module.ko || (dmesg && false)
dmesg
