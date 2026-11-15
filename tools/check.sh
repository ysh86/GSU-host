#!/bin/sh
# Syntax-check gsu_host.c against stub headers and run the GSU programs on
# the mini simulator. Proves syntax and program logic only, not hardware.
set -e
cd "$(dirname "$0")"
gcc -std=c11 -Wall -Wextra -fsyntax-only -Istubs ../gsu_host.c
gcc -std=c11 -Wall -Wextra -fsyntax-only -DGSU_PROGRAM_DEMO=1 -Istubs ../gsu_host.c
python3 -I gsusim.py
echo "check.sh: all OK"
