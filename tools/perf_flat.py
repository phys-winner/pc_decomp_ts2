"""Self and inclusive time per symbol from simpleperf report-sample output.
   python flat.py FILE [THREAD_NAME] [N]"""
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

path = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else None
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
self_t = collections.Counter()
incl_t = collections.Counter()
threads = collections.Counter()
total = 0


def flush(s):
    global total
    if not s or "symbol" not in s:
        return
    threads[s.get("thread_name", "?")] += s["count"]
    if want and s.get("thread_name") != want:
        return
    total += s["count"]
    self_t[s["symbol"]] += s["count"]
    for sym in set([s["symbol"]] + s["chain"]):
        incl_t[sym] += s["count"]


cur = None
in_chain = False
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
    elif t.startswith("thread_name:"):
        cur["thread_name"] = t.split(":", 1)[1].strip()
    elif t.startswith("symbol:"):
        sym = t.split(":", 1)[1].strip()
        if in_chain:
            cur["chain"].append(sym)
        else:
            cur["symbol"] = sym
flush(cur)
print("threads:", ", ".join("%s %.1f%%" % (k, 100.0 * v / sum(threads.values())) for k, v in threads.most_common(8)))
print("-- self")
for sym, v in self_t.most_common(top):
    print("%6.2f%%  %s" % (100.0 * v / total, cls(sym)))
print("-- inclusive")
for sym, v in incl_t.most_common(top):
    print("%6.2f%%  %s" % (100.0 * v / total, cls(sym)))
