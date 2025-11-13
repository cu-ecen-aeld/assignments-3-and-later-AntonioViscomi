#!/bin/bash

WRITEFILE=$1
WRITESTR=$2

if [ $# -ne 2 ]
then
	echo "Missing arguments. Exit with value 1."
	exit 1
else 
	mkdir -p "$(dirname ${WRITEFILE})"
	if [ $? -ne 0 ]
	then
		echo "Cannot create ${(dirname ${WRITEFILE})}"
		exit 1	
	else
		echo $WRITESTR > $WRITEFILE
		if [ $? -ne 0  ]
		then
			echo "Cannot create ${WRITEFILE}"
			exit 1
		fi
	fi
fi

exit 0
