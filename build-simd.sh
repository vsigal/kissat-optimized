#!/bin/bash
#
# SIMD-optimized build script for Kissat SAT Solver
# Supports Intel (GCC) and AMD Genoa 4/5 (AOCC)
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

# Compiler paths
AOCC_PATH="/opt/AMD/aocc-compiler"
AOCC_BIN="${AOCC_PATH}/bin/clang"

# Default options
USE_AOCC=0
USE_GCC=0
VERBOSE=0
CLEAN=0
JOBS=$(nproc)

# Print help
print_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --aocc        Use AMD AOCC compiler (for AMD Genoa 4/5)"
    echo "  --gcc         Use GCC compiler (for Intel)"
    echo "  --auto        Auto-detect best compiler (default)"
    echo "  --clean       Clean build directory first"
    echo "  --verbose     Verbose output"
    echo "  -j N          Number of parallel jobs (default: $(nproc))"
    echo "  --help        Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 --gcc                    # Build with GCC for Intel"
    echo "  $0 --aocc                   # Build with AOCC for AMD"
    echo "  $0 --gcc --clean            # Clean build with GCC"
    echo "  $0 --aocc -j 32             # AOCC build with 32 jobs"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --aocc)
            USE_AOCC=1
            shift
            ;;
        --gcc)
            USE_GCC=1
            shift
            ;;
        --auto)
            USE_AOCC=0
            USE_GCC=0
            shift
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        -j)
            JOBS="$2"
            shift 2
            ;;
        --help)
            print_help
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            print_help
            exit 1
            ;;
    esac
done

# Detect CPU vendor
detect_cpu() {
    local vendor=$(grep -m1 'vendor_id' /proc/cpuinfo | cut -d: -f2 | tr -d ' ')
    local model=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2)
    
    echo -e "${BLUE}Detected CPU: ${model}${NC}"
    
    if [[ "$vendor" == "AuthenticAMD" ]]; then
        echo "AMD"
    elif [[ "$vendor" == "GenuineIntel" ]]; then
        echo "Intel"
    else
        echo "Unknown"
    fi
}

# Check for AOCC
check_aocc() {
    if [[ -x "$AOCC_BIN" ]]; then
        local version=$($AOCC_BIN --version 2>/dev/null | head -1)
        echo -e "${GREEN}Found AOCC: ${version}${NC}"
        return 0
    else
        echo -e "${YELLOW}AOCC not found at ${AOCC_BIN}${NC}"
        return 1
    fi
}

# Check for GCC
check_gcc() {
    if command -v gcc &> /dev/null; then
        local version=$(gcc --version | head -1)
        echo -e "${GREEN}Found GCC: ${version}${NC}"
        return 0
    else
        echo -e "${YELLOW}GCC not found${NC}"
        return 1
    fi
}

# Select compiler
select_compiler() {
    local cpu=$(detect_cpu)
    
    echo ""
    echo -e "${BLUE}=== Compiler Selection ===${NC}"
    
    # If explicitly specified, use that
    if [[ $USE_AOCC -eq 1 ]]; then
        if check_aocc; then
            SELECTED_COMPILER="AOCC"
            return 0
        else
            echo -e "${RED}ERROR: AOCC requested but not found${NC}"
            exit 1
        fi
    fi
    
    if [[ $USE_GCC -eq 1 ]]; then
        if check_gcc; then
            SELECTED_COMPILER="GCC"
            return 0
        else
            echo -e "${RED}ERROR: GCC requested but not found${NC}"
            exit 1
        fi
    fi
    
    # Auto-detect based on CPU
    echo "Auto-detecting best compiler..."
    
    if [[ "$cpu" == "AMD" ]]; then
        echo "AMD CPU detected - preferring AOCC"
        if check_aocc; then
            SELECTED_COMPILER="AOCC"
        else
            echo -e "${YELLOW}AOCC not found, falling back to GCC${NC}"
            if check_gcc; then
                SELECTED_COMPILER="GCC"
            else
                echo -e "${RED}ERROR: No compiler found${NC}"
                exit 1
            fi
        fi
    elif [[ "$cpu" == "Intel" ]]; then
        echo "Intel CPU detected - preferring GCC"
        if check_gcc; then
            SELECTED_COMPILER="GCC"
        else
            echo -e "${YELLOW}GCC not found, trying AOCC${NC}"
            if check_aocc; then
                SELECTED_COMPILER="AOCC"
            else
                echo -e "${RED}ERROR: No compiler found${NC}"
                exit 1
            fi
        fi
    else
        # Unknown CPU, try both
        echo "Unknown CPU - trying available compilers"
        if check_gcc; then
            SELECTED_COMPILER="GCC"
        elif check_aocc; then
            SELECTED_COMPILER="AOCC"
        else
            echo -e "${RED}ERROR: No compiler found${NC}"
            exit 1
        fi
    fi
}

# Detect CPU SIMD capabilities
detect_cpu_features() {
    local cpu_flags=$(grep -m1 'flags' /proc/cpuinfo | cut -d: -f2)
    
    HAS_AVX512F=$(echo "$cpu_flags" | grep -c 'avx512f')
    HAS_AVX512BW=$(echo "$cpu_flags" | grep -c 'avx512bw')
    HAS_AVX512VL=$(echo "$cpu_flags" | grep -c 'avx512vl')
    HAS_AVX512VPOPCNT=$(echo "$cpu_flags" | grep -c 'avx512_vpopcntdq')
    HAS_AVX512BITALG=$(echo "$cpu_flags" | grep -c 'avx512_bitalg')
    HAS_GFNI=$(echo "$cpu_flags" | grep -c 'gfni')
    HAS_AVX2=$(echo "$cpu_flags" | grep -c 'avx2')
    HAS_AVX=$(echo "$cpu_flags" | grep -c ' avx ')
}

# Set compiler flags for GCC (Intel-optimized)
set_gcc_flags_intel() {
    detect_cpu_features
    
    if [[ $HAS_AVX512F -gt 0 ]]; then
        # Intel Sapphire Rapids / Emerald Rapids (4th/5th gen Xeon)
        # Supports: AVX-512, GFNI, VPOPCNTDQ, BITALG, VBMI2, VNNI, BF16
        
        CFLAGS="-march=sapphirerapids"
        CFLAGS="${CFLAGS} -O3"
        CFLAGS="${CFLAGS} -fomit-frame-pointer"
        CFLAGS="${CFLAGS} -ffast-math"
        CFLAGS="${CFLAGS} -funroll-loops"
        
        # Enable all relevant instruction sets
        CFLAGS="${CFLAGS} -mavx512f -mavx512bw -mavx512vl"
        [[ $HAS_AVX512VPOPCNT -gt 0 ]] && CFLAGS="${CFLAGS} -mavx512vpopcntdq"
        [[ $HAS_AVX512BITALG -gt 0 ]] && CFLAGS="${CFLAGS} -mavx512bitalg"
        [[ $HAS_GFNI -gt 0 ]] && CFLAGS="${CFLAGS} -mgfni"
        CFLAGS="${CFLAGS} -mavx512vbmi -mavx512vbmi2"
        CFLAGS="${CFLAGS} -mavx512vnni -mavx512bf16"
        
        # Link-time optimization
        CFLAGS="${CFLAGS} -flto=auto"
        LDFLAGS="-flto=auto"
        
        echo -e "${GREEN}GCC flags for Intel (AVX-512):${NC}"
        echo "  Architecture: Sapphire Rapids"
        echo "  SIMD: AVX-512 + GFNI + VPOPCNTDQ + BITALG"
    elif [[ $HAS_AVX2 -gt 0 ]]; then
        # AVX2 only (Haswell and newer)
        CFLAGS="-march=haswell"
        CFLAGS="${CFLAGS} -O3"
        CFLAGS="${CFLAGS} -fomit-frame-pointer"
        CFLAGS="${CFLAGS} -ffast-math"
        CFLAGS="${CFLAGS} -funroll-loops"
        [[ $HAS_GFNI -gt 0 ]] && CFLAGS="${CFLAGS} -mgfni"
        
        echo -e "${YELLOW}GCC flags for Intel (AVX2):${NC}"
        echo "  Architecture: Haswell+"
        echo "  SIMD: AVX2 (AVX-512 not available on this CPU)"
    else
        # Generic x86-64
        CFLAGS="-march=x86-64-v2"
        CFLAGS="${CFLAGS} -O3"
        
        echo -e "${YELLOW}GCC flags (Generic x86-64):${NC}"
        echo "  Architecture: x86-64-v2"
        echo "  SIMD: SSE4.2 (AVX not available)"
    fi
    
    CC="gcc"
}

# Set compiler flags for AOCC (AMD Genoa 4/5)
set_aocc_flags_amd() {
    detect_cpu_features
    
    if [[ $HAS_AVX512F -gt 0 ]]; then
        # AMD Genoa (Zen 4) / Genoa-X / Bergamo
        # Supports: AVX-512, GFNI, VPOPCNTDQ, BITALG, VBMI2
        # Also supports: VAES, VPCLMULQDQ
        
        CFLAGS="-march=znver4"
        CFLAGS="${CFLAGS} -O3"
        CFLAGS="${CFLAGS} -fomit-frame-pointer"
        CFLAGS="${CFLAGS} -ffast-math"
        CFLAGS="${CFLAGS} -funroll-loops"
        
        # AMD-specific optimizations
        CFLAGS="${CFLAGS} -mavx512f -mavx512bw -mavx512vl"
        [[ $HAS_AVX512VPOPCNT -gt 0 ]] && CFLAGS="${CFLAGS} -mavx512vpopcntdq"
        [[ $HAS_AVX512BITALG -gt 0 ]] && CFLAGS="${CFLAGS} -mavx512bitalg"
        [[ $HAS_GFNI -gt 0 ]] && CFLAGS="${CFLAGS} -mgfni"
        CFLAGS="${CFLAGS} -mavx512vbmi -mavx512vbmi2"
        CFLAGS="${CFLAGS} -mvaes -mvpclmulqdq"
        
        # AOCC-specific optimizations
        CFLAGS="${CFLAGS} -finline-functions"
        CFLAGS="${CFLAGS} -fvectorize"
        CFLAGS="${CFLAGS} -fslp-vectorize"
        
        # Link-time optimization
        CFLAGS="${CFLAGS} -flto"
        LDFLAGS="-flto -fuse-ld=lld"
        
        echo -e "${GREEN}AOCC flags for AMD Genoa 4/5 (AVX-512):${NC}"
        echo "  Architecture: Zen 4 (znver4)"
        echo "  SIMD: AVX-512 + GFNI + VPOPCNTDQ + BITALG + VAES"
    elif [[ $HAS_AVX2 -gt 0 ]]; then
        # Zen 3 or older
        CFLAGS="-march=znver3"
        CFLAGS="${CFLAGS} -O3"
        CFLAGS="${CFLAGS} -fomit-frame-pointer"
        [[ $HAS_GFNI -gt 0 ]] && CFLAGS="${CFLAGS} -mgfni"
        
        echo -e "${YELLOW}AOCC flags for AMD (AVX2):${NC}"
        echo "  Architecture: Zen 3"
        echo "  SIMD: AVX2 (AVX-512 not available)"
    else
        CFLAGS="-march=x86-64-v2"
        CFLAGS="${CFLAGS} -O3"
        
        echo -e "${YELLOW}AOCC flags (Generic):${NC}"
        echo "  Architecture: x86-64-v2"
        echo "  SIMD: SSE4.2"
    fi
    
    CC="${AOCC_BIN}"
    
    # Set environment for AOCC
    export PATH="${AOCC_PATH}/bin:${PATH}"
    export LD_LIBRARY_PATH="${AOCC_PATH}/lib:${LD_LIBRARY_PATH}"
}

# Set generic flags as fallback
set_generic_flags() {
    CFLAGS="-march=native"
    CFLAGS="${CFLAGS} -O3"
    CFLAGS="${CFLAGS} -fomit-frame-pointer"
    
    CC="${CC:-gcc}"
    
    echo -e "${YELLOW}Using generic -march=native flags${NC}"
}

# Configure and build
build() {
    echo ""
    echo -e "${BLUE}=== Build Configuration ===${NC}"
    
    if [[ "$SELECTED_COMPILER" == "GCC" ]]; then
        set_gcc_flags_intel
    elif [[ "$SELECTED_COMPILER" == "AOCC" ]]; then
        set_aocc_flags_amd
    else
        set_generic_flags
    fi
    
    # Additional flags for Kissat
    CFLAGS="${CFLAGS} -DNDEBUG"
    CFLAGS="${CFLAGS} -DKISSAT_HAS_AVX512=1"
    CFLAGS="${CFLAGS} -DKISSAT_HAS_AVX512_BITOPS=1"
    CFLAGS="${CFLAGS} -DKISSAT_HAS_GFNI=1"
    
    # Create build directory
    if [[ $CLEAN -eq 1 ]]; then
        echo "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
    fi
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    echo ""
    echo -e "${BLUE}=== Configuring ===${NC}"
    echo "CC=${CC}"
    echo "CFLAGS=${CFLAGS}"
    
    if [[ $VERBOSE -eq 1 ]]; then
        echo "LDFLAGS=${LDFLAGS}"
    fi
    
    # Export for configure
    export CC
    export CFLAGS
    export LDFLAGS
    
    # Run configure
    echo "Running configure..."
    
    if [[ $VERBOSE -eq 1 ]]; then
        ../configure
    else
        ../configure 2>&1 | grep -E "(compiler|configure:|gcc|clang)" || true
    fi
    
    # After configure, we need to modify the makefile to use our flags
    echo ""
    echo -e "${BLUE}=== Patching makefile with SIMD flags ===${NC}"
    
    # Get the CFLAGS we want to use
    OUR_CFLAGS="${CFLAGS}"
    
    # The makefile has CC with embedded flags - we need to fix this
    # Replace CC line to just the compiler, no flags
    sed -i 's|^CC=/usr/bin/gcc-12.*|CC=/usr/bin/gcc-12|' makefile
    
    # Ensure CFLAGS is set
    if ! grep -q "^CFLAGS" makefile; then
        sed -i "/^CC=/a\\CFLAGS = ${OUR_CFLAGS}" makefile
    else
        sed -i "s|^CFLAGS.*|CFLAGS = ${OUR_CFLAGS}|" makefile
    fi
    
    # Fix the pattern rule to use CFLAGS
    sed -i 's|$(CC) -c $<|$(CC) $(CFLAGS) -c $<|' makefile
    
    echo "CC=${CC}"
    echo "CFLAGS=${OUR_CFLAGS}"
    
    echo ""
    echo -e "${BLUE}=== Building with ${JOBS} jobs ===${NC}"
    
    if [[ $VERBOSE -eq 1 ]]; then
        make -j"${JOBS}" V=1
    else
        make -j"${JOBS}" 2>&1 | tail -20
    fi
    
    echo ""
    echo -e "${GREEN}=== Build Complete ===${NC}"
    
    # Verify SIMD support in binary
    if command -v objdump &> /dev/null; then
        echo ""
        echo -e "${BLUE}=== Verifying SIMD Instructions ===${NC}"
        
        local has_avx512=$(objdump -d ./kissat 2>/dev/null | grep -E "\b(vpmovmskb|vgather|vscatter|vpbroadcast)\b" | head -5 | wc -l)
        local has_gfni=$(objdump -d ./kissat 2>/dev/null | grep -E "\bvgf2p8\b" | head -5 | wc -l)
        
        if [[ $has_avx512 -gt 0 ]]; then
            echo -e "${GREEN}✓ AVX-512 instructions detected in binary${NC}"
        else
            echo -e "${YELLOW}⚠ AVX-512 instructions not found${NC}"
        fi
        
        if [[ $has_gfni -gt 0 ]]; then
            echo -e "${GREEN}✓ GFNI instructions detected in binary${NC}"
        else
            echo -e "${YELLOW}⚠ GFNI instructions not found (may not be used in this build)${NC}"
        fi
    fi
    
    echo ""
    echo -e "${GREEN}Binary: ${BUILD_DIR}/kissat${NC}"
    ls -lh ./kissat
}

# Main
main() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  Kissat SIMD Build Script${NC}"
    echo -e "${BLUE}========================================${NC}"
    
    select_compiler
    build
    
    echo ""
    echo -e "${GREEN}Build successful!${NC}"
    echo ""
    echo "To test:"
    echo "  cd ${BUILD_DIR}"
    echo "  ./kissat -q ../f1.cnf"
}

main "$@"
