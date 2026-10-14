#!/usr/bin/env bash
set -euo pipefail
KERNEL_SOURCE=$(cd "$(dirname "$0")/../.." && pwd)
WORKSPACE=${X620_WORKSPACE:-$(dirname "$KERNEL_SOURCE")}
KERNEL_OUTPUT="$WORKSPACE/out/kernel"
mkdir -p "$KERNEL_OUTPUT" "$WORKSPACE/out" "$WORKSPACE/dist"
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
export KBUILD_BUILD_USER=builder KBUILD_BUILD_HOST=x620
export KBUILD_BUILD_TIMESTAMP='Tue Oct 13 00:00:00 UTC 2026'
export KBUILD_BUILD_VERSION=1
if [[ ! -f "$KERNEL_OUTPUT/.config" ]]; then
    cp "$KERNEL_SOURCE/arch/arm64/configs/x620_relay.config" "$KERNEL_OUTPUT/.config"
    make -C "$KERNEL_SOURCE" O="$KERNEL_OUTPUT" olddefconfig
else
    bash "$KERNEL_SOURCE/scripts/kconfig/merge_config.sh" -m -O "$KERNEL_OUTPUT" \
        "$KERNEL_OUTPUT/.config" "$KERNEL_SOURCE/arch/arm64/configs/x620_relay.config"
    make -C "$KERNEL_SOURCE" O="$KERNEL_OUTPUT" olddefconfig
fi
make -C "$KERNEL_SOURCE" O="$KERNEL_OUTPUT" -j"${BUILD_JOBS:-10}" Image modules
aarch64-linux-gnu-gcc -E -P -x assembler-with-cpp -nostdinc -undef -D__DTS__ \
    -I "$KERNEL_SOURCE/arch/arm64/boot/dts/mediatek" \
    -I "$KERNEL_SOURCE/scripts/dtc/include-prefixes" \
    "$KERNEL_SOURCE/arch/arm64/boot/dts/mediatek/mt6797-leeco-x620.dts" > "$WORKSPACE/out/x620.preprocessed.dts"
dtc -I dts -O dtb -p 65536 -o "$WORKSPACE/dist/x620.dtb" "$WORKSPACE/out/x620.preprocessed.dts"
cp "$KERNEL_OUTPUT/arch/arm64/boot/Image" "$WORKSPACE/dist/Image"
gzip -n -9 -c "$WORKSPACE/dist/Image" > "$WORKSPACE/dist/Image.gz"
cp "$KERNEL_OUTPUT/.config" "$WORKSPACE/dist/kernel.config"
echo "Kernel and DTB ready in $WORKSPACE/dist"
