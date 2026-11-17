"""Run actual Activity cheat/dialog/lifecycle methods against instrumented Android boundaries."""
import json
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import check_touch as T

EXTRA = r'''
 int cheatSerial,cheatMenuContext;boolean cheatCatalogRequest;long cheatMenuSize,cheatMenuEpoch;
 java.util.List<CheatCommands.Command> cheats;
 static long cheatReply=(9L<<32)|1;static int nativeCheatRequests,nativeCheatCancels;static String lastCommand;
 static boolean mutateRows;static String[] rows={"motherLode\tAdd money","aging\ton|off","help\tAll commands"};
 static long nativeCheatsRequest(String command){nativeCheatRequests++;lastCommand=command;return cheatReply;}
 static long nativeCheatsStatus(int serial){return cheatReply;}
 static String[] nativeCheatsRows(int serial){if(mutateRows)layoutEpoch++;return rows;}
 static void nativeCheatsCancel(){nativeCheatCancels++;}
 boolean cheatRussian(){return true;}
 void showTouchSettings(){}
'''

CHECK = r'''package org.sims2.port;
import android.app.AlertDialog;import android.widget.*;
public class CheatsActivityCheck {
 static boolean blocked(GameActivity a)throws Exception{java.lang.reflect.Field f=TouchLayer.class.getDeclaredField("blocked");f.setAccessible(true);return f.getBoolean(a.touch);}
 static GameActivity start(){GameActivity a=new GameActivity();a.gamepad=new GamepadLayer();GameActivity.active=a;GameActivity.context=TouchLayer.LIVE;GameActivity.layoutEpoch++;a.createObjectControls(new FrameLayout(a));GameActivity.nativeCheatRequests=GameActivity.nativeCheatCancels=0;GameActivity.cheatReply=(9L<<32)|1;GameActivity.mutateRows=false;return a;}
 static void choose(AlertDialog d,int i){d.choice.call(d,i);d.dismiss();}
 static EditText editor(AlertDialog d){return(EditText)((LinearLayout)((ScrollView)d.view).child).children.get(0);}
 public static void main(String[] args)throws Exception{
  GameActivity a=start();a.showCheats();assert a.cheatSerial==9&&blocked(a)&&!a.gamepad.enabled&&GameActivity.nativeCheatRequests==1;
  a.showCheats();assert GameActivity.nativeCheatRequests==1;
  a.handleCheatReceipt((8L<<32)|2);assert a.cheatSerial==9&&a.cheatDialog==null;
  a.handleCheatReceipt((9L<<32)|2);assert a.cheatSerial==0&&a.cheatDialog.showing&&a.cheatDialog.choices.length==8&&blocked(a);
  AlertDialog groups=a.cheatDialog;choose(groups,0);assert a.cheatDialog!=groups&&!groups.showing&&a.cheatDialog.showing&&blocked(a)&&GameActivity.nativeCheatCancels==0;
  AlertDialog money=a.cheatDialog;choose(money,0);AlertDialog command=a.cheatDialog;assert editor(command).getText().toString().equals("motherLode")&&blocked(a);
  String help=((TextView)((LinearLayout)((ScrollView)command.view).child).children.get(1)).text;
  assert help.contains("50 000")&&help.contains("Синтаксис и справка оригинальной игры:\nAdd money");
  GameActivity.cheatReply=(10L<<32)|1;command.getButton(-1).performClick();assert a.cheatSerial==10&&!command.getButton(-1).enabled&&GameActivity.nativeCheatRequests==2;
  command.getButton(-1).performClick();assert GameActivity.nativeCheatRequests==2;
  command.dismiss();assert a.cheatDialog==null&&GameActivity.nativeCheatCancels==1&&blocked(a)&&!a.gamepad.enabled;
  a.handleCheatReceipt((10L<<32)|2);assert !blocked(a)&&a.gamepad.enabled;
  a=start();a.showCheats();GameActivity.events.clear();a.onBackPressed();assert GameActivity.nativeCheatCancels==1&&GameActivity.events.isEmpty();a.handleCheatReceipt((9L<<32)|5);assert !blocked(a)&&a.gamepad.enabled;
  a=start();a.showCheats();GameActivity.mutateRows=true;a.handleCheatReceipt((9L<<32)|2);assert a.cheatDialog==null&&!blocked(a)&&a.gamepad.enabled;
  a=start();a.showCheats();a.handleCheatReceipt((9L<<32)|2);a.onPause();assert GameActivity.nativeCheatCancels==1&&!a.gamepad.enabled;a.onResume();assert !a.gamepad.enabled;a.onWindowFocusChanged(true);assert blocked(a)&&!a.gamepad.enabled;
  a.cheatDialog.dismiss();assert !blocked(a)&&a.gamepad.enabled;
  a=start();a.showCheats();a.handleCheatReceipt((9L<<32)|2);AlertDialog old=a.cheatDialog;GameActivity b=start();a.onDestroy();assert !old.showing&&GameActivity.active==b&&GameActivity.nativeCheatCancels==0;
  a=start();a.showCheats();a.handleCheatReceipt((9L<<32)|2);a.showCheatConsole("help","");editor(a.cheatDialog).setText("help\nquit");int count=GameActivity.nativeCheatRequests;a.cheatDialog.getButton(-1).performClick();assert GameActivity.nativeCheatRequests==count;
  System.out.println("PASS actual cheat Activity: catalog receipts/stale serial, dialog replacement, grouping, explicit Execute once, queued Back/dismiss cancellation, STARTED polling, layout race recovery, pause/resume/focus/gamepad and stale Activity destruction");
 }
}'''


def main():
    activity=(T.ROOT/'android/app/src/org/sims2/port/GameActivity.java').read_text(encoding='utf-8')
    native=T.NATIVE.replace('static int cheatRequests;static void touchCheatsRequested(){cheatRequests++;}static void nativeCheatsCancel(){}','')
    native=native.replace('boolean cheatUi(){return false;}','')+EXTRA
    signatures=['private boolean cheatUi(','private String cheatText(','private boolean cheatMenuValid(',
        'private void showCheats(','private void showCheatDialog(','private AlertDialog.Builder cheatBuilder(',
        'private void showCheatGroups(','private void showCheatGroup(','private void showCheatConsole(',
        'private void handleCheatReceipt(','static void touchObjectControlsChanged(','static void touchCheatsRequested(',
        'private Button objectButton(','private void createObjectControls(','private void refreshObjectControls(',
        'private boolean ownsInput(','private void cancelTextSession(',
        'public void onBackPressed(','public void onWindowFocusChanged(','protected void onPause(',
        'protected void onResume(','protected void onDestroy(']
    native+='\n'.join(T.method(activity,s) for s in signatures)+'\n}'
    stubs=dict(T.STUBS)
    stubs['org/sims2/port/GamepadLayer.java']='package org.sims2.port;class GamepadLayer{boolean enabled=true;void release(){}void setEnabledInput(boolean v){enabled=v;}}'
    stubs['android/hardware/input/InputManager.java']='package android.hardware.input;public class InputManager{public void unregisterInputDeviceListener(Object o){}}'
    dialog=stubs['android/app/AlertDialog.java']
    a=dialog.index('public static class Button {');b=dialog.index('\n public interface Click',a)
    dialog=dialog[:a]+'''public static class Button extends android.widget.Button {public Runnable action;public Button(){super(new android.content.Context());}public void performClick(){if(enabled){if(listener!=null)listener.click(this);else if(action!=null)action.run();}}public void click(){performClick();}}
'''+dialog[b:]
    dialog=dialog.replace('public Pair negative,choice;','public Pair negative,choice,neutral;')
    dialog=dialog.replace('public Builder setNegativeButton(String s,Pair p)',
        'public Builder setItems(String[] a,Pair p){d.choices=a;d.choice=p;return this;}public Builder setNeutralButton(String s,Pair p){d.neutral=p;return this;}public Builder setNegativeButton(String s,Pair p)')
    stubs['android/app/AlertDialog.java']=dialog
    stubs['android/text/InputType.java']=stubs['android/text/InputType.java'].replace('TYPE_CLASS_TEXT=1','TYPE_TEXT_FLAG_NO_SUGGESTIONS=0x80000,TYPE_CLASS_TEXT=1')
    translations={s.attrib['name']:s.text for s in ET.parse(T.ROOT/'android/app/res/values-ru/strings.xml').getroot()}
    names=list(translations)
    constants=''.join('public static final int '+name+'='+str(i)+';' for i,name in enumerate(names))
    stubs['org/sims2/port/R.java']='package org.sims2.port;class R{static class style{static final int PortDialog=1234;}static class string{'+constants+'}static String[] values={'+','.join(json.dumps(translations[n],ensure_ascii=False) for n in names)+'};}'
    sources={**stubs,'org/sims2/port/GameActivity.java':native,'org/sims2/port/CheatsActivityCheck.java':CHECK}
    with tempfile.TemporaryDirectory(prefix='sims2-cheat-activity-') as folder:
        out=Path(folder);paths=[]
        for name,source in sources.items():
            path=out/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(source,encoding='utf-8');paths.append(str(path))
        paths.extend(str(T.ROOT/'android/app/src/org/sims2/port'/name) for name in ('TouchLayer.java','CheatCommands.java'))
        subprocess.run([str(T.JDK/'bin/javac.exe'),'--release','8','-encoding','UTF-8','-d',str(out),*paths],check=True)
        subprocess.run([str(T.JDK/'bin/java.exe'),'-ea','-cp',str(out),'org.sims2.port.CheatsActivityCheck'],check=True)


if __name__=='__main__':main()
