#!/usr/bin/env python3
"""Generate the pulsar binning (P4c) test assets from a plain .input file.

Outputs (all into the .input's directory):
  test-pulsar.input        PULSAR BINNING TRUE + PULSAR CONFIG FILE injected
  test-pulsar-mpi2.input   EXECUTE TIME (SEC) = 2 variant for the mpifxcorr benchmark
  pulsar.cfg               pulsar config: 1 polyco file, 4 bins, weights
  test.polyco              tempo-style polyco covering the scan epoch

Usage: gen_test_pulsar.py <input_file> [--scrunch] [--negative-weight]

The polyco tmid is hardcoded to the test.vex scan start (2020y100d07h00m00s =
MJD 58948.2916667, epoch of all the standard test assets).  f0 = 2.0 Hz makes
the pulse phase advance ~4.2 turns over the 2.1 s dataset, so all 4 bins are
covered.  --negative-weight gives bin 1 a negative weight (scrunch RFI-stripping
semantics: it folds into the data but not into the baseline weight).
"""

import os
import sys

# MJD of 2020-04-09 07:00:00 UTC (test.vex scan start), fractional form used by
# the tempo polyco format's MJD field
TMID_MJD = 58948.2916667
POLYCO_F0 = 2.0          # Hz; phase advance rate used by getBins
NBINS = 4


def kv(key, val):
    """Format a key:val line for DiFX's key:val parser (getinputkeyval):
    the value starts at column 20 (0-based), i.e. after the key + ':' padded
    with spaces to 20 chars.  Shorter padding silently corrupts the value
    (leading spaces survive)."""
    prefix = key + ':'
    pad = max(1, 20 - len(prefix))
    return prefix + ' ' * pad + val


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    inputfile = args[0]
    scrunch = '--scrunch' in args
    negative = '--negative-weight' in args

    d = os.path.dirname(os.path.abspath(inputfile))
    tag = 'pulsar-scrunch' if scrunch else 'pulsar'
    outinput = os.path.join(d, 'test-%s.input' % tag)
    cfg = os.path.join(d, 'pulsar%s.cfg' % ('-scrunch' if scrunch else ''))
    polyco = os.path.join(d, 'test.polyco')

    # --- .input variant: PULSAR BINNING TRUE + PULSAR CONFIG FILE ---
    # .input is a key:val stream; the CONFIG section reads
    # WRITE AUTOCORRS -> PULSAR BINNING -> [PULSAR CONFIG FILE] -> PHASED ARRAY
    # (configuration.cpp:1290-1299), so the new line goes right after
    # PULSAR BINNING.
    with open(inputfile) as f:
        lines = f.read().splitlines()
    out = []
    for line in lines:
        if line.startswith('PULSAR BINNING'):
            out.append(kv('PULSAR BINNING', 'TRUE'))
            # relative to the workdir cwd, same semantics as DATA TABLE links
            out.append(kv('PULSAR CONFIG FILE', 'config/%s' % os.path.basename(cfg)))
        else:
            out.append(line)
    with open(outinput, 'w') as f:
        f.write('\n'.join(out) + '\n')

    # EXECUTE TIME truncation for the mpifxcorr benchmark (same as zoom/mpc)
    mpi2 = outinput.replace('.input', '-mpi2.input')
    with open(outinput) as f:
        lines = f.read().splitlines()
    with open(mpi2, 'w') as f:
        for line in lines:
            if line.startswith('EXECUTE TIME'):
                f.write(kv('EXECUTE TIME (SEC)', '2') + '\n')
            else:
                f.write(line + '\n')

    # --- pulsar config (configuration.cpp processPulsarConfig) ---
    if scrunch:
        weights = ['1.0', '-1.0', '1.0', '1.0'] if negative else ['1.0'] * NBINS
        scrunchline = 'TRUE'
    else:
        weights = ['1.0'] * NBINS
        scrunchline = 'FALSE'
    with open(cfg, 'w') as f:
        f.write(kv('NUM POLYCO FILES', '1') + '\n')
        # relative to the workdir cwd, like the .input's PULSAR CONFIG FILE
        f.write(kv('POLYCO FILE', 'config/%s' % os.path.basename(polyco)) + '\n')
        f.write(kv('NUM PULSAR BINS', str(NBINS)) + '\n')
        f.write(kv('SCRUNCH OUTPUT', scrunchline) + '\n')
        for i in range(NBINS):
            f.write(kv('BIN PHASE END', '%.3f' % ((i + 1) / float(NBINS))) + '\n')
            f.write(kv('BIN WEIGHT', weights[i]) + '\n')

    # --- tempo-style polyco (polyco.cpp loadPolycoFile) ---
    # line 1: psrname date(yymmdd) hhmmss mjd(fractional) dm doppler logresid
    # line 2: refphase f0 observatory timespan ncoeff obsfrequency [binaryphase]
    # then ncoeff/3 lines of 3 coefficients (phase polynomial in minutes).
    # Coefficients all zero: phase = refphase + 60*f0*t, i.e. a constant
    # POLYCO_F0 Hz pulse train with a nominal period of 1/POLYCO_F0 s.
    #
    # NOTE: processPulsarConfig counts the polyco blocks by reading ncoeff
    # from fixed column 50 (psrline[49], configuration.cpp:3360), NOT by
    # tokenising like loadPolycoFile - a short line reads ncoeff=0 and
    # makes it think the file has one extra (empty) polyco block, which
    # then fails to load.  Pad the second line so ncoeff lands there.
    ncoeff = 6
    line2 = '0.0  %.6f  0  120' % POLYCO_F0
    line2 += ' ' * (49 - len(line2)) + '%5d' % ncoeff + '  2400.0'
    with open(polyco, 'w') as f:
        f.write('TESTPSR  200409 070000  %.8f  0.0  0.0  0.0\n' % TMID_MJD)
        f.write(line2 + '\n')
        for i in range(ncoeff // 3):
            f.write('0.0  0.0  0.0\n')

    print('wrote', outinput)
    print('wrote', mpi2)
    print('wrote', cfg)
    print('wrote', polyco)
    return 0


if __name__ == '__main__':
    sys.exit(main())
