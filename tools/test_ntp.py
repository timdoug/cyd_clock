#!/usr/bin/env python3
"""Run production NTP logic with a deterministic host clock and sanitizers."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="cyd-ntp-test-") as work:
        work = Path(work)
        # Preserve the production constants, structs and inline functions.
        # Only platform includes are replaced by tests/ntp_host.h's adapter.
        header = (ROOT / "main/ntp_internal.h").read_text()
        portable = {"ntp.h", "ntp_nts.h", "util.h"}
        header = "\n".join(line for line in header.splitlines()
                           if not (line.startswith('#include "') and
                                   line.split('"')[1] not in portable))
        (work / "ntp_internal_host.h").write_text(header + "\n")
        binary = work / "ntp_test"
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O2", "-g",
                        "-fsanitize=undefined,address", "-fno-sanitize-recover=all",
                        "-I", str(work), "-I", str(ROOT / "main"),
                        str(ROOT / "tests/ntp_test.c"), "-lm", "-o", str(binary)],
                       check=True)
        cases = sys.argv[1:] or ["persist-zero", "persist-positive", "persist-negative"]
        for case in cases:
            subprocess.run([str(binary), case], check=True)


if __name__ == "__main__":
    main()
