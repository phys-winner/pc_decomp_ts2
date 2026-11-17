"""Run IDA's auto-analysis headlessly (idalib) over a PE image and export its
function catalog as JSON: start, end, size, name and IDA flags per function,
plus the tail chunks IDA attached to each one.

    python tools/ida_catalog.py research/ida/Sims2EP9.exe research/ida/Sims2EP9.ida.json

The database (.i64) is kept next to the input so later questions can be
answered from it without repeating the analysis.
"""
import json
import sys
import time

import idapro  # noqa: F401  (must load before the ida_* modules)
import ida_auto
import ida_funcs
import ida_nalt
import idautils


def main():
    path, out = sys.argv[1], sys.argv[2]
    start = time.time()
    if idapro.open_database(path, True):
        raise SystemExit(f"cannot open {path}")
    ida_auto.auto_wait()
    analysed = time.time() - start
    functions = []
    for ea in idautils.Functions():
        f = ida_funcs.get_func(ea)
        tails = []
        if f.tailqty:
            iterator = ida_funcs.func_tail_iterator_t(f)
            ok = iterator.first()
            while ok:
                chunk = iterator.chunk()
                tails.append([chunk.start_ea, chunk.end_ea])
                ok = iterator.next()
        functions.append({"address": ea, "end": f.end_ea, "size": f.end_ea - ea,
                          "name": ida_funcs.get_func_name(ea), "flags": f.flags, "tails": tails})
    data = {"input": path, "image_base": ida_nalt.get_imagebase(), "analysis_seconds": round(analysed, 1),
            "function_count": len(functions), "functions": functions}
    with open(out, "w") as handle:
        json.dump(data, handle)
    idapro.close_database(True)
    print(f"{len(functions)} functions in {analysed:.0f}s -> {out}", flush=True)


if __name__ == "__main__":
    main()
