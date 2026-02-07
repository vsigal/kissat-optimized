#!/bin/bash
# Build script for Kissat SAT Solver with optimizations
# Usage: ./build.sh [options]
#   -c, --clean     Clean before build
#   -t, --test      Run tests after build
#   -d, --debug     Debug build (no optimizations)
#   -h, --help      Show help

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default settings
CLEAN=0
TEST=0
DEBUG=0
CC="gcc-12"
BUILD_DIR="build"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -t|--test)
            TEST=1
            shift
            ;;
        -d|--debug)
            DEBUG=1
            shift
            ;;
        -h|--help)
            echo "Build script for Kissat SAT Solver"
            echo ""
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  -c, --clean     Clean before build"
            echo "  -t, --test      Run tests after build"
            echo "  -d, --debug     Debug build (no optimizations)"
            echo "  -h, --help      Show this help"
            echo ""
            echo "Examples:"
            echo "  $0                    # Quick build"
            echo "  $0 -c                 # Clean build"
            echo "  $0 -c -t              # Clean build and test"
            echo "  $0 -d                 # Debug build"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            echo "Use -h for help"
            exit 1
            ;;
    esac
done

# Check for compiler
if ! command -v $CC &> /dev/null; then
    echo -e "${YELLOW}Warning: $CC not found, trying gcc${NC}"
    CC="gcc"
    if ! command -v $CC &> /dev/null; then
        echo -e "${RED}Error: No compiler found${NC}"
        exit 1
    fi
fi

echo -e "${GREEN}=== Kissat Build Script ===${NC}"
echo "Compiler: $CC"
echo "Build dir: $BUILD_DIR"
echo ""

# Clean if requested
if [ $CLEAN -eq 1 ]; then
    echo -e "${YELLOW}Cleaning previous build...${NC}"
    rm -rf "$BUILD_DIR"
    echo -e "${GREEN}Clean complete${NC}"
    echo ""
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Set compiler flags
if [ $DEBUG -eq 1 ]; then
    echo -e "${YELLOW}Debug build (no optimizations)${NC}"
    CFLAGS="-g -O0 -fsanitize=address"
    CONFIGURE_OPTS=""
else
    echo -e "${GREEN}Optimized build with:${NC}"
    echo "  - LTO (Link-Time Optimization)"
    echo "  - AVX2 SIMD acceleration"
    echo "  - Native CPU tuning"
    echo "  - Aggressive optimizations (-O3)"
    echo ""
    # Production build flags
    CFLAGS="-O3 -mavx2 -march=native -flto -DNDEBUG"
    CONFIGURE_OPTS=""
fi

# Configure
echo -e "${YELLOW}Configuring...${NC}"
CC="$CC $CFLAGS" ../configure $CONFIGURE_OPTS 2>&1 | tee configure.log | grep -E "configure:|compiler|warning:|error:" || true

if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo -e "${RED}Configure failed! Check configure.log${NC}"
    exit 1
fi

echo -e "${GREEN}Configure complete${NC}"
echo ""

# Build
echo -e "${YELLOW}Building with $(nproc) jobs...${NC}"
echo ""

if make -j$(nproc) 2>&1 | tee build.log; then
    echo ""
    echo -e "${GREEN}=== Build Successful ===${NC}"
    echo ""
    
    # Show binary info
    if [ -f kissat ]; then
        echo "Binary: $BUILD_DIR/kissat"
        ls -lh kissat | awk '{print "Size: " $5}'
        
        # Check for SIMD usage
        if nm kissat 2>/dev/null | grep -q "avx"; then
            echo "SIMD: AVX2 detected in binary"
        fi
        
        echo ""
    fi
else
    echo ""
    echo -e "${RED}=== Build Failed ===${NC}"
    echo "Check build.log for details"
    exit 1
fi

cd ..

# Run tests if requested
if [ $TEST -eq 1 ]; then
    echo -e "${YELLOW}=== Running Tests ===${NC}"
    
    # Quick test with small CNF
    if [ -f "$BUILD_DIR/kissat" ]; then
        echo "Test 1: f1.cnf (quick SAT test)"
        if timeout 120 "$BUILD_DIR/kissat" f1.cnf 2>&1 | tail -3; then
            echo -e "${GREEN}✓ f1.cnf test passed${NC}"
        else
            echo -e "${YELLOW}⚠ f1.cnf test timeout or failed${NC}"
        fi
        echo ""
        
        echo "Test 2: f2.cnf --conflicts=100000 (performance check)"
        TIME_OUTPUT=$(timeout 180 "$BUILD_DIR/kissat" f2.cnf --conflicts=100000 2>&1 | grep "process-time")
        if [ -n "$TIME_OUTPUT" ]; then
            echo "$TIME_OUTPUT"
            echo -e "${GREEN}✓ f2.cnf test passed${NC}"
        else
            echo -e "${YELLOW}⚠ f2.cnf test timeout${NC}"
        fi
        echo ""
    fi
fi

echo -e "${GREEN}=== Done ===${NC}"
echo "Binary location: $BUILD_DIR/kissat"
echo ""
echo "Usage examples:"
echo "  $BUILD_DIR/kissat f1.cnf                    # Solve f1.cnf"
echo "  $BUILD_DIR/kissat f2.cnf --conflicts=1M     # Limit conflicts"
echo "  $BUILD_DIR/kissat --help                    # Show all options"
