#!/usr/bin/env bash

set -ex

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
ROOT_DIR=$SCRIPT_DIR/..
OPUS_DIR=$ROOT_DIR/third_party/opus-1.4

export PATH=/opt/devkitpro/devkitARM/bin:$PATH

if [ ! -x "$OPUS_DIR/configure" ]; then
    echo "Extract opus-1.4.tar.gz (https://downloads.xiph.org/releases/opus/) to third_party/opus-1.4" >&2
    exit 1
fi

cd $OPUS_DIR

# fixed-point Opus with ARMv6 asm (the float portlib took ~32% of core 0 on Old 2DS).
# installed in extra_lib, the Makefile looks there first
./configure --host=arm-none-eabi --prefix=/opt/devkitpro/extra_lib \
            --enable-fixed-point --disable-rtcd --disable-intrinsics \
            --disable-shared --enable-static \
            --disable-extra-programs --disable-doc \
            CFLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -mtp=soft -O2 -ffunction-sections -fdata-sections"

make -j$(nproc)
make install

cd -
