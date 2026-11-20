"""Port renderer priority plus the shared launcher's actual selection code on the JVM."""
import json
from pathlib import Path
import subprocess
import tempfile

from build_game_apk import ROOT, ANDROID_JAR, JAVA_HOME, launcher_root

CHECK = r'''package port.launcher;
import java.util.*;
public class RendererDefaultsCheck {
 static Spec.Mode mode(String id){Spec.Mode m=new Spec.Mode();m.id=id;return m;}
 static List<Renderers.Status> states(boolean stock,boolean turnip){return Arrays.asList(
  new Renderers.Status(mode("gles"),true,true,null),new Renderers.Status(mode("dxvk"),true,stock,null),new Renderers.Status(mode("turnip"),true,turnip,null));}
 public static void main(String[] args){
  assert Renderers.effective("gles",states(true,true),"gles",false).equals("gles");
  assert Renderers.effective("gles",states(false,false),"gles",false).equals("gles");
  assert Renderers.effective("gles",states(true,true),"dxvk",true).equals("dxvk");
  assert Renderers.effective("gles",states(false,true),"turnip",true).equals("turnip");
  List<Renderers.Status> unavailable=Arrays.asList(new Renderers.Status(mode("gles"),true,true,null),new Renderers.Status(mode("dxvk"),false,false,null),new Renderers.Status(mode("turnip"),true,true,null));
  assert Renderers.effective("gles",unavailable,"dxvk",true).equals("gles");
  for (String model : Arrays.asList("Mali-G76 MC4", "Immortalis-G720", ""))
   assert Probe.mali(null,Probe.parse("stock.vendor=13b5\nstock.device="+model)) : model;
  assert !Probe.mali(null,Probe.parse("stock.vendor=1010\nstock.device=PowerVR"));
  System.out.println("PASS actual launcher selection: OpenGL ES default and fallback, explicit Vulkan choices preserved");
  System.out.println("PASS PanVK probing uses the ARM Vulkan vendor without a model allowlist");
 }
}'''


def main():
    spec=json.loads((ROOT/'android/port.json').read_text(encoding='utf-8'))
    assert spec['renderers']['default']=='gles'
    assert spec['distributions'][0]['renderers']==['gles','dxvk','turnip','panvk']
    modes={m['id']:m for m in spec['renderers']['modes']}
    assert modes['dxvk']['vulkan']=='1.3' and modes['dxvk']['minSdk']==30
    assert modes['turnip']['kind']=='turnip' and modes['turnip']['minSdk']==30
    assert modes['panvk']['kind']=='panvk' and modes['panvk']['minSdk']==30
    with tempfile.TemporaryDirectory(prefix='sims2-renderer-default-') as folder:
        out=Path(folder);test=out/'RendererDefaultsCheck.java';test.write_text(CHECK,encoding='utf-8')
        sources=sorted((launcher_root()/'android/src/port/launcher').glob('*.java'))
        subprocess.run([str(JAVA_HOME/'bin/javac.exe'),'--release','8','-encoding','UTF-8','-cp',str(ANDROID_JAR),'-d',str(out),*[str(p)for p in sources],str(test)],check=True)
        subprocess.run([str(JAVA_HOME/'bin/java.exe'),'-ea','-cp',str(out)+';'+str(ANDROID_JAR),'port.launcher.RendererDefaultsCheck'],check=True)


if __name__=='__main__':main()
