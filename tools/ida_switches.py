"""Export the switch tables IDA recognised in an analysed database as JSON:
for each indirect `jmp`, the jump table's address, entry count and element
size, the lowest case and (for two-level switches) the byte index table, plus
the targets IDA resolved. tools/lift_sims2.py reads the tables to the exact
size instead of scanning for code addresses (a scan is capped at 256 entries
and runs into neighbouring tables).

    python tools/ida_switches.py research/ida/Sims2EP9.exe.i64 research/ida/Sims2EP9.switches.json

Open a copy when IDA itself has the database open.
"""
import json
import sys
import time

import idapro  # noqa: F401  (must load before the ida_* modules)
import ida_auto
import ida_idaapi
import ida_bytes
import ida_nalt
import ida_segment
import idautils


def main():
    path, out = sys.argv[1], sys.argv[2]
    start = time.time()
    if idapro.open_database(path, False):
        raise SystemExit(f"cannot open {path}")
    ida_auto.auto_wait()
    switches = []
    for seg_ea in idautils.Segments():
        seg = ida_segment.getseg(seg_ea)
        if seg.perm & ida_segment.SEGPERM_EXEC == 0 and ida_segment.get_segm_name(seg) != ".text":
            continue
        for ea in idautils.Heads(seg.start_ea, seg.end_ea):
            if not ida_bytes.is_code(ida_bytes.get_flags(ea)):
                continue
            si = ida_nalt.get_switch_info(ea)
            if si is None:
                continue
            targets = sorted({x for x in idautils.CodeRefsFrom(ea, 0)})
            entry = {"jump": ea, "table": si.jumps, "count": si.get_jtable_size(),
                     "element_size": si.get_jtable_element_size(), "lowcase": si.lowcase,
                     "default": si.defjump if si.defjump != ida_idaapi.BADADDR else None,
                     "flags": si.flags, "targets": targets}
            if si.is_indirect():
                entry["values"] = si.values
                entry["values_size"] = si.get_vtable_element_size()
                entry["ncases"] = si.ncases
            if si.elbase:
                entry["elbase"] = si.elbase
            switches.append(entry)
    data = {"input": path, "switch_count": len(switches), "switches": switches}
    with open(out, "w") as handle:
        json.dump(data, handle)
    idapro.close_database(False)
    print(f"{len(switches)} switches in {time.time() - start:.0f}s -> {out}", flush=True)


if __name__ == "__main__":
    main()
