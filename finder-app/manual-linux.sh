#!/bin/bash

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=https://github.com/gregkh/linux.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
ARCH=arm64

if command -v aarch64-none-linux-gnu-gcc >/dev/null 2>&1
then
    CROSS_COMPILE=aarch64-none-linux-gnu-
else
    CROSS_COMPILE=aarch64-linux-gnu-
fi

FINDER_APP_DIR=$(realpath $(dirname $0))

if [ $# -ge 1 ]
then
    OUTDIR=$1
fi

echo "Using output directory ${OUTDIR}"

mkdir -p ${OUTDIR}
OUTDIR=$(realpath ${OUTDIR})

############################################################
# Build Linux Kernel
############################################################

cd ${OUTDIR}

if [ ! -d linux-stable ]
then
    echo "Cloning Linux kernel ${KERNEL_VERSION}"

    git clone \
        --depth 1 \
        --single-branch \
        --branch ${KERNEL_VERSION} \
        ${KERNEL_REPO} \
        linux-stable
fi

cd linux-stable

if [ ! -f arch/${ARCH}/boot/Image ]
then
    echo "Building kernel"

    make ARCH=${ARCH} \
        CROSS_COMPILE=${CROSS_COMPILE} \
        mrproper

    make ARCH=${ARCH} \
        CROSS_COMPILE=${CROSS_COMPILE} \
        defconfig

    make -j$(nproc) \
        ARCH=${ARCH} \
        CROSS_COMPILE=${CROSS_COMPILE} \
        all
fi

cp arch/${ARCH}/boot/Image ${OUTDIR}

############################################################
# Rootfs
############################################################

cd ${OUTDIR}

sudo rm -rf rootfs

mkdir -p rootfs

cd rootfs

mkdir -p \
bin \
dev \
etc \
home \
lib \
proc \
sbin \
sys \
tmp \
usr \
var \
usr/bin \
usr/lib \
usr/sbin \
var/log

############################################################
# BusyBox
############################################################

cd ${OUTDIR}

if [ ! -d busybox ]
then
    git clone https://git.busybox.net/busybox
fi

cd busybox

git checkout ${BUSYBOX_VERSION}

make distclean

make \
    ARCH=${ARCH} \
    CROSS_COMPILE=${CROSS_COMPILE} \
    defconfig

make -j$(nproc) \
    ARCH=${ARCH} \
    CROSS_COMPILE=${CROSS_COMPILE}

make \
    ARCH=${ARCH} \
    CROSS_COMPILE=${CROSS_COMPILE} \
    CONFIG_PREFIX=${OUTDIR}/rootfs \
    install

############################################################
# Shared Libraries
############################################################

echo "Copying shared libraries"

mkdir -p ${OUTDIR}/rootfs/lib

LIBS="\
ld-linux-aarch64.so.1 \
libc.so.6 \
libm.so.6 \
libresolv.so.2"

for lib in ${LIBS}
do
    LIBPATH=$(${CROSS_COMPILE}gcc \
        -print-file-name=${lib})

    echo "${lib} -> ${LIBPATH}"

    if [ ! -f "${LIBPATH}" ]
    then
        echo "ERROR locating ${lib}"
        exit 1
    fi

    cp -L \
        "${LIBPATH}" \
        ${OUTDIR}/rootfs/lib/
done

rm -rf ${OUTDIR}/rootfs/lib64
ln -sf lib ${OUTDIR}/rootfs/lib64

echo "Libraries copied:"
ls -l ${OUTDIR}/rootfs/lib

############################################################
# Device Nodes
############################################################

sudo mknod -m 666 \
    ${OUTDIR}/rootfs/dev/null \
    c 1 3 || true

sudo mknod -m 600 \
    ${OUTDIR}/rootfs/dev/console \
    c 5 1 || true

############################################################
# Build Writer
############################################################

cd ${FINDER_APP_DIR}

make clean

make CROSS_COMPILE=${CROSS_COMPILE}

############################################################
# Copy Assignment Files
############################################################

cp writer ${OUTDIR}/rootfs/home/
cp finder.sh ${OUTDIR}/rootfs/home/
cp finder-test.sh ${OUTDIR}/rootfs/home/
cp autorun-qemu.sh ${OUTDIR}/rootfs/home/

cp -aL conf \
    ${OUTDIR}/rootfs/home/

sed -i \
's#../conf/assignment.txt#conf/assignment.txt#g' \
${OUTDIR}/rootfs/home/finder-test.sh

############################################################
# Init
############################################################

ln -sf sbin/init \
    ${OUTDIR}/rootfs/init

############################################################
# Ownership
############################################################

sudo chown -R root:root \
    ${OUTDIR}/rootfs

############################################################
# Initramfs
############################################################

cd ${OUTDIR}/rootfs

find . | \
cpio -H newc -ov \
--owner root:root \
> ${OUTDIR}/initramfs.cpio

gzip -f ${OUTDIR}/initramfs.cpio

echo "Kernel image:"
ls -l ${OUTDIR}/Image

echo "Initramfs:"
ls -l ${OUTDIR}/initramfs.cpio.gz

echo "manual-linux.sh completed successfully"
