package org.sims2.port;

import android.app.Activity;
import org.sims2.port.launcher.Launch;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Environment;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.Button;
import android.content.Context;
import android.text.InputFilter;
import android.text.InputType;
import android.view.inputmethod.InputMethodManager;
import android.view.inputmethod.EditorInfo;
import java.io.File;
import java.nio.charset.StandardCharsets;

/** Original game UI fills the surface. Android dialogs edit only settings and native text. */
public final class GameActivity extends Activity implements SurfaceHolder.Callback {
    private static native void nativeStart(String game,String data,String images,String args,boolean verbose);
    private static native void nativeSurface(Object surface);
    static native void nativeKey(int vk,boolean down);
    static native void nativeMouseButton(int button,boolean down);
    static native void nativeMouseMove(float x,float y,boolean relative);
    static native void nativeMouseWarp(float x,float y);
    static native void nativeMouseWheel(int delta);
    static native int nativeTouchContext();
    static native boolean nativeTouchRewards();
    static native int nativeTouchHitKind(float x,float y);
    static native float[] nativeTouchUiPick(float x,float y,float diameter);
    static native long nativeTouchSwipeFeedback();
    static native long nativeTouchTarget(float x,float y,float radius);
    static native long nativeTouchToolToken();
    static native long nativeTouchCursor();
    static native int nativeTouchObjectCost();
    static native boolean nativeTouchObjectPreview(long token);
    private static native int nativeUpscaleStatus();
    private static native long nativeCheatsRequest(String command);
    private static native long nativeCheatsStatus(int serial);
    private static native String[] nativeCheatsRows(int serial);
    private static native void nativeCheatsCancel();
    static native long nativeTouchGameSize();
    static native long nativeTouchLayoutEpoch();
    static native long nativeTouchRotateObject(float x,float y,long tool,int context,long gameSize,long layoutEpoch,int direction);
    static native long nativeTouchRotateObjectStatus(int serial);
    static native int nativeTouchFillKind();
    static native long nativeTouchFillRoom(float x,float y,long tool,int context,long gameSize,long layoutEpoch);
    static native long nativeTouchFillRoomStatus(int serial);
    static native int nativeTouchUiScale();
    static native boolean nativeTouchSetUiScale(int percent);
    static native boolean nativeTouchPanCas(float normalizedDy);
    static native boolean nativeTouchCamera(float normalizedDx,float normalizedDy,float radians,float zoom,float pitch);
    static native boolean nativeTouchPan(float fromX,float fromY,float toX,float toY);
    static native void nativeTouchCameraContact(boolean active);
    static native boolean nativeTouchScroll(float x,float y,int amount,int context,long gameSize,long layoutEpoch);
    static native long nativeTouchScrollOwner();
    static native void nativeTouchStopFling();
    static native boolean nativeTouchFling(float x,float y,int amount,int context,long gameSize,long layoutEpoch,long owner);
    static native boolean nativeTouchCatalogInfo(float x,float y,int context,long gameSize,long layoutEpoch);
    static native boolean nativeTouchClearCatalog();
    static native boolean nativeTouchToolMenu(int action,int context,long gameSize,long layoutEpoch);
    static native byte[] nativeTouchReadText(float x,float y);
    static native long nativeTouchReplaceText(byte[] utf8);
    static native long nativeTouchReplaceTextStatus(int serial);
    static native void nativeTouchCancelText();
    private static native void nativePause(boolean paused);
    private File base;
    private Launch launch;
    private Context strings;
    private FrameLayout root;
    private boolean firstFrame;
    private static native long nativeHudState();
    private static native void nativeRenderer(int mode,String libraries,String cache);
    private boolean started;
    private TouchLayer touch;
    private GamepadLayer gamepad;
    private float preferredRefreshRate;
    private static GameActivity active;
    private AlertDialog settingsDialog,textDialog,cheatDialog;
    private int cheatSerial;
    private boolean cheatCatalogRequest;
    private int cheatMenuContext;
    private long cheatMenuSize,cheatMenuEpoch;
    private java.util.List<CheatCommands.Command> cheats;
    private LinearLayout loadingView;
    private TextView objectCost;
    private LinearLayout objectControls;
    private Button rotateButton,placeButton,cancelButton,helpButton,toolsButton;
    private FrameLayout toolMenu;
    private int toolMenuContext;
    private long toolMenuSize,toolMenuEpoch;
    private int safeLeft,safeTop,safeRight,safeBottom;
    private EditText textEditor;
    private TextView scaleStatus;
    private int textSerial,desiredScale,lastSubmittedScale;
    private boolean scaleRequested;
    private int upscaleStatus;
    private String textDraft="";
    private final android.os.Handler controlHandler=new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable updateControls=new Runnable(){public void run(){
        if(active!=GameActivity.this||!started||touch==null)return;
        if(!firstFrame&&(nativeHudState()&0x80000000L)!=0){firstFrame=true;root.removeView(loadingView);loadingView=null;launch.firstFrame(root);}
        touch.syncContext(nativeTouchContext());touch.syncToolToken(nativeTouchToolToken());
        touch.pollCommands();
        if(toolMenu!=null&&!toolMenuValid())closeToolMenu();
        refreshObjectControls();
        showTimeHint();
        int status=nativeUpscaleStatus();if(status!=0&&status!=upscaleStatus){upscaleStatus=status;touchNotice(text(R.string.upscale_fallback,status));}
        if(textSerial!=0)handleTextReceipt(nativeTouchReplaceTextStatus(textSerial));
        if(cheatSerial!=0)handleCheatReceipt(nativeCheatsStatus(cheatSerial));
        if(settingsDialog!=null&&scaleStatus!=null)scaleStatus.setText(text(R.string.scale_status,desiredScale,nativeTouchUiScale()));
        controlHandler.postDelayed(this,250);
    }};
    static String text(int resource,Object...args){return active==null?"":(active.strings==null?active:active.strings).getString(resource,args);}
    static void touchNotice(String text){if(active!=null)Toast.makeText(active,text,Toast.LENGTH_LONG).show();}
    static void touchSettingsRequested(){if(active!=null)active.showTouchSettings();}
    static void touchTextRequested(float x,float y){if(active!=null)active.showTextEditor(x,y);}
    static void touchObjectControlsChanged(){if(active!=null)active.refreshObjectControls();}
    static void touchCheatsRequested(){if(active!=null)active.showCheats();}

    private boolean cheatUi(){return cheatDialog!=null||cheatSerial!=0||toolMenu!=null;}
    private boolean cheatRussian(){return "ru".equals(strings.getResources().getConfiguration().getLocales().get(0).getLanguage());}
    private String cheatText(String en,String ru){return cheatRussian()?ru:en;}
    private boolean cheatMenuValid(){return active==this&&started&&nativeTouchContext()==cheatMenuContext&&
            nativeTouchGameSize()==cheatMenuSize&&nativeTouchLayoutEpoch()==cheatMenuEpoch;}
    private void showCheats(){
        if(active!=this||!started||settingsDialog!=null||textDialog!=null||cheatUi()||textSerial!=0)return;
        cheatMenuContext=nativeTouchContext();cheatMenuSize=nativeTouchGameSize();cheatMenuEpoch=nativeTouchLayoutEpoch();
        touch.setBlocked(true);if(gamepad!=null)gamepad.setEnabledInput(false);
        cheatCatalogRequest=true;handleCheatReceipt(nativeCheatsRequest(null));
    }
    private void showCheatDialog(AlertDialog.Builder builder){
        if(!cheatMenuValid()){
            if(cheatDialog!=null)cheatDialog.dismiss();
            else if(active==this){touch.setBlocked(!hasWindowFocus()||cheatSerial!=0);if(gamepad!=null)gamepad.setEnabledInput(hasWindowFocus()&&cheatSerial==0);}
            return;
        }
        AlertDialog previous=cheatDialog;
        AlertDialog next=builder.create();cheatDialog=next;
        if(previous!=null)previous.dismiss();
        touch.setBlocked(true);if(gamepad!=null)gamepad.setEnabledInput(false);refreshObjectControls();
        next.setOnDismissListener(d->{
            if(cheatDialog!=next)return;
            cheatDialog=null;
            if(active==this){
                if(cheatSerial!=0)nativeCheatsCancel();
                touch.setBlocked(!hasWindowFocus()||cheatSerial!=0);hideSystemUi();
                if(gamepad!=null)gamepad.setEnabledInput(hasWindowFocus()&&cheatSerial==0);
            }
        });
        next.show();next.getWindow().getDecorView().setForceDarkAllowed(false);
    }
    private AlertDialog.Builder cheatBuilder(String title){
        return new AlertDialog.Builder(this,R.style.PortDialog).setTitle(title).setNegativeButton(text(R.string.close),null);
    }
    private void showCheatGroups(){
        String[] groups=CheatCommands.groups(cheatRussian()),labels=groups.clone();
        int[] counts=new int[groups.length];for(CheatCommands.Command command:cheats)counts[command.group()]++;
        for(int i=0;i<labels.length;i++)labels[i]+=" ("+counts[i]+")";
        showCheatDialog(cheatBuilder(cheatText("Game cheats","Читы игры"))
                .setItems(labels,(d,which)->showCheatGroup(which))
                .setNeutralButton(cheatText("Console","Консоль"),(d,which)->showCheatConsole("","")));
    }
    private void showCheatGroup(int group){
        java.util.List<CheatCommands.Command> items=new java.util.ArrayList<>();
        for(CheatCommands.Command command:cheats)if(command.group()==group)items.add(command);
        String[] labels=new String[items.size()];for(int i=0;i<labels.length;i++)labels[i]=items.get(i).title(cheatRussian());
        showCheatDialog(cheatBuilder(CheatCommands.groups(cheatRussian())[group]).setItems(labels,(d,which)->{
            CheatCommands.Command command=items.get(which);showCheatConsole(command.name,command.help(cheatRussian()));
        }).setNeutralButton(cheatText("All groups","Все группы"),(d,which)->showCheatGroups()));
    }
    private void showCheatConsole(String initial,String originalHelp){
        AlertDialog.Builder builder=cheatBuilder(cheatText("Game console","Консоль игры"));
        LinearLayout content=new LinearLayout(builder.getContext());content.setOrientation(LinearLayout.VERTICAL);
        int padding=Math.round(16*getResources().getDisplayMetrics().density);content.setPadding(padding,padding,padding,padding);
        EditText entry=new EditText(builder.getContext());entry.setTextColor(Color.BLACK);entry.setSingleLine(true);
        entry.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        entry.setImeOptions(EditorInfo.IME_ACTION_DONE|EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        entry.setFilters(new InputFilter[]{new InputFilter.LengthFilter(128)});entry.setText(initial);entry.setSelection(entry.length());content.addView(entry);
        TextView help=new TextView(builder.getContext());help.setTextColor(Color.BLACK);
        help.setText(originalHelp.isEmpty()?cheatText("Enter a game command. Select a cheat in the list to read its original syntax.","Введите команду игры. Выберите чит в списке, чтобы прочитать его исходный синтаксис.")
                :originalHelp);content.addView(help);
        ScrollView scroll=new ScrollView(builder.getContext());scroll.addView(content);
        builder.setView(scroll).setPositiveButton(cheatText("Execute","Выполнить"),null)
                .setNeutralButton(cheatText("All groups","Все группы"),(d,which)->showCheatGroups());
        showCheatDialog(builder);
        final AlertDialog dialog=cheatDialog;if(dialog==null)return;
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->{
            if(cheatSerial!=0||!cheatMenuValid())return;
            String command=entry.getText().toString().trim();
            if(command.isEmpty()||command.indexOf('\0')>=0||command.indexOf('\n')>=0||command.indexOf('\r')>=0){entry.setError(text(R.string.invalid_text));return;}
            cheatCatalogRequest=false;handleCheatReceipt(nativeCheatsRequest(command));
        });
        entry.setOnEditorActionListener((v,action,event)->{if(action!=EditorInfo.IME_ACTION_DONE)return false;
            if(cheatDialog==dialog)dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();return true;});
    }
    private void handleCheatReceipt(long receipt){
        int status=(int)receipt&255,serial=(int)(receipt>>>32);
        if(cheatSerial!=0&&serial!=cheatSerial)return;
        if(status==1){cheatSerial=serial;if(cheatDialog!=null)cheatDialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);return;}
        cheatSerial=0;
        if(status==2&&cheatCatalogRequest&&cheatMenuValid()){
            cheats=CheatCommands.parse(nativeCheatsRows(serial));showCheatGroups();return;
        }
        String message=status==2?cheatText("Command sent to the game","Команда передана игре")
                :status==5?cheatText("Command cancelled before execution","Команда отменена до выполнения")
                :status==6?text(R.string.wait_command):cheatText("The game could not accept the command. Check its syntax and the current game mode.","Игра не приняла команду. Проверьте синтаксис и текущий режим игры.");
        touchNotice(message);
        if(cheatDialog!=null){
            Button execute=cheatDialog.getButton(AlertDialog.BUTTON_POSITIVE);if(execute!=null)execute.setEnabled(true);
            if(!cheatMenuValid())cheatDialog.dismiss();
        }else if(active==this){touch.setBlocked(!hasWindowFocus());if(gamepad!=null)gamepad.setEnabledInput(hasWindowFocus());}
    }

    private Button objectButton(String label,String icon){
        Button button=new Button(this);button.setText(icon);button.setAllCaps(false);
        button.setContentDescription(text(R.string.selected_object,label));button.setTextSize(26);
        button.setTextColor(Color.rgb(35,60,112));button.setMinWidth(0);button.setMinimumWidth(0);
        button.setMinHeight(0);button.setMinimumHeight(0);
        float density=getResources().getDisplayMetrics().density;
        button.setPadding(Math.round(12*density),0,Math.round(12*density),0);
        android.graphics.drawable.GradientDrawable background=new android.graphics.drawable.GradientDrawable();
        background.setColor(Color.rgb(200,211,239));background.setCornerRadius(22*density);
        background.setStroke(Math.max(1,Math.round(density)),Color.rgb(101,126,169));button.setBackground(background);
        return button;
    }
    private void createObjectControls(FrameLayout root){
        float density=getResources().getDisplayMetrics().density;
        objectControls=new LinearLayout(this);objectControls.setOrientation(LinearLayout.VERTICAL);objectControls.setVisibility(View.GONE);
        rotateButton=objectButton(text(R.string.rotate),"↻");placeButton=objectButton(text(R.string.place),"✓");cancelButton=objectButton(text(R.string.cancel),"×");
        objectCost=new TextView(this);objectCost.setTextColor(Color.WHITE);objectCost.setTextSize(16);objectCost.setGravity(android.view.Gravity.CENTER);
        objectControls.addView(objectCost);
        for(Button button:new Button[]{rotateButton,placeButton,cancelButton}){
            LinearLayout.LayoutParams layout=new LinearLayout.LayoutParams(Math.round(52*density),Math.round(48*density));
            layout.leftMargin=layout.rightMargin=Math.round(4*density);objectControls.addView(button,layout);
        }
        FrameLayout.LayoutParams layout=new FrameLayout.LayoutParams(-2,-2,android.view.Gravity.RIGHT|android.view.Gravity.CENTER_VERTICAL);
        layout.rightMargin=Math.round(8*density);root.addView(objectControls,layout);
        rotateButton.setOnClickListener(v->{if(active==this&&started)touch.rotateObjectPreview();refreshObjectControls();});
        placeButton.setOnClickListener(v->{if(active==this&&started)touch.placeObjectPreview();refreshObjectControls();});
        cancelButton.setOnClickListener(v->{if(active==this&&started)touch.cancelObjectPreview();refreshObjectControls();});
        helpButton=objectButton(text(R.string.interface_settings),"⚙");
        helpButton.setContentDescription(text(R.string.interface_settings));helpButton.setVisibility(View.GONE);
        FrameLayout.LayoutParams helpLayout=new FrameLayout.LayoutParams(Math.round(52*density),Math.round(48*density),android.view.Gravity.RIGHT|android.view.Gravity.TOP);
        helpLayout.rightMargin=helpLayout.topMargin=Math.round(8*density);root.addView(helpButton,helpLayout);
        helpButton.setOnClickListener(v->{if(active==this&&started)showTouchSettings();});
        toolsButton=objectButton(text(R.string.tools_menu),"…");
        toolsButton.setContentDescription(text(R.string.tools_menu));toolsButton.setVisibility(View.GONE);
        FrameLayout.LayoutParams toolsLayout=new FrameLayout.LayoutParams(Math.round(52*density),Math.round(48*density),android.view.Gravity.RIGHT|android.view.Gravity.BOTTOM);
        toolsLayout.rightMargin=toolsLayout.bottomMargin=Math.round(8*density);root.addView(toolsButton,toolsLayout);
        toolsButton.setOnClickListener(v->showToolMenu());
        toolsButton.setOnLongClickListener(v->{showToolMenu();return true;});
    }
    private boolean toolMenuValid(){
        return active==this&&started&&(toolMenuContext==TouchLayer.BUY||toolMenuContext==TouchLayer.BUILD)
                &&nativeTouchContext()==toolMenuContext&&nativeTouchGameSize()==toolMenuSize&&nativeTouchLayoutEpoch()==toolMenuEpoch;
    }
    private void closeToolMenu(){
        if(toolMenu==null)return;
        root.removeView(toolMenu);toolMenu=null;
        if(active==this){touch.setBlocked(!hasWindowFocus()||settingsDialog!=null||textDialog!=null||cheatDialog!=null||cheatSerial!=0);
            if(gamepad!=null)gamepad.setEnabledInput(hasWindowFocus()&&settingsDialog==null&&textDialog==null&&cheatDialog==null&&cheatSerial==0);}
        refreshObjectControls();
    }
    private void showToolMenu(){
        if(active!=this||!started||settingsDialog!=null||textDialog!=null||cheatUi()||textSerial!=0)return;
        toolMenuContext=nativeTouchContext();toolMenuSize=nativeTouchGameSize();toolMenuEpoch=nativeTouchLayoutEpoch();
        if(!toolMenuValid())return;
        touch.setBlocked(true);if(gamepad!=null)gamepad.setEnabledInput(false);
        toolMenu=new FrameLayout(this);toolMenu.setBackgroundColor(0x9907172E);
        toolMenu.setOnClickListener(v->closeToolMenu());
        int[] titles={R.string.tool_hand,R.string.tool_eyedropper,R.string.tool_design,R.string.tool_sledgehammer,
                R.string.tool_day_night,R.string.tool_45_degree,R.string.tool_top_down,R.string.tool_undo,R.string.tool_redo};
        int[] icons={R.drawable.tool_hand,R.drawable.tool_eyedropper,R.drawable.tool_design,R.drawable.tool_sledgehammer,R.drawable.tool_day_night,R.drawable.tool_45_degree,R.drawable.tool_top_down,R.drawable.tool_undo,R.drawable.tool_redo};
        float density=getResources().getDisplayMetrics().density;
        float radius=Math.min(root.getHeight()*.36f,180*density);
        for(int action=0;action<titles.length;action++){
            final int selected=action;
            Button button=objectButton(text(titles[action]),text(titles[action]));button.setTextSize(13);
            button.setPadding(Math.round(4*density),0,Math.round(4*density),0);
            button.setContentDescription(text(titles[action]));
            button.setCompoundDrawablesWithIntrinsicBounds(0,icons[action],0,0);
            FrameLayout.LayoutParams layout=new FrameLayout.LayoutParams(Math.round(Math.min(100*density,radius*.60f)),Math.round(72*density),android.view.Gravity.CENTER);
            button.setTranslationX((float)Math.sin(action*2*Math.PI/titles.length)*radius);
            button.setTranslationY(-(float)Math.cos(action*2*Math.PI/titles.length)*radius);
            toolMenu.addView(button,layout);
            button.setOnClickListener(v->{
                boolean valid=toolMenuValid();int mode=toolMenuContext;long size=toolMenuSize,epoch=toolMenuEpoch;
                closeToolMenu();
                if(valid&&!nativeTouchToolMenu(selected,mode,size,epoch))touchNotice(text(R.string.choose_tool));
            });
        }
        Button close=objectButton(text(R.string.close),"×");close.setContentDescription(text(R.string.close));
        toolMenu.addView(close,new FrameLayout.LayoutParams(Math.round(56*density),Math.round(56*density),android.view.Gravity.CENTER));
        close.setOnClickListener(v->closeToolMenu());
        root.addView(toolMenu,new FrameLayout.LayoutParams(-1,-1));refreshObjectControls();
    }
    private void refreshObjectControls(){
        if(objectControls==null||touch==null)return;
        boolean visible=active==this&&started&&settingsDialog==null&&textDialog==null&&!cheatUi()&&touch.objectControlsVisible();
        objectControls.setVisibility(visible?View.VISIBLE:View.GONE);
        boolean help=active==this&&started&&settingsDialog==null&&textDialog==null&&!cheatUi()&&
                (nativeTouchContext()&(TouchLayer.OPTIONS|TouchLayer.MODAL|TouchLayer.LOADING|TouchLayer.TEXT_FOCUS))==TouchLayer.OPTIONS;
        helpButton.setVisibility(help?View.VISIBLE:View.GONE);
        int mode=nativeTouchContext();
        boolean tools=active==this&&started&&settingsDialog==null&&textDialog==null&&!cheatUi()&&(mode==TouchLayer.BUY||mode==TouchLayer.BUILD);
        toolsButton.setVisibility(tools?View.VISIBLE:View.GONE);
        int cost=visible?nativeTouchObjectCost():-1;
        objectCost.setText(cost>=0?"§ "+cost:"");objectCost.setVisibility(cost>=0?View.VISIBLE:View.GONE);
        boolean ready=visible&&touch.canUseObjectPreview();
        rotateButton.setEnabled(ready);placeButton.setEnabled(ready);cancelButton.setEnabled(visible&&touch.canCancelObject());
        rotateButton.setAlpha(ready?1:.45f);placeButton.setAlpha(ready?1:.45f);
    }

    private void applySafeInsets(android.view.WindowInsets insets){
        android.graphics.Insets safe=android.os.Build.VERSION.SDK_INT>=30
                ?insets.getInsets(android.view.WindowInsets.Type.systemBars()|android.view.WindowInsets.Type.displayCutout())
                :insets.getSystemWindowInsets();
        int left=safe.left,top=safe.top,right=safe.right,bottom=safe.bottom;
        if(android.os.Build.VERSION.SDK_INT<30&&insets.getDisplayCutout()!=null){
            android.view.DisplayCutout cutout=insets.getDisplayCutout();
            left=Math.max(left,cutout.getSafeInsetLeft());top=Math.max(top,cutout.getSafeInsetTop());
            right=Math.max(right,cutout.getSafeInsetRight());bottom=Math.max(bottom,cutout.getSafeInsetBottom());
        }
        if(android.os.Build.VERSION.SDK_INT>=31){
            for(int corner=0;corner<4;++corner){
                android.view.RoundedCorner rounded=insets.getRoundedCorner(corner);
                if(rounded==null)continue;
                int inset=(int)Math.ceil(rounded.getRadius()*(1-Math.sqrt(.5)));
                if(corner==0||corner==3)left=Math.max(left,inset);else right=Math.max(right,inset);
                if(corner<2)top=Math.max(top,inset);else bottom=Math.max(bottom,inset);
            }
        }
        if(left==safeLeft&&top==safeTop&&right==safeRight&&bottom==safeBottom)return;
        safeLeft=left;safeTop=top;safeRight=right;safeBottom=bottom;
        if(touch!=null)touch.releaseAll();
        FrameLayout.LayoutParams layout=(FrameLayout.LayoutParams)objectControls.getLayoutParams();
        layout.leftMargin=left;layout.rightMargin=right+Math.round(8*getResources().getDisplayMetrics().density);
        layout.topMargin=top;layout.bottomMargin=bottom;objectControls.setLayoutParams(layout);
        FrameLayout.LayoutParams helpLayout=(FrameLayout.LayoutParams)helpButton.getLayoutParams();
        helpLayout.leftMargin=left;helpLayout.rightMargin=right+Math.round(8*getResources().getDisplayMetrics().density);
        helpLayout.topMargin=top+Math.round(8*getResources().getDisplayMetrics().density);helpLayout.bottomMargin=bottom;helpButton.setLayoutParams(helpLayout);
        FrameLayout.LayoutParams toolsLayout=(FrameLayout.LayoutParams)toolsButton.getLayoutParams();
        toolsLayout.rightMargin=right+Math.round(8*getResources().getDisplayMetrics().density);
        toolsLayout.bottomMargin=bottom+Math.round(8*getResources().getDisplayMetrics().density);toolsButton.setLayoutParams(toolsLayout);
    }

    static String userdataName(String requested){
        if(requested==null)return "userdata";
        if(!requested.matches("userdata-qa-[A-Za-z0-9_-]{1,64}"))throw new IllegalArgumentException("Invalid QA profile name");
        return requested;
    }

    static String displayRates(android.view.Display display){
        android.view.Display.Mode current=display.getMode();
        java.util.TreeSet<Integer> rates=new java.util.TreeSet<>();
        for(android.view.Display.Mode mode:display.getSupportedModes())
            if(mode.getPhysicalWidth()==current.getPhysicalWidth()&&mode.getPhysicalHeight()==current.getPhysicalHeight()){
                int hz=Math.round(mode.getRefreshRate());if(hz>=30&&hz<=240)rates.add(hz);
            }
        StringBuilder list=new StringBuilder();
        for(int hz:rates){if(list.length()>0)list.append(',');list.append(hz);}
        return list.toString();
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        GamePreferences.initControls(this);
        launch=Launch.attach(this);
        String selectedLanguage=System.getenv("S2_STEAM_LANGUAGE");
        if(selectedLanguage==null||selectedLanguage.isEmpty())selectedLanguage=System.getenv("S2_LANGUAGE");
        if(selectedLanguage==null||selectedLanguage.isEmpty())selectedLanguage=launch.option("game_language");
        android.content.res.Configuration language=new android.content.res.Configuration(getResources().getConfiguration());
        language.setLocale(GamePreferences.gameLocale(selectedLanguage,java.util.Locale.getDefault()));strings=createConfigurationContext(language);
        setVolumeControlStream(android.media.AudioManager.STREAM_MUSIC);
        if(android.os.Build.VERSION.SDK_INT>=33)getOnBackInvokedDispatcher().registerOnBackInvokedCallback(0,this::onBackPressed);
        touch = new TouchLayer(this);
        try{android.system.Os.setenv("S2_SYSTEM_LANGUAGE",GamePreferences.systemLanguage(java.util.Locale.getDefault()),true);}
        catch(android.system.ErrnoException e){android.util.Log.w("Sims2","System language: "+e);}
        if (active != null && active != this) {
            active.controlHandler.removeCallbacks(active.updateControls);
            active.cancelTextSession();
            if (active.touch != null) { touch.takePendingCommand(active.touch); active.touch.releaseAll(); active.touch.releaseLocal(); }
        }
        active = this;
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if(android.os.Build.VERSION.SDK_INT>=30)getWindow().setDecorFitsSystemWindows(false);
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        /* The whole panel, the camera cutout's edge included (the game's
         * resolution follows the surface: sims2_settings.c). */
        if (android.os.Build.VERSION.SDK_INT >= 28)
            attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        /* The display's fastest mode at its resolution (120 Hz panels run
         * apps at 60 otherwise); the game's Frame Rate Limit paces within it. */
        if (android.os.Build.VERSION.SDK_INT >= 23) {
            android.view.Display display = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode current = display.getMode(), best = current;
            for (android.view.Display.Mode m : display.getSupportedModes())
                if (m.getPhysicalWidth() == current.getPhysicalWidth() && m.getPhysicalHeight() == current.getPhysicalHeight()
                        && m.getRefreshRate() > best.getRefreshRate()) best = m;
            attributes.preferredDisplayModeId = best.getModeId();
            preferredRefreshRate = best.getRefreshRate();
            try{android.system.Os.setenv("S2_DISPLAY_RATES",displayRates(display),true);}
            catch(android.system.ErrnoException failure){android.util.Log.w("Sims2","Display rates: "+failure);}
        }
        getWindow().setAttributes(attributes);
        hideSystemUi();
        base = new File(Environment.getExternalStorageDirectory(), "Sims2");
        /* Development: a library pushed into the app's own files (run-as)
         * replaces the packaged one, so Java changes do not need it rebuilt. */
        File dev = new File(getFilesDir(), "libsims2.so");
        try {
            if(dev.isFile()){
                if(!dev.setReadOnly())throw new UnsatisfiedLinkError("Cannot make development engine read-only");
                System.load(dev.getAbsolutePath());
            }else System.loadLibrary("sims2");
            String renderer=launch.renderer();
            nativeRenderer("turnip".equals(renderer)?2:"dxvk".equals(renderer)?1:0,getApplicationInfo().nativeLibraryDir,getCacheDir().getAbsolutePath());
        }catch(LinkageError failure){launch.failed(failure.getMessage());Launch.openLauncher(this,launch.distribution());finish();return;}
        started = true;
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        root.setForceDarkAllowed(false);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(-1, -1));
        root.addView(touch, new FrameLayout.LayoutParams(-1, -1));
        gamepad=new GamepadLayer(this,touch);root.addView(gamepad,new FrameLayout.LayoutParams(-1,-1));
        ((android.hardware.input.InputManager)getSystemService(Context.INPUT_SERVICE)).registerInputDeviceListener(gamepad,controlHandler);
        createObjectControls(root);
        root.setOnApplyWindowInsetsListener((view,insets)->{applySafeInsets(insets);return insets;});
        loadingView=new LinearLayout(this);loadingView.setOrientation(LinearLayout.VERTICAL);loadingView.setGravity(android.view.Gravity.CENTER);
        loadingView.setBackgroundColor(Color.BLACK);
        android.widget.ProgressBar spinner=new android.widget.ProgressBar(this);loadingView.addView(spinner);
        TextView loadingText=new TextView(this);loadingText.setText(text(R.string.loading));loadingText.setTextColor(Color.WHITE);loadingText.setTextSize(18);loadingText.setGravity(android.view.Gravity.CENTER);
        loadingView.addView(loadingText);root.addView(loadingView,new FrameLayout.LayoutParams(-1,-1));
        setContentView(root);
        hideSystemUi();
        File data;
        try{data=new File(base,userdataName(getIntent().getStringExtra("userdata")));}
        catch(IllegalArgumentException failure){launch.failed(failure.getMessage());Launch.openLauncher(this,launch.distribution());finish();return;}
        if(!data.isDirectory()&&!data.mkdirs()){launch.failed("Cannot create game profile");Launch.openLauncher(this,launch.distribution());finish();return;}
        if(!"userdata".equals(data.getName())){
            int qaFps=getIntent().getIntExtra("qa_fps",0);
            if(qaFps>=5&&qaFps<=8)try{android.system.Os.setenv("S2_FPS_LIMIT",Integer.toString(qaFps),true);}
            catch(android.system.ErrnoException failure){android.util.Log.w("Sims2","QA frame limit: "+failure);}
        }
        SessionLog.begin(this,data);
        String args = TouchLayer.readText(new File(base, "args.txt")).trim();
        launch.starting();
        new Thread(()->{
            try{
                File resources=GameAssets.prepare(getAssets(),getFilesDir());
                runOnUiThread(()->{if(active==this&&!isFinishing())nativeStart(launch.folder("game").getAbsolutePath(),
                        data.getAbsolutePath(),resources.getAbsolutePath(),args,new File(base,"verbose").exists());});
            }catch(java.io.IOException failure){runOnUiThread(()->{
                if(active!=this)return;
                launch.failed(failure.getMessage());Launch.openLauncher(this,launch.distribution());finish();
            });}
        },"Port resources").start();
    }

    private void nativeExited(int code){if(code==67)launch.failed(text(R.string.image_mismatch));launch.exiting(code,null);}

    private boolean ownsInput(){return active==this||active==null;}
    private void submitScale(){
        if(active!=this||desiredScale==lastSubmittedScale)return;
        lastSubmittedScale=desiredScale;
        scaleRequested=true;
        if(!nativeTouchSetUiScale(desiredScale))touchNotice(text(R.string.scale_busy));
    }
    private void showTimeHint(){
        if(active!=this||!started||!firstFrame||!hasWindowFocus()||nativeTouchContext()!=TouchLayer.LIVE
                ||settingsDialog!=null||textDialog!=null||cheatUi()||textSerial!=0
                ||!GamePreferences.uiSwipeControls()||GamePreferences.timeHintShown())return;
        AlertDialog.Builder builder=new AlertDialog.Builder(this,R.style.PortDialog);
        TextView help=new TextView(builder.getContext());help.setText(text(R.string.time_swipe_hint));help.setTextColor(Color.BLACK);
        int padding=Math.round(20*getResources().getDisplayMetrics().density);help.setPadding(padding,padding,padding,padding);
        settingsDialog=builder.setTitle(text(R.string.time_controls)).setView(help).setPositiveButton(text(R.string.done),null).create();
        settingsDialog.setOnDismissListener(d->{
            settingsDialog=null;
            if(active==this){touch.setBlocked(!hasWindowFocus());hideSystemUi();if(gamepad!=null)gamepad.setEnabledInput(hasWindowFocus());}
        });
        touch.setBlocked(true);if(gamepad!=null)gamepad.setEnabledInput(false);refreshObjectControls();
        settingsDialog.show();GamePreferences.markTimeHintShown();
    }
    private void showTouchSettings(){
        if(active!=this||!started||settingsDialog!=null||textDialog!=null||cheatUi()||textSerial!=0)return;
        touch.setBlocked(true);
        desiredScale=Math.max(100,Math.min(300,nativeTouchUiScale()));lastSubmittedScale=desiredScale;scaleRequested=false;
        AlertDialog.Builder builder=new AlertDialog.Builder(this,R.style.PortDialog);
        Context dialogContext=builder.getContext();
        LinearLayout content=new LinearLayout(dialogContext);content.setOrientation(LinearLayout.VERTICAL);
        int padding=Math.round(20*getResources().getDisplayMetrics().density);content.setPadding(padding,padding,padding,padding);
        scaleStatus=new TextView(dialogContext);scaleStatus.setTextColor(Color.BLACK);scaleStatus.setText(text(R.string.scale_value,desiredScale));content.addView(scaleStatus);
        SeekBar scale=new SeekBar(dialogContext);scale.setMax(200);scale.setProgress(desiredScale-100);content.addView(scale);
        scale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            public void onProgressChanged(SeekBar b,int p,boolean user){desiredScale=p+100;scaleStatus.setText(text(R.string.scale_value,desiredScale));}
            public void onStartTrackingTouch(SeekBar b){lastSubmittedScale=-1;}
            public void onStopTrackingTouch(SeekBar b){submitScale();}
        });
        TextView help=new TextView(dialogContext);help.setTextColor(Color.BLACK);help.setText(text(R.string.gestures_help));
        android.widget.CheckBox swipe=new android.widget.CheckBox(dialogContext);swipe.setText(text(R.string.ui_swipe_controls));
        swipe.setTextColor(Color.BLACK);swipe.setChecked(GamePreferences.uiSwipeControls());content.addView(swipe);
        swipe.setOnCheckedChangeListener((button,checked)->{touch.releaseAll();GamePreferences.uiSwipeControls(checked);});
        TextView restart=new TextView(dialogContext);restart.setTextColor(Color.BLACK);restart.setText(text(R.string.port_settings_restart));content.addView(restart);
        org.sims2.port.launcher.PortOptions.append(this,content,launch);
        content.addView(help);
        ScrollView scroll=new ScrollView(dialogContext);scroll.addView(content);
        settingsDialog=builder.setTitle(text(R.string.interface_settings)).setView(scroll).setPositiveButton(text(R.string.close),null).create();
        settingsDialog.setOnDismissListener(d->{
            submitScale();
            if(active==this&&scaleRequested){
                int applied=nativeTouchUiScale();
                if(applied!=desiredScale)touchNotice(text(R.string.scale_pending,desiredScale,applied));
            }
            settingsDialog=null;scaleStatus=null;
            if(active==this){touch.setBlocked(!hasWindowFocus());hideSystemUi();}
        });
        settingsDialog.show();
        settingsDialog.getWindow().getDecorView().setForceDarkAllowed(false);
    }
    private void showTextEditor(float x,float y){
        if(active!=this||!started||textDialog!=null||settingsDialog!=null||cheatUi()||textSerial!=0)return;
        // Read fences the completed UI click and captures this field's full value and identity.
        byte[] original=nativeTouchReadText(x,y);if(original==null)return;
        textDraft=new String(original,StandardCharsets.UTF_8);
        if(textDraft.length()>128){nativeTouchCancelText();touchNotice(text(R.string.field_too_long));return;}
        touch.setBlocked(true);
        AlertDialog.Builder builder=new AlertDialog.Builder(this,R.style.PortDialog);
        textEditor=new EditText(builder.getContext());textEditor.setTextColor(Color.BLACK);textEditor.setSingleLine(true);
        textEditor.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        textEditor.setImeOptions(EditorInfo.IME_ACTION_DONE|EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        textEditor.setFilters(new InputFilter[]{new InputFilter.LengthFilter(128)});
        textEditor.setText(textDraft);textEditor.setSelection(textEditor.length());
        textDialog=builder.setTitle(text(R.string.edit_text)).setView(textEditor)
                .setPositiveButton(text(R.string.done),null).setNegativeButton(text(R.string.cancel),(d,w)->cancelTextSession()).create();
        textDialog.setOnShowListener(d->{
            textDialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->{
                if(textSerial!=0||active!=this)return;
                textDraft=textEditor.getText().toString();byte[] bytes=textDraft.getBytes(StandardCharsets.UTF_8);
                if(bytes.length>512||textDraft.indexOf('\0')>=0){textEditor.setError(text(R.string.invalid_text));return;}
                handleTextReceipt(nativeTouchReplaceText(bytes));
            });
            textEditor.setOnEditorActionListener((v,action,event)->{
                if(action!=EditorInfo.IME_ACTION_DONE)return false;
                if(textDialog!=null)textDialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
                return true;
            });
            final EditText editor=textEditor;
            editor.requestFocus();editor.post(()->{
                if(active==this&&textDialog!=null&&textEditor==editor)
                    ((InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE)).showSoftInput(editor,InputMethodManager.SHOW_IMPLICIT);
            });
        });
        textDialog.setOnDismissListener(d->{
            if(textEditor!=null)textDraft=textEditor.getText().toString();if(ownsInput())nativeTouchCancelText();
            textDialog=null;textEditor=null;
            if(active==this){touch.setBlocked(!hasWindowFocus());hideSystemUi();}
        });
        textDialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING|WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        textDialog.show();
        textDialog.getWindow().getDecorView().setForceDarkAllowed(false);
    }
    private void handleTextReceipt(long receipt){
        int status=(int)receipt&255,serial=(int)(receipt>>>32);
        if(textSerial!=0&&serial!=textSerial)return;
        if(status==1){textSerial=serial;if(textDialog!=null)textDialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);return;}
        textSerial=0;
        if(status==2||status==3){if(textDialog!=null)textDialog.dismiss();return;}
        String reason=status==4?text(R.string.field_changed):status==5?text(R.string.text_cancelled):status==6?text(R.string.text_busy):text(R.string.text_unknown);
        if(textEditor!=null)textEditor.setError(reason);else touchNotice(reason);
        if(textDialog!=null)textDialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
    }
    private void cancelTextSession(){
        if(textEditor!=null)textDraft=textEditor.getText().toString();
        if(started&&ownsInput())nativeTouchCancelText();
        // An already STARTED command is polled by its original serial, never repeated.
    }
    private void hideSystemUi(){
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_STABLE|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        if(android.os.Build.VERSION.SDK_INT>=30){android.view.WindowInsetsController c=getWindow().getInsetsController();if(c!=null){c.hide(android.view.WindowInsets.Type.systemBars());c.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);}}
    }
    @Override public void onWindowFocusChanged(boolean hasFocus){
        super.onWindowFocusChanged(hasFocus);
        if(gamepad!=null)gamepad.setEnabledInput(hasFocus&&active==this&&settingsDialog==null&&textDialog==null&&!cheatUi());
        if(active!=this&&active!=null){if(touch!=null)touch.releaseLocal();return;}
        if(touch!=null){if(started)touch.setBlocked(!hasFocus||settingsDialog!=null||textDialog!=null||cheatUi());else touch.releaseLocal();}
        if(hasFocus&&active==this)hideSystemUi();
    }
    @Override public void onBackPressed(){if(!started){Launch.openLauncher(this,launch.distribution());finish();return;}if(toolMenu!=null){closeToolMenu();return;}if(active==this&&cheatUi()){nativeCheatsCancel();if(cheatDialog!=null)cheatDialog.dismiss();return;}if(active==this)touch.back();hideSystemUi();}
    @Override public void surfaceCreated(SurfaceHolder holder){
        if(!started||active!=this)return;Surface surface=holder.getSurface();
        if(android.os.Build.VERSION.SDK_INT>=30)surface.setFrameRate(preferredRefreshRate,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        nativeSurface(surface);
    }
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int w,int h){if(started&&ownsInput()&&touch!=null)touch.releaseAll();}
    @Override public void surfaceDestroyed(SurfaceHolder holder){if(started&&ownsInput()){cancelTextSession();if(touch!=null)touch.releaseAll();nativeSurface(null);}}
    @Override protected void onPause(){
        controlHandler.removeCallbacks(updateControls);
        if(gamepad!=null)gamepad.setEnabledInput(false);
        if(started){if(ownsInput()){nativeCheatsCancel();cancelTextSession();touch.releaseAll();nativePause(true);}else touch.releaseLocal();}
        super.onPause();
    }
    @Override protected void onResume(){
        super.onResume();hideSystemUi();
        if(gamepad!=null)gamepad.setEnabledInput(active==this&&hasWindowFocus()&&settingsDialog==null&&textDialog==null&&!cheatUi());
        if(started&&active==this){nativeTouchCameraContact(false);nativePause(false);controlHandler.removeCallbacks(updateControls);controlHandler.post(updateControls);}
    }
    @Override protected void onDestroy(){
        controlHandler.removeCallbacks(updateControls);
        if(cheatDialog!=null){AlertDialog previous=cheatDialog;cheatDialog=null;previous.dismiss();}
        if(gamepad!=null){gamepad.release();((android.hardware.input.InputManager)getSystemService(Context.INPUT_SERVICE)).unregisterInputDeviceListener(gamepad);}
        if(touch!=null){if(started&&ownsInput()){nativeCheatsCancel();cancelTextSession();touch.releaseAll();}else touch.releaseLocal();}
        if(active==this)active=null;super.onDestroy();
    }
    @Override public boolean dispatchKeyEvent(android.view.KeyEvent event){return started&&active==this&&hasWindowFocus()&&gamepad!=null&&gamepad.key(event)||super.dispatchKeyEvent(event);}
    @Override public boolean dispatchGenericMotionEvent(android.view.MotionEvent event){return started&&active==this&&hasWindowFocus()&&gamepad!=null&&gamepad.motion(event)||super.dispatchGenericMotionEvent(event);}
    @Override public boolean dispatchTouchEvent(android.view.MotionEvent event){if(gamepad!=null)gamepad.hidePointer();return super.dispatchTouchEvent(event);}
}
