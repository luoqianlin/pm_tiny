#!/bin/sh
#for busybox
prog_name="pm_tiny"
if [ -n "$1" ]; then
  prog_name=$1
fi
pgrp_id=$(ps -o pid,ppid,pgid,comm | grep $prog_name | awk -v prog_name="$prog_name" '{if($4=prog_name) print $3 }')

ps -o pgid,ppid,pid,comm | awk -v pgrp_id="$pgrp_id" '{if(NR==1||$1==pgrp_id) { print }}'
