#!/bin/bash
set -e

DTS_NAME="sun55i-a523-trimui-smart-pro-s"
SRC_DTS="dts/${DTS_NAME}.dts"
TMP_DTS="build/${DTS_NAME}.tmp.dts"
OUT_DTB="build/${DTS_NAME}.dtb"

if [ ! -f "$SRC_DTS" ]; then
    echo "❌ Erreur : Put your file ${DTS_NAME}.dts in folder 'dts/'"
    exit 1
fi

echo "🧠 Pre-processing  DTS with GCC"
# Correction here :  we add accesss to 'input' folder and root bindings for GCC
aarch64-linux-gnu-gcc -E -nostdinc -I kernel-source/include -I include -I dts -x assembler-with-cpp -o "$TMP_DTS" "$SRC_DTS"

echo "🔨 Binary compilation of DTB via DTC..."
dtc -I dts -O dtb -p 1024 -o "$OUT_DTB" "$TMP_DTS"

echo "✅ Yatta ! its done : $OUT_DTB"
echo "------------------------------------------------------"
ls -lh "$OUT_DTB"

