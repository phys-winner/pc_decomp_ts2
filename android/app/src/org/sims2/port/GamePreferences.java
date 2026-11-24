package org.sims2.port;

import android.content.Context;
import android.os.Environment;
import org.sims2.port.launcher.Launch;
import org.sims2.port.launcher.PortHooks;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Collections;
import java.util.Locale;

/** Launcher choices stay in the launcher; old language selections migrate once. */
public final class GamePreferences implements PortHooks {
    private static android.content.SharedPreferences controls;
    static void initControls(Context context){controls=context.getSharedPreferences("controls",Context.MODE_PRIVATE);}
    static boolean uiSwipeControls(){return controls==null||controls.getBoolean("ui_swipe_controls",true);}
    static void uiSwipeControls(boolean enabled){if(controls!=null)controls.edit().putBoolean("ui_swipe_controls",enabled).apply();}
    static boolean showGameTools(){return controls!=null&&controls.getBoolean("show_game_tools",false);}
    static void showGameTools(boolean enabled){if(controls!=null)controls.edit().putBoolean("show_game_tools",enabled).apply();}
    static boolean floorHintShown(){return controls==null||controls.getBoolean("floor_swipe_hint_shown",false);}
    static void markFloorHintShown(){if(controls!=null)controls.edit().putBoolean("floor_swipe_hint_shown",true).apply();}
    static boolean takeCheatMenuHint(boolean qa){
        String key=qa?"cheat_menu_hint_shown":"cheat_menu_hold_hint_shown";
        if(controls==null||controls.getBoolean(key,false))return false;
        controls.edit().putBoolean(key,true).apply();
        return true;
    }
    static boolean timeHintShown(){return controls==null||controls.getBoolean("time_swipe_hint_shown",false);}
    static void markTimeHintShown(){if(controls!=null)controls.edit().putBoolean("time_swipe_hint_shown",true).apply();}
    static boolean modsHintShown(){return controls==null||controls.getBoolean("mods_layout_hint_shown",false);}
    static void markModsHintShown(){if(controls!=null)controls.edit().putBoolean("mods_layout_hint_shown",true).apply();}
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
    /** files/launcher flag: the first-start question has been answered. */
    static final String MINIMAL_GRAPHICS_ASKED = "minimal-graphics.asked";
    /** files/launcher flag: turn the preset on once, then the game deletes it. */
    static final String MINIMAL_GRAPHICS_ONCE = "minimal-graphics.once";
    private static final String[] OFFER_TITLE = {
        "Minimal graphics", "Минимальная графика", "Minimale Grafik", "Minimalna grafika"};
    private static final String[] OFFER_BODY = {
        "This device cannot run the 64-bit game library. Minimal graphics uses less memory and runs better on a weak device. You can change this later in the game's graphics options.",
        "Это устройство не поддерживает 64-битную библиотеку игры. Минимальная графика снижает потребление памяти и повышает производительность на слабом устройстве. Позже это можно изменить в графических настройках игры.",
        "Dieses Gerät kann die 64-Bit-Bibliothek des Spiels nicht ausführen. Minimale Grafik braucht weniger Speicher und läuft auf einem schwachen Gerät besser. Später lässt sich das in den Grafikeinstellungen des Spiels ändern.",
        "To urządzenie nie obsługuje 64-bitowej biblioteki gry. Minimalna grafika zużywa mniej pamięci i działa lepiej na słabym urządzeniu. Później można to zmienić w ustawieniach grafiki gry."};
    private static final String[] OFFER_ACCEPT = {"Turn on", "Включить", "Aktivieren", "Włącz"};
    private static final String[] OFFER_DECLINE = {"Not now", "Не сейчас", "Nicht jetzt", "Nie teraz"};
    private static final String[] ETC2_TITLE = {
        "32-bit device", "32-битное устройство", "32-Bit-Gerät", "Urządzenie 32-bitowe"};
    private static final String[] ETC2_BODY = {
        "This device cannot run the 64-bit game library. Minimal graphics uses less memory and runs better on a weak device. Converting textures to ETC2 also uses less memory when the graphics chip supports it. You can change graphics later in the game, and convert textures from this screen.",
        "Это устройство не поддерживает 64-битную библиотеку игры. Минимальная графика снижает потребление памяти и повышает производительность на слабом устройстве. Конвертация текстур в ETC2 тоже снижает потребление памяти, если видеочип её поддерживает. Графику позже можно изменить в игре, а текстуры — на этом экране.",
        "Dieses Gerät kann die 64-Bit-Bibliothek des Spiels nicht ausführen. Minimale Grafik braucht weniger Speicher und läuft auf einem schwachen Gerät besser. Texturen in ETC2 umzuwandeln spart ebenfalls Speicher, wenn der Grafikchip das Format unterstützt. Die Grafik lässt sich später im Spiel ändern, die Texturen auf diesem Bildschirm.",
        "To urządzenie nie obsługuje 64-bitowej biblioteki gry. Minimalna grafika zużywa mniej pamięci i działa lepiej na słabym urządzeniu. Konwersja tekstur do ETC2 też zużywa mniej pamięci, jeśli układ graficzny obsługuje ten format. Grafikę można później zmienić w grze, a tekstury na tym ekranie."};
    private static final String[] ETC2_CONTINUE = {"Continue", "Продолжить", "Weiter", "Kontynuuj"};
    private static final String[] ETC2_TEXTURES = {
        "Convert textures to ETC2", "Конвертировать текстуры в ETC2", "Texturen nach ETC2 umwandeln", "Konwertuj tekstury do ETC2"};

    /** True when the device has no arm64-v8a and the question has not been answered. */
    static boolean wantsMinimalGraphicsOffer(String[] abis, boolean asked) {
        if (asked) return false;
        if (abis != null) for (String abi : abis) if ("arm64-v8a".equals(abi)) return false;
        return true;
    }

    /** The graphics question may also offer ETC2. nativeBcUnsupported is true only when DXT/BC
     *  is known to be missing. A missing scan is not alreadyConverted. */
    static boolean wantsEtc2Offer(boolean graphicsOffer, boolean probed, boolean etc2,
            boolean nativeBcUnsupported, boolean alreadyConverted, boolean converterPresent) {
        return graphicsOffer && probed && etc2 && nativeBcUnsupported && !alreadyConverted && converterPresent;
    }

    static File launcherFlag(Context context, String name) {
        File dir = new File(context.getFilesDir(), "launcher");
        if (!dir.isDirectory()) dir.mkdirs();
        return new File(dir, name);
    }

    /** The profile already has the preset's snapshot: drop the one-shot and do not ask native again. */
    static boolean armMinimalGraphics(File once, File backup) {
        if (backup.isFile()) {
            if (once.isFile()) once.delete();
            return false;
        }
        return once.isFile();
    }

    static boolean saveMinimalGraphicsAnswer(File asked, File once, boolean enable) {
        if (enable) {
            if (!once.isFile() && !writeFlag(once)) return false;
        } else if (once.isFile() && !once.delete()) {
            return false;
        }
        return asked.isFile() || writeFlag(asked);
    }

    private static boolean writeFlag(File file) {
        if (file.isFile()) return true;
        File dir = file.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) return false;
        File part = new File(file.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(part)) {
            out.write('1');
            out.getFD().sync();
        } catch (IOException e) {
            part.delete();
            return false;
        }
        if (!part.renameTo(file)) {
            part.delete();
            return file.isFile();
        }
        return true;
    }

    private static int offerColumn(String language) {
        if ("ru".equals(language)) return 1;
        if ("de".equals(language)) return 2;
        if ("pl".equals(language)) return 3;
        return 0;
    }

    @Override public String[] startOffer(Context context) {
        if (!wantsMinimalGraphicsOffer(android.os.Build.SUPPORTED_64_BIT_ABIS,
                launcherFlag(context, MINIMAL_GRAPHICS_ASKED).isFile())) return null;
        int column = offerColumn(Launch.language(context));
        return new String[] { OFFER_TITLE[column], OFFER_BODY[column], OFFER_ACCEPT[column], OFFER_DECLINE[column] };
    }

    @Override public boolean startOfferAnswer(Context context, boolean accept) {
        return saveMinimalGraphicsAnswer(launcherFlag(context, MINIMAL_GRAPHICS_ASKED),
                launcherFlag(context, MINIMAL_GRAPHICS_ONCE), accept);
    }

    @Override public boolean offerEtc2(Context context, boolean probed, boolean etc2, boolean nativeBcUnsupported,
            boolean alreadyConverted, boolean converterPresent) {
        return wantsEtc2Offer(wantsMinimalGraphicsOffer(android.os.Build.SUPPORTED_64_BIT_ABIS,
                launcherFlag(context, MINIMAL_GRAPHICS_ASKED).isFile()),
            probed, etc2, nativeBcUnsupported, alreadyConverted, converterPresent);
    }

    @Override public String[] startOfferEtc2(Context context) {
        if (!wantsMinimalGraphicsOffer(android.os.Build.SUPPORTED_64_BIT_ABIS,
                launcherFlag(context, MINIMAL_GRAPHICS_ASKED).isFile())) return null;
        int column = offerColumn(Launch.language(context));
        return new String[] { ETC2_TITLE[column], ETC2_BODY[column], ETC2_CONTINUE[column],
            OFFER_TITLE[column], ETC2_TEXTURES[column] };
    }

    @Override public void migrate(Context context){
        org.sims2.port.launcher.PortOptions.migrateUpscaler(context);
        if(context.getSharedPreferences("controls",Context.MODE_PRIVATE).getBoolean("show_fps",false))
            Launch.adopt(context,Collections.singletonMap("option.fps_counter","1"));
        String old=TouchLayer.readText(new File(Environment.getExternalStorageDirectory(),"Sims2/userdata/language.txt")).trim();
        for(String code:CODES)if(code.equals(old)){
            Launch.adopt(context,Collections.singletonMap("option.game_language",code));break;
        }
    }
    @Override public android.view.View optionEditor(Context context, String key){
        return "ts2_extender".equals(key) ? ExtenderSettings.button(context) : null;
    }
    @Override public boolean optionResult(android.app.Activity activity, int request, int result, android.content.Intent data){
        return ExtenderSettings.importResult(activity, request, result, data);
    }
}
