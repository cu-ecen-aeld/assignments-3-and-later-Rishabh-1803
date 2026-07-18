#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=https://github.com/torvalds/linux.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-

if [ $# -lt 1 ]
then
    echo "Using default directory ${OUTDIR} for output"
else
    OUTDIR=$1
    echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

#################################################
# Build Linux Kernel
#################################################

cd ${OUTDIR}

if [ ! -d "${OUTDIR}/linux-stable" ]
then
    echo "Cloning Linux kernel ${KERNEL_VERSION}"

    git clone ${KERNEL_REPO} \
        --depth 1 \
        --single-branch \
        --branch ${KERNEL_VERSION} \
        linux-stable
fi

if [ ! -f "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" ]
then
    cd linux-stable

    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

    make -j$(nproc) \
        ARCH=${ARCH} \
        CROSS_COMPILE=${CROSS_COMPILE}
fi

cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/Image

#################################################
# Create Root Filesystem
#################################################

echo "Creating root filesystem"

sudo rm -rf ${OUTDIR}/rootfs

mkdir -p ${OUTDIR}/rootfs

cd ${OUTDIR}/rootfs

mkdir -p \
bin \
dev \
etc \
home \
lib \
lib64 \
proc \
sbin \
sys \
tmp \
usr/bin \
usr/lib \
usr/sbin \
var/log

#################################################
# Build Busybox
#################################################

cd ${OUTDIR}

if [ ! -d "${OUTDIR}/busybox" ]
then
    git clone https://git.busybox.net/busybox
fi

cd busybox

git checkout ${BUSYBOX_VERSION}

make distclean || true

make ARCH=${ARCH} \
     CROSS_COMPILE=${CROSS_COMPILE} \
     defconfig

make -j$(nproc) \
     ARCH=${ARCH} \
     CROSS_COMPILE=${CROSS_COMPILE}

make CONFIG_PREFIX=${OUTDIR}/rootfs \
     ARCH=${ARCH} \
     CROSS_COMPILE=${CROSS_COMPILE} \
     install

#################################################
# Shared Libraries
#################################################

mkdir -p ${OUTDIR}/rootfs/lib

cp -L /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 \
      ${OUTDIR}/rootfs/lib/

cp -L /usr/aarch64-linux-gnu/lib/libc.so.* \
      ${OUTDIR}/rootfs/lib/

cp -L /usr/aarch64-linux-gnu/lib/libm.so.* \
      ${OUTDIR}/rootfs/lib/

cp -L /usr/aarch64-linux-gnu/lib/libresolv.so.* \
      ${OUTDIR}/rootfs/lib/

#################################################
# Device Nodes
#################################################

sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3 || true
sudo mknod -m 600 ${OUTDIR}/rootfs/dev/console c 5 1 || true

#################################################
# Build Writer Application
#################################################

cd ${FINDER_APP_DIR}

make clean
make CROSS_COMPILE=${CROSS_COMPILE}

#################################################
# Copy Assignment Files
#################################################

mkdir -p ${OUTDIR}/rootfs/home/conf

cp ${FINDER_APP_DIR}/finder.sh \
   ${OUTDIR}/rootfs/home/

cp ${FINDER_APP_DIR}/finder-test.sh \
   ${OUTDIR}/rootfs/home/

cp ${FINDER_APP_DIR}/writer \
   ${OUTDIR}/rootfs/home/

cp ${FINDER_APP_DIR}/autorun-qemu.sh \
   ${OUTDIR}/rootfs/home/

cp ${FINDER_APP_DIR}/conf/assignment.txt \
   ${OUTDIR}/rootfs/home/conf/

cp ${FINDER_APP_DIR}/conf/username.txt \
   ${OUTDIR}/rootfs/home/conf/

chmod +x ${OUTDIR}/rootfs/home/finder.sh
chmod +x ${OUTDIR}/rootfs/home/finder-test.sh
chmod +x ${OUTDIR}/rootfs/home/writer
chmod +x ${OUTDIR}/rootfs/home/autorun-qemu.sh

#################################################
# Create init symlink
#################################################

ln -sf sbin/init ${OUTDIR}/rootfs/init

#################################################
# Set ownership
#################################################

sudo chown -R root:root ${OUTDIR}/rootfs

#################################################
# Create initramfs
#################################################

cd ${OUTDIR}/rootfs

find . | cpio -H newc -ov --owner root:root \
    > ${OUTDIR}/initramfs.cpio

cd ${OUTDIR}

gzip -f initramfs.cpio

echo ""
echo "Build complete"
echo "Kernel Image: ${OUTDIR}/Image"
echo "Initramfs: ${OUTDIR}/initramfs.cpio.gz"
