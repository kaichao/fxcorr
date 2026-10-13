# env.sh -- source this before running fxcorr tools on p419 nodes
# (companion of build.sh; keeps the runtime side of the same eight pits)
# 用法：拷到源码树根（$HOME/fxcorr），source ~/fxcorr/env.sh。
# 本文件为 2026-10-13 p419（cstu0030 节点）重建版，自现场入仓。

export DIFXROOT=$HOME/fxcorr/install

# pit 1: python3 must be miniforge's (RHEL7 /usr/bin/python3 is 2.7.5)
export PATH=/public/software/apps/miniforge3-25.3.1-0/bin:$PATH
export PATH=$DIFXROOT/bin:$PATH

# pit 7/8: fftw single-precision dir (no SONAME) + GSL under mathlib
export LD_LIBRARY_PATH=/public/software/mathlib/fftw/lib64:/public/software/mathlib/gsl/2.7/lib:$DIFXROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
