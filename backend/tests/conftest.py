"""Test bootstrap: build the native accelerator if it is missing."""
import os
import subprocess
import sys
from pathlib import Path

_NATIVE_DIR = Path(__file__).resolve().parent.parent / "app" / "native"
_SRC = _NATIVE_DIR / "rta_core.c"
_LIB = _NATIVE_DIR / "librtacore.so"


def pytest_sessionstart(session):  # noqa: ARG001
    if _LIB.exists():
        return
    if not _SRC.exists():
        return
    cc = os.environ.get("CC", "cc")
    subprocess.run(
        [cc, "-O3", "-std=c11", "-fPIC", "-shared", str(_SRC), "-o", str(_LIB)],
        check=True,
    )
    print(f"\n[pytest] built {_LIB}", file=sys.stderr)
