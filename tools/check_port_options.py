"""Execute the in-game option UI and shared launcher persistence on the host JVM."""
import json
from pathlib import Path
import subprocess
import tempfile

from audio_probe import compile_probe
from check_touch import JDK, ROOT
from build_game_apk import launcher_root

LAUNCHER = launcher_root() / 'android/src/port/launcher'
STUBS = {
    'android/content/Context.java': '''package android.content;
public class Context {public java.io.File files;public Context(java.io.File dir){files=dir;}
public java.io.File getFilesDir(){return files;}public String getString(int id){return "message:"+id;}}''',
    'android/os/Process.java': 'package android.os;public class Process {public static boolean bit64=true;public static boolean is64Bit(){return bit64;}}',
    'android/graphics/Color.java': 'package android.graphics;public class Color {public static final int BLACK=0xff000000;}',
    'android/view/View.java': '''package android.view;public class View {public interface Click {void onClick(View v);}
public Click click;public void setOnClickListener(Click c){click=c;}public void performClick(){click.onClick(this);}
public void setForceDarkAllowed(boolean allowed){assert !allowed;}}''',
    'android/view/Window.java': 'package android.view;public class Window {public View getDecorView(){return new View();}}',
    'android/widget/Button.java': '''package android.widget;public class Button extends android.view.View {
public String text;public Button(android.content.Context c){}public void setAllCaps(boolean b){assert !b;}
public void setTextColor(int c){assert c==android.graphics.Color.BLACK;}public void setText(String s){text=s;}}''',
    'android/widget/CheckBox.java': '''package android.widget;public class CheckBox extends Button {
private boolean checked;public CheckBox(android.content.Context c){super(c);}public boolean isChecked(){return checked;}
public void setChecked(boolean value){checked=value;}public void performClick(){checked=!checked;super.performClick();}}''',
    'android/widget/LinearLayout.java': '''package android.widget;public class LinearLayout {public java.util.List<Button> children=new java.util.ArrayList<>();
public void addView(Button b){children.add(b);}}''',
    'android/widget/Toast.java': '''package android.widget;public class Toast {public static final int LENGTH_LONG=1;public static String last;
public static Toast makeText(android.content.Context c,String message,int duration){last=message;return new Toast();}public void show(){}}''',
    'android/app/AlertDialog.java': '''package android.app;public class AlertDialog {
public static AlertDialog latest;public boolean showing;public String[] labels;public int checked;public Choice callback;
public interface Choice {void onClick(AlertDialog d,int which);}public void show(){latest=this;showing=true;}
public void dismiss(){showing=false;}public android.view.Window getWindow(){return new android.view.Window();}
public static class Builder {AlertDialog d=new AlertDialog();public Builder(android.content.Context c,int theme){assert theme==1;}
public Builder setTitle(String title){return this;}public Builder setMessage(CharSequence message){return this;}public Builder setPositiveButton(String text,Choice callback){return this;}public Builder setSingleChoiceItems(String[] labels,int checked,Choice callback){d.labels=labels;d.checked=checked;d.callback=callback;return this;}
public Builder setNegativeButton(String text,Choice callback){return this;}public AlertDialog create(){return d;}public AlertDialog show(){d.show();return d;}}}''',
    'org/sims2/port/R.java': 'package org.sims2.port;public class R {public static class style {public static final int PortDialog=1;}public static class string {public static final int settings_save_failed=2,cancel=3,enable=4;}}',
    'org/sims2/port/launcher/Work.java': 'package org.sims2.port.launcher;class Work {static void warn(String text,Throwable error){}}',
    'org/sims2/port/launcher/Texts.java': 'package org.sims2.port.launcher;class Texts {static String of(android.content.Context c,Object text){return String.valueOf(text);}}',
    'org/sims2/port/launcher/PortHooks.java': '''package org.sims2.port.launcher;class PortHooks {
static String problem;static PortHooks of(android.content.Context c){return new PortHooks();}
String optionProblem(android.content.Context c,String key,String value){return problem;}CharSequence optionWarning(android.content.Context c,String key,String value){return null;}}''',
    'org/sims2/port/launcher/Launch.java': '''package org.sims2.port.launcher;public class Launch {
public String distribution(){return "legacy";}static String option(android.content.Context c,Spec.Option o){return State.get(c,State.CHOICES,"option."+o.key,o.value);}}''',
}

CHECK = '''package org.sims2.port.launcher;
import android.app.AlertDialog;import android.content.Context;import android.widget.*;
public class OptionsCheck {
 static AlertDialog open(Button b){b.performClick();assert AlertDialog.latest.showing;return AlertDialog.latest;}
 public static void main(String[] args)throws Exception{
  Context context=new Context(new java.io.File(args[0]));Launch launch=new Launch();LinearLayout content=new LinearLayout();
  PortOptions.append(context,content,launch);assert content.children.size()==Spec.instance.options.size();
  int selections=0;
  for(int i=0;i<Spec.instance.options.size();i++){
   Spec.Option option=Spec.instance.options.get(i);Button button=content.children.get(i);
   if("bool".equals(option.type)){
    CheckBox check=(CheckBox)button;boolean initial=check.isChecked();
    assert initial=="1".equals(option.value)&&check.text.equals(option.title);
    check.performClick();assert check.isChecked()!=initial&&Launch.option(context,option).equals(initial?"0":"1");
    check.performClick();assert check.isChecked()==initial&&Launch.option(context,option).equals(initial?"1":"0");
    continue;
   }
   assert button.text.contains(option.title+": ");AlertDialog dialog=open(button);
   assert dialog.labels.length==option.choices.size()&&dialog.checked==option.choices.indexOf(option.value);
   dialog.callback.onClick(dialog,-1);dialog.callback.onClick(dialog,dialog.labels.length);
   assert State.get(context,State.CHOICES,"option."+option.key,null)==null&&dialog.showing;
   for(int j=0;j<option.choices.size();j++){
    dialog=open(button);dialog.callback.onClick(dialog,j);selections++;
    assert !dialog.showing&&State.get(context,State.CHOICES,"option."+option.key,null).equals(option.choices.get(j));
    assert button.text.equals(option.title+": "+option.choiceTitles.get(j));
   }
  }
  Button button=content.children.get(0);Spec.Option option=Spec.instance.options.get(0);String before=Launch.option(context,option);
  PortHooks.problem="not available";AlertDialog dialog=open(button);dialog.callback.onClick(dialog,0);
  assert dialog.showing&&Toast.last.equals("not available")&&Launch.option(context,option).equals(before);dialog.dismiss();PortHooks.problem=null;
  // A launcher's edit made after opening the dialog survives this option's atomic read/merge/write.
  dialog=open(button);Context other=new Context(context.files);assert State.put(other,State.CHOICES,"renderer.legacy","gles");
  dialog.callback.onClick(dialog,0);assert State.get(other,State.CHOICES,"renderer.legacy",null).equals("gles");
  assert Launch.option(other,option).equals(option.choices.get(0));
  java.io.File dir=State.folder(context);assert !new java.io.File(dir,"choices.tmp").exists();
  // Simulate an unwritable destination without relying on host permission semantics.
  java.io.File blocked=new java.io.File(args[0],"file-instead-of-dir");assert blocked.createNewFile();context.files=blocked;
  dialog=open(button);dialog.callback.onClick(dialog,0);assert dialog.showing&&Toast.last.equals("message:2");dialog.dismiss();
  context.files=new java.io.File(args[0]);Spec.instance.options.get(0).arm64Only=true;android.os.Process.bit64=false;
  content=new LinearLayout();PortOptions.append(context,content,launch);assert content.children.size()==Spec.instance.options.size()-1;
  // FSR 3.1 left the port: a saved \"3\" becomes FSR 1; other choices stay.
  assert State.put(context,State.CHOICES,"option.render_upscaler","3");PortOptions.migrateUpscaler(context);
  assert State.get(context,State.CHOICES,"option.render_upscaler",null).equals("1");
  assert State.put(context,State.CHOICES,"option.render_upscaler","0");PortOptions.migrateUpscaler(context);
  assert State.get(context,State.CHOICES,"option.render_upscaler",null).equals("0");
  System.out.println("PASS in-game port options: "+selections+" manifest choices, resolution default100%, MSAA0/2/4, explicit validation, shared atomic state, fresh reads, failed-save feedback, FSR 3.1 choice -> FSR 1");
 }
}'''


def main():
    spec = json.loads((ROOT / 'android/port.json').read_text(encoding='utf-8'))
    options = spec['options']
    by_key = {option['key']: option for option in options}
    assert by_key['screen_resolution']['default'] == '100'
    assert {choice['value'] for choice in by_key['render_msaa']['choices']} == {'0', '2', '4'}
    assert by_key['frame_rate_limit']['default'] == 'game'
    assert 'loading_animations' not in by_key   # no player option: loading screens keep the game's own pacing (sims2_loading.c)
    assert {choice['value'] for choice in by_key['frame_rate_limit']['choices']} == {'game', '0', '30', '60', '90', '120'}
    required = {'game_language', 'screen_resolution', 'frame_rate_limit', 'ui_fsr', 'render_aa', 'render_msaa', 'render_upscaler'}
    assert required <= by_key.keys()
    generated = '''package org.sims2.port.launcher;import java.util.*;class Spec {
static class Option {String key,type,value;Object title;boolean arm64Only;List<String> choices=new ArrayList<>();List<Object> choiceTitles=new ArrayList<>();}
static Spec instance=new Spec();List<Option> options=new ArrayList<>();static Spec get(android.content.Context c){return instance;}
List<Option> options(String distribution){assert distribution.equals("legacy");return options;}Spec(){Option o;
'''
    for option in options:
        assert option['type'] in ('choice', 'bool')
        generated += 'o=new Option();'
        for name in ('key', 'type', 'default'):
            generated += f'o.{"value" if name == "default" else name}={json.dumps(option[name], ensure_ascii=False)};'
        title = option['title']
        generated += f'o.title={json.dumps(title.get("en") if isinstance(title, dict) else title, ensure_ascii=False)};'
        for choice in option.get('choices', ()):
            title = choice['title']
            generated += f'o.choices.add({json.dumps(choice["value"])});'
            generated += f'o.choiceTitles.add({json.dumps(title.get("en") if isinstance(title, dict) else title, ensure_ascii=False)});'
        generated += 'options.add(o);\n'
    sources = dict(STUBS)
    sources['org/sims2/port/launcher/Spec.java'] = generated + '}}'
    sources['org/sims2/port/launcher/OptionsCheck.java'] = CHECK
    sources['org/sims2/port/launcher/State.java'] = (LAUNCHER / 'State.java').read_text(encoding='utf-8').replace('package port.launcher;', 'package org.sims2.port.launcher;')
    with tempfile.TemporaryDirectory(prefix='sims2-port-options-') as folder:
        out = Path(folder)
        paths = []
        for name, source in sources.items():
            path = out / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding='utf-8')
            paths.append(str(path))
        paths.append(str(ROOT / 'android/app/src/org/sims2/port/launcher/PortOptions.java'))
        subprocess.run([str(JDK / 'bin/javac.exe'), '--release', '8', '-encoding', 'UTF-8', '-d', str(out), *paths], check=True)
        subprocess.run([str(JDK / 'bin/java.exe'), '-ea', '-cp', str(out), 'org.sims2.port.launcher.OptionsCheck', str(out / 'state')], check=True)
    settings = (ROOT / 'android/host/sims2_settings.c').read_text(encoding='utf-8')
    msaa = settings[settings.index('    const char *msaa=getenv('):settings.index('    const char *upscaler=getenv(')]
    cap = settings[settings.index('    const char *cap=getenv('):settings.index('    const char *fsr = getenv(')]
    override = settings[settings.index('    if (fps_override) {'):settings.index('    if (render_override)')]
    harness = '''#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *fixture,*override;
static const char *fixture_env(const char *name){return !strcmp(name,"S2_FPS_LIMIT")?override:fixture;}
#define getenv(name) fixture_env(name)
''' + next(line for line in settings.splitlines() if line.startswith('static int clamp(')) + '''
static int parse(void){int render_msaa;
''' + msaa + '''return render_msaa;}
static int parse_cap(int initial,int *saved){int fps_limit=initial,saved_fps=initial;
const char *fps_override=getenv("S2_FPS_LIMIT");
''' + cap + override + '''
*saved=saved_fps;return fps_limit;}
int main(void){const char *values[]={NULL,"","0","1","2","3","4","8","-2","2x"," 4","04"};
for(unsigned i=0;i<sizeof values/sizeof *values;i++){fixture=values[i];assert(parse()==(i==4?2:i==6?4:0));}
const char *caps[]={NULL,"","game","0","30","60","90","120","15","75","240","-30","30x"," 60","060","Game"};
const int expected[]={75,75,75,0,30,60,90,120,75,75,75,75,75,75,75,75};
for(unsigned i=0;i<sizeof caps/sizeof *caps;i++){
 fixture=caps[i];override=NULL;int saved=-1;assert(parse_cap(75,&saved)==expected[i]&&saved==expected[i]);
 override="30";assert(parse_cap(75,&saved)==30&&saved==expected[i]);
 override="0";assert(parse_cap(75,&saved)==0&&saved==expected[i]);
 override="300";assert(parse_cap(75,&saved)==240&&saved==expected[i]);
 override="1";assert(parse_cap(75,&saved)==5&&saved==expected[i]);
}
puts("PASS actual native MSAA/FPS parsers: exact values, malformed FPS keeps game setting, S2 override wins without overwriting saved choice");}
'''
    compile_probe('port-options', harness, {})


if __name__ == '__main__':
    main()
