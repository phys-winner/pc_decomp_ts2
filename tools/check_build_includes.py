"""The host object cache follows nested .inc files without touching other objects."""
import tempfile
from pathlib import Path
from build_android import ROOT, included_fragments, header_digest


def main():
    out = ROOT / 'build/test/build-includes'
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as d:
        p = Path(d)
        a, b, source, other = (p / n for n in ('a.inc', 'b.inc', 'one.c', 'other.c'))
        source.write_text('#include "a.inc"\n#include "a.inc"\n')
        a.write_text('#include "b.inc"\n')
        b.write_text('#include "a.inc"\nint value=1;\n')
        other.write_text('int unrelated;\n')
        assert included_fragments(source) == sorted([a.resolve(), b.resolve()])
        assert included_fragments(other) == []
        old = header_digest(included_fragments(source))
        other_key = header_digest(included_fragments(other))
        b.write_text('#include "a.inc"\nint value=2;\n')
        assert old != header_digest(included_fragments(source))
        assert other_key == header_digest(included_fragments(other))
        a.write_text('#include "missing.inc"\n')
        try:
            included_fragments(source)
        except FileNotFoundError:
            pass
        else:
            raise AssertionError('missing dependency was silently cached')
    print('PASS include cache: nested mutation, duplicate/cycle, unrelated object and missing dependency')


if __name__ == '__main__':
    main()
