#!/bin/sh
prog_name="pm_tiny"
if [ -n "$1" ]; then
  prog_name=$1
fi
echo "$prog_name"
prog_pid=$(ps axo tty,sess,pgrp,ppid,pid,pcpu,comm | grep $prog_name | awk -v prog_name="$prog_name" '{if($7==prog_name){ print $5 }}')
echo $prog_pid

ps axo tty,sess,pgrp,ppid,pid,pcpu,comm,pmem,rss,user | awk -v prog_pid="$prog_pid" '{if(NR==1||$4==prog_pid||$5==prog_pid) { print }}'
