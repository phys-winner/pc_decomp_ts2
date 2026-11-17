"""Turn the host's "[bt] ..." log lines (native offsets) into guest functions.

    python tools/symbolize_bt.py LOGFILE [--binary build/arm64/sims2-arm64]
"""
import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SYMBOLIZER = Path(r"C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt"
                  r"\windows-x86_64\bin\llvm-symbolizer.exe")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--binary", default=str(ROOT / "build/arm64/sims2-arm64"))
    args = parser.parse_args()
    for line in Path(args.log).read_text(errors="replace").splitlines():
        m = re.search(r"\[bt\] ([^:]*):((?: 0x[0-9a-f]+)+)", line)
        if not m:
            continue
        offsets = m.group(2).split()
        out = subprocess.run([str(SYMBOLIZER), "--obj=" + args.binary,
                              *offsets], capture_output=True, text=True).stdout
        names = [l for l in out.splitlines() if l and not l.startswith("??:") and ":" not in l[:3]]
        frames = [l for l in out.split("\n\n") if l.strip()]
        print(f"== {m.group(1)}")
        for offset, frame in zip(offsets, frames):
            print(f"   {offset:>12} {frame.splitlines()[0]}")


if __name__ == "__main__":
    main()
