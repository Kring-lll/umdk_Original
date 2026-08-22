#!/usr/bin/env python3
"""
URMA Log Batch Filter

Searches for target log files under patha, mirrors the directory structure
from patha into pathb, and writes filtered URMA_PERF / urma_path_info lines
into <mirrored_dir>/urma_perf.log in pathb.

Two grep passes are applied to each source log:
    1. grep "URMA_PERF" -A 18
    2. grep "urma_path_info" -A 3

Usage:
    python filter_urma_logs.py <patha> <pathb> <loga>

Arguments:
    patha  - root search directory
    pathb  - output directory (mirrored structure is created here)
    loga   - target log file name or glob pattern (e.g. "*.log", "urma*.log")
"""

import argparse
import fnmatch
import os
import subprocess
import sys
from collections import OrderedDict


def find_log_files(patha, loga_pattern):
    """Find all files matching loga_pattern under patha.

    Returns list of absolute file paths. The pattern can be a glob.
    """
    # Normalize patha to absolute, strip trailing separator
    patha_abs = os.path.abspath(patha)
    if not os.path.isdir(patha_abs):
        print("Error: patha '%s' is not a directory" % patha_abs, file=sys.stderr)
        sys.exit(1)

    # If the pattern contains path separators, keep only the basename part for
    # the recursive search — we recurse the whole tree and match by name.
    basename = os.path.basename(loga_pattern)

    matches = []
    for root, dirs, files in os.walk(patha_abs):
        # Use glob-style matching on filenames
        for m in fnmatch.filter(files, basename):
            matches.append(os.path.join(root, m))
    return patha_abs, matches


def mirror_dir(patha_abs, pathb_abs, source_file):
    """Create the mirrored directory structure in pathb for a source file.

    Returns the mirrored directory path (where urma_perf.log will be written).
    """
    source_dir = os.path.dirname(source_file)
    rel_dir = os.path.relpath(source_dir, patha_abs)
    dest_dir = os.path.join(pathb_abs, rel_dir)
    os.makedirs(dest_dir, exist_ok=True)
    return dest_dir


def filter_log(source_file, dest_file):
    """Run two grep passes on source_file, append results to dest_file."""
    # grep URMA_PERF -A 18
    cmd1 = ['grep', 'URMA_PERF', '-A', '18', source_file]
    # grep urma_path_info -A 3
    cmd2 = ['grep', 'urma_path_info', '-A', '3', source_file]

    with open(dest_file, 'a', encoding='utf-8', errors='ignore') as out:
        for cmd in (cmd1, cmd2):
            try:
                result = subprocess.run(
                    cmd, stdout=out, stderr=subprocess.PIPE, check=False
                )
            except FileNotFoundError:
                print("Error: 'grep' not found. Please install grep.",
                      file=sys.stderr)
                sys.exit(1)
            # grep returns 1 for "no matches", which is not an error here
            if result.returncode not in (0, 1):
                msg = result.stderr.decode('utf-8', errors='ignore').strip()
                if msg:
                    print("Warning: grep error on %s: %s"
                          % (source_file, msg), file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(
        description='Batch filter URMA perf logs from a directory tree.'
    )
    parser.add_argument('patha', help='Root search directory')
    parser.add_argument('pathb', help='Output directory for mirrored structure')
    parser.add_argument('loga', help='Target log file name or glob pattern')
    args = parser.parse_args()

    patha_abs, matches = find_log_files(args.patha, args.loga)
    pathb_abs = os.path.abspath(args.pathb)

    if not matches:
        print("No files matching '%s' found under '%s'"
              % (args.loga, args.patha), file=sys.stderr)
        sys.exit(1)

    print("Found %d log file(s):" % len(matches))
    for f in matches:
        print("  %s" % f)

    # Group source files by destination directory so that all logs in the
    # same source directory are merged into one urma_perf.log
    groups = OrderedDict()
    for source_file in matches:
        dest_dir = mirror_dir(patha_abs, pathb_abs, source_file)
        groups.setdefault(dest_dir, []).append(source_file)

    for dest_dir, source_files in groups.items():
        dest_file = os.path.join(dest_dir, 'urma_perf.log')

        # Clear once per destination, then append all source files
        with open(dest_file, 'w', encoding='utf-8'):
            pass

        for source_file in sorted(source_files):
            filter_log(source_file, dest_file)
            print("Filtered: %s -> %s" % (source_file, dest_file))

    print("\nDone. Output directory: %s" % pathb_abs)


if __name__ == '__main__':
    main()
