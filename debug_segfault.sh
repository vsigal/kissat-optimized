#!/bin/bash
# Debug script for AVX2 segfault issue
# Run this in new session to diagnose the crash

set -e

cd /home/vsigal/src/kissat

echo "=== Kissat AVX2 Segfault Debug ==="
echo ""

echo "1. Checking current build..."
if [ ! -f build/kissat ]; then
    echo "   Build not found, configuring..."
    rm -rf build
    CC="gcc-12 -O3 -mavx2 -march=native -g" ./configure
    cd build && make -j$(nproc)
    cd ..
else
    echo "   Build exists"
fi

echo ""
echo "2. Testing AVX2 detection..."
./build/kissat -v f2.cnf 2>&1 | grep "simd-0" || echo "   SIMD message not found (check verbose level)"

echo ""
echo "3. Running with timeout to catch segfault..."
timeout 30 ./build/kissat f2.cnf 2>&1 | tail -15

echo ""
echo "4. Exit code analysis..."
timeout 30 ./build/kissat f2.cnf > /dev/null 2>&1
EXIT_CODE=$?
if [ $EXIT_CODE -eq 139 ]; then
    echo "   Exit code 139 = SIGSEGV (confirmed)"
elif [ $EXIT_CODE -eq 124 ]; then
    echo "   Exit code 124 = timeout (no crash within 30s)"
else
    echo "   Exit code: $EXIT_CODE"
fi

echo ""
echo "5. GDB backtrace (interactive, type 'run ../f2.cnf', then 'bt' after crash)..."
echo "   Starting GDB..."
cd build && gdb -q ./kissat -ex "set pagination off" -ex "run ../f2.cnf" -ex "bt" -ex "quit" 2>&1 | tail -30

echo ""
echo "=== Debug Complete ==="
echo ""
echo "Next steps:"
echo "1. Check backtrace above for crash location"
echo "2. If crash is in simdscan.c, check gather operations"
echo "3. If crash is in arena/vector code, check for buffer overflow"
echo "4. See SESSION_CONTINUE.md for detailed debugging steps"
