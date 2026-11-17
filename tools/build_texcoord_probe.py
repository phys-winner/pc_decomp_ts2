"""Build TEXCOORD and existing vertex-color GPU regressions; no device actions."""
from build_android import ROOT
from build_vertex_color_probe import build_probe


if __name__ == '__main__':
    build_probe('android/tools/texcoord_probe.c', ROOT / 'build/test/texcoord', 'texcoord_probe')
