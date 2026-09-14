#!/usr/bin/env python3
"""Generate .input variants for the P10 input-format tests.

Takes the base test.input (difxcalc output of fxcorr/test/test.v2d, VDIF
single-thread) and produces per-format variants with the DATASTREAM table
(DATA FORMAT / DATA FRAME SIZE) and the DATA TABLE file names rewritten:

  test-mk5b.input   DATA FORMAT Mark5B,  DATA FRAME SIZE 10016,
                    files TEST1.m5b / TEST2-usb.m5b
  test-lba.input    DATA FORMAT LBASTD, DATA FRAME SIZE 320004096
                    (the sentinel vex2difx writes for LBA; see tests/ATNF
                    v521c), files TEST1.lba / TEST2-usb.lba
  test-ivdif.input  DATA FORMAT INTERLACEDVDIF:0:1 (2 threads, configuration
                    parses the ":thread0:thread1" suffix, configuration.cpp
                    1416-1423), DATA FRAME SIZE stays 8032 (single-thread
                    frame; multiplexed framebytes is derived),
                    files TEST1.ivdif / TEST2-usb.ivdif

The EXECUTE TIME truncation for mpifxcorr baselines is a separate concern:
test-mpi2.input is cut to a whole integration (see README.md), so this
script only rewrites the format-specific fields of the full test.input.

Usage: gen_test_p10.py <test.input> [outdir]
"""

import re
import sys


VARIANTS = {
    "mk5b": {
        "format": "MARK5B",      # .input is case-sensitive: all caps (configuration.cpp:1479)
        "framesize": "10016",
        "files": (("TEST1.vdif", "TEST1.m5b"), ("TEST2-usb.vdif", "TEST2-usb.m5b")),
    },
    "lba": {
        "format": "LBASTD",
        "framesize": "320004096",
        "files": (("TEST1.vdif", "TEST1.lba"), ("TEST2-usb.vdif", "TEST2-usb.lba")),
    },
    "ivdif": {
        "format": "INTERLACEDVDIF:0:1",
        "framesize": None,     # keep 8032 (single-thread frame)
        "files": (("TEST1.vdif", "TEST1.ivdif"), ("TEST2-usb.vdif", "TEST2-usb.ivdif")),
    },
}


def main():
    if len(sys.argv) < 2:
        print("usage: gen_test_p10.py <test.input> [outdir]")
        sys.exit(1)
    src = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else "."

    with open(src) as f:
        text = f.read()

    for name, var in VARIANTS.items():
        out = text
        # DATA FORMAT: <format>  (once per datastream; rewrite all entries)
        out = re.sub(r"DATA FORMAT:\s*\S+", "DATA FORMAT:        " + var["format"], out)
        if var["framesize"] is not None:
            out = re.sub(r"DATA FRAME SIZE:\s*\d+", "DATA FRAME SIZE:    " + var["framesize"], out)
        # DATA TABLE file names
        for old, new in var["files"]:
            out = re.sub(r"(\s)" + re.escape(old) + r"(\s|$)", r"\1" + new + r"\2", out)

        with open(f"{outdir}/test-{name}.input", "w") as f:
            f.write(out)
        print(f"wrote {outdir}/test-{name}.input")

    # INTERLACEDVDIF needs the muxed DATA FRAME SIZE note: single-thread 8032
    # stays; verify the format line got the thread suffix
    with open(f"{outdir}/test-ivdif.input") as f:
        assert "INTERLACEDVDIF:0:1" in f.read(), "INTERLACEDVDIF thread suffix missing"


if __name__ == "__main__":
    main()
