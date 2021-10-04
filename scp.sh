#!/bin/bash
LOCAL_DIR=".ax_build/_install/Release/bin"
IFS=";"
for HOST in $HOSTS; do
 sshpass -p $MY_PASSWORD scp ${LOCAL_DIR}/pm_tiny ${HOST}:/usr/bin/
 sshpass -p $MY_PASSWORD scp ${LOCAL_DIR}/pm ${HOST}:/usr/bin/
done

