#!/bin/sh
# Validate that both arguments were specified
if [ $# -lt 2 ]; then
    echo "Error: invalid number of arguments"
    echo "Usage: $0 <filesdir> <searchstr>"
    exit 1
fi

filesdir=$1
searchstr=$2

# Validate that filesdir is a directory on the filesystem
if [ ! -d "$filesdir" ]; then
    echo "Error: ${filesdir} is not a directory on the filesystem"
    exit 1
fi

# X: number of files in the directory and all subdirectories
numfiles=$(find "$filesdir" -type f | wc -l | tr -d '[:space:]')

# Y: number of lines across those files that contain searchstr
numlines=$(grep -r "$searchstr" "$filesdir" 2>/dev/null | wc -l | tr -d '[:space:]')

echo "The number of files are ${numfiles} and the number of matching lines are ${numlines}"