#!/bin/sh
prog_name="pm_tiny"
if [ -n "$1" ]; then
  prog_name=$1
fi
echo "$prog_name"
pgrp_id=$(ps axo tty,sess,pgrp,ppid,pid,pcpu,comm | grep $prog_name | awk -v prog_name="$prog_name" '{if($7==prog_name){ print $3 }}')
echo $pgrp_id

ps axo tty,sess,pgrp,ppid,pid,pcpu,comm | awk -v pgrp_id="$pgrp_id" '{if(NR==1||$3==pgrp_id) { print }}'
