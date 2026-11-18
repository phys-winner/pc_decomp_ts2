package org.sims2.port;

import android.content.Context;
import android.os.Environment;
import org.sims2.port.launcher.Launch;
import org.sims2.port.launcher.PortHooks;
import java.io.File;
import java.util.Collections;
import java.util.Locale;

/** Launcher choices stay in the launcher; old language selections migrate once. */
public final class GamePreferences implements PortHooks {
    private static android.content.SharedPreferences controls;
    static void initControls(Context context){controls=context.getSharedPreferences("controls",Context.MODE_PRIVATE);}
    static boolean uiSwipeControls(){return controls==null||controls.getBoolean("ui_swipe_controls",true);}
    static void uiSwipeControls(boolean enabled){if(controls!=null)controls.edit().putBoolean("ui_swipe_controls",enabled).apply();}
    static boolean timeHintShown(){return controls==null||controls.getBoolean("time_swipe_hint_shown",false);}
    static void markTimeHintShown(){if(controls!=null)controls.edit().putBoolean("time_swipe_hint_shown",true).apply();}
    static final int MIN_TARGET_RADIUS=4, MAX_TARGET_RADIUS=48;
    private static final int DEFAULT_TARGET_RADIUS=16;
    static int targetRadius(){
        int radius=controls==null?DEFAULT_TARGET_RADIUS:controls.getInt("world_target_radius",DEFAULT_TARGET_RADIUS);
        return Math.max(MIN_TARGET_RADIUS,Math.min(MAX_TARGET_RADIUS,radius));
    }
    static void targetRadius(int radius){
        if(controls!=null)controls.edit().putInt("world_target_radius",radius).apply();
    }
    static boolean targetRadiusHintShown(){return controls==null||controls.getBoolean("world_target_radius_hint_shown",false);}
    static void markTargetRadiusHintShown(){if(controls!=null)controls.edit().putBoolean("world_target_radius_hint_shown",true).apply();}
    static final String[] CODES={"english","french","german","italian","spanish","swedish","finnish","dutch","danish","brazilian","czech","japanese","koreana","russian","schinese","tchinese","polish","thai","norwegian","portuguese","hungarian"};
    static Locale gameLocale(String selection,Locale system){
        if("system".equals(selection))selection=systemLanguage(system);
        String[] tags={"en","fr","de","it","es","sv","fi","nl","da","pt-BR","cs","ja","ko","ru","zh-Hans","zh-Hant","pl","th","nb","pt-PT","hu"};
        int[] ids={1,2,3,4,5,6,7,8,9,10,11,14,15,16,17,18,20,21,22,23,25};
        for(int i=0;i<CODES.length;i++)if(CODES[i].equalsIgnoreCase(selection)||Integer.toString(ids[i]).equals(selection))return Locale.forLanguageTag(tags[i]);
        return Locale.ENGLISH;
    }
    static String systemLanguage(Locale locale){
        String[] languages={"en","fr","de","it","es","sv","fi","nl","da","pt","cs","ja","ko","ru","zh","zh","pl","th","no","pt","hu"};
        if("pt".equals(locale.getLanguage()))return "BR".equals(locale.getCountry())?"brazilian":"portuguese";
        if("zh".equals(locale.getLanguage()))return "Hant".equals(locale.getScript())||"TW".equals(locale.getCountry())||"HK".equals(locale.getCountry())||"MO".equals(locale.getCountry())?"tchinese":"schinese";
        if("nb".equals(locale.getLanguage())||"nn".equals(locale.getLanguage()))return "norwegian";
        for(int i=0;i<languages.length;i++)if(languages[i].equals(locale.getLanguage()))return CODES[i];
        return "english";
    }
    @Override public void migrate(Context context){
        String old=TouchLayer.readText(new File(Environment.getExternalStorageDirectory(),"Sims2/userdata/language.txt")).trim();
        for(String code:CODES)if(code.equals(old)){
            Launch.adopt(context,Collections.singletonMap("option.game_language",code));break;
        }
    }
}
