#!/bin/sh

# Validate that both arguments were specified
if [ $# -lt 2 ]; then
    echo "Error: invalid number of arguments"
    echo "Usage: $0 <writefile> <writestr>"
    exit 1
fi

writefile=$1
writestr=$2

# Create the containing directory path if it does not already exist
if ! mkdir -p "$(dirname "$writefile")"; then
    echo "Error: could not create directory path for ${writefile}"
    exit 1
fi

# Write the string, overwriting any existing file
if ! echo "$writestr" > "$writefile"; then
    echo "Error: could not create file ${writefile}"
    exit 1
fi