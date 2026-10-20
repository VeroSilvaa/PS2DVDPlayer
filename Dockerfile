FROM ghcr.io/ps2dev/ps2dev:latest

RUN apk add --no-cache \
    build-base \
    git \
    bash \
    zip \
    gawk \
    python3 \
    cmake \
    pkgconf

ENV PS2DEV=/usr/local/ps2dev
ENV PS2SDK=/usr/local/ps2dev/ps2sdk
ENV GSKIT=/usr/local/ps2dev/gsKit

ENV PATH=/usr/local/ps2dev/bin:/usr/local/ps2dev/ee/bin:/usr/local/ps2dev/iop/bin:/usr/local/ps2dev/dvp/bin:/usr/local/ps2dev/ps2sdk/bin:$PATH

RUN ln -sf /usr/bin/pkgconf \
    /usr/local/ps2dev/ps2sdk/bin/mips64r5900el-ps2-elf-pkg-config

# ============================================================
# gsKit
# ============================================================

RUN git clone --depth 1 \
    https://github.com/ps2dev/gsKit.git \
    /tmp/gsKit && \
    cd /tmp/gsKit && \
    make && \
    make install && \
    rm -rf /tmp/gsKit

# ============================================================
# PS2 HDD STACK
#
# ps2atad.irx = ATA driver
# ps2hdd.irx  = APA/HDD driver
# ps2fs.irx   = PFS filesystem
# ============================================================

RUN git clone --depth 1 \
    https://github.com/ps2dev/ps2sdk.git \
    /tmp/ps2sdk-src && \
    \
    export PS2SDKSRC=/tmp/ps2sdk-src && \
    \
    echo "======================================" && \
    echo " BUILDING PS2ATAD" && \
    echo "======================================" && \
    cd /tmp/ps2sdk-src/iop/dev9/atad && \
    make clean && \
    make && \
    \
    echo "======================================" && \
    echo " BUILDING PS2HDD / APA" && \
    echo "======================================" && \
    cd /tmp/ps2sdk-src/iop/hdd/apa && \
    make clean && \
    make && \
    \
    echo "======================================" && \
    echo " BUILDING PS2FS / PFS" && \
    echo "======================================" && \
    cd /tmp/ps2sdk-src/iop/hdd/pfs && \
    make clean && \
    make && \
    \
    echo "======================================" && \
    echo " INSTALLING HDD IRX MODULES" && \
    echo "======================================" && \
    \
    cp /tmp/ps2sdk-src/iop/dev9/atad/irx/ps2atad.irx \
       ${PS2SDK}/iop/irx/ps2atad.irx && \
    \
    cp /tmp/ps2sdk-src/iop/hdd/apa/irx/ps2hdd.irx \
       ${PS2SDK}/iop/irx/ps2hdd.irx && \
    \
    cp /tmp/ps2sdk-src/iop/hdd/pfs/irx/ps2fs.irx \
       ${PS2SDK}/iop/irx/ps2fs.irx && \
    \
    echo "======================================" && \
    echo " INSTALLED HDD MODULES" && \
    echo "======================================" && \
    \
    ls -lh \
       ${PS2SDK}/iop/irx/ps2atad.irx \
       ${PS2SDK}/iop/irx/ps2hdd.irx \
       ${PS2SDK}/iop/irx/ps2fs.irx && \
    \
    rm -rf /tmp/ps2sdk-src

WORKDIR /workspace