"""Exercise the actual APK resource installer on a JVM with an AssetManager boundary."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
JDK = Path(os.environ.get('JAVA_HOME', r'C:\Portable\jdk-24.0.1'))
STUB = r'''
package android.content.res;
public class AssetManager {
 public byte[] zip;public String version;public int opened;
 public java.io.InputStream open(String name){
  if(name.equals("runtime.version"))return new java.io.ByteArrayInputStream(version.getBytes(java.nio.charset.StandardCharsets.US_ASCII)){
   @Override public synchronized int read(byte[] b,int off,int count){return super.read(b,off,Math.min(count,3));}
  };
  ++opened;return new java.io.ByteArrayInputStream(zip);
 }
}
'''
CHECK = r'''
package org.sims2.port;
import java.io.*;
import java.nio.file.*;
import java.util.zip.*;
import android.content.res.AssetManager;
public class AssetsCheck {
 static byte[] archive(String... files)throws Exception{
  ByteArrayOutputStream bytes=new ByteArrayOutputStream();
  try(ZipOutputStream zip=new ZipOutputStream(bytes)){
   for(int i=0;i<files.length;i+=2){
    zip.putNextEntry(new ZipEntry(files[i]));zip.write(files[i+1].getBytes("UTF-8"));zip.closeEntry();
   }
  }
  return bytes.toByteArray();
 }
 public static void main(String[] args)throws Exception{
  File files=new File(args[0]);AssetManager assets=new AssetManager();
  assets.version=new String(new char[64]).replace('\0','a');
  assets.zip=archive("overlay/EP9/ui.package","first");
  File root=GameAssets.prepare(assets,files),data=new File(root,"overlay/EP9/ui.package");
  assert new String(Files.readAllBytes(data.toPath()),"UTF-8").equals("first");
  assert GameAssets.prepare(assets,files).equals(root)&&assets.opened==1;
  assets.version=new String(new char[64]).replace('\0','b');
  assets.zip=archive("overlay/new.package","second", "overlay/EP9/TSBin/Sims2EP9.exe","exe",
                     "overlay/EP9/TSBin/ijl15.dll","dll");
  GameAssets.prepare(assets,files);
  assert !data.exists()&&new File(root,"overlay/new.package").isFile();
  assert new String(Files.readAllBytes(root.toPath().resolve("overlay/EP9/TSBin/Sims2EP9.exe")),"UTF-8").equals("exe");
  assert new String(Files.readAllBytes(root.toPath().resolve("overlay/EP9/TSBin/ijl15.dll")),"UTF-8").equals("dll");
  assets.version=new String(new char[64]).replace('\0','c');
  assets.zip=archive("overlay/../../escaped","bad");
  try{GameAssets.prepare(assets,files);throw new AssertionError("Traversal accepted");}catch(IOException expected){}
  assert !new File(files,"escaped").exists()&&new File(root,"overlay/new.package").isFile();
  assets.zip=archive("overlay/retry.package","retry");GameAssets.prepare(assets,files);
  assert new File(root,"overlay/retry.package").isFile();
  assets.version="bad";
  try{GameAssets.prepare(assets,files);throw new AssertionError("Version accepted");}catch(IOException expected){}
  assert new File(root,"overlay/retry.package").isFile();
  System.out.println("PASS actual APK resources: short reads, install, cached launch, update without stale files, traversal rejection and interrupted-install recovery");
 }
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='s2-assets-') as directory:
        base = Path(directory)
        paths = []
        for name, source in [('android/content/res/AssetManager.java', STUB), ('org/sims2/port/AssetsCheck.java', CHECK)]:
            path = base / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding='utf-8')
            paths.append(path)
        subprocess.run([str(JDK / 'bin/javac.exe'), '--release', '8', '-d', str(base), *map(str, paths),
                        str(ROOT / 'android/app/src/org/sims2/port/GameAssets.java')], check=True)
        files = base / 'files'
        files.mkdir()
        subprocess.run([str(JDK / 'bin/java.exe'), '-ea', '-cp', str(base), 'org.sims2.port.AssetsCheck', str(files)], check=True)


if __name__ == '__main__':
    main()
