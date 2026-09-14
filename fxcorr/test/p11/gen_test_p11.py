#!/usr/bin/env python3
"""Generate the delay-bearing VEX variant for the P11 reader-semantics tests.

fxcorr/test/test.vex places T1 and T2 ~100 m apart, so the geometric delay
(~0.3 us) never pushes a subint's delay-corrected start before the data
origin -- the upstream count>0 realignment path (datastream.cpp:538-574)
is unreachable.  This script moves TEST2 to the antipode of TEST1: the
~12744 km baseline yields a geometric delay up to ~42.5 ms, so the first
subint of the batch always starts delay-corrected before the data origin
and the skip/realign semantics fire (algo-plan.md P11).  The delay changes
by only ~6 us over the 2.097 s observation, so the tosubtract compensation
rounds to 0 samples -- the block-skip semantics are verifiable on their
own, the compensation term stays on the code-review path.

Outputs (written to outdir):

  test-delay.vex   test.vex with TEST2's site_position negated (antipode)
  test-delay.v2d   test.v2d with the vex= line pointing at test-delay.vex

Usage: gen_test_p11.py <test.vex> <test.v2d> [outdir]
"""

import math
import os
import re
import sys


def main():
    if len(sys.argv) < 3:
        print("usage: gen_test_p11.py <test.vex> <test.v2d> [outdir]")
        sys.exit(1)
    vexpath, v2dpath = sys.argv[1], sys.argv[2]
    outdir = sys.argv[3] if len(sys.argv) > 3 else "."

    with open(vexpath) as f:
        vex = f.read()

    # negate TEST2's site_position (only within the "def TEST2;" block)
    test2 = re.search(r"def TEST2;(.*?)enddef;", vex, re.S)
    if not test2:
        print("no 'def TEST2;' block found in", vexpath)
        sys.exit(1)
    pos = re.search(r"site_position =(-?\d+\.\d+) m: (-?\d+\.\d+) m:(-?\d+\.\d+) m;", test2.group(1))
    if not pos:
        print("no site_position line found in TEST2 block")
        sys.exit(1)
    x, y, z = float(pos.group(1)), float(pos.group(2)), float(pos.group(3))
    antipode = "site_position = %.5f m: %.5f m: %.5f m;" % (-x, -y, -z)
    vex = vex[:test2.start(1)] + test2.group(1).replace(pos.group(0), antipode) + vex[test2.end(1):]

    with open(os.path.join(outdir, "test-delay.vex"), "w") as f:
        f.write(vex)

    with open(v2dpath) as f:
        v2d = f.read()
    v2d = re.sub(r"^vex=.*$", "vex=test-delay.vex", v2d, count=1, flags=re.M)
    with open(os.path.join(outdir, "test-delay.v2d"), "w") as f:
        f.write(v2d)

    # rough expectation for the README check: delay <= baseline/c
    r = math.sqrt(x*x + y*y + z*z)
    print("TEST2 moved to antipode: baseline = %.0f km, max geometric delay ~ %.2f ms"
          % (2*r/1000.0, 2*r/299792458.0*1000.0))
    print("wrote %s/test-delay.vex and %s/test-delay.v2d" % (outdir, outdir))


if __name__ == "__main__":
    main()
