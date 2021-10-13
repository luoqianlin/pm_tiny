#!/bin/sh
i=0;
while true 
do
	echo $0 $1 $2
	i=$((i=i+1))
	echo "i=$i"
	v=$((i%100))
	echo "v=${v}"
	if [ $v -eq 0 ];	then
		exit 1
	fi
	sleep 1
done
