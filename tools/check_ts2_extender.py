"""Check the production TS2 Extender INI parser and all annotated lift sites."""
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from audio_probe import compile_probe
from build_android import ROOT
from check_touch import JDK
from check_virtual_query import function
from ts2_extender_patches import PATCHES, apply


def check_zip_paths():
    sources = {
        "android/content/Context.java":
            "package android.content; public class Context {public java.io.File getCacheDir(){return new java.io.File(\"cache\");}}",
        "android/graphics/Bitmap.java": "package android.graphics; public class Bitmap {}",
        "android/graphics/BitmapFactory.java": """package android.graphics;
public class BitmapFactory {
 public static class Options { public boolean inJustDecodeBounds; public int outWidth,outHeight; }
 public static Bitmap decodeFile(String path, Options opts){return null;}
 public static Bitmap decodeFile(String path){return null;}
}""",
        "org/sims2/port/ExtenderZipCheck.java": r"""package org.sims2.port;
import java.io.*;
import java.nio.file.*;
import java.util.zip.*;
public class ExtenderZipCheck {
 static void entry(ZipOutputStream out,String name,byte[] data)throws IOException {
   out.putNextEntry(new ZipEntry(name));out.write(data);out.closeEntry();
 }
 public static void main(String[] args)throws Exception {
   File profile=new File("userdata");assert profile.mkdir();assert new File("cache").mkdir();
   File file=new File(profile,ExtenderAssets.ZIP_NAME);
   try(ZipOutputStream out=new ZipOutputStream(new FileOutputStream(file))){
     entry(out,"TSBin/TS2Extender.asi",new byte[]{1});
     entry(out,"TSBin\\TS2Extender.ini",new byte[]{'I','N','I'});
     entry(out,"Documents\\Downloads\\ld_timingFix.package",new byte[]{'D','B','P','F',1});
     entry(out,"TSBin\\wininet.dll",new byte[]{2});
   }
   assert ExtenderAssets.zipIni(file).equals("INI");
   File staged=ExtenderAssets.prepare(new android.content.Context(),profile);
   assert new File(staged,"ld_timingFix.package").length()==5;
   assert !new File(staged,"wininet.dll").exists()&&!new File(staged,"TS2Extender.asi").exists();
   assert !new File(staged,"ld_dateStoodUpFix.package").exists();
   System.out.println("PASS mixed-separator LC ZIP, INI and package staging, no Windows DLL extraction");
 }
}
""",
    }
    with TemporaryDirectory(prefix="sims2-extender-zip-") as temp:
        root = Path(temp)
        for name, source in sources.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
        production = ROOT / "android/app/src/org/sims2/port/ExtenderAssets.java"
        subprocess.run([str(JDK / "bin/javac.exe"), "--release", "8", "-d", temp,
                        str(production), *sources], cwd=temp, check=True)
        subprocess.run([str(JDK / "bin/java.exe"), "-ea", "-cp", temp,
                        "org.sims2.port.ExtenderZipCheck"], cwd=temp, check=True)


def main():
    native = (ROOT / "android/host/sims2_extender.c").read_text(encoding="utf-8")
    interface = (ROOT / "android/host/sims2_extender.h").read_text(encoding="utf-8")
    parser = "\n".join(function(native, name) for name in (
        "static char *trim(", "static int boolean(", "static int number(",
        "static void read_option(", "static void read_ini(", "void sub_A06B1356(void)"))
    compile_probe("ts2-extender-ini", r"""
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define strcasecmp _stricmp
unsigned s2_extender_flags;
static int enabled, charged;
static uint32_t g_ecx, memory[128];
#define MEM32(va) memory[(va) / 4]
void sub_A06B1356_lifted(void) { charged = (int)MEM32(g_ecx + 0x54); }
static int skip_intro, console, single_core, ui_scale, shadow_quality, imposter_quality, design_price, uniform_fix;
static int orbit_camera;
static double ui_reference, orbit_x, orbit_y;
""" + interface + parser + r"""
int main(void) {
    FILE *ini = tmpfile();
    assert(ini);
    s2_extender_flags = S2_EXTENDER_RNG | S2_EXTENDER_GROUPS;
    shadow_quality = 1;
    assert(fputs("\xef\xbb\xbf[General]\r\nSkipIntro = true ; QA\nConsole=1\n"
                 "[fixes]\nFIXRNG=false\nFixPoolShadows=1\nFixOFBUniform=1\n"
                 "[Enhancements]\nUIScale=1\nUIScaleResolution=600.5\nDesignToolPrice=27\n"
                 "ShadowQuality=nan\nImposterQuality=3\nSeparates4All = true\n"
                 "[Sims 3 Camera]\nEnabled=1\nXSensitivity=-2.5\nYSensitivity=0\n"
                 "[Other]\nFixPinkFlashing=true\n[Advanced]\nSingleCore=1\nExtendedLua=true\n", ini) >= 0);
    rewind(ini);
    read_ini(ini);
    fclose(ini);
    assert(skip_intro && console && uniform_fix && ui_scale && ui_reference == 600.5 && design_price == 27);
    assert(shadow_quality == 1 && imposter_quality == 3 && orbit_camera && orbit_x == -2.5 && orbit_y == 0);
    assert(single_core && s2_extender_flags == (S2_EXTENDER_GROUPS | S2_EXTENDER_POOL | S2_EXTENDER_SEPARATES | S2_EXTENDER_LUA));
    g_ecx = 8;
    MEM32(g_ecx + 0x54) = 15;
    sub_A06B1356();
    assert(charged == 15);
    enabled = 1;
    sub_A06B1356();
    assert(charged == 27);
    MEM32(g_ecx + 0x54) = 0;
    sub_A06B1356();
    assert(charged == 0);
    puts("PASS production TS2 Extender INI parsing, section isolation and invalid values");
    puts("PASS DesignToolPrice changes the original tool argument only when enabled");
    return 0;
}
""", {})
    chunks = {chunk for scope, _ in PATCHES.values() for chunk in ((scope,) if isinstance(scope, str) else scope)}
    for chunk in chunks:
        source = ROOT / "generated/sims2-x86" / f"sims2_{chunk}.c"
        original = source.read_bytes()
        patched = apply(original, source.name)
        assert patched.startswith(b'#include "sims2_extender.h"') and len(patched) > len(original)
    print(f"PASS {len(PATCHES)} TS2 Extender patch sites across {len(chunks)} lifted chunks")
    check_zip_paths()


if __name__ == "__main__":
    main()
