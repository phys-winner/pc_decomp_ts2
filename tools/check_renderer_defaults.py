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
  new Renderers.Status(mode("dxvk"),true,stock,null),new Renderers.Status(mode("turnip"),true,turnip,null),new Renderers.Status(mode("gles"),true,true,null));}
 public static void main(String[] args){
  assert Renderers.effective("dxvk",states(true,true),"dxvk",false).equals("dxvk");
  assert Renderers.effective("dxvk",states(false,true),"dxvk",false).equals("turnip");
  assert Renderers.effective("dxvk",states(false,false),"dxvk",false).equals("gles");
  assert Renderers.effective("dxvk",states(true,true),"gles",true).equals("gles");
  List<Renderers.Status> unavailable=Arrays.asList(new Renderers.Status(mode("dxvk"),false,false,null),new Renderers.Status(mode("turnip"),true,true,null),new Renderers.Status(mode("gles"),true,true,null));
  assert Renderers.effective("dxvk",unavailable,"dxvk",false).equals("turnip");
  System.out.println("PASS actual launcher selection: system DXVK first, verified Turnip fallback, GLES fallback, explicit GLES preserved");
 }
}'''


def main():
    spec=json.loads((ROOT/'android/port.json').read_text(encoding='utf-8'))
    assert spec['renderers']['default']=='dxvk'
    assert spec['distributions'][0]['renderers']==['dxvk','turnip','gles']
    modes={m['id']:m for m in spec['renderers']['modes']}
    assert modes['dxvk']['vulkan']=='1.3' and modes['dxvk']['minSdk']==30
    assert modes['turnip']['kind']=='turnip' and modes['turnip']['minSdk']==30
    with tempfile.TemporaryDirectory(prefix='sims2-renderer-default-') as folder:
        out=Path(folder);test=out/'RendererDefaultsCheck.java';test.write_text(CHECK,encoding='utf-8')
        sources=sorted((launcher_root()/'android/src/port/launcher').glob('*.java'))
        subprocess.run([str(JAVA_HOME/'bin/javac.exe'),'--release','8','-encoding','UTF-8','-cp',str(ANDROID_JAR),'-d',str(out),*[str(p)for p in sources],str(test)],check=True)
        subprocess.run([str(JAVA_HOME/'bin/java.exe'),'-ea','-cp',str(out)+';'+str(ANDROID_JAR),'port.launcher.RendererDefaultsCheck'],check=True)


if __name__=='__main__':main()
