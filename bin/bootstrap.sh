#!/bin/sh
set -e
cd "$(dirname "$0")/.."
cc -O2 -o kiln bootstrap.c
echo "kiln built successfully."
