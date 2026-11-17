"""Who the time of SYMBOL comes from (simpleperf report-sample output):
   python callers.py FILE SYMBOL [DEPTH] [N]  — the caller chains DEPTH deep."""
import collections
import sys
import json as _json
import os as _os
_MAP = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "build", "rtti_methods.json")
_M = _json.load(open(_MAP)) if _os.path.exists(_MAP) else {}   # tools/rtti_classes.py --dump


def cls(sym):
    """sym with its class (RTTI vtable slot) when it is a known method."""
    if sym.startswith("sub_") and sym[4:] in _M:
        n = _M[sym[4:]][0]
        return "%s [%s]" % (sym, n[4:n.index("@@")] if "@@" in n else n)
    return sym

path, target = sys.argv[1], sys.argv[2]
depth = int(sys.argv[3]) if len(sys.argv) > 3 else 2
top = int(sys.argv[4]) if len(sys.argv) > 4 else 25
chains = collections.Counter()
total = hit = 0
cur = None
in_chain = False


def flush(s):
    global total, hit
    if not s or "symbol" not in s:
        return
    total += s["count"]
    syms = [s["symbol"]] + s["chain"]
    if target in syms:
        i = syms.index(target)
        hit += s["count"]
        chains[" <- ".join(syms[i + 1:i + 1 + depth])] += s["count"]


for line in open(path, encoding="utf-8", errors="replace"):
    if line.startswith("sample:"):
        flush(cur)
        cur = {"chain": [], "count": 0}
        in_chain = False
        continue
    if cur is None:
        continue
    t = line.strip()
    if t.startswith("callchain:"):
        in_chain = True
    elif t.startswith("event_count:"):
        cur["count"] = int(t.split()[1])
    elif t.startswith("symbol:"):
        sym = t.split(":", 1)[1].strip()
        if in_chain:
            cur["chain"].append(sym)
        else:
            cur["symbol"] = sym
flush(cur)
print("%s: %.2f%% inclusive" % (target, 100.0 * hit / total))
for c, v in chains.most_common(top):
    print("%6.2f%%  %s" % (100.0 * v / total, " <- ".join(cls(x) for x in c.split(" <- "))))
