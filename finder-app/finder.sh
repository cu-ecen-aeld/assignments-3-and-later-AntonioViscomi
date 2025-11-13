#!/bin/bash

FILESDIR=$1
SEARCHSTR=$2
args_num=$#

if [ $# -ne 2 ]
then
	echo "Missing arguments. Exit with value 1."
	exit 1
else
	if [ -d ${FILESDIR} ]
	then
		X=$(find "$FILESDIR" -type f | wc -l)
		Y=$(grep -r "$SEARCHSTR" "$FILESDIR"| wc -l)
		echo "The number of files are ${X} and the number of matching lines are ${Y}"
	else
		echo "${FILESDIR} is not a directory."
		exit 1
	fi

fi

exit 0