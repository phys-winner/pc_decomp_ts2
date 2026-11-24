"""Execute the Android locale mapping and one-time launcher migration on the JVM."""
import json
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from check_touch import JDK, ROOT

STUBS = {
    'android/app/Activity.java': 'package android.app; public class Activity extends android.content.Context {}',
    'android/content/Intent.java': 'package android.content; public class Intent {}',
    'android/view/View.java': 'package android.view; public class View {}',
    'android/content/Context.java': 'package android.content; public class Context {public static final int MODE_PRIVATE=0;public String getString(int r){return "warning";}final android.content.SharedPreferences p=new android.content.SharedPreferences();public android.content.SharedPreferences getSharedPreferences(String n,int mode){return p;}public java.io.File getFilesDir(){return new java.io.File(System.getProperty("java.io.tmpdir"),"prefs-files");}}',
    'android/os/Build.java': 'package android.os; public class Build { public static String[] SUPPORTED_64_BIT_ABIS = new String[]{"arm64-v8a"}; }',
    'android/content/SharedPreferences.java': 'package android.content; public class SharedPreferences {java.util.Map<String,Object> values=new java.util.HashMap<>();public boolean getBoolean(String k,boolean v){return (Boolean)values.getOrDefault(k,v);}public int getInt(String k,int v){return (Integer)values.getOrDefault(k,v);}public SharedPreferences edit(){return this;}public SharedPreferences putBoolean(String k,boolean v){values.put(k,v);return this;}public SharedPreferences putInt(String k,int v){values.put(k,v);return this;}public void apply(){}}',
    'android/os/Environment.java': 'package android.os; public class Environment {public static java.io.File getExternalStorageDirectory(){return new java.io.File(System.getProperty("java.io.tmpdir"));}}',
    'org/sims2/port/launcher/PortHooks.java': 'package org.sims2.port.launcher; public interface PortHooks {void migrate(android.content.Context c);default CharSequence optionWarning(android.content.Context c,String k,String v){return null;}default android.view.View optionEditor(android.content.Context c,String k){return null;}default boolean optionResult(android.app.Activity a,int q,int r,android.content.Intent i){return false;}default String[] startOffer(android.content.Context c){return null;}default boolean startOfferAnswer(android.content.Context c,boolean accept){return true;}default boolean offerEtc2(android.content.Context c,boolean probed,boolean etc2,boolean nativeBcUnsupported,boolean alreadyConverted,boolean converterPresent){return false;}default String[] startOfferEtc2(android.content.Context c){return null;}}',
    'org/sims2/port/ExtenderSettings.java': 'package org.sims2.port; final class ExtenderSettings {static android.view.View button(android.content.Context c){return new android.view.View();}static boolean importResult(android.app.Activity a,int q,int r,android.content.Intent i){return false;}}',
    'org/sims2/port/launcher/Launch.java': '''package org.sims2.port.launcher;
public class Launch {public static java.util.Map<String,String> choices=new java.util.HashMap<>();
public static String languageCode="en";
public static String language(android.content.Context c){return languageCode;}
public static void adopt(android.content.Context c,java.util.Map<String,String> values){for(String key:values.keySet())choices.putIfAbsent(key,values.get(key));}}''',
    'org/sims2/port/launcher/PortOptions.java': 'package org.sims2.port.launcher; public class PortOptions {public static int upscaler;public static void migrateUpscaler(android.content.Context c){upscaler++;}}',
    'org/sims2/port/TouchLayer.java': 'package org.sims2.port; class TouchLayer {static String old="";static String readText(java.io.File f){return old;}}',
    'org/sims2/port/PreferencesCheck.java': '''package org.sims2.port;
import java.util.Locale;import org.sims2.port.launcher.Launch;
public class PreferencesCheck {public static void main(String[] args){
 String[] tags={"en","fr","de","it","es","sv","fi","nl","da","pt-BR","cs","ja","ko","ru","zh-CN","zh-TW","pl","th","no","pt-PT","hu"};
 for(int i=0;i<tags.length;i++)assert GamePreferences.systemLanguage(Locale.forLanguageTag(tags[i])).equals(GamePreferences.CODES[i]):tags[i];
 int[] ids={1,2,3,4,5,6,7,8,9,10,11,14,15,16,17,18,20,21,22,23,25};
 for(int i=0;i<tags.length;i++){
  String code=GamePreferences.CODES[i];Locale resolved=GamePreferences.gameLocale(code,Locale.FRENCH);
  assert GamePreferences.systemLanguage(resolved).equals(code):code;
  assert GamePreferences.gameLocale(code.toUpperCase(Locale.ROOT),Locale.FRENCH).equals(resolved):code;
  assert GamePreferences.gameLocale(Integer.toString(ids[i]),Locale.FRENCH).equals(resolved):code;
  assert GamePreferences.gameLocale("system",Locale.forLanguageTag(tags[i])).equals(resolved):code;
 }
 for(String bad:new String[]{null,"","invalid","99"})assert GamePreferences.gameLocale(bad,Locale.FRENCH).equals(Locale.ENGLISH);
 for(String tag:new String[]{"zh-Hant","zh-HK","zh-MO"})assert GamePreferences.systemLanguage(Locale.forLanguageTag(tag)).equals("tchinese");
 for(String tag:new String[]{"nb-NO","nn-NO"})assert GamePreferences.systemLanguage(Locale.forLanguageTag(tag)).equals("norwegian");
 for(String tag:new String[]{"ar","uk","und",""})assert GamePreferences.systemLanguage(Locale.forLanguageTag(tag)).equals("english");
 GamePreferences prefs=new GamePreferences();android.content.Context c=new android.content.Context();
 assert GamePreferences.uiSwipeControls();GamePreferences.initControls(c);assert GamePreferences.uiSwipeControls();
 GamePreferences.uiSwipeControls(false);assert !GamePreferences.uiSwipeControls();GamePreferences.initControls(c);assert !GamePreferences.uiSwipeControls();GamePreferences.uiSwipeControls(true);assert GamePreferences.uiSwipeControls();
 assert !GamePreferences.showGameTools();
 GamePreferences.showGameTools(true);GamePreferences.initControls(c);assert GamePreferences.showGameTools();
 GamePreferences.showGameTools(false);GamePreferences.initControls(c);
 assert !GamePreferences.showGameTools();
 assert !GamePreferences.floorHintShown();GamePreferences.markFloorHintShown();
 assert GamePreferences.takeCheatMenuHint(true)&&!GamePreferences.takeCheatMenuHint(true);
 assert GamePreferences.takeCheatMenuHint(false)&&!GamePreferences.takeCheatMenuHint(false);
 GamePreferences.initControls(c);assert !GamePreferences.takeCheatMenuHint(true)&&!GamePreferences.takeCheatMenuHint(false);
 GamePreferences.initControls(c);assert GamePreferences.floorHintShown()&&!GamePreferences.timeHintShown();
 assert !GamePreferences.timeHintShown();GamePreferences.markTimeHintShown();assert GamePreferences.timeHintShown()&&GamePreferences.uiSwipeControls();GamePreferences.initControls(c);assert GamePreferences.timeHintShown();
 assert GamePreferences.targetRadius()==16&&GamePreferences.targetRadius()*3==GamePreferences.MAX_TARGET_RADIUS;
 assert !GamePreferences.targetRadiusHintShown();GamePreferences.markTargetRadiusHintShown();
 GamePreferences.initControls(c);assert GamePreferences.targetRadiusHintShown();
 GamePreferences.targetRadius(24);
 GamePreferences.initControls(c);assert GamePreferences.targetRadius()==24;
 GamePreferences.targetRadius(-1);assert GamePreferences.targetRadius()==4;
 GamePreferences.targetRadius(100);assert GamePreferences.targetRadius()==48;
 assert prefs.optionWarning(c,"adaptive_ui","0")==null&&prefs.optionWarning(c,"ui_fsr","1")==null;
 assert prefs.optionWarning(c,"adaptive_ui","1")==null;
 prefs.migrate(c);assert Launch.choices.isEmpty()&&org.sims2.port.launcher.PortOptions.upscaler==1;
 TouchLayer.old="russian\\n";prefs.migrate(c);assert Launch.choices.get("option.game_language").equals("russian");
 Launch.choices.put("option.game_language","system");prefs.migrate(c);assert Launch.choices.get("option.game_language").equals("system");
 Launch.choices.clear();TouchLayer.old="unrecognized";prefs.migrate(c);assert Launch.choices.isEmpty();
 c.getSharedPreferences("controls",0).edit().putBoolean("show_fps",true).apply();
 prefs.migrate(c);assert Launch.choices.get("option.fps_counter").equals("1");
 for(String mode:new String[]{"0","2"}){
  Launch.choices.put("option.fps_counter",mode);prefs.migrate(c);assert Launch.choices.get("option.fps_counter").equals(mode);
 }
 assert !GamePreferences.wantsMinimalGraphicsOffer(new String[]{"arm64-v8a"}, false);
 assert !GamePreferences.wantsMinimalGraphicsOffer(new String[]{"armeabi-v7a","arm64-v8a"}, false);
 assert !GamePreferences.wantsMinimalGraphicsOffer(new String[]{}, true);
 assert GamePreferences.wantsMinimalGraphicsOffer(null, false);
 assert GamePreferences.wantsMinimalGraphicsOffer(new String[]{"armeabi-v7a"}, false);
 assert GamePreferences.wantsMinimalGraphicsOffer(new String[]{"x86_64"}, false);
 assert GamePreferences.wantsEtc2Offer(true, true, true, true, false, true);
 assert !GamePreferences.wantsEtc2Offer(false, true, true, true, false, true);
 assert !GamePreferences.wantsEtc2Offer(true, false, true, true, false, true);
 assert !GamePreferences.wantsEtc2Offer(true, true, false, true, false, true);
 assert !GamePreferences.wantsEtc2Offer(true, true, true, false, false, true);
 assert !GamePreferences.wantsEtc2Offer(true, true, true, true, true, true);
 assert !GamePreferences.wantsEtc2Offer(true, true, true, true, false, false);
 java.io.File dir=new java.io.File(System.getProperty("java.io.tmpdir"), "min-gfx-"+System.nanoTime());
 assert dir.mkdirs();
 java.io.File asked=new java.io.File(dir,"asked"), once=new java.io.File(dir,"once"), backup=new java.io.File(dir,"backup");
 assert GamePreferences.saveMinimalGraphicsAnswer(asked, once, false)&&asked.isFile()&&!once.isFile();
 asked.delete();
 assert GamePreferences.saveMinimalGraphicsAnswer(asked, once, true)&&asked.isFile()&&once.isFile();
 asked.delete();
 assert GamePreferences.saveMinimalGraphicsAnswer(asked, once, false)&&asked.isFile()&&!once.isFile();
 asked.delete(); once.delete();
 assert GamePreferences.saveMinimalGraphicsAnswer(asked, once, true)&&once.isFile();
 assert GamePreferences.armMinimalGraphics(once, backup);
 try { assert backup.createNewFile(); } catch (java.io.IOException e) { throw new AssertionError(e); }
 assert !GamePreferences.armMinimalGraphics(once, backup)&&!once.isFile();
 android.os.Build.SUPPORTED_64_BIT_ABIS=new String[0];
 new java.io.File(c.getFilesDir(),"launcher/"+GamePreferences.MINIMAL_GRAPHICS_ASKED).delete();
 new java.io.File(c.getFilesDir(),"launcher/"+GamePreferences.MINIMAL_GRAPHICS_ONCE).delete();
 Launch.languageCode="ru";
 String[] offer=prefs.startOffer(c);
 assert offer!=null&&offer.length==4&&offer[0].equals("Минимальная графика")&&offer[2].equals("Включить")&&offer[3].equals("Не сейчас");
 String[] etc=prefs.startOfferEtc2(c);
 assert etc!=null&&etc.length==5&&etc[0].equals("32-битное устройство")&&etc[2].equals("Продолжить")&&etc[3].equals("Минимальная графика")&&etc[4].equals("Конвертировать текстуры в ETC2");
 assert prefs.offerEtc2(c, true, true, true, false, true)&&!prefs.offerEtc2(c, true, true, false, false, true);
 Launch.languageCode="de"; offer=prefs.startOffer(c); assert offer[0].equals("Minimale Grafik")&&offer[2].equals("Aktivieren");
 etc=prefs.startOfferEtc2(c); assert etc[0].equals("32-Bit-Gerät")&&etc[2].equals("Weiter")&&etc[4].equals("Texturen nach ETC2 umwandeln");
 Launch.languageCode="pl"; offer=prefs.startOffer(c); assert offer[0].equals("Minimalna grafika")&&offer[2].equals("Włącz");
 etc=prefs.startOfferEtc2(c); assert etc[0].equals("Urządzenie 32-bitowe")&&etc[2].equals("Kontynuuj")&&etc[4].equals("Konwertuj tekstury do ETC2");
 Launch.languageCode="en"; offer=prefs.startOffer(c); assert offer[0].equals("Minimal graphics")&&offer[2].equals("Turn on")&&offer[3].equals("Not now");
 etc=prefs.startOfferEtc2(c); assert etc[0].equals("32-bit device")&&etc[2].equals("Continue")&&etc[4].equals("Convert textures to ETC2")&&etc[1].contains("ETC2");
 assert prefs.startOfferAnswer(c, true);
 assert new java.io.File(c.getFilesDir(),"launcher/"+GamePreferences.MINIMAL_GRAPHICS_ONCE).isFile();
 assert new java.io.File(c.getFilesDir(),"launcher/"+GamePreferences.MINIMAL_GRAPHICS_ASKED).isFile();
 assert prefs.startOffer(c)==null&&prefs.startOfferEtc2(c)==null;
 android.os.Build.SUPPORTED_64_BIT_ABIS=new String[]{"arm64-v8a"};
 assert prefs.startOffer(c)==null&&prefs.startOfferEtc2(c)==null;
 assert !prefs.offerEtc2(c, true, true, true, false, true);
 Launch.languageCode="en";
 System.out.println("PASS system locales and migration of language and FPS preferences without overwriting launcher choices");
}}''',
}




def main():
    spec = json.loads((ROOT / 'android/port.json').read_text(encoding='utf-8'))
    options = {o['key']: o for o in spec['options']}
    assert options['adaptive_ui']['default'] == '1'
    assert options['adaptive_ui']['type'] == 'bool'
    assert 'choices' not in options['adaptive_ui']
    assert options['game_language']['default'] == 'system'
    assert len({c['value'] for c in options['game_language']['choices']}) == 22
    assert 'three_finger_settings' not in options
    activity=(ROOT/'android/app/src/org/sims2/port/GameActivity.java').read_text(encoding='utf-8')
    assert 'GamePreferences.gameLocale(selectedLanguage,java.util.Locale.getDefault())' in activity
    assert 'GamePreferences.armMinimalGraphics' in activity and 'PORT_OPTION_MINIMAL_GRAPHICS' in activity
    assert 'userdata' in activity[activity.index('armMinimalGraphics')-400:activity.index('armMinimalGraphics')]
    launcher = (ROOT / 'android/launcher/android/src/port/launcher/LauncherActivity.java').read_text(encoding='utf-8')
    assert 'offerBeforeStart' in launcher and 'startOfferAnswer' in launcher
    assert 'playAfterTextures' in launcher and 'offerEtc2' in launcher and 'startOfferEtc2' in launcher
    shared = (ROOT.parent / 'launcher/android/src/port/launcher/LauncherActivity.java').read_text(encoding='utf-8')
    assert 'offerBeforeStart' in shared and 'playAfterTextures' in shared and 'rememberTextureOffer' in shared
    shared_hooks = (ROOT.parent / 'launcher/android/src/port/launcher/PortHooks.java').read_text(encoding='utf-8')
    assert 'startOfferEtc2' in shared_hooks and 'offerEtc2' in shared_hooks
    probe = (ROOT.parent / 'launcher/tools/launcher.py').read_text(encoding='utf-8')
    assert 'abi="arm64"' in probe and 'armv7a-linux-androideabi' in probe
    assert 'launch.language()' not in activity
    assert activity.index('System.getenv("S2_STEAM_LANGUAGE")') < activity.index('System.getenv("S2_LANGUAGE")') < activity.index('launch.option("game_language")')
    assert spec['app']['hooks'] == 'org.sims2.port.GamePreferences'
    resources=ROOT/'android/app/res'
    folders=('values','values-fr','values-de','values-it','values-es','values-sv','values-fi',
             'values-nl','values-da','values-pt-rBR','values-cs','values-ja','values-ko','values-ru',
             'values-b+zh+Hans','values-b+zh+Hant','values-pl','values-th','values-nb','values-pt','values-hu')
    translations=[]
    for folder in folders:
        nodes=list(ET.parse(resources/folder/'strings.xml').getroot())
        rows={node.attrib['name']:node.text for node in nodes}
        assert len(rows)==len(nodes) and len(rows)>=40, folder
        assert rows.get('floor_swipe_hint'), folder
        assert all(rows.values()), folder
        translations.append(rows)
    english=translations[0]
    for folder,rows in zip(folders,translations):
        assert rows.keys() <= english.keys(), folder
        for key,value in rows.items():
            assert sorted(re.findall(r'%[12]\$[ds]|%%',value))==sorted(re.findall(r'%[12]\$[ds]|%%',english[key])), (folder,key)
            assert not re.search(r"(?<!\\)'",value), (folder,key,'unescaped Android apostrophe')
        if folder!='values':
            assert rows['gestures_help']!=english['gestures_help'] and rows['gamepad_help']!=english['gamepad_help'], folder
    print('PASS: base messages in all 21 game languages, with Android fallback for newer labels; Android quoting and format arguments; game locale independent of launcher locale')
    with tempfile.TemporaryDirectory(prefix='sims2-preferences-') as folder:
        out = Path(folder)
        sources = []
        for name, source in STUBS.items():
            path = out / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding='utf-8')
            sources.append(str(path))
        sources.append(str(ROOT / 'android/app/src/org/sims2/port/GamePreferences.java'))
        subprocess.run([str(JDK / 'bin/javac.exe'), '--release', '8', '-encoding', 'UTF-8', '-d', str(out), *sources], check=True)
        subprocess.run([str(JDK / 'bin/java.exe'), '-ea', '-cp', str(out), 'org.sims2.port.PreferencesCheck'], check=True)


if __name__ == '__main__':
    main()
