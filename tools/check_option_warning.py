"""Execute both production option-confirmation paths before any preference write."""
from pathlib import Path
import subprocess,tempfile
from check_touch import JDK,ROOT,method
from build_game_apk import launcher_root

STUBS={
'android/content/Context.java':'package android.content;public class Context {public String getString(int id){return "label";}}',
'android/graphics/Color.java':'package android.graphics;public class Color {public static final int BLACK=0xff000000;}',
'android/os/Process.java':'package android.os;public class Process {public static boolean is64Bit(){return true;}}',
'android/view/View.java':'''package android.view;public class View {public interface Click {void click(View v);}public Click listener;public void setOnClickListener(Click c){listener=c;}public void setForceDarkAllowed(boolean b){}public void press(){listener.click(this);}}''',
'android/widget/Button.java':'''package android.widget;public class Button extends android.view.View {public String text;public Button(android.content.Context c){}public void setAllCaps(boolean b){}public void setTextColor(int c){}public void setText(String s){text=s;}}''',
'android/widget/CheckBox.java':'''package android.widget;public class CheckBox extends Button {boolean checked;public CheckBox(android.content.Context c){super(c);}public void setChecked(boolean b){checked=b;}public boolean isChecked(){return checked;}public void press(){checked=!checked;super.press();}}''',
'android/widget/LinearLayout.java':'''package android.widget;public class LinearLayout {public java.util.List<android.widget.Button> children=new java.util.ArrayList<>();public void addView(android.widget.Button b){children.add(b);}}''',
'android/widget/Toast.java':'package android.widget;public class Toast {public static final int LENGTH_LONG=1;public static Toast makeText(android.content.Context c,String s,int length){return new Toast();}public void show(){}}',
'android/app/AlertDialog.java':'''package android.app;public class AlertDialog {
 public interface Click {void click(AlertDialog d,int which);}public static AlertDialog latest;public CharSequence message;public Click positive,negative,choice;public boolean dismissed;
 public void dismiss(){dismissed=true;}public void show(){latest=this;}public Window getWindow(){return new Window();}
 public static class Window {public android.view.View getDecorView(){return new android.view.View();}}
 public static class Builder {AlertDialog d=new AlertDialog();public Builder(android.content.Context c){}public Builder(android.content.Context c,int style){}
  public Builder setTitle(CharSequence t){return this;}public Builder setMessage(CharSequence t){d.message=t;return this;}
  public Builder setSingleChoiceItems(String[] a,int selected,Click c){d.choice=c;return this;}
  public Builder setPositiveButton(CharSequence t,Click c){d.positive=c;return this;}public Builder setNegativeButton(CharSequence t,Click c){d.negative=c;return this;}
  public AlertDialog create(){return d;}public AlertDialog show(){d.show();return d;}}
}''',
'org/sims2/port/R.java':'package org.sims2.port;public class R {public static class style {public static final int PortDialog=1;}public static class string {public static final int cancel=2,enable=3,settings_save_failed=4;}}',
'org/sims2/port/launcher/Spec.java':'''package org.sims2.port.launcher;public class Spec {static Option fixture=new Option();public static Spec get(android.content.Context c){return new Spec();}public java.util.List<Option> options(String d){return java.util.Collections.singletonList(fixture);}public static class Option {boolean arm64Only;String type="choice",key="adaptive_ui",title="Adaptive";java.util.List<String> choices=java.util.Arrays.asList("0","1"),choiceTitles=java.util.Arrays.asList("Off","On");}}''',
'org/sims2/port/launcher/Launch.java':'package org.sims2.port.launcher;public class Launch {String distribution(){return "legacy";}static String option(android.content.Context c,Spec.Option o){return State.value;}}',
'org/sims2/port/launcher/State.java':'package org.sims2.port.launcher;public class State {static final String CHOICES="choices";static int writes;static boolean fail;static String value="0";static boolean put(android.content.Context c,String file,String k,String v){assert k.equals("option.adaptive_ui");if(fail)return false;writes++;value=v;return true;}}',
'org/sims2/port/launcher/Texts.java':'package org.sims2.port.launcher;public class Texts {static final int OPTIONS=1,OK=2,CANCEL=3,CONTINUE=4;static String of(android.content.Context c,Object o){return o.toString();}static String t(android.content.Context c,int i){return "label";}}',
'org/sims2/port/launcher/PortHooks.java':'''package org.sims2.port.launcher;public class PortHooks {static PortHooks fixture=new PortHooks();String problem;static PortHooks of(android.content.Context c){return fixture;}String optionProblem(android.content.Context c,String k,String v){return problem;}CharSequence optionWarning(android.content.Context c,String k,String v){return v.equals("1")?"RED WARNING":null;}}'''
}
CHECK=r'''
public class WarningCheck extends android.content.Context {
 static class Ui {android.app.AlertDialog.Builder dialog(){return new android.app.AlertDialog.Builder(new android.content.Context());}void show(android.app.AlertDialog.Builder b){b.show();}}
 Ui ui=new Ui();PortHooks hooks=PortHooks.fixture;int refreshes;void refresh(int part){refreshes++;}
 METHOD
 public static void main(String[] args){
  WarningCheck c=new WarningCheck();Spec.Option option=Spec.fixture;
  c.selectOption(option,"1");assert State.writes==0&&State.value.equals("0");
  android.app.AlertDialog warning=android.app.AlertDialog.latest;assert warning.message.equals("RED WARNING");
  warning.dismiss();assert State.value.equals("0");
  c.selectOption(option,"1");warning=android.app.AlertDialog.latest;warning.positive.click(warning,0);
  assert State.writes==1&&State.value.equals("1")&&c.refreshes==1;
  c.selectOption(option,"0");assert State.writes==2&&State.value.equals("0");
  c.hooks.problem="unavailable";c.selectOption(option,"1");assert State.writes==2;c.hooks.problem=null;
  android.widget.LinearLayout content=new android.widget.LinearLayout();PortOptions.append(c,content,new Launch());
  android.widget.Button button=content.children.get(0);assert button.text.endsWith("Off");
  button.press();android.app.AlertDialog choices=android.app.AlertDialog.latest;choices.choice.click(choices,1);
  warning=android.app.AlertDialog.latest;assert choices.dismissed&&warning.message.equals("RED WARNING")&&State.writes==2;
  warning.dismiss();assert State.value.equals("0")&&button.text.endsWith("Off");
  button.press();choices=android.app.AlertDialog.latest;choices.choice.click(choices,1);
  warning=android.app.AlertDialog.latest;warning.positive.click(warning,0);
  assert State.writes==3&&State.value.equals("1")&&button.text.endsWith("On");
  button.press();choices=android.app.AlertDialog.latest;choices.choice.click(choices,0);
  assert State.writes==4&&State.value.equals("0")&&button.text.endsWith("Off");
  option.type="bool";State.value="1";
  content=new android.widget.LinearLayout();PortOptions.append(c,content,new Launch());
  android.widget.CheckBox checkbox=(android.widget.CheckBox)content.children.get(0);
  assert checkbox.isChecked();android.app.AlertDialog previous=android.app.AlertDialog.latest;
  checkbox.press();assert !checkbox.isChecked()&&State.value.equals("0")&&State.writes==5;
  checkbox.press();assert checkbox.isChecked()&&State.value.equals("1")&&State.writes==6;
  assert android.app.AlertDialog.latest==previous;
  State.fail=true;checkbox.press();assert checkbox.isChecked()&&State.value.equals("1")&&State.writes==6;
  assert android.app.AlertDialog.latest==previous;
  System.out.println("PASS production in-game checkbox: saved on/off, no warning, rollback on save failure");
  System.out.println("PASS production launcher and in-game option confirmation: warning before write, cancel retains Off, confirm persists On, disable immediate, availability refusal");
 }
}
'''

if __name__=='__main__':
    source=(launcher_root()/'android/src/port/launcher/LauncherActivity.java').read_text(encoding='utf-8')
    check='package org.sims2.port.launcher;\n'+CHECK.replace('METHOD',method(source,'private void selectOption('))
    with tempfile.TemporaryDirectory(prefix='s2-option-warning-') as folder:
        base=Path(folder);paths=[]
        for name,s in {**STUBS,'org/sims2/port/launcher/WarningCheck.java':check}.items():
            p=base/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s,encoding='utf-8');paths.append(str(p))
        paths.append(str(ROOT/'android/app/src/org/sims2/port/launcher/PortOptions.java'))
        subprocess.run([str(JDK/'bin/javac.exe'),'--release','8','-encoding','UTF-8','-d',str(base),*paths],check=True)
        subprocess.run([str(JDK/'bin/java.exe'),'-ea','-cp',str(base),'org.sims2.port.launcher.WarningCheck'],check=True)
