#!/bin/bash
# Compile all BPF probes to .bpf.o object files
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BPF_DIR="$PROJECT_ROOT/src/ebpf"
PROBES_DIR="$BPF_DIR/probes"
OUTPUT_DIR="$PROJECT_ROOT/build/bpf"

CLANG=${CLANG:-clang}
BPFTOOL=${BPFTOOL:-bpftool}

INCLUDES="-I$BPF_DIR/include -I/usr/include"

mkdir -p "$OUTPUT_DIR"

echo "=== Compiling BPF probes ==="
echo "  Clang: $($CLANG --version | head -1)"
echo "  Output: $OUTPUT_DIR"

for src in "$PROBES_DIR"/*.bpf.c; do
    name=$(basename "$src" .bpf.c)
    obj="$OUTPUT_DIR/${name}.bpf.o"

    echo "  Compiling: $name"
    $CLANG -g -O2 -target bpf \
        -D__TARGET_ARCH_x86 \
        $INCLUDES \
        -c "$src" -o "$obj"

    echo "    -> $obj ($(stat -c%s "$obj") bytes)"
done

echo ""
echo "=== BPF compilation complete ==="
ls -la "$OUTPUT_DIR"/*.bpf.o
