#!/usr/bin/env bash
# build.sh -- p419 (V9) one-shot build: fxcorr core libs + f/fp/x/xp/sim
#
# Environment fixes are the eight pits recorded in fxcorr/build.md
# "目标集群构建" (2026-09-29), plus pits 9/10 of the V9 round (same file,
# 追加节) and the V9 tools fxcorr-fp/fxcorr-xp.
#
# 部署：把仓库内容 rsync 到目标机（约定 $HOME/fxcorr）后，本文件与 env.sh
# 一起拷到源码树根，然后：
#     cd ~/fxcorr && bash build.sh      # 产物落 $HOME/fxcorr/install/bin
# 本文件为 2026-10-13 p419（cstu0030 节点）重建版，自现场入仓。
set -eu

FX=$HOME/fxcorr
export DIFXROOT=$FX/install
PATCH=$FX/fftw-patch
FFTW_LIB=/public/software/mathlib/fftw/lib64
FFTW_INC=/public/software/mathlib/fftw/include
GSL=/public/software/mathlib/gsl/2.7

# pit 1: /usr/bin/python3 is 2.7.5 (RHEL7); install-difx is a python3 script
export PATH=/public/software/apps/miniforge3-25.3.1-0/bin:$PATH

# pit 2/4/6: FFTW here is only single-precision under /public/software/mathlib
# (pc named fftwf.pc), double precision exists only as a runtime .so.3 in
# /usr/lib64.  Build a patch dir: .so symlinks + standard-named .pc files.
mkdir -p "$PATCH/lib/pkgconfig"
ln -sf /usr/lib64/libfftw3.so.3      "$PATCH/lib/libfftw3.so"
ln -sf "$FFTW_LIB/libfftw3f.so"      "$PATCH/lib/libfftw3f.so"

cat > "$PATCH/lib/pkgconfig/fftw3.pc" <<PC
prefix=/public/software/mathlib/fftw
exec_prefix=\${prefix}
libdir=$PATCH/lib
includedir=$FFTW_INC
Name: fftw3
Description: double precision FFTW3 (runtime from /usr/lib64)
Version: 3.3.2
Libs: -L\${libdir} -lfftw3 -lm
Cflags: -I\${includedir}
PC

cat > "$PATCH/lib/pkgconfig/fftw3f.pc" <<PC
prefix=/public/software/mathlib/fftw
exec_prefix=\${prefix}
libdir=$PATCH/lib
includedir=$FFTW_INC
Name: fftw3f
Description: single precision FFTW3
Version: 3.3.7
Libs: -L\${libdir} -lfftw3f -lm
Cflags: -I\${includedir}
PC

# pit 5: some sources compile with only -I$DIFXROOT/include in the path
mkdir -p "$DIFXROOT/include"
ln -sf "$FFTW_INC/fftw3.h" "$DIFXROOT/include/fftw3.h"

# pit 5b (V9 round): libraries/vdifio/utils/Makefile.am:50 writes
#   vdifPhase_CFLAGS = $(FFTW_CFLAGS) $(INCLUDES)
# but configure defines FFTW3_CFLAGS (with the 3) -- upstream typo, so the
# -I list is empty and fftw3.h is not found.  In-tree build with -I. means
# libraries/vdifio/utils itself: drop the symlink there, no source change.
ln -sf "$FFTW_INC/fftw3.h" "$FX/libraries/vdifio/utils/fftw3.h"

# pit 8: GSL lives under mathlib too.
# $DIFXROOT/lib/pkgconfig is where install-difx drops each component's pc as
# it installs (it only prepends <cwd>/pkgconfig itself), so every downstream
# PKG_CHECK_MODULES(<earlier component>) needs it on the path.
export PKG_CONFIG_PATH="$PATCH/lib/pkgconfig:$GSL/lib/pkgconfig:$DIFXROOT/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# pit 7: the single-precision lib has no SONAME, so DT_NEEDED records the full
# file name and the linker path must contain its directory at run time too.
export LD_LIBRARY_PATH="$FFTW_LIB:$GSL/lib:$DIFXROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# build.md: setup.bash defaults DIFXMPIDIR to /usr which does not exist here
export DIFXMPIDIR=/opt/hpc/software/mpi/hpcx/v2.7.4/gcc-7.3.1

cd "$FX"
# pit 3: --noipp (no /opt/intel on this cluster)
python3 install-difx --noipp \
  --doonly=difxio,codifio,difxmessage,mark5access,vdifio,fxcorrcommon,fxcorr-f,fxcorr-fp,fxcorr-x,fxcorr-xp,fxcorr-sim

echo "== build.sh done -> $DIFXROOT/bin"
ls "$DIFXROOT/bin" | grep fxcorr || true
