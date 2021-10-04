#!/bin/sh
#for busybox
show_pm_tiny_process() {
  prog_name="pm_tiny"
  if [ -n "$1" ]; then
    prog_name=$1
  fi

  echo "$prog_name"

  prog_pid=$(ps -o pid,ppid,pgid,comm | grep $prog_name | awk -v prog_name="$prog_name" '{if($4==prog_name) print $1 }')

  echo "pid:${prog_pid}"
  echo "-----"
  ps -o pgid,ppid,pid,comm,tty | awk -v prog_pid="$prog_pid" '{if(NR==1||$2==prog_pid||$3==prog_pid) { print }}'
}

pm_tiny_pid() {
  PM_PID=$(ps -ef | grep pm_tiny | grep -v grep | awk '{ print $1 }')
}

pm_tiny_exist() {
  pm_tiny_pid
  if [ -z "$PM_PID" ]; then
    return 0
  fi
  return 1
}

while true; do
  if ! pm_tiny_exist; then
    echo "pm_tiny not exist"
    break
  fi
done
