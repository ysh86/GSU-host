#!/bin/sh
# Syntax-check gsu_host.c against stub headers and run the GSU programs on
# the mini simulator. Proves syntax and program logic only, not hardware.
set -e
cd "$(dirname "$0")"
gcc -std=c11 -Wall -Wextra -fsyntax-only -Istubs ../gsu_host.c
gcc -std=c11 -Wall -Wextra -fsyntax-only -DGSU_PROGRAM_DEMO=1 -Istubs ../gsu_host.c
gcc -std=c11 -Wall -Wextra -fsyntax-only -DGSU_MEASURE=1 -Istubs ../gsu_host.c
gcc -std=c11 -Wall -Wextra -fsyntax-only -DGSU_DIAG=1 -Istubs ../gsu_host.c
gcc -std=c11 -Wall -Wextra -fsyntax-only -DGSU_CLOCK_21MHZ=0 -Istubs ../gsu_host.c
gcc -std=c11 -Wall -Wextra -fsyntax-only -DGSU_TRACE=1 -DGSU_PROGRAM_DEMO=1 -Istubs ../gsu_host.c
gcc -std=c11 -Wall -Wextra -fsyntax-only -DGSU_BOOT_STAGE1=1 -Istubs ../gsu_host.c
gcc -std=c11 -Wall -Wextra -fsyntax-only -DGSU_MC1=1 -Istubs ../gsu_host.c
gcc -std=c11 -Wall -Wextra -fsyntax-only -DRELOAD_CACHE_ON_RESUME=1 -Istubs ../gsu_host.c
python3 -I gsusim.py
echo "check.sh: all OK"
