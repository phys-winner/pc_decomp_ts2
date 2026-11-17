"""The link order of the generated code: the functions the device actually
spends time in packed together at the start of .text.

The executable carries ~270 MB of lifted code and the kernel gives it 4 KB
pages only (transparent huge pages are off on the target), so a frame that
touches a few thousand functions scattered across all of it misses the
instruction TLB and L2 over and over. lld lays out the listed symbols first,
in this order (tools/build_android.py passes the file with
--symbol-ordering-file; the objects are built with -ffunction-sections):

1. functions sampled by simpleperf on the device, most samples first
   (build/pgo/*.symbols.txt: `simpleperf report --sort symbol -n` output);
2. every other function the PGO profile saw run, by entry count.

    python tools/make_symbol_order.py [--out build/pgo/order.txt]
"""
import argparse
import collections
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFDATA = Path(r"C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt"
                r"\windows-x86_64\bin\llvm-profdata.exe")


def sampled():
    counts = collections.Counter()
    for path in sorted((ROOT / "build/pgo").glob("*.symbols.txt")):
        total, rows = 0, []
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"\s*[\d.]+%\s+(\d+)\s+(\S+)\s*$", line)
            if m:
                rows.append((m.group(2), int(m.group(1))))
                total += int(m.group(1))
        for name, n in rows:            # each profile weighs the same, whatever its length
            counts[name] += n / max(total, 1)
    return [name for name, _ in counts.most_common() if re.match(r"[A-Za-z_][\w.]*$", name)]


def profiled(profile):
    text = subprocess.run([str(PROFDATA), "show", "--all-functions", "--counts", str(profile)],
                          capture_output=True, text=True).stdout
    out, name = [], None
    for line in text.splitlines():
        m = re.match(r"\s{2}(\S+):$", line)
        if m:
            name = m.group(1)
            continue
        m = re.match(r"\s+Function count: (\d+)", line)
        if m and name:
            if int(m.group(1)) > 0:
                out.append((int(m.group(1)), name))
            name = None
    out.sort(reverse=True)
    return [n for _, n in out]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=str(ROOT / "build/pgo/order.txt"))
    parser.add_argument("--profile", default=str(ROOT / "build/pgo/abb24fae-20261022.profdata"))
    args = parser.parse_args()
    order, seen = [], set()
    for name in sampled() + profiled(args.profile):
        # a profile names static functions as "file.c;name": the linker wants the symbol
        name = name.split(";")[-1]
        if name not in seen:
            seen.add(name)
            order.append(name)
    Path(args.out).write_text("\n".join(order) + "\n")
    print(f"{len(order)} symbols ordered -> {args.out}")


if __name__ == "__main__":
    main()
