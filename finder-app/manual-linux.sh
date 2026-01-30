#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-

# 1. Install toolchain if missing (required for GitHub Actions)
if ! command -v ${CROSS_COMPILE}gcc &> /dev/null
then
    echo "${CROSS_COMPILE}gcc could not be found, attempting to install..."
    sudo apt-get update && sudo apt-get install -y gcc-aarch64-linux-gnu
fi

if [ $# -lt 1 ]
then
    echo "Using default directory ${OUTDIR} for output"
else
    OUTDIR=$1
    echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p "${OUTDIR}"

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
    git clone --depth 1 --single-branch --branch ${KERNEL_VERSION} ${KERNEL_REPO} linux-stable
fi

if [ ! -e "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # Kernel build steps
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
    cp arch/${ARCH}/boot/Image "${OUTDIR}/"
fi

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
    echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm -rf "${OUTDIR}/rootfs"
fi

# Create base directories
mkdir -p "${OUTDIR}/rootfs"
cd "${OUTDIR}/rootfs"
mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir -p usr/bin usr/lib usr/sbin
mkdir -p var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
    git clone https://github.com/mirror/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    make distclean
    make defconfig
else
    cd busybox
fi

# Make and install busybox
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX="${OUTDIR}/rootfs" install

# Check library dependencies
cd "${OUTDIR}/rootfs"
echo "Library dependencies"
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
if [ "$SYSROOT" = "/" ] || [ -z "$SYSROOT" ]; then
    SYSROOT="/usr/aarch64-linux-gnu"
fi

echo "Using SYSROOT: ${SYSROOT}"

# Ensure destination directories exist
mkdir -p "${OUTDIR}/rootfs/lib"
mkdir -p "${OUTDIR}/rootfs/lib64"

# 1. Find and copy the loader and libraries to /lib
find "${SYSROOT}" -name "ld-linux-aarch64.so.1" -exec cp -L {} "${OUTDIR}/rootfs/lib/" \;
find "${SYSROOT}" -name "libm.so.6" -exec cp -L {} "${OUTDIR}/rootfs/lib/" \;
find "${SYSROOT}" -name "libresolv.so.2" -exec cp -L {} "${OUTDIR}/rootfs/lib/" \;
find "${SYSROOT}" -name "libc.so.6" -exec cp -L {} "${OUTDIR}/rootfs/lib/" \;

# 2. Duplicate them into /lib64 to satisfy all possible search paths
cp -L "${OUTDIR}/rootfs/lib/"* "${OUTDIR}/rootfs/lib64/"

# 3. Create the symlink for init at the root (prevents Kernel Panic)
ln -sf bin/busybox "${OUTDIR}/rootfs/init"

# TODO: Make device nodes
sudo rm -f "${OUTDIR}/rootfs/dev/null"
sudo rm -f "${OUTDIR}/rootfs/dev/console"
sudo mknod -m 666 "${OUTDIR}/rootfs/dev/null" c 1 3
sudo mknod -m 600 "${OUTDIR}/rootfs/dev/console" c 5 1

# TODO: Clean and build the writer utility
cd "${FINDER_APP_DIR}"
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# TODO: Copy the finder related scripts and executables to the /home directory
cp "${FINDER_APP_DIR}/writer" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/finder.sh" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/finder-test.sh" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/autorun-qemu.sh" "${OUTDIR}/rootfs/home/"

mkdir -p "${OUTDIR}/rootfs/home/conf"
cp "${FINDER_APP_DIR}/conf/assignment.txt" "${OUTDIR}/rootfs/home/conf/"
cp "${FINDER_APP_DIR}/conf/username.txt" "${OUTDIR}/rootfs/home/conf/"

# Fix the path in finder-test.sh
sed -i 's|\.\./conf/assignment.txt|conf/assignment.txt|g' "${OUTDIR}/rootfs/home/finder-test.sh"

# TODO: Chown the root directory
cd "${OUTDIR}/rootfs"
sudo chown -R root:root *

# TODO: Create initramfs.cpio.gz
cd "${OUTDIR}/rootfs"
sudo find . | cpio -H newc -ov --owner root:root > "${OUTDIR}/initramfs.cpio"

cd "${OUTDIR}"
gzip -f initramfs.cpio
