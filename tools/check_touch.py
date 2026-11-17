"""Execute native-first touch input and actual Activity methods on the host JVM."""
import os,re,subprocess,tempfile,json,xml.etree.ElementTree as ET
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
JDK=Path(os.environ.get("JAVA_HOME",r"C:\Portable\jdk-24.0.1"))
STUBS = {'android/content/Context.java': 'package android.content; public class Context {public static final String '
                                 'INPUT_SERVICE="input",INPUT_METHOD_SERVICE="ime",ALARM_SERVICE="alarm";public Object getSystemService(String s){return '
                                 'INPUT_SERVICE.equals(s)?new android.hardware.input.InputManager():ALARM_SERVICE.equals(s)?android.app.AlarmManager.instance:new android.view.inputmethod.InputMethodManager();} public '
                                 'android.content.res.Resources getResources(){return new '
                                 'android.content.res.Resources();}}',
 'android/content/res/Resources.java': 'package android.content.res; public class Resources { public '
                                       'android.util.DisplayMetrics getDisplayMetrics() { return new '
                                       'android.util.DisplayMetrics(); } }',
 'android/util/DisplayMetrics.java': 'package android.util; public class DisplayMetrics { public float '
                                     'density=1; }',
 'android/os/Looper.java': 'package android.os; public class Looper { public static Looper '
                           'getMainLooper(){return new Looper();} }',
 'android/os/Handler.java': 'package android.os;\n'
                            'import java.util.*;\n'
                            'public class Handler {\n'
                            '    static long now; static final List<Job> jobs=new ArrayList<>();\n'
                            '    static class Job { Runnable r; long at; Handler owner; Job(Runnable r,long '
                            'at,Handler owner){this.r=r;this.at=at;this.owner=owner;} }\n'
                            '    public Handler(Looper l){}\n'
                            '    public void post(Runnable r){postDelayed(r,0);}\n'
                            '    public void postDelayed(Runnable r,long delay){jobs.add(new '
                            'Job(r,now+delay,this));}\n'
                            '    public void removeCallbacks(Runnable '
                            'r){jobs.removeIf(j->j.owner==this&&j.r==r);}\n'
                            '    public void removeCallbacksAndMessages(Object '
                            'token){jobs.removeIf(j->j.owner==this);}\n'
                            '    public static long uptime(){return now;}\n'
                            '    public static void block(long delta){now+=delta;}\n'
                            '    public static void advance(long delta){\n'
                            '        long end=now+delta;\n'
                            '        for(;;){\n'
                            '            Job next=null;\n'
                            '            for(Job j:jobs) if(j.at<=Math.max(end,now) && (next==null || '
                            'j.at<next.at)) next=j;\n'
                            '            if(next==null) break;\n'
                            '            jobs.remove(next); now=Math.max(now,next.at); next.r.run();\n'
                            '        }\n'
                            '        now=Math.max(now,end);\n'
                            '    }\n'
                            '}',
 'android/view/View.java': 'package android.view;\n'
                           'public class View {\n'
                           '    public View(android.content.Context c){}\n'
                           '    public static int width=2400,height=1080;\n'
                           '    public int getWidth(){return width;} public int getHeight(){return height;}\n'
                           '    public static final int VISIBLE=0,GONE=8;public int visibility;public boolean enabled=true;public float alpha=1;public Object background;public String description;\n'
                           '    public void setVisibility(int v){visibility=v;}public void setEnabled(boolean b){enabled=b;}public void setAlpha(float v){alpha=v;}public void setBackground(Object b){background=b;}public void setContentDescription(String s){description=s;}\n'
                           '    public void setMinimumWidth(int n){}public void setMinimumHeight(int n){}\n'
                           '    public void setPadding(int a,int b,int c,int d){}\n'
                           '    public boolean onTouchEvent(MotionEvent e){return false;}\n'
                           '    public void setForceDarkAllowed(boolean b){}\n'
                           '    public java.util.List<android.graphics.Rect> exclusions;public void setSystemGestureExclusionRects(java.util.List<android.graphics.Rect> rects){exclusions=rects;}\n'
                           '    public void setTranslationX(float f){}public void setTranslationY(float f){}public void setBackgroundColor(int c){}public void invalidate(){} public boolean performHapticFeedback(int '
                           'c){return true;}\n'
                           '    public Object layout; public Object getLayoutParams(){return layout;} public void setLayoutParams(Object p){layout=p;}\n'
                           '    public int animationInvalidations;\n'
                           '    public void postInvalidateOnAnimation(){animationInvalidations++;}\n'
                           '    protected void onDetachedFromWindow(){}\n'
                           '    protected void onDraw(android.graphics.Canvas c){}\n'
                           '    protected void onSizeChanged(int w,int h,int ow,int oh){}\n'
                           '}',
 'android/view/MotionEvent.java': 'package android.view;\n'
                                  'public class MotionEvent {\n'
                                  '    public static final int '
                                  'ACTION_DOWN=0,ACTION_UP=1,ACTION_MOVE=2,ACTION_CANCEL=3,ACTION_POINTER_DOWN=5,ACTION_POINTER_UP=6;\n'
                                  '    int action,index; int[] ids; float[] xs,ys; float[][] historicalXs,historicalYs;\n'
                                  '    public MotionEvent(int a,int i,int[] id,float[] x,float[] '
                                  'y){action=a;index=i;ids=id;xs=x;ys=y;}\n'
                                  '    public static MotionEvent obtain(long down,long now,int action,float x,float y,int meta){return new MotionEvent(action,0,new int[]{0},new float[]{x},new float[]{y});}public void recycle(){}public int getActionMasked(){return action;} public int '
                                  'getActionIndex(){return index;}\n'
                                  '    public int getPointerId(int i){return ids[i];} public int '
                                  'getPointerCount(){return ids.length;}\n'
                                  '    public float getX(int i){return xs[i];} public float getY(int '
                                  'i){return ys[i];}\n'
                                  '    public float getX(){return xs[0];}public float getY(){return ys[0];}\n'
                                  '    public long getEventTime(){return android.os.Handler.uptime();}\n'
                                  '    public MotionEvent withHistory(float[][] x,float[][] y){historicalXs=x;historicalYs=y;return this;}\n'
                                  '    public int getHistorySize(){return historicalXs==null?0:historicalXs.length;}\n'
                                  '    public float getHistoricalX(int pointer,int pos){return historicalXs[pos][pointer];}\n'
                                  '    public float getHistoricalY(int pointer,int pos){return historicalYs[pos][pointer];}\n'
                                  '    public int findPointerIndex(int id){for(int i=0;i<ids.length;i++) '
                                  'if(ids[i]==id)return i; return -1;}\n'
                                  '}',
 'android/os/Build.java': 'package android.os; public class Build {public static class VERSION {public '
                          'static int SDK_INT=36;}}',
 'android/view/HapticFeedbackConstants.java': 'package android.view; public class HapticFeedbackConstants '
                                              '{public static final int LONG_PRESS=0;}',
 'android/view/Surface.java': 'package android.view; public class Surface {public static final int '
                              'FRAME_RATE_COMPATIBILITY_DEFAULT=0;public void setFrameRate(float r,int c){}}',
 'android/view/SurfaceHolder.java': 'package android.view; public interface SurfaceHolder {Surface '
                                    'getSurface();}',
 'android/graphics/Paint.java': 'package android.graphics; public class Paint {public static final int '
                                'ANTI_ALIAS_FLAG=1;public enum Align {CENTER} public void setTextSize(float x){}public void setTextAlign(Align x){}public enum Style {STROKE} public Paint(int x){}public '
                                'void setStrokeWidth(float x){}public void setColor(int x){}public void '
                                'setStyle(Style x){}}',
 'android/graphics/Canvas.java': 'package android.graphics; public class Canvas {public float '
                                 'circleX,circleY;public String drawnText;public int circles;public void drawRoundRect(float a,float b,float c,float d,float e,float f,Paint p){}public void drawText(String s,float x,float y,Paint p){drawnText=s;}public void drawRect(float a,float '
                                 'b,float c,float d,Paint p){}public void drawLine(float a,float b,float '
                                 'c,float d,Paint p){}public void drawCircle(float a,float b,float c,Paint '
                                 'p){circleX=a;circleY=b;circles++;}}'}

STUBS.update({
'android/view/DisplayCutout.java':'''package android.view;public class DisplayCutout {int l,t,r,b;public DisplayCutout(int a,int c,int d,int e){l=a;t=c;r=d;b=e;}public int getSafeInsetLeft(){return l;}public int getSafeInsetTop(){return t;}public int getSafeInsetRight(){return r;}public int getSafeInsetBottom(){return b;}}''',
'android/widget/CheckBox.java':'''package android.widget;public class CheckBox extends TextView {public boolean checked;public interface Changed {void change(Object button,boolean value);}public Changed changed;public CheckBox(android.content.Context c){super(c);}public void setChecked(boolean v){checked=v;if(changed!=null)changed.change(this,v);}public void setOnCheckedChangeListener(Changed c){changed=c;}}''',
'org/sims2/port/GamePreferences.java':'''package org.sims2.port;class GamePreferences {static boolean swipes=true,hintShown=true;static boolean uiSwipeControls(){return swipes;}static void uiSwipeControls(boolean b){swipes=b;}static boolean timeHintShown(){return hintShown;}static void markTimeHintShown(){hintShown=true;}}''',
'android/graphics/Rect.java':'''package android.graphics;public class Rect {public int left,top,right,bottom;public Rect(int l,int t,int r,int b){left=l;top=t;right=r;bottom=b;}}''',
'android/graphics/Color.java':'''package android.graphics;public class Color {public static final int WHITE=-1,BLACK=0xff000000;public static int rgb(int r,int g,int b){return (255<<24)|(r<<16)|(g<<8)|b;}}''',
'android/graphics/drawable/GradientDrawable.java':'''package android.graphics.drawable;public class GradientDrawable {public int color,stroke,strokeColor;public float radius;public void setColor(int c){color=c;}public void setCornerRadius(float r){radius=r;}public void setStroke(int w,int c){stroke=w;strokeColor=c;}}''',
'android/view/Gravity.java':'''package android.view;public class Gravity {public static final int TOP=48,BOTTOM=80,RIGHT=5,CENTER=17,CENTER_VERTICAL=16,CENTER_HORIZONTAL=1;}''',
'android/widget/FrameLayout.java':'''package android.widget;public class FrameLayout extends android.view.View {public interface Click {void click(Object v);}public Click click;public void setOnClickListener(Click c){click=c;}public java.util.List<Object> children=new java.util.ArrayList<>();public FrameLayout(android.content.Context c){super(c);}public void addView(Object v,Object p){children.add(v);if(v instanceof android.view.View)((android.view.View)v).setLayoutParams(p);}public void removeView(Object v){children.remove(v);}public static class LayoutParams {public int width,height,gravity,topMargin,leftMargin,rightMargin,bottomMargin;public LayoutParams(int w,int h){this(w,h,0);}public LayoutParams(int w,int h,int g){width=w;height=h;gravity=g;}}}''',
'android/graphics/Insets.java':'''package android.graphics;public class Insets {public int left,top,right,bottom;public Insets(int l,int t,int r,int b){left=l;top=t;right=r;bottom=b;}}''',
'android/view/RoundedCorner.java':'''package android.view;public class RoundedCorner {int radius;public RoundedCorner(int r){radius=r;}public int getRadius(){return radius;}}''',
'android/view/WindowInsets.java':'''package android.view;public class WindowInsets {public android.graphics.Insets safe;public DisplayCutout cutout;public android.graphics.Insets getSystemWindowInsets(){return safe;}public DisplayCutout getDisplayCutout(){return cutout;}public RoundedCorner[] corners=new RoundedCorner[4];public WindowInsets(int l,int t,int r,int b){safe=new android.graphics.Insets(l,t,r,b);}public android.graphics.Insets getInsets(int type){assert type==3;return safe;}public RoundedCorner getRoundedCorner(int n){return corners[n];}public static class Type {public static int systemBars(){return 1;}public static int displayCutout(){return 2;}}}''',
'android/view/Display.java':'''package android.view;public class Display {Mode current;Mode[] modes;public Display(Mode c,Mode... m){current=c;modes=m;}public Mode getMode(){return current;}public Mode[] getSupportedModes(){return modes;}public static class Mode {int w,h;float hz;public Mode(int x,int y,float rate){w=x;h=y;hz=rate;}public int getPhysicalWidth(){return w;}public int getPhysicalHeight(){return h;}public float getRefreshRate(){return hz;}}}''',
'android/widget/Button.java':'''package android.widget;public class Button extends TextView {public interface Click {void click(Object v);}public Click listener;public float textSize;public int textColor;public boolean caps;public Button(android.content.Context c){super(c);}public void setAllCaps(boolean b){caps=b;}public void setTextSize(float n){textSize=n;}public void setTextColor(int c){textColor=c;}public void setMinWidth(int n){}public void setMinHeight(int n){}public void setPadding(int a,int b,int c,int d){}public void setOnClickListener(Click c){listener=c;}public interface LongClick {boolean click(Object v);}public LongClick longListener;public void setOnLongClickListener(LongClick c){longListener=c;}public void performClick(){if(enabled&&listener!=null)listener.click(this);}}''',
'android/widget/ScrollView.java':'''package android.widget;public class ScrollView {public final android.content.Context context;public Object child;public ScrollView(android.content.Context c){context=c;}public void addView(Object view){child=view;}}''',
'android/R.java':'''package android;public class R {public static class style {public static final int Theme_Material_Light_Dialog_Alert=1234;}}''',
'android/content/Intent.java':'''package android.content;public class Intent {public static final int FLAG_ACTIVITY_NEW_TASK=1,FLAG_ACTIVITY_CLEAR_TASK=2;public boolean copied;public Intent(Context c,Class<?> k){}public Intent putExtras(Intent i){copied=true;return this;}public Intent addFlags(int f){return this;}}''',
'android/app/PendingIntent.java':'''package android.app;public class PendingIntent {public static final int FLAG_CANCEL_CURRENT=1,FLAG_IMMUTABLE=2;public static PendingIntent getActivity(android.content.Context c,int n,android.content.Intent i,int flags){assert (flags&FLAG_IMMUTABLE)!=0&&i.copied;return new PendingIntent();}}''',
'android/app/AlarmManager.java':'''package android.app;public class AlarmManager {public static final int ELAPSED_REALTIME=3;public static final AlarmManager instance=new AlarmManager();public static int scheduled;public static boolean fail;public void set(int type,long at,PendingIntent p){assert type==ELAPSED_REALTIME&&at==1750;if(fail)throw new IllegalStateException();scheduled++;}}''',
'android/os/SystemClock.java':'''package android.os;public class SystemClock {public static long elapsedRealtime(){return 1000;}public static long uptimeMillis(){return android.os.Handler.uptime();}}''',
'android/os/Process.java':'''package android.os;public class Process {public static int kills;public static int myPid(){return 42;}public static void killProcess(int pid){assert pid==42;kills++;}}''',
'android/util/AtomicFile.java':'''package android.util;import java.io.*;public class AtomicFile {final File file;public static boolean fail;public static int writes;public AtomicFile(File f){file=f;}public FileOutputStream startWrite()throws IOException {if(fail)throw new IOException("disk full");file.getParentFile().mkdirs();return new FileOutputStream(new File(file+".new"));}public void finishWrite(FileOutputStream out)throws IOException {out.close();java.nio.file.Files.move(new File(file+".new").toPath(),file.toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);writes++;}public void failWrite(FileOutputStream out){try{out.close();}catch(IOException e){}}}''',
'android/view/Window.java': 'package android.view; public class Window {public void setSoftInputMode(int v){}public View getDecorView(){return new View(new android.content.Context());}}',
'android/view/WindowManager.java': 'package android.view; public class WindowManager {public static class LayoutParams {public static final int SOFT_INPUT_ADJUST_NOTHING=1,SOFT_INPUT_STATE_ALWAYS_VISIBLE=2;}}',
'android/view/inputmethod/EditorInfo.java':'package android.view.inputmethod;public class EditorInfo {public static final int IME_ACTION_DONE=6,IME_FLAG_NO_EXTRACT_UI=33554432;}',
'android/view/inputmethod/InputMethodManager.java':'package android.view.inputmethod; public class InputMethodManager {public static final int SHOW_IMPLICIT=1;public static int shows;public void showSoftInput(Object v,int mode){assert v!=null;shows++;}}',
'android/text/InputFilter.java':'package android.text; public interface InputFilter {class LengthFilter implements InputFilter {public LengthFilter(int n){}}}',
'android/text/InputType.java':'package android.text; public class InputType {public static final int TYPE_CLASS_TEXT=1,TYPE_TEXT_FLAG_CAP_SENTENCES=2;}',
'android/widget/TextView.java':'''package android.widget;public class TextView extends android.view.View {public int icon;public void setCompoundDrawablesWithIntrinsicBounds(int a,int b,int c,int d){icon=b;}public String text="";public int textColor;public final android.content.Context context;public TextView(android.content.Context c){super(c);context=c;}public void setText(String s){text=s;}public void setTextColor(int c){textColor=c;}public void setTextSize(int s){}public void setGravity(int g){}}''',
'android/widget/EditText.java':'''package android.widget;public class EditText extends TextView {public String error;public EditText(android.content.Context c){super(c);}public interface EditorAction {boolean action(Object v,int a,Object e);}public EditorAction editorAction;public int imeOptions;public void setImeOptions(int i){imeOptions=i;}public void setOnEditorActionListener(EditorAction l){editorAction=l;}public void setSingleLine(boolean b){}public void setInputType(int t){}public void setFilters(android.text.InputFilter[] f){}public void setSelection(int p){}public int length(){return text.length();}public String getText(){return text;}public void setError(String s){error=s;}public void requestFocus(){}public void post(Runnable r){new android.os.Handler(android.os.Looper.getMainLooper()).post(r);}}''',
'android/widget/LinearLayout.java':'''package android.widget;public class LinearLayout extends android.view.View {public static final int VERTICAL=1,HORIZONTAL=0;public java.util.List<Object> children=new java.util.ArrayList<>();public int orientation;public LinearLayout(android.content.Context c){super(c);}public void setOrientation(int i){orientation=i;}public void setPadding(int a,int b,int c,int d){}public void addView(Object o){children.add(o);}public void addView(Object o,Object p){children.add(o);}public static class LayoutParams {public int width,height,leftMargin,rightMargin;public LayoutParams(int w,int h){width=w;height=h;}}}''',
'android/widget/SeekBar.java':'''package android.widget;public class SeekBar {public static SeekBar latest;public int progress;public OnSeekBarChangeListener listener;public SeekBar(android.content.Context c){latest=this;}public void setMax(int i){}public void setProgress(int i){progress=i;}public void setOnSeekBarChangeListener(OnSeekBarChangeListener l){listener=l;}public interface OnSeekBarChangeListener {void onProgressChanged(SeekBar b,int p,boolean user);void onStartTrackingTouch(SeekBar b);void onStopTrackingTouch(SeekBar b);}}''',
'android/app/AlertDialog.java':'''package android.app;
public class AlertDialog {
 public static final int BUTTON_POSITIVE=-1;public boolean showing;public Listener show,dismiss;public Pair negative,choice;public String[] choices;public int checked,theme;public Object view;
 public static class Button {public boolean enabled=true;public Runnable action;public void setEnabled(boolean b){enabled=b;}public void setOnClickListener(Click c){action=()->c.click(null);}public void performClick(){click();}public void click(){if(enabled&&action!=null)action.run();}}
 public interface Click {void click(Object v);}public interface Listener {void call(Object d);}public interface Pair {void call(Object d,int n);}
 public final Button positive=new Button();public void setOnDismissListener(Listener l){dismiss=l;}public void setOnShowListener(Listener l){show=l;}
 public Button getButton(int b){return positive;}public android.view.Window getWindow(){return new android.view.Window();}
 public void show(){showing=true;if(show!=null)show.call(this);}public void dismiss(){showing=false;if(dismiss!=null)dismiss.call(this);}public void cancel(){if(negative!=null)negative.call(this,0);dismiss();}
 public static class Builder {AlertDialog d=new AlertDialog();android.content.Context themed=new android.content.Context();public Builder(android.content.Context c){}public Builder(android.content.Context c,int theme){d.theme=theme;}public android.content.Context getContext(){return themed;}public Builder setTitle(String s){return this;}public Builder setView(Object v){d.view=v;return this;}public Builder setSingleChoiceItems(String[] a,int selected,Pair p){d.choices=a;d.checked=selected;d.choice=p;return this;}public Builder setPositiveButton(String s,Pair p){if(p!=null)d.positive.action=()->{p.call(d,-1);d.dismiss();};return this;}public Builder setNegativeButton(String s,Pair p){d.negative=p;return this;}public AlertDialog create(){return d;}}
}'''
})
STUBS.update({
 'android/view/ViewConfiguration.java': 'package android.view;public class ViewConfiguration {public static ViewConfiguration get(android.content.Context c){return new ViewConfiguration();}public int getScaledMinimumFlingVelocity(){return 50;}public int getScaledMaximumFlingVelocity(){return 8000;}}',
 'android/view/VelocityTracker.java': 'package android.view;public class VelocityTracker {public static float vx,vy;public static int records,recycles;public static VelocityTracker obtain(){return new VelocityTracker();}public void addMovement(MotionEvent e){records++;}public void computeCurrentVelocity(int u,int max){assert u==1000&&max>0;}public float getXVelocity(int id){return vx;}public float getYVelocity(int id){return vy;}public void recycle(){recycles++;}}',
 'android/widget/OverScroller.java': 'package android.widget;public class OverScroller {int vx,vy,x,y,ticks;boolean finished=true;public OverScroller(android.content.Context c){}public void forceFinished(boolean b){finished=b;}public void fling(int x,int y,int vx,int vy,int minX,int maxX,int minY,int maxY){assert minX<maxX&&minY<maxY;this.x=x;this.y=y;this.vx=vx;this.vy=vy;ticks=0;finished=false;}public boolean computeScrollOffset(){if(finished||ticks++>=5){finished=true;return false;}x+=vx/30;y+=vy/30;return true;}public int getCurrX(){return x;}public int getCurrY(){return y;}}'
})
STUBS['android/view/View.java']=STUBS['android/view/View.java'].replace('protected void onDraw(', 'public void announceForAccessibility(CharSequence text){} protected void onDraw(')

NATIVE = r'''package org.sims2.port;
import android.app.AlertDialog;import android.widget.*;import android.view.*;import android.text.*;
import android.view.inputmethod.InputMethodManager;import android.view.inputmethod.EditorInfo;import java.nio.charset.StandardCharsets;import android.content.Context;
import android.app.AlarmManager;import android.app.PendingIntent;import android.content.Intent;import android.util.AtomicFile;import java.io.File;
import android.graphics.Color;
class ActivityBase extends Context {
 public Intent getIntent(){return new Intent(this,GameActivity.class);}
 public boolean hasWindowFocus(){return true;}public void onWindowFocusChanged(boolean f){}
 public void onBackPressed(){}public void surfaceCreated(SurfaceHolder h){}public void surfaceDestroyed(SurfaceHolder h){}
 public void finishAffinity(){}
 public void finish(){}
 protected void onPause(){}protected void onResume(){}protected void onDestroy(){}
}
public class GameActivity extends ActivityBase {
 static class Launch {static void openLauncher(GameActivity a,String id){}String distribution(){return "legacy";}}
 Launch launch=new Launch();
 static int context,kind=2,tool=1,picks,clicks,wheel,moves,warps,settingsRequests,textRequests,notices;
 static long generation=1;static float x,y;static boolean mouse,rightMouse;static boolean[] keys=new boolean[256];
 static java.util.List<String> events=new java.util.ArrayList<>();static Runnable onPick;
 static boolean cameraContact;static java.util.List<String> contactEvents=new java.util.ArrayList<>();
 static void nativeTouchCameraContact(boolean active){cameraContact=active;contactEvents.add("contact:"+active);}
 static java.util.function.BiFunction<Float,Float,Integer> hitAt;
 static int nativeTouchContext(){return context;}static long nativeTouchToolToken(){return(generation<<32)|(tool&0xffffffffL);}
 static boolean rewards;static boolean nativeTouchRewards(){return rewards;}
 static float pickX=-1,pickY=-1;static long swipeFeedback;static int flings;
 static float[] nativeTouchUiPick(float x,float y,float diameter){assert diameter>0;int k=nativeTouchHitKind(x,y);return new float[]{k,pickX<0?x:pickX,pickY<0?y:pickY};}
 static long nativeTouchSwipeFeedback(){return swipeFeedback;}
 static long nativeTouchScrollOwner(){return 123;}
 static int flingStops;static void nativeTouchStopFling(){flingStops++;}
 static boolean nativeTouchFling(float x,float y,int amount,int mode,long size,long epoch,long owner){assert owner==123;flings++;return nativeTouchScroll(x,y,amount,mode,size,epoch);}
 static long nativeTouchTarget(float x,float y,float radius){return(nativeTouchHitKind(x,y)&3)!=2?-1:((long)Math.round(y*1080)<<32)|(Math.round(x*2400)&0xffffffffL);}
 static long gameSize=(1080L<<32)|2400,layoutEpoch;
 static int nativeTouchObjectCost(){return 80;}static boolean nativeTouchObjectPreview(long token){return token==nativeTouchToolToken();}
 static long nativeTouchGameSize(){return gameSize;}static long nativeTouchLayoutEpoch(){return layoutEpoch;}
 static int lastDirection,rotateCalls,rotatePolls,rotatePollSerial,rotationEffects;static boolean selectedObject=true;
 static long rotateReply=2,rotateStatus=(42L<<32)|2,rotateToken,rotateSize,rotateEpoch;static int rotateContext;static float rotateX,rotateY;
 static long nativeTouchRotateObject(float xx,float yy,long tok,int mode,long size,long epoch,int direction){
  assert direction==-1||direction==1;lastDirection=direction;
  rotateCalls++;rotateToken=tok;rotateContext=mode;rotateSize=size;rotateEpoch=epoch;rotateX=xx;rotateY=yy;
  if(!selectedObject||tok!=nativeTouchToolToken()||mode!=context||size!=gameSize||epoch!=layoutEpoch||kind!=2)return 4;
  if((rotateReply&255)==2)rotationEffects++;return rotateReply;
 }
 static long nativeTouchRotateObjectStatus(int serial){rotatePolls++;rotatePollSerial=serial;return rotateStatus;}
 static int fillKind,fillCalls,fillPolls,fillPollSerial,fillEffects;static float fillX,fillY;
 static long fillReply=2,fillStatus=(43L<<32)|2,fillToken,fillSize,fillEpoch;static int fillContext;
 static int nativeTouchFillKind(){return fillKind;}
 static long nativeTouchFillRoom(float xx,float yy,long tok,int mode,long size,long epoch){
  fillCalls++;fillX=xx;fillY=yy;fillToken=tok;fillContext=mode;fillSize=size;fillEpoch=epoch;
  if((fillKind!=1&&fillKind!=2)||tool!=3||mode!=512||tok!=nativeTouchToolToken()||mode!=context||size!=gameSize||epoch!=layoutEpoch||kind!=2)return 4;
  if((fillReply&255)==2)fillEffects++;return fillReply;
 }
 static long nativeTouchFillRoomStatus(int serial){fillPolls++;fillPollSerial=serial;return fillStatus;}
 static int nativeTouchHitKind(float xx,float yy){picks++;events.add("pick");contactEvents.add("pick:"+cameraContact);if(onPick!=null){Runnable r=onPick;onPick=null;r.run();}return hitAt==null?kind:hitAt.apply(xx,yy);}
 static void nativeKey(int k,boolean d){keys[k]=d;events.add("key:"+k+":"+d);}
 static void nativeMouseButton(int b,boolean d){contactEvents.add("button:"+cameraContact);if(b==1){rightMouse=d;events.add("right:"+d);return;}mouse=d;if(d)clicks++;events.add("button:"+d+":"+Math.round(x*2400)+":"+Math.round(y*1080));}
 static void nativeMouseMove(float xx,float yy,boolean r){contactEvents.add("move:"+cameraContact);x=xx;y=yy;moves++;events.add("move:"+Math.round(x*2400)+":"+Math.round(y*1080));}
 static void nativeMouseWarp(float xx,float yy){contactEvents.add("warp:"+cameraContact);x=xx;y=yy;warps++;events.add("warp");}
 static void nativeMouseWheel(int delta){wheel+=delta;events.add("wheel");}
 static int uiScale=200,scaleCalls;static boolean scaleAccept=true,scaleApply=true;
 static int nativeTouchUiScale(){return uiScale;}static boolean nativeTouchSetUiScale(int value){scaleCalls++;if(scaleAccept&&scaleApply)uiScale=value;return scaleAccept;}
 static int casPans;static float panTotal;static boolean nativeTouchPanCas(float dy){casPans++;panTotal+=dy;return true;}
 static int cameraCommands;static float cameraDx,cameraDy,cameraTurn,cameraZoom,cameraPitch;
 static boolean nativeTouchCamera(float dx,float dy,float radians,float zoom,float pitch){cameraPitch+=pitch;cameraZoom+=zoom;cameraCommands++;cameraDx+=dx;cameraDy+=dy;cameraTurn+=radians;events.add("camera");return true;}
 static boolean nativeTouchPan(float x,float y,float xx,float yy){return nativeTouchCamera(xx-x,yy-y,0,0,0);}
 static int scrolls,scrollAmount,infos;static boolean scrollAccept=true;
 static boolean nativeTouchScroll(float x,float y,int amount,int mode,long size,long epoch){assert mode==context&&size==gameSize&&epoch==layoutEpoch;scrolls++;scrollAmount+=amount;return scrollAccept;}
 static boolean nativeTouchCatalogInfo(float x,float y,int mode,long size,long epoch){assert mode==context&&size==gameSize&&epoch==layoutEpoch;infos++;return true;}
 static int toolMenuRequests,lastToolMenuAction;static boolean nativeTouchToolMenu(int action,int mode,long size,long epoch){assert action>=0&&action<9&&mode==context&&size==gameSize&&epoch==layoutEpoch;toolMenuRequests++;lastToolMenuAction=action;return true;}
 static int catalogClears;static boolean nativeTouchClearCatalog(){catalogClears++;events.add("catalog-clear");assert keys[72]||keys[27];return true;}
 static byte[] original="Сенсор".getBytes(StandardCharsets.UTF_8),replaced;static int reads,replaces,polls,cancels,lastSerial;
 static long replaceReceipt=2,statusReceipt=2;
 static byte[] nativeTouchReadText(float x,float y){reads++;return original;}
 static long nativeTouchReplaceText(byte[] b){replaces++;replaced=b;return replaceReceipt;}
 static long nativeTouchReplaceTextStatus(int s){polls++;lastSerial=s;return statusReceipt;}
 static void nativeTouchCancelText(){cancels++;}
 static int cheatRequests;static void touchCheatsRequested(){cheatRequests++;}static void nativeCheatsCancel(){}
 boolean cheatUi(){return toolMenu!=null;}
 static String lastNotice;static void touchNotice(String s){notices++;lastNotice=s;}
 static String text(int id,Object...args){return String.format(java.util.Locale.ROOT,R.values[id],args);}
 static void touchSettingsRequested(){settingsRequests++;}
 static Runnable onTextRequest;static void touchTextRequested(float x,float y){textRequests++;if(onTextRequest!=null){Runnable r=onTextRequest;onTextRequest=null;r.run();}}
 static Object currentSurface;static int surfaceCalls,pauseCalls;static boolean paused;
 static void nativeSurface(Object s){currentSurface=s;surfaceCalls++;layoutEpoch++;}static void nativePause(boolean b){contactEvents.add("pause:"+b+":"+cameraContact);paused=b;pauseCalls++;if(b)layoutEpoch++;}
 static GameActivity active;boolean started=true,firstFrame;float preferredRefreshRate=120;
 GamepadLayer gamepad;TouchLayer touch=new TouchLayer(this);android.os.Handler controlHandler=new android.os.Handler(android.os.Looper.getMainLooper());Runnable updateControls=()->{};
 AlertDialog settingsDialog,textDialog,languageDialog,cheatDialog;EditText textEditor;TextView scaleStatus;int textSerial,desiredScale,lastSubmittedScale,cheatSerial;boolean scaleRequested;String textDraft="";
 LinearLayout objectControls;TextView objectCost;Button rotateButton,placeButton,cancelButton,helpButton,toolsButton;FrameLayout root=new FrameLayout(this),toolMenu;int toolMenuContext;long toolMenuSize,toolMenuEpoch;
 int safeLeft,safeTop,safeRight,safeBottom;
 File base;
 void hideSystemUi(){}
'''
CHECK=r'''package org.sims2.port;
import android.os.Handler;import android.view.MotionEvent;import android.widget.*;
public class TouchCheck {
 static MotionEvent event(int a,int i,int[] ids,float[] x,float[] y){return new MotionEvent(a,i,ids,x,y);}
 static MotionEvent one(int a,float x,float y){return event(a,0,new int[]{7},new float[]{x},new float[]{y});}
 static MotionEvent two(int a,int i,float x,float y,float xx,float yy){return event(a,i,new int[]{7,19},new float[]{x,xx},new float[]{y,yy});}
 static MotionEvent three(int a,int i){return event(a,i,new int[]{7,19,31},new float[]{400,700,1000},new float[]{400,400,400});}
 static TouchLayer layer(int mode,int kind,int tool){GameActivity.context=mode;GameActivity.kind=kind;GameActivity.tool=tool;GameActivity.generation++;GameActivity.hitAt=null;GameActivity.onPick=null;GameActivity.fillKind=0;TouchLayer t=new TouchLayer(new android.content.Context());t.syncContext(mode);return t;}
 static void tap(TouchLayer t,float x,float y){t.onTouchEvent(one(0,x,y));t.onTouchEvent(one(1,x,y));}
 static void clean(){Handler.advance(1000);assert !GameActivity.mouse&&!GameActivity.rightMouse;for(boolean b:GameActivity.keys)assert !b;}
 static void noExtra(TouchLayer t){t.releaseAll();clean();}
 static void touchUx(){
  TouchLayer t=layer(128,1,0);GameActivity.events.clear();GameActivity.pickX=.5f;GameActivity.pickY=.5f;
  tap(t,1000,500);Handler.advance(50);
  assert GameActivity.events.contains("button:true:1200:540"):GameActivity.events;
  clean();GameActivity.pickX=GameActivity.pickY=-1;noExtra(t);
  for(int kind:new int[]{9,41}){
   t=layer(128,kind,0);android.view.VelocityTracker.vy=-2400;
   t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,600,400));t.onTouchEvent(one(1,600,400));
   int before=GameActivity.flings;Handler.advance(32);assert GameActivity.flings>before;
   int clicks=GameActivity.clicks;before=GameActivity.flings;
   tap(t,600,400);Handler.advance(1000);
   assert GameActivity.flings==before&&GameActivity.clicks==clicks:"stop contact must not click";
   tap(t,600,400);Handler.advance(50);assert GameActivity.clicks==clicks+1;clean();noExtra(t);
  }
  for(int cancel=0;cancel<5;++cancel){
   t=layer(128,9,0);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,600,400));t.onTouchEvent(one(1,600,400));
   int before=GameActivity.flings;
   if(cancel==0)GameActivity.layoutEpoch++;if(cancel==1)GameActivity.context|=1;
   if(cancel==2)t.setBlocked(true);if(cancel==3)t.releaseLocal();if(cancel==4)t.onTouchEvent(one(3,600,400));
   Handler.advance(1000);assert GameActivity.flings==before;noExtra(t);
  }
  for(int kind:new int[]{513,257,13}){
   t=layer(128,kind,0);int before=GameActivity.flings;
   t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,600,400));t.onTouchEvent(one(1,600,400));
   Handler.advance(250);assert GameActivity.flings==before;noExtra(t);
  }
  android.view.VelocityTracker.vy=0;
  GameActivity.swipeFeedback=0;t=layer(128,513,0);t.pollCommands();
  android.graphics.Canvas c=new android.graphics.Canvas();t.onDraw(c);assert c.drawnText==null;
  GameActivity.swipeFeedback=(1L<<44)|(2L<<40)|(5L<<36)|(3L<<32)|0x80000000L;
  t.pollCommands();t.onDraw(c);assert c.drawnText.equals("Увлечения · 3/5"):c.drawnText;
  GameActivity.swipeFeedback=(3L<<44)|(4L<<40)|(6L<<36)|(2L<<32)|0x8C6B0001L;
  t.pollCommands();t.onDraw(c);assert c.drawnText.equals("Голова · 2/6"):c.drawnText;
  GameActivity.context|=1024;t.syncContext(GameActivity.context);t.pollCommands();
  c=new android.graphics.Canvas();t.onDraw(c);assert c.drawnText.equals("Голова · 2/6");
  Handler.advance(500);t.releaseAll();c=new android.graphics.Canvas();t.onDraw(c);assert c.drawnText.equals("Голова · 2/6");
  Handler.advance(901);c=new android.graphics.Canvas();t.onDraw(c);assert c.drawnText==null;
  GameActivity.context=128;t.syncContext(GameActivity.context);
  GameActivity.swipeFeedback=(2L<<44)|(4L<<40)|(6L<<36)|(4L<<32)|0x8C6B0005L;
  t.pollCommands();t.onDraw(c);assert c.drawnText.equals("Косметика · 4/6"):c.drawnText;
  Handler.advance(1401);c=new android.graphics.Canvas();t.onDraw(c);assert c.drawnText==null;noExtra(t);
 }
 static void tapsAndMenus(){
  for(int kind:new int[]{1,2})for(int motion=0;motion<4;motion++){
   TouchLayer t=layer(128,kind,1);int clicks=GameActivity.clicks;t.onTouchEvent(one(0,600,500));
   if(motion==0)t.onTouchEvent(one(1,640,500));
   if(motion==1){t.onTouchEvent(one(2,601,500).withHistory(new float[][]{{640}},new float[][]{{500}}));t.onTouchEvent(one(1,600,500));}
   if(motion==2)t.onTouchEvent(one(1,600,500).withHistory(new float[][]{{640}},new float[][]{{500}}));
   if(motion==3){Handler.block(500);t.onTouchEvent(one(1,600,500));}
   clean();assert GameActivity.clicks==clicks:"release displacement, batched excursions and delayed long contacts are not taps";noExtra(t);
  }
  TouchLayer t=layer(128,2,1);GameActivity.events.clear();tap(t,600,500);Handler.advance(50);assert GameActivity.mouse;
  GameActivity.tool=0;GameActivity.generation++;t.syncToolToken(GameActivity.nativeTouchToolToken());
  assert GameActivity.mouse&&GameActivity.events.stream().noneMatch(s->s.equals("key:27:true")):"opening an action menu must not cancel its own tap";
  clean();assert GameActivity.events.stream().noneMatch(s->s.equals("key:27:true"));
  t=layer(129,1,1);tap(t,600,500);Handler.advance(50);int moves=GameActivity.moves;
  GameActivity.context=128;clean();assert GameActivity.moves==moves:"closing a dialog must not issue a held mouse move into the world";
  for(int change=0;change<3;change++){
   t=layer(128,1,1);int clicks=GameActivity.clicks;tap(t,600,500);
   if(change==0)GameActivity.layoutEpoch++;if(change==1)GameActivity.gameSize++;if(change==2)android.view.View.width--;
   clean();assert GameActivity.clicks==clicks;android.view.View.width=2400;GameActivity.gameSize=(1080L<<32)|2400;
  }
  t=layer(256,2,2);GameActivity.rewards=true;GameActivity.events.clear();t.cancelObjectPreview();
  assert GameActivity.events.contains("key:27:true")&&!GameActivity.events.contains("key:72:true");clean();GameActivity.rewards=false;
  t=layer(128,1|256|1024|(1<<11),1);android.graphics.Canvas canvas=new android.graphics.Canvas();
  t.onDraw(canvas);assert canvas.circles==0;
  t.onTouchEvent(one(0,500,500));t.onTouchEvent(one(2,520,500));t.onDraw(canvas);assert canvas.circles==4;
  t.onTouchEvent(one(1,520,500));canvas.circles=0;t.onDraw(canvas);assert canvas.circles==0;noExtra(t);
  t.onSizeChanged(2400,1080,2400,1080);assert t.exclusions.size()==1&&t.exclusions.get(0).right==2400&&t.exclusions.get(0).bottom==1080;
  GamePreferences.hintShown=false;GameActivity a=new GameActivity();GameActivity.active=a;a.firstFrame=true;GameActivity.context=64;
  a.showTimeHint();assert a.settingsDialog==null&&!GamePreferences.hintShown;
  GameActivity.context=128;a.showTimeHint();assert a.settingsDialog!=null&&GamePreferences.hintShown;
  android.app.AlertDialog hint=a.settingsDialog;a.showTimeHint();assert a.settingsDialog==hint;
  hint.dismiss();a.showTimeHint();assert a.settingsDialog==null;
  a=new GameActivity();GameActivity.active=a;a.firstFrame=true;a.showTimeHint();assert a.settingsDialog==null;
  GameActivity.active=null;noExtra(t);
 }
 static void navigationMouseMoves(){
  for(int[] state:new int[][]{{64,0},{128,0},{128,1},{256,1},{512,1}}){
   TouchLayer t=layer(state[0],2,state[1]);int warps=GameActivity.warps,moves=GameActivity.moves;
   t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));t.onTouchEvent(one(1,800,550));clean();
   assert GameActivity.warps==warps&&GameActivity.moves==moves:"navigation must not synthesize PC mouse motion";
   t=layer(state[0],2,state[1]);warps=GameActivity.warps;moves=GameActivity.moves;
   t.onTouchEvent(one(0,600,500));t.onTouchEvent(two(5,1,600,500,1000,500));
   t.onTouchEvent(two(2,0,650,550,1200,700));t.onTouchEvent(two(6,1,650,550,1200,700));
   t.onTouchEvent(one(2,700,600));t.onTouchEvent(one(1,700,600));clean();
   assert GameActivity.warps==warps&&GameActivity.moves==moves:"navigation pair and remaining finger retain explicit camera coordinates";
   t=layer(state[0],2,state[1]);warps=GameActivity.warps;int clicks=GameActivity.clicks;GameActivity.events.clear();
   tap(t,600,500);clean();
   assert GameActivity.warps==warps&&GameActivity.clicks==clicks+1;
   assert GameActivity.events.indexOf("move:600:500")>=0;
   assert GameActivity.events.indexOf("move:600:500")<GameActivity.events.indexOf("button:true:600:500"):"ordinary world click still positions its real target before pressing";
  }
  TouchLayer held=layer(128,2,1);int warps=GameActivity.warps,moves=GameActivity.moves,clicks=GameActivity.clicks;
  held.onTouchEvent(one(0,600,500));Handler.advance(451);held.onTouchEvent(one(2,850,550));held.onTouchEvent(one(1,850,550));clean();
  assert GameActivity.warps==warps&&GameActivity.moves==moves&&GameActivity.clicks==clicks:"pan after world hold remains mouse-motion free";
  for(int[] state:new int[][]{{4,2,0},{128,1,1},{256,2,2},{512,2,3},{256,2,6}}){
   TouchLayer t=layer(state[0],state[1],state[2]);warps=GameActivity.warps;t.onTouchEvent(one(0,600,500));
   assert GameActivity.warps==warps+1:"CAS, UI, preview and non-navigation tools keep initial native mouse positioning";
   noExtra(t);
  }
  for(int[] state:new int[][]{{4,0},{256,2},{512,3}}){
   TouchLayer t=layer(state[0],2,state[1]);warps=GameActivity.warps;
   t.onTouchEvent(one(0,600,500));t.onTouchEvent(two(5,1,600,500,1000,500));
   assert GameActivity.warps==warps+2:"CAS and selected build-tool camera pairs keep their original warp";
   noExtra(t);
  }
 }
 static void cameraContacts(){
  TouchLayer t=layer(128,2,1);GameActivity.contactEvents.clear();
  t.onTouchEvent(one(0,600,500));assert GameActivity.cameraContact;
  assert GameActivity.contactEvents.get(0).equals("contact:true")&&GameActivity.contactEvents.get(1).equals("pick:true"):"protect the first owner hit fence";
  t.onTouchEvent(one(1,600,500));assert !GameActivity.cameraContact;clean();
  assert !GameActivity.contactEvents.contains("move:true")&&!GameActivity.contactEvents.contains("button:true"):"normal interaction runs after contact clears";
  t=layer(128,2,1);t.onTouchEvent(one(0,600,500));Handler.advance(451);assert GameActivity.cameraContact;
  t.onTouchEvent(one(2,850,550));assert GameActivity.cameraContact;t.onTouchEvent(one(1,850,550));assert !GameActivity.cameraContact;noExtra(t);
  for(int cancel=0;cancel<9;cancel++){
   t=layer(128,2,1);t.onTouchEvent(one(0,600,500));assert GameActivity.cameraContact;
   if(cancel==0)t.onTouchEvent(one(3,600,500));if(cancel==1)t.setBlocked(true);if(cancel==2)t.syncContext(2);
   if(cancel==3){GameActivity.tool=2;GameActivity.generation++;t.syncToolToken(GameActivity.nativeTouchToolToken());}
   if(cancel==4)t.onSizeChanged(2400,1080,2400,1080);if(cancel==5)t.onDetachedFromWindow();if(cancel==6)t.releaseAll();if(cancel==7)t.back();if(cancel==8)t.syncContext(128|1024);
   assert !GameActivity.cameraContact:"cancel/context/tool/lifecycle clears contact";noExtra(t);
  }
  t=layer(128,2,1);t.onTouchEvent(one(0,600,500));t.onTouchEvent(two(5,1,600,500,1000,500));assert GameActivity.cameraContact;
  t.onTouchEvent(two(2,0,650,550,1200,700));t.onTouchEvent(two(6,1,650,550,1200,700));assert GameActivity.cameraContact;
  t.onTouchEvent(one(2,700,600));assert GameActivity.cameraContact;t.onTouchEvent(one(1,700,600));assert !GameActivity.cameraContact;noExtra(t);
  t=layer(128,2,1);GameActivity.hitAt=(x,y)->x<.3?2:1;t.onTouchEvent(one(0,600,500));t.onTouchEvent(two(5,1,600,500,1000,500));assert !GameActivity.cameraContact;noExtra(t);
  t=layer(128,1,1);GameActivity.contactEvents.clear();t.onTouchEvent(one(0,600,500));assert !GameActivity.cameraContact;
  assert GameActivity.contactEvents.contains("pick:true")&&GameActivity.contactEvents.contains("warp:false");noExtra(t);
  TouchLayer old=layer(128,2,1);old.onTouchEvent(one(0,600,500));old.releaseAll();old.releaseLocal();
  TouchLayer current=layer(128,2,1);current.onTouchEvent(one(0,600,500));GameActivity.contactEvents.clear();old.releaseAll();old.releaseLocal();
  assert GameActivity.cameraContact&&GameActivity.contactEvents.isEmpty():"retired layers cannot clear a new owner";noExtra(current);
  for(int rollover=0;rollover<2;rollover++){
   t=layer(128,2,1);t.onTouchEvent(one(0,600,500));assert GameActivity.cameraContact;
   if(rollover==0)GameActivity.generation++;else GameActivity.onPick=()->GameActivity.generation++;
   int pans=GameActivity.cameraCommands;t.onTouchEvent(one(0,700,550));assert GameActivity.cameraContact;
   t.onTouchEvent(one(2,850,600));assert GameActivity.cameraCommands>pans;
   t.onTouchEvent(one(1,850,600));assert !GameActivity.cameraContact:"interrupted DOWN/token rollover must retain a consumable stream";noExtra(t);
  }
  t=layer(128,2,1);t.onTouchEvent(one(0,600,500));final TouchLayer blockDuringPick=t;
  GameActivity.onPick=()->blockDuringPick.setBlocked(true);t.onTouchEvent(one(0,700,550));assert !GameActivity.cameraContact;noExtra(t);
  GameActivity a=new GameActivity();GameActivity.active=a;GameActivity.cameraContact=true;GameActivity.contactEvents.clear();a.onResume();
  assert !GameActivity.cameraContact&&GameActivity.contactEvents.indexOf("contact:false")<GameActivity.contactEvents.indexOf("pause:false:false");a.onDestroy();GameActivity.active=null;
 }
 static void world(){
  tapsAndMenus();
  for(int mode:new int[]{2,66,3,1026,4,64}){
   TouchLayer back=layer(mode,1,0);GameActivity.events.clear();back.back();
   int expected=mode==2?0x70:27;assert GameActivity.events.get(0).equals("key:"+expected+":true");
   Handler.advance(89);assert GameActivity.keys[expected];Handler.advance(1);assert !GameActivity.keys[expected];noExtra(back);
  }
  for(int mode:new int[]{64,128,256,512}){
   TouchLayer previous=layer(mode,1,0);GameActivity.context=2;previous.syncContext(2);
   TouchLayer back=layer(2,1,0);back.takePendingCommand(previous);GameActivity.events.clear();back.back();
   int expected=mode==64?27:mode==256?0x71:mode==512?0x72:0x70;
   assert GameActivity.events.get(0).equals("key:"+expected+":true");noExtra(back);noExtra(previous);
  }
  TouchLayer t=layer(64,2,0);int clicks=GameActivity.clicks;tap(t,600,500);assert GameActivity.clicks==clicks;Handler.advance(50);assert GameActivity.mouse;Handler.advance(89);assert GameActivity.mouse;Handler.advance(1);assert !GameActivity.mouse&&GameActivity.clicks==clicks+1;noExtra(t);
  t=layer(512,2,0);clicks=GameActivity.clicks;tap(t,600,500);clean();assert GameActivity.clicks==clicks;
  t=layer(512,0,3);tap(t,600,500);clean();assert GameActivity.clicks==clicks;
  t=layer(0,1,0);tap(t,600,500);clean();assert GameActivity.clicks==clicks+1;noExtra(t);
  t=layer(128,2,1);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));assert !GameActivity.mouse;assert GameActivity.cameraCommands>0&&!GameActivity.keys[0x25];t.onTouchEvent(one(1,800,550));clean();
  // A tool change published while the prepress fence waits suppresses the old world click.
  t=layer(512,2,1);clicks=GameActivity.clicks;tap(t,600,500);GameActivity.onPick=()->{GameActivity.tool=2;GameActivity.generation++;};clean();assert GameActivity.clicks==clicks;
 }
 static void directTools(){
  for(int mode:new int[]{256,512})for(int tool:new int[]{2,3,4,5,6}){
   TouchLayer cancel=layer(mode,2,tool);GameActivity.events.clear();int clicks=GameActivity.clicks;cancel.back();
   assert GameActivity.events.get(0).equals("key:"+72+":true");clean();
   assert GameActivity.events.contains("key:"+72+":false")&&GameActivity.clicks==clicks;
  }
  for(int blocked:new int[]{1,2,8,16,32,1024}){
   TouchLayer cancel=layer(512|blocked,1,3);GameActivity.events.clear();cancel.back();
   assert GameActivity.events.stream().noneMatch(e->e.equals("key:72:true"));clean();
  }
  TouchLayer t=layer(512,2,3);GameActivity.events.clear();t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));assert GameActivity.mouse;
  assert GameActivity.events.indexOf("button:true:600:500")>=0;assert GameActivity.events.get(GameActivity.events.size()-1).equals("move:800:550");
  t.onTouchEvent(one(1,850,575));assert !GameActivity.mouse;assert GameActivity.events.get(GameActivity.events.size()-1).equals("button:false:850:575");clean();
  t=layer(512,2,3);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));GameActivity.events.clear();t.onTouchEvent(two(5,1,800,550,1000,550));
  assert GameActivity.events.get(0).equals("key:27:true");assert GameActivity.events.get(1).startsWith("button:false");int clicks=GameActivity.clicks;
  t.onTouchEvent(two(6,0,800,550,1000,550));t.onTouchEvent(event(2,0,new int[]{19},new float[]{1500},new float[]{900}));t.onTouchEvent(event(1,0,new int[]{19},new float[]{1500},new float[]{900}));clean();assert GameActivity.clicks==clicks;
  t=layer(512,2,3);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));GameActivity.events.clear();t.onTouchEvent(one(3,800,550));assert GameActivity.events.get(0).equals("key:27:true");assert GameActivity.events.get(1).startsWith("button:false");int cancelMoves=GameActivity.moves;t.onTouchEvent(one(1,900,600));clean();assert GameActivity.moves==cancelMoves;
  t=layer(512,2,3);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));GameActivity.events.clear();t.back();assert GameActivity.events.get(0).equals("key:27:true");assert GameActivity.events.get(1).startsWith("button:false");assert GameActivity.events.indexOf("key:27:false")<GameActivity.events.indexOf("key:72:true");clean();assert GameActivity.events.contains("key:72:false");
  t=layer(256,2,2);clicks=GameActivity.clicks;t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));assert GameActivity.clicks==clicks&&!GameActivity.mouse;t.onTouchEvent(one(1,800,550));clean();assert GameActivity.clicks==clicks&&t.canUseObjectPreview();t.placeObjectPreview();clean();assert GameActivity.clicks==clicks+1;
  t=layer(256,2,2);GameActivity.events.clear();clicks=GameActivity.clicks;t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));int moves=GameActivity.moves;
  t.onTouchEvent(two(5,1,800,550,1400,800));t.onTouchEvent(two(2,0,900,650,1600,900));t.onTouchEvent(two(6,0,900,650,1600,900));t.onTouchEvent(event(1,0,new int[]{19},new float[]{1600},new float[]{900}));clean();assert GameActivity.clicks==clicks&&GameActivity.moves==moves;assert GameActivity.events.stream().noneMatch(s->s.startsWith("button:"));
  t=layer(256,2,2);clicks=GameActivity.clicks;t.onTouchEvent(one(0,600,500));Handler.advance(451);assert !GameActivity.keys[0xBE];t.onTouchEvent(one(1,600,500));clean();assert GameActivity.clicks==clicks;
  t=layer(512,2,3);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));GameActivity.events.clear();GameActivity.generation++;t.syncToolToken(GameActivity.nativeTouchToolToken());assert !GameActivity.mouse;assert GameActivity.events.get(0).equals("key:27:true");assert GameActivity.events.get(1).startsWith("button:false");int picks=GameActivity.picks;
  t.onTouchEvent(two(5,1,800,550,1200,600));t.onTouchEvent(three(5,2));t.onTouchEvent(one(1,800,550));assert GameActivity.picks==picks;noExtra(t);
  t=layer(4,5,0);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,550));t.syncContext(0);assert !GameActivity.mouse;picks=GameActivity.picks;t.onTouchEvent(two(5,1,800,550,1200,600));assert GameActivity.picks==picks;noExtra(t);
  // CAS model drag interruption releases only the camera capture, never native Escape/discard.
  t=layer(4|1024,2,0);GameActivity.events.clear();t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,800,650));assert GameActivity.mouse;assert GameActivity.events.get(GameActivity.events.size()-1).equals("move:800:500");t.onTouchEvent(two(5,1,800,650,1000,650));assert !GameActivity.mouse;assert GameActivity.events.stream().noneMatch(s->s.startsWith("key:27:"));noExtra(t);
 }
 static void ui(){
  for(int lift:new int[]{0,1}){
   TouchLayer t=layer(4,1|4,0);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,650,500));assert GameActivity.mouse;
   t.onTouchEvent(two(5,1,650,500,1000,600));t.onTouchEvent(two(2,0,700,500,1500,900));assert Math.round(GameActivity.x*2400)==700;
   t.onTouchEvent(two(6,lift,700,500,1500,900));if(lift==0){assert !GameActivity.mouse;t.onTouchEvent(event(1,0,new int[]{19},new float[]{1500},new float[]{900}));}else{assert GameActivity.mouse;t.onTouchEvent(one(1,700,500));}clean();
  }
  TouchLayer t=layer(4,1|8,0);int clicks=GameActivity.clicks,wheel=GameActivity.wheel;t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,600,600));t.onTouchEvent(one(1,600,600));clean();assert GameActivity.clicks==clicks&&GameActivity.wheel!=wheel;
  t=layer(8|1|1024,1|8|16,0);wheel=GameActivity.wheel;int edits=GameActivity.textRequests;t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,600,400));assert !GameActivity.mouse;t.onTouchEvent(one(1,600,400));clean();assert GameActivity.wheel!=wheel&&GameActivity.textRequests==edits&&GameActivity.clicks==clicks;
  t=layer(4,1|16,0);int requests=GameActivity.textRequests;tap(t,600,500);Handler.advance(139);assert GameActivity.textRequests==requests;Handler.advance(1);assert GameActivity.textRequests==requests+1&&!GameActivity.mouse;
  t=layer(0,17,0);tap(t,600,500);clean();assert GameActivity.textRequests==requests+2;requests=GameActivity.textRequests;
  t=layer(4|1024,1,0);requests=GameActivity.textRequests;t.syncContext(4|1024);Handler.advance(500);assert GameActivity.textRequests==requests;
  t=layer(4,1|16,0);t.onTouchEvent(one(0,600,500));Handler.advance(451);t.onTouchEvent(one(1,600,500));clean();assert GameActivity.textRequests==requests;
  t=layer(4,17,0);tap(t,400,300);final TouchLayer textLayer=t;GameActivity.onTextRequest=()->textLayer.setBlocked(true);int picks=GameActivity.picks,moves=GameActivity.warps;t.onTouchEvent(one(0,800,600));assert GameActivity.picks==picks&&GameActivity.warps==moves;noExtra(t);
  t=layer(0,1,0);GameActivity.events.clear();t.onTouchEvent(one(0,400,300));t.onTouchEvent(one(1,404,300));clean();assert GameActivity.events.contains("button:true:400:300")&&!GameActivity.events.contains("button:true:404:300");
  // Rapid UI pulses finish at captured coordinates exactly once.
  t=layer(0,1,0);GameActivity.events.clear();tap(t,400,300);tap(t,800,600);clean();assert GameActivity.events.contains("button:false:400:300")&&GameActivity.events.contains("button:false:800:600");
 }
 static void objectPreview(){
  GameActivity.rotateReply=2;GameActivity.selectedObject=true;GameActivity.gameSize=(1080L<<32)|2400;
  TouchLayer t=layer(256,2,2);int clicks=GameActivity.clicks,notices=GameActivity.notices;
  assert t.objectControlsVisible()&&t.canCancelObject()&&!t.canUseObjectPreview();
  assert GameActivity.notices==notices;
  t.placeObjectPreview();t.rotateObjectPreview();assert GameActivity.clicks==clicks;
  tap(t,600,500);clean();assert t.canUseObjectPreview()&&GameActivity.clicks==clicks;
  // A button's own screen coordinate never becomes a placement coordinate; repeated clicks do not replay.
  GameActivity.x=.99f;GameActivity.y=.01f;t.placeObjectPreview();t.placeObjectPreview();assert !t.canUseObjectPreview();
  Handler.advance(49);assert GameActivity.clicks==clicks;Handler.advance(1);assert GameActivity.mouse&&GameActivity.clicks==clicks+1;
  assert Math.round(GameActivity.x*2400)==600&&Math.round(GameActivity.y*1080)==500;Handler.advance(90);assert !GameActivity.mouse;
  t.placeObjectPreview();clean();assert GameActivity.clicks==clicks+1;
  t=layer(512,2,2);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,890,610));t.onTouchEvent(one(1,890,610));
  int rotations=GameActivity.rotateCalls;t.rotateObjectPreview();assert GameActivity.rotateCalls==rotations+1;
  assert Math.round(GameActivity.rotateX*2400)==890&&Math.round(GameActivity.rotateY*1080)==610&&t.canUseObjectPreview();
  GameActivity.events.clear();t.cancelObjectPreview();t.cancelObjectPreview();assert !t.canUseObjectPreview();
  assert GameActivity.events.stream().filter(s->s.equals("key:27:true")).count()==1;clean();
  // Every saved owner/layout dimension is checked without waiting for the250ms control poll.
  for(int change=0;change<7;change++){
   t=layer(256,2,2);tap(t,600,500);clicks=GameActivity.clicks;rotations=GameActivity.rotateCalls;
   if(change==0)GameActivity.generation++;if(change==1)GameActivity.context|=1;
   if(change==2)GameActivity.gameSize++;if(change==3)GameActivity.layoutEpoch++;
   if(change==4)android.view.View.width--;if(change==5)android.view.View.height--;if(change==6)GameActivity.tool=1;
   t.placeObjectPreview();t.rotateObjectPreview();clean();assert GameActivity.clicks==clicks&&GameActivity.rotateCalls==rotations&&!t.canUseObjectPreview();
   android.view.View.width=2400;android.view.View.height=1080;GameActivity.gameSize=(1080L<<32)|2400;
  }
  // Reentrant owner changes during button rehit cannot place/rotate the old selection.
  for(int action=0;action<2;action++){
   t=layer(256,2,2);tap(t,600,500);clicks=GameActivity.clicks;rotations=GameActivity.rotateCalls;
   GameActivity.onPick=()->GameActivity.layoutEpoch++;
   if(action==0)t.placeObjectPreview();else t.rotateObjectPreview();clean();
   assert GameActivity.clicks==clicks&&GameActivity.rotateCalls==rotations;
  }
  t=layer(256,2,2);tap(t,600,500);clicks=GameActivity.clicks;t.placeObjectPreview();GameActivity.layoutEpoch++;clean();assert GameActivity.clicks==clicks;
  // A changed layout/mode during the accepted pulse cancels before the balanced UP; never replay it.
  for(int change=0;change<5;change++){
   t=layer(256,2,2);tap(t,600,500);t.placeObjectPreview();Handler.advance(50);assert GameActivity.mouse;GameActivity.events.clear();
   if(change==0)GameActivity.layoutEpoch++;if(change==1)GameActivity.context|=1;if(change==2)GameActivity.gameSize++;
   if(change==3)android.view.View.width--;if(change==4)android.view.View.height--;
   Handler.advance(90);assert !GameActivity.mouse&&GameActivity.events.get(0).equals("key:27:true")&&GameActivity.events.get(1).startsWith("button:false");
   clean();android.view.View.width=2400;android.view.View.height=1080;GameActivity.gameSize=(1080L<<32)|2400;
  }
  // Original placement finalizes on UP: any earlier semantic tool change cancels before the balanced UP.
  t=layer(256,2,2);tap(t,600,500);t.placeObjectPreview();Handler.advance(50);GameActivity.tool=1;GameActivity.events.clear();t.syncToolToken(GameActivity.nativeTouchToolToken());
  assert !GameActivity.mouse&&GameActivity.events.get(0).equals("key:27:true")&&GameActivity.events.get(1).startsWith("button:false");Handler.advance(90);clean();
  // A replaced tool generation likewise cancels before UP.
  t=layer(256,2,2);tap(t,600,500);t.placeObjectPreview();Handler.advance(50);GameActivity.tool=1;GameActivity.generation++;GameActivity.events.clear();t.syncToolToken(GameActivity.nativeTouchToolToken());
  assert !GameActivity.mouse&&GameActivity.events.get(0).equals("key:27:true")&&GameActivity.events.get(1).startsWith("button:false");clean();
  // A new UI tap, camera gesture, cancellation or lifecycle transition consumes the old preview.
  for(int change=0;change<5;change++){
   t=layer(256,2,2);tap(t,600,500);clicks=GameActivity.clicks;
   if(change==0){GameActivity.kind=1;tap(t,200,800);GameActivity.kind=2;clean();clicks++;}
   if(change==1){t.onTouchEvent(one(0,600,500));t.onTouchEvent(two(5,1,600,500,1000,500));t.onTouchEvent(two(2,0,500,500,1100,500));t.onTouchEvent(two(6,1,500,500,1100,500));t.onTouchEvent(one(1,500,500));}
   if(change==2)t.onTouchEvent(one(3,600,500));if(change==3){t.setBlocked(true);t.setBlocked(false);}if(change==4)t.onSizeChanged(2400,1080,2400,1080);
   t.placeObjectPreview();clean();assert !t.canUseObjectPreview()&&GameActivity.clicks==clicks;
  }
  // STARTED rotation disables Place and retains its original serial through an Activity handoff.
  t=layer(256,2,2);tap(t,600,500);GameActivity.rotateReply=(42L<<32)|1;GameActivity.rotateStatus=(42L<<32)|1;
  t.rotateObjectPreview();clicks=GameActivity.clicks;rotations=GameActivity.rotateCalls;t.placeObjectPreview();assert !t.canUseObjectPreview()&&GameActivity.clicks==clicks;
  TouchLayer replacement=new TouchLayer(new android.content.Context());replacement.takePendingCommand(t);t.releaseAll();t.releaseLocal();
  GameActivity.rotateStatus=(42L<<32)|2;replacement.pollCommands();assert GameActivity.rotateCalls==rotations&&!replacement.canUseObjectPreview();noExtra(replacement);GameActivity.rotateReply=2;
  // Known no-effect receipts leave a valid preview available for an explicit retry.
  for(int status:new int[]{4,5,6}){
   t=layer(256,2,2);tap(t,600,500);GameActivity.rotateReply=status;t.rotateObjectPreview();assert t.canUseObjectPreview();
   GameActivity.rotateReply=(43L<<32)|1;GameActivity.rotateStatus=(43L<<32)|status;t.rotateObjectPreview();assert !t.canUseObjectPreview();
   t.pollCommands();assert t.canUseObjectPreview();noExtra(t);
  }
  GameActivity.rotateReply=2;
  // Kind6 exact eyedropper/design tools are click-only; drags/holds never begin world capture.
  t=layer(256,2,6);clicks=GameActivity.clicks;tap(t,600,500);clean();assert GameActivity.clicks==clicks+1&&!t.objectControlsVisible();
  t=layer(256,2,6);clicks=GameActivity.clicks;t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,900,650));assert !GameActivity.mouse;t.onTouchEvent(one(1,900,650));clean();assert GameActivity.clicks==clicks;
  t=layer(256,2,6);t.onTouchEvent(one(0,600,500));Handler.advance(450);t.onTouchEvent(one(1,600,500));clean();assert GameActivity.clicks==clicks;
  t=layer(256,2,6);int wheel=GameActivity.wheel;t.onTouchEvent(one(0,500,500));t.onTouchEvent(two(5,1,500,500,700,500));t.onTouchEvent(two(2,0,400,500,800,500));
  t.onTouchEvent(two(6,1,400,500,800,500));t.onTouchEvent(one(1,400,500));clean();assert GameActivity.cameraZoom>0&&GameActivity.clicks==clicks;
  // Real Activity methods render exactly three accessible controls, guarded even for stale callbacks.
  GameActivity a=new GameActivity();GameActivity.active=a;GameActivity.context=256;GameActivity.tool=2;GameActivity.kind=2;GameActivity.generation++;
  a.createObjectControls(new FrameLayout(a));a.refreshObjectControls();assert a.objectControls.visibility==0&&a.objectControls.children.size()==4;
  assert !a.rotateButton.enabled&&!a.placeButton.enabled&&a.cancelButton.enabled;
  assert a.rotateButton.description.contains("Повернуть")&&a.placeButton.description.contains("Поставить")&&a.cancelButton.description.contains("Отмена");
  assert ((android.graphics.drawable.GradientDrawable)a.placeButton.background).color==android.graphics.Color.rgb(200,211,239);
  clicks=GameActivity.clicks;tap(a.touch,600,500);assert a.placeButton.enabled&&GameActivity.clicks==clicks;
  a.placeButton.performClick();a.placeButton.performClick();clean();assert GameActivity.clicks==clicks+1&&!a.placeButton.enabled;
  tap(a.touch,600,500);a.onPause();a.onResume();a.refreshObjectControls();assert !a.placeButton.enabled;
  for(int mode:new int[]{1,2,4,8,16,32,64,128,256|1,256|1024,512|16}){GameActivity.context=mode;a.refreshObjectControls();assert a.objectControls.visibility==8;}
  assert a.helpButton.description.equals(GameActivity.text(R.string.interface_settings));
  for(int mode:new int[]{2,2|64,2|128,2|256,2|512}){GameActivity.context=mode;a.refreshObjectControls();assert a.helpButton.visibility==0;}
  for(int mode:new int[]{0,1,4,8,16,32,64,128,256,512,2|1,2|16,2|1024}){GameActivity.context=mode;a.refreshObjectControls();assert a.helpButton.visibility==8;}
  GameActivity.context=2;a.refreshObjectControls();a.helpButton.performClick();assert a.settingsDialog!=null;
  a.refreshObjectControls();assert a.helpButton.visibility==8;a.settingsDialog.dismiss();a.refreshObjectControls();assert a.helpButton.visibility==0;
  a.textDialog=new android.app.AlertDialog();a.refreshObjectControls();assert a.helpButton.visibility==8;a.textDialog=null;
  a.started=false;a.refreshObjectControls();assert a.helpButton.visibility==8;a.started=true;
  GameActivity.context=256;tap(a.touch,600,500);a.settingsDialog=new android.app.AlertDialog();a.refreshObjectControls();assert a.objectControls.visibility==8;
  clicks=GameActivity.clicks;a.touch.setBlocked(true);a.placeButton.listener.click(null);a.rotateButton.listener.click(null);assert GameActivity.clicks==clicks;
  a.settingsDialog=null;a.onDestroy();GameActivity.active=null;
 }
 static void radialMenu(){
  GameActivity a=new GameActivity();GameActivity.active=a;GameActivity.context=256;GameActivity.tool=1;
  a.createObjectControls(a.root);a.refreshObjectControls();assert a.toolsButton.visibility==0;
  int clicks=GameActivity.clicks,requests=GameActivity.toolMenuRequests;
  a.toolsButton.performClick();assert a.toolMenu!=null&&a.toolMenu.children.size()==10;
  assert a.objectControls.visibility==8&&a.toolsButton.visibility==8;
  for(int i=0;i<9;++i)assert ((Button)a.toolMenu.children.get(i)).icon!=0;
  ((Button)a.toolMenu.children.get(3)).performClick();assert a.toolMenu==null&&GameActivity.toolMenuRequests==requests+1&&GameActivity.lastToolMenuAction==3;
  assert GameActivity.clicks==clicks;
  a.toolsButton.longListener.click(null);assert a.toolMenu!=null;a.onBackPressed();assert a.toolMenu==null&&GameActivity.toolMenuRequests==requests+1;
  a.showToolMenu();FrameLayout stale=a.toolMenu;GameActivity.layoutEpoch++;
  ((Button)stale.children.get(0)).performClick();assert a.toolMenu==null&&GameActivity.toolMenuRequests==requests+1;
  GameActivity.context=128;a.showToolMenu();assert a.toolMenu==null;a.refreshObjectControls();assert a.toolsButton.visibility==8;
  a.onDestroy();GameActivity.active=null;clean();
 }
 static void objectRotation(){
  TouchLayer t=layer(256,2,2);int calls=GameActivity.rotateCalls,clicks=GameActivity.clicks;
  t.onTouchEvent(one(0,600,500));Handler.advance(450);
  assert t.canUseObjectPreview()&&GameActivity.rotateCalls==calls;
  t.onTouchEvent(one(2,650,520));assert t.canUseObjectPreview()&&GameActivity.rotateCalls==calls;
  t.onTouchEvent(one(1,650,520));assert t.canUseObjectPreview()&&GameActivity.clicks==clicks;
  t.placeObjectPreview();clean();assert GameActivity.clicks==clicks+1;
  for(int direction:new int[]{-1,1}){
   t=layer(256,2,2);calls=GameActivity.rotateCalls;int camera=GameActivity.cameraCommands;
   t.onTouchEvent(one(0,600,500));Handler.advance(450);
   t.onTouchEvent(two(5,1,600,500,1000,500));
   t.onTouchEvent(two(2,0,600,500,700,500+direction*350));
   assert GameActivity.rotateCalls==calls+1&&GameActivity.lastDirection==direction;
   assert GameActivity.cameraCommands==camera;
   t.onTouchEvent(two(6,1,600,500,700,500+direction*350));t.onTouchEvent(one(1,600,500));
   assert t.canUseObjectPreview();noExtra(t);
  }
  t=layer(256,2,2);calls=GameActivity.rotateCalls;t.onTouchEvent(one(0,600,500));Handler.advance(450);
  GameActivity.layoutEpoch++;t.onTouchEvent(two(5,1,600,500,1000,500));
  t.onTouchEvent(two(2,0,600,500,700,850));assert GameActivity.rotateCalls==calls;noExtra(t);
  GameActivity.layoutEpoch=0;
 }
 static void catalogDrag(){
  TouchLayer t=layer(256,105,1);int clicks=GameActivity.clicks,scrolls=GameActivity.scrolls;
  GameActivity.hitAt=(x,y)->y<.5f?2:105;
  t.onTouchEvent(one(0,1800,800));
  t.onTouchEvent(one(2,1750,760));assert GameActivity.clicks==clicks+1&&GameActivity.scrolls==scrolls&&!GameActivity.mouse;
  GameActivity.tool=2;GameActivity.generation++;t.onTouchEvent(one(2,1600,700));
  assert GameActivity.clicks==clicks+1&&GameActivity.scrolls==scrolls;
  t.onTouchEvent(one(2,1300,450));t.onTouchEvent(one(1,1300,450));clean();
  assert GameActivity.clicks==clicks+1&&t.canUseObjectPreview();noExtra(t);
  t=layer(256,105,1);clicks=GameActivity.clicks;scrolls=GameActivity.scrolls;
  t.onTouchEvent(one(0,1800,800));t.onTouchEvent(one(2,1500,800));t.onTouchEvent(one(1,1500,800));clean();
  assert GameActivity.clicks==clicks&&GameActivity.scrolls>scrolls;noExtra(t);
 }
 static TouchLayer fillLayer(int kind){TouchLayer t=layer(512,2,3);GameActivity.fillKind=kind;return t;}
 static void areaFill(){
  GameActivity.gameSize=(1080L<<32)|2400;GameActivity.layoutEpoch=0;GameActivity.fillReply=2;
  for(int kind:new int[]{1,2}){
   TouchLayer t=fillLayer(kind);int calls=GameActivity.fillCalls,clicks=GameActivity.clicks,effects=GameActivity.fillEffects;
   GameActivity.events.clear();t.onTouchEvent(one(0,600,500));Handler.advance(449);assert GameActivity.fillCalls==calls;
   Handler.advance(1);assert GameActivity.fillCalls==calls&&GameActivity.clicks==clicks;
   assert GameActivity.lastNotice.contains("подготовлено")&&GameActivity.lastNotice.contains("Отпустите");
   Handler.advance(1200);assert GameActivity.fillCalls==calls; // Holding never mutates the native tool.
   t.onTouchEvent(one(2,650,550));t.onTouchEvent(one(1,650,550));assert GameActivity.fillCalls==calls+1&&GameActivity.fillEffects==effects+1;
   assert GameActivity.fillToken==GameActivity.nativeTouchToolToken()&&GameActivity.fillContext==512;
   assert GameActivity.fillSize==GameActivity.gameSize&&GameActivity.fillEpoch==0;
   assert Math.round(GameActivity.fillX*2400)==600&&Math.round(GameActivity.fillY*1080)==500;
   t.onTouchEvent(one(1,650,550));clean();assert GameActivity.fillCalls==calls+1&&GameActivity.clicks==clicks;
   assert GameActivity.events.stream().noneMatch(s->s.startsWith("button:")||s.startsWith("key:"));
  }
  // A drag before the hold threshold retains the native stroke, rather than area fill.
  TouchLayer t=fillLayer(1);int calls=GameActivity.fillCalls,clicks=GameActivity.clicks;
  t.onTouchEvent(one(0,600,500));Handler.advance(449);t.onTouchEvent(one(2,650,550));Handler.advance(500);assert GameActivity.mouse;
  t.onTouchEvent(one(1,700,575));clean();assert GameActivity.fillCalls==calls&&GameActivity.clicks==clicks+1;
  t=fillLayer(1);tap(t,600,500);clean();assert GameActivity.fillCalls==calls&&GameActivity.clicks==clicks+2;
  // Both pointer release orders cancel locally; the remaining finger cannot fill, pan or place.
  for(int lift:new int[]{0,1}){
   t=fillLayer(2);t.onTouchEvent(one(0,600,500));Handler.advance(450);GameActivity.events.clear();clicks=GameActivity.clicks;
   t.onTouchEvent(two(5,1,600,500,1000,700));t.onTouchEvent(two(2,0,700,600,1100,800));
   t.onTouchEvent(two(6,lift,700,600,1100,800));int remaining=lift==0?19:7;
   t.onTouchEvent(event(2,0,new int[]{remaining},new float[]{1200},new float[]{900}));t.onTouchEvent(event(1,0,new int[]{remaining},new float[]{1200},new float[]{900}));
   clean();assert GameActivity.fillCalls==calls&&GameActivity.clicks==clicks&&GameActivity.events.isEmpty();
  }
  // Only exact Floor/Wallpaper in Build may arm, even if an advisory kind is stale.
  for(int[] setup:new int[][]{{512,2,3,0},{512,2,3,3},{256,2,3,1},{4,2,0,1},{512,1,3,1},{512,0,3,1},{512,2,2,1}}){
   t=layer(setup[0],setup[1],setup[2]);GameActivity.fillKind=setup[3];t.onTouchEvent(one(0,600,500));Handler.advance(450);t.onTouchEvent(one(1,600,500));clean();assert GameActivity.fillCalls==calls;
  }
  // Cancellation before release is local: Back, CANCEL, blocking, lifecycle and size callbacks.
  for(int cancel=0;cancel<7;cancel++){
   t=fillLayer(1);t.onTouchEvent(one(0,600,500));Handler.advance(450);GameActivity.events.clear();
   if(cancel==0)t.back();if(cancel==1)t.onTouchEvent(one(3,600,500));if(cancel==2)t.setBlocked(true);
   if(cancel==3)t.releaseAll();if(cancel==4)t.onSizeChanged(2300,1080,2400,1080);if(cancel==5)t.onDetachedFromWindow();if(cancel==6)t.releaseLocal();
   t.onTouchEvent(one(1,600,500));clean();assert GameActivity.fillCalls==calls&&GameActivity.events.isEmpty();
  }
  // Capture snapshots after the owner fence. Reject every changed identity at release, even without a poll/callback.
  t=fillLayer(1);GameActivity.onPick=()->{GameActivity.generation++;GameActivity.gameSize=(600L<<32)|800;GameActivity.layoutEpoch=17;};
  t.onTouchEvent(one(0,600,500));Handler.advance(450);t.onTouchEvent(one(1,600,500));assert GameActivity.fillCalls==++calls;
  assert GameActivity.fillToken==GameActivity.nativeTouchToolToken()&&GameActivity.fillSize==GameActivity.gameSize&&GameActivity.fillEpoch==17;noExtra(t);
  for(int changed=0;changed<7;changed++){
   t=fillLayer(1);t.onTouchEvent(one(0,600,500));Handler.advance(450);GameActivity.events.clear();
   if(changed==0)GameActivity.generation++;if(changed==1)GameActivity.context|=1024;if(changed==2)GameActivity.gameSize++;if(changed==3)GameActivity.layoutEpoch++;
   if(changed==4)android.view.View.width--;if(changed==5)android.view.View.height--;if(changed==6)GameActivity.context=256;
   t.onTouchEvent(one(1,600,500));clean();assert GameActivity.fillCalls==calls&&GameActivity.events.isEmpty();android.view.View.width=2400;android.view.View.height=1080;
  }
  // The regular context/tool poll and layout-epoch poll discard local preparation too.
  for(int changed=0;changed<3;changed++){
   t=fillLayer(2);t.onTouchEvent(one(0,600,500));Handler.advance(450);GameActivity.events.clear();
   if(changed==0){GameActivity.context|=1024;t.syncContext(GameActivity.context);}
   if(changed==1){GameActivity.generation++;t.syncToolToken(GameActivity.nativeTouchToolToken());}
   if(changed==2){GameActivity.layoutEpoch++;t.pollCommands();}
   t.onTouchEvent(two(5,1,600,500,1000,700));t.onTouchEvent(one(1,600,500));clean();assert GameActivity.fillCalls==calls&&GameActivity.events.isEmpty();
  }
  // A native STARTED result retains one serial, blocks new input, and polls only Fill status.
  t=fillLayer(1);GameActivity.fillReply=(43L<<32)|1;GameActivity.fillStatus=(43L<<32)|1;
  t.onTouchEvent(one(0,600,500));Handler.advance(450);t.onTouchEvent(one(1,600,500));assert GameActivity.fillCalls==++calls;
  int polls=GameActivity.fillPolls,rotatePolls=GameActivity.rotatePolls;t.pollCommands();t.pollCommands();
  assert GameActivity.fillCalls==calls&&GameActivity.fillPolls==polls+2&&GameActivity.fillPollSerial==43&&GameActivity.rotatePolls==rotatePolls;
  GameActivity.events.clear();t.back();tap(t,1000,500);t.releaseAll();t.setBlocked(true);t.setBlocked(false);assert GameActivity.events.isEmpty();
  GameActivity.fillStatus=(42L<<32)|2;t.pollCommands();tap(t,1000,500);assert GameActivity.events.isEmpty();
  GameActivity.fillStatus=43L<<32;t.pollCommands();assert GameActivity.lastNotice.contains("не подтверждён")&&!GameActivity.lastNotice.contains("отменено");
  t.onTouchEvent(two(5,1,600,500,1000,700));t.onTouchEvent(one(1,600,500));clean();assert GameActivity.fillCalls==calls&&GameActivity.events.isEmpty();
  // Actual owner pause/resume cannot reissue STARTED work or commit an armed pre-pause gesture.
  GameActivity a=new GameActivity();GameActivity.active=a;GameActivity.context=512;GameActivity.kind=2;GameActivity.tool=3;GameActivity.fillKind=1;GameActivity.generation++;
  a.touch.onTouchEvent(one(0,600,500));Handler.advance(450);a.onPause();a.onResume();a.touch.onTouchEvent(one(1,600,500));assert GameActivity.fillCalls==calls;
  a.touch.onTouchEvent(one(0,600,500));Handler.advance(450);a.touch.onTouchEvent(one(1,600,500));assert GameActivity.fillCalls==++calls;
  a.onPause();a.onResume();GameActivity.fillStatus=(43L<<32)|5;a.touch.pollCommands();assert GameActivity.fillCalls==calls&&GameActivity.fillPollSerial==43;
  assert GameActivity.lastNotice.contains("до выполнения");a.onDestroy();GameActivity.active=null;
  GameActivity.fillReply=2;GameActivity.fillKind=0;GameActivity.gameSize=(1080L<<32)|2400;GameActivity.layoutEpoch=0;
 }
 static void commandHandoff(){
  for(int kind:new int[]{1,2}){
   GameActivity a=new GameActivity();GameActivity.active=a;GameActivity.context=kind==1?256:512;GameActivity.kind=2;
   GameActivity.tool=kind==1?2:3;GameActivity.fillKind=kind==1?0:1;GameActivity.selectedObject=true;GameActivity.generation++;
   GameActivity.rotateReply=(42L<<32)|1;GameActivity.rotateStatus=(42L<<32)|1;
   GameActivity.fillReply=(43L<<32)|1;GameActivity.fillStatus=(43L<<32)|1;
   int calls=kind==1?GameActivity.rotateCalls:GameActivity.fillCalls;
   a.touch.onTouchEvent(one(0,600,500));Handler.advance(450);a.touch.onTouchEvent(one(1,600,500));if(kind==1)a.touch.rotateObjectPreview();
   assert (kind==1?GameActivity.rotateCalls:GameActivity.fillCalls)==calls+1;
   GameActivity b=new GameActivity();b.claim();GameActivity.active=b;b.touch.setBlocked(false);GameActivity.events.clear();
   int polls=kind==1?GameActivity.rotatePolls:GameActivity.fillPolls;
   a.touch.pollCommands();a.onPause();a.onWindowFocusChanged(false);a.touch.onTouchEvent(one(3,600,500));
   assert (kind==1?GameActivity.rotatePolls:GameActivity.fillPolls)==polls;
   b.onBackPressed();tap(b.touch,1000,500);assert GameActivity.events.isEmpty();
   b.touch.pollCommands();assert (kind==1?GameActivity.rotatePollSerial:GameActivity.fillPollSerial)==(kind==1?42:43);
   assert (kind==1?GameActivity.rotateCalls:GameActivity.fillCalls)==calls+1;
   GameActivity.rotateStatus=(42L<<32)|2;GameActivity.fillStatus=(43L<<32)|2;b.touch.pollCommands();
   b.onBackPressed();assert GameActivity.events.contains("key:"+72+":true");clean();b.onDestroy();GameActivity.active=null;
  }
  GameActivity.rotateReply=GameActivity.fillReply=2;GameActivity.fillKind=0;
 }
 static void multi(){
  for(int mode:new int[]{64,128,256,512})for(int lift:new int[]{0,1}){
   TouchLayer resumed=layer(mode,2,1);GameActivity.events.clear();int clicks=GameActivity.clicks;
   resumed.onTouchEvent(one(0,500,500));resumed.onTouchEvent(two(5,1,500,500,800,500));
   resumed.onTouchEvent(two(2,0,500,470,800,530));resumed.onTouchEvent(two(6,lift,500,470,800,530));
   int remaining=lift==0?19:7,commands=GameActivity.cameraCommands;
   resumed.onTouchEvent(event(2,0,new int[]{remaining},new float[]{900},new float[]{600}));
   assert GameActivity.cameraCommands==commands+1;
   resumed.onTouchEvent(event(5,1,new int[]{remaining,55},new float[]{900,1200},new float[]{600,600}));
   resumed.onTouchEvent(event(2,0,new int[]{remaining,55},new float[]{880,1220},new float[]{580,620}));
   assert GameActivity.cameraCommands==commands+2;
   resumed.onTouchEvent(event(6,1,new int[]{remaining,55},new float[]{880,1220},new float[]{580,620}));
   resumed.onTouchEvent(event(1,0,new int[]{remaining},new float[]{880},new float[]{580}));clean();
   assert GameActivity.clicks==clicks&&GameActivity.events.stream().noneMatch(s->s.startsWith("button:"));
  }
  for(int lift:new int[]{0,1}){
   TouchLayer t=layer(4|1024,2,0);int wheel=GameActivity.wheel;t.onTouchEvent(one(0,500,500));t.onTouchEvent(two(5,1,500,500,700,500));t.onTouchEvent(two(2,0,400,500,800,500));assert GameActivity.wheel>wheel&&!GameActivity.mouse;
   t.onTouchEvent(two(6,lift,400,500,800,500));wheel=GameActivity.wheel;int remaining=lift==0?19:7;t.onTouchEvent(event(2,0,new int[]{remaining},new float[]{1200},new float[]{700}));t.onTouchEvent(event(1,0,new int[]{remaining},new float[]{1200},new float[]{700}));assert GameActivity.wheel==wheel;clean();
  }
  TouchLayer buildCamera=layer(512,2,3);int beforeClicks=GameActivity.clicks,beforeWheel=GameActivity.wheel;buildCamera.onTouchEvent(one(0,500,500));buildCamera.onTouchEvent(two(5,1,500,500,700,500));buildCamera.onTouchEvent(two(2,0,400,500,800,500));assert GameActivity.cameraZoom>0&&GameActivity.clicks==beforeClicks;noExtra(buildCamera);
  TouchLayer t=layer(4,1,0);int pans=GameActivity.casPans,clicks=GameActivity.clicks;t.onTouchEvent(one(0,500,500));t.onTouchEvent(two(5,1,500,500,700,500));t.onTouchEvent(two(2,0,500,600,700,600));assert GameActivity.casPans==pans+1&&GameActivity.clicks==clicks;noExtra(t);
  t=layer(128,2,1);t.onTouchEvent(one(0,400,400));t.onTouchEvent(two(5,1,400,400,700,400));GameActivity.kind=1;t.onTouchEvent(three(5,2));t.onTouchEvent(three(2,0));int wheel=GameActivity.wheel;t.onTouchEvent(three(6,0));t.onTouchEvent(event(2,0,new int[]{19,31},new float[]{300,1500},new float[]{400,400}));assert GameActivity.wheel==wheel;noExtra(t);
 }
 static void contextHold(){
  for(int cancel=0;cancel<3;cancel++){
   TouchLayer t=layer(128,1,1);GameActivity.events.clear();int clicks=GameActivity.clicks;
   t.onTouchEvent(one(0,900,700));Handler.advance(450);assert GameActivity.rightMouse;
   if(cancel==0)t.onTouchEvent(one(1,900,700));else if(cancel==1)t.releaseAll();else t.back();
   clean();assert GameActivity.clicks==clicks;
   assert GameActivity.events.contains("right:true")&&GameActivity.events.contains("right:false");
   assert GameActivity.events.stream().noneMatch(s->s.startsWith("button:"));
  }
  for(int kind:new int[]{5,17}){
   TouchLayer t=layer(128,kind,1);GameActivity.events.clear();t.onTouchEvent(one(0,900,700));Handler.advance(450);
   assert !GameActivity.rightMouse;t.onTouchEvent(one(1,900,700));noExtra(t);
  }
 }
 static void widgetSwipes(){
  for(int kind:new int[]{41,105}){
   TouchLayer t=layer(64,kind,0);int scrolls=GameActivity.scrolls,clicks=GameActivity.clicks,amount=GameActivity.scrollAmount;
   t.onTouchEvent(one(0,900,800));t.onTouchEvent(one(2,836,800));t.onTouchEvent(one(1,836,800));clean();
   assert GameActivity.scrolls==scrolls+1&&GameActivity.scrollAmount==amount-1&&GameActivity.clicks==clicks&&!GameActivity.mouse;
  }

  for(int flag:new int[]{256,512}){
   TouchLayer t=layer(128,1|flag,1);int clicks=GameActivity.clicks,scrolls=GameActivity.scrolls,amount=GameActivity.scrollAmount;
   t.onTouchEvent(one(0,500,500));t.onTouchEvent(one(2,flag==256?564:500,flag==512?540:500));
   assert GameActivity.scrolls==scrolls+1&&GameActivity.scrollAmount==amount+1&&!GameActivity.mouse;
   t.onTouchEvent(one(2,flag==256?436:500,flag==512?460:500));
   assert GameActivity.scrollAmount==amount-1;t.onTouchEvent(one(1,500,500));clean();assert GameActivity.clicks==clicks;
   t=layer(128,1|flag,1);tap(t,500,500);clean();assert GameActivity.clicks==clicks+1;
   GamePreferences.swipes=false;t=layer(128,1|flag,1);scrolls=GameActivity.scrolls;
   t.onTouchEvent(one(0,500,500));t.onTouchEvent(one(2,600,600));assert !GameActivity.mouse&&GameActivity.scrolls==scrolls;noExtra(t);GamePreferences.swipes=true;
   t=layer(128,1|flag,1);scrolls=GameActivity.scrolls;t.onTouchEvent(one(0,500,500));GameActivity.layoutEpoch++;
   t.onTouchEvent(one(2,600,600));assert GameActivity.scrolls==scrolls;noExtra(t);
   t=layer(128,1|flag,1);scrolls=GameActivity.scrolls;
   t.onTouchEvent(one(0,500,500));t.onTouchEvent(one(2,flag==256?505:500,flag==512?505:500));
   t.onTouchEvent(one(2,flag==256?531:500,flag==512?519:500));assert GameActivity.scrolls==scrolls;
   t.onTouchEvent(one(2,flag==256?532:500,flag==512?520:500));assert GameActivity.scrolls==scrolls+1;
   // Keep the original widget as owner when the finger leaves its bounds.
   GameActivity.kind=2;t.onTouchEvent(one(2,flag==256?900:500,flag==512?900:500));assert GameActivity.scrolls>scrolls+1&&!GameActivity.mouse;
   noExtra(t);
  }
  TouchLayer t=layer(64,129,0);int cheats=GameActivity.cheatRequests,clicks=GameActivity.clicks;
  t.onTouchEvent(one(0,500,500));Handler.advance(450);assert GameActivity.cheatRequests==cheats+1;
  t.onTouchEvent(one(1,500,500));clean();assert GameActivity.clicks==clicks;
  t=layer(64,129,0);tap(t,500,500);clean();assert GameActivity.clicks==clicks+1;
 }
 static void cancelTap(){
  for(int mode:new int[]{64,128,256,512}){
   TouchLayer tilt=layer(mode,2,1);GameActivity.cameraPitch=GameActivity.cameraTurn=GameActivity.cameraDx=GameActivity.cameraDy=0;
   tilt.onTouchEvent(one(0,500,500));tilt.onTouchEvent(two(5,1,500,500,900,500));
   tilt.onTouchEvent(two(2,0,500,680,900,680));
   assert Math.abs(GameActivity.cameraPitch-(float)Math.PI/6)<.0001f;
   assert GameActivity.cameraTurn==0&&GameActivity.cameraDx==0&&GameActivity.cameraDy==0;
   noExtra(tilt);
  }
  TouchLayer town=layer(64,2,0);GameActivity.cameraDx=GameActivity.cameraDy=0;int commands=GameActivity.cameraCommands;GameActivity.events.clear();
  town.onTouchEvent(one(0,500,500));town.onTouchEvent(one(2,740,608));
  assert GameActivity.cameraCommands==commands+1&&Math.abs(GameActivity.cameraDx-.1f)<.0001f&&Math.abs(GameActivity.cameraDy-.1f)<.0001f;
  town.onTouchEvent(one(1,740,608));clean();assert GameActivity.events.stream().noneMatch(s->s.startsWith("key:"));
  town=layer(64,2,0);GameActivity.cameraPitch=GameActivity.cameraTurn=0;commands=GameActivity.cameraCommands;
  town.onTouchEvent(one(0,500,500));town.onTouchEvent(two(5,1,500,500,700,500));
  town.onTouchEvent(two(2,0,500,480,700,520));assert GameActivity.cameraCommands==commands+1&&Math.abs(GameActivity.cameraTurn-(float)Math.atan2(40,200))<.0001f&&GameActivity.cameraPitch==0;noExtra(town);
  town=layer(64,2,0);GameActivity.cameraPitch=0;town.onTouchEvent(one(0,500,500));town.onTouchEvent(two(5,1,500,500,700,500));town.onTouchEvent(two(2,0,500,500,700,540));assert GameActivity.cameraPitch==0;noExtra(town);
  for(int mode:new int[]{256,512})for(int tool:new int[]{2,3,4,5,6})for(int lift:new int[]{0,1}){
   TouchLayer t=layer(mode,2,tool);GameActivity.events.clear();int clicks=GameActivity.clicks;
   t.onTouchEvent(one(0,500,500));Handler.block(60);t.onTouchEvent(two(5,1,500,500,700,500));
   Handler.block(40);t.onTouchEvent(two(6,lift,500,500,700,500));
   t.onTouchEvent(event(1,0,new int[]{lift==0?19:7},new float[]{lift==0?700:500},new float[]{500}));
   assert GameActivity.events.contains("key:"+72+":true")&&GameActivity.clicks==clicks;clean();
  }
  // Reuse the actual view: the previous swipe/hold flags must not poison the next chord.
  for(int previous=0;previous<3;previous++){
   TouchLayer t=layer(256,previous==2?1:2,previous==2?1:2);
   t.onTouchEvent(one(0,500,500));
   if(previous==0)t.onTouchEvent(one(2,560,500));
   if(previous==1)Handler.advance(450);
   t.onTouchEvent(one(1,560,500));
   if(previous==2){GameActivity.kind=2;GameActivity.tool=2;GameActivity.generation++;}
   GameActivity.events.clear();
   t.onTouchEvent(one(0,600,500));t.onTouchEvent(two(5,1,600,500,800,500));
   t.onTouchEvent(two(6,1,600,500,800,500));assert GameActivity.events.contains("key:72:true");
   t.onTouchEvent(one(1,600,500));clean();
  }
  for(int gap:new int[]{0,120,180,220})for(int lift:new int[]{0,1}){
   TouchLayer t=layer(256,2,2);GameActivity.events.clear();int clicks=GameActivity.clicks,clears=GameActivity.catalogClears;
   t.onTouchEvent(one(0,500,500));Handler.block(gap);t.onTouchEvent(two(5,1,500,500,700,500));
   Handler.block(40);t.onTouchEvent(two(2,0,510,504,690,496));t.onTouchEvent(two(6,lift,510,504,690,496));
   assert GameActivity.events.contains("key:72:true")&&GameActivity.catalogClears==clears+1;
   assert GameActivity.events.indexOf("key:72:true")<GameActivity.events.indexOf("catalog-clear");Handler.block(500);
   t.onTouchEvent(event(1,0,new int[]{lift==0?19:7},new float[]{500},new float[]{500}));clean();assert GameActivity.clicks==clicks&&GameActivity.catalogClears==clears+1;
  }
  // Actual device injector timing/coordinates, including its stationary MOVE events and control poll.
  // Physical2340x1080 maps to game1170x540; UI scale does not change the gesture's pixel coordinates.
  android.view.View.width=2340;long previousSize=GameActivity.gameSize;GameActivity.gameSize=(540L<<32)|1170;
  for(int elapsed:new int[]{77,181})for(int lift:new int[]{0,1}){
   TouchLayer t=layer(256,2,2);GameActivity.events.clear();int clicks=GameActivity.clicks;
   t.onTouchEvent(event(0,0,new int[]{0},new float[]{1530},new float[]{350}));
   Handler.block(26);t.onTouchEvent(event(5,1,new int[]{0,1},new float[]{1530,1670},new float[]{350,350}));
   Handler.block((elapsed-26)/2);t.onTouchEvent(event(2,0,new int[]{0,1},new float[]{1530,1670},new float[]{350,350})
       .withHistory(new float[][]{{1533,1667}},new float[][]{{353,347}}));
   t.syncContext(GameActivity.context);t.syncToolToken(GameActivity.nativeTouchToolToken());t.pollCommands();
   Handler.block(elapsed-26-(elapsed-26)/2);
   t.onTouchEvent(event(6,lift,new int[]{0,1},new float[]{1530,1670},new float[]{350,350}));
   t.onTouchEvent(event(1,0,new int[]{lift==0?1:0},new float[]{lift==0?1670:1530},new float[]{350}));
   assert GameActivity.events.contains("key:72:true")&&GameActivity.clicks==clicks;clean();
  }
  android.view.View.width=2400;GameActivity.gameSize=previousSize;
  for(int reject=0;reject<6;reject++){
   TouchLayer t=layer(512,2,3);GameActivity.events.clear();t.onTouchEvent(one(0,500,500));
   Handler.block(reject==0?221:50);t.onTouchEvent(two(5,1,500,500,700,500));
   if(reject==1)t.onTouchEvent(two(2,0,501,530,701,530));
   if(reject==2)Handler.block(451);
   if(reject==3)GameActivity.generation++;
   if(reject==4)GameActivity.layoutEpoch++;
   if(reject==5){t.onTouchEvent(one(3,500,500));}
   t.onTouchEvent(two(6,0,500,500,700,500));t.onTouchEvent(event(1,0,new int[]{19},new float[]{700},new float[]{500}));
   assert !GameActivity.events.contains("key:27:true")&&!GameActivity.events.contains("key:72:true");noExtra(t);
  }
  // Android can batch a pan that returns to its start into a single MOVE with intermediate history.
  TouchLayer batched=layer(256,2,2);GameActivity.events.clear();
  batched.onTouchEvent(one(0,500,500));Handler.block(26);batched.onTouchEvent(two(5,1,500,500,700,500));
  Handler.block(60);batched.onTouchEvent(two(2,0,500,500,700,500).withHistory(new float[][]{{550,750}},new float[][]{{500,500}}));
  batched.onTouchEvent(two(6,1,500,500,700,500));batched.onTouchEvent(one(1,500,500));
  assert !GameActivity.events.contains("key:27:true");noExtra(batched);
  int[] zoom=new int[2];
  for(int n=0;n<2;n++){
   TouchLayer t=layer(n==0?64:4,2,0);int wheel=GameActivity.wheel;
   t.onTouchEvent(one(0,500,500));t.onTouchEvent(two(5,1,500,500,700,500));
   t.onTouchEvent(two(2,0,484,500,716,500));zoom[n]=GameActivity.wheel-wheel;noExtra(t);
  }
  assert zoom[0]==0&&zoom[1]==360&&GameActivity.cameraZoom>0;
  // Fractional moves accumulate complete wheel detents, pinned over the original scroll widget.
  TouchLayer t=layer(8,9,0);int wheel=GameActivity.wheel,clicks=GameActivity.clicks;
  t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,600,509));assert GameActivity.wheel==wheel;
  t.onTouchEvent(one(2,600,529));assert GameActivity.wheel==wheel;
  t.onTouchEvent(one(2,600,530));assert GameActivity.wheel==wheel+120;
  t.onTouchEvent(one(1,900,700));clean();assert GameActivity.clicks==clicks&&Math.round(GameActivity.x*2400)==600&&Math.round(GameActivity.y*1080)==500;
  t=layer(4,9,0);wheel=GameActivity.wheel;clicks=GameActivity.clicks;
  t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,609,501));assert GameActivity.wheel==wheel;
  t.onTouchEvent(one(2,629,502));assert GameActivity.wheel==wheel;
  t.onTouchEvent(one(2,630,502));assert GameActivity.wheel==wheel+120;
  t.onTouchEvent(one(2,630,900));assert GameActivity.wheel==wheel+120;
  t.onTouchEvent(one(1,630,900));clean();assert GameActivity.clicks==clicks;
  t=layer(8|1|1024,25,0);wheel=GameActivity.wheel;int wheelEvents=GameActivity.events.size();
  t.onTouchEvent(one(0,600,500));
  for(int y=501;y<=799;y++)t.onTouchEvent(one(2,600,y));
  assert GameActivity.wheel==wheel+9*120&&!GameActivity.mouse;
  int detents=0;for(int n=wheelEvents;n<GameActivity.events.size();n++)if(GameActivity.events.get(n).equals("wheel"))detents++;
  assert detents==9;t.onTouchEvent(one(3,600,799));clean();
  t=layer(8,25,0);wheel=GameActivity.wheel;wheelEvents=GameActivity.events.size();
  t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,600,680));
  assert GameActivity.wheel==wheel+6*120;
  detents=0;for(int n=wheelEvents;n<GameActivity.events.size();n++)if(GameActivity.events.get(n).equals("wheel"))detents++;
  assert detents==6;t.onTouchEvent(one(1,600,680));clean();
  t=layer(4,41,0);int scrolls=GameActivity.scrolls;wheel=GameActivity.wheel;clicks=GameActivity.clicks;
  t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,664,500));assert GameActivity.scrolls==scrolls+1;
  t.onTouchEvent(one(1,664,500));clean();assert GameActivity.clicks==clicks&&GameActivity.wheel==wheel;
  int infos=GameActivity.infos;t=layer(512,73,0);t.onTouchEvent(one(0,600,500));Handler.advance(449);assert GameActivity.infos==infos;
  Handler.advance(1);assert GameActivity.infos==infos+1;t.onTouchEvent(one(1,600,500));clean();assert GameActivity.clicks==clicks;
  t=layer(512,73,0);t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,600,560));Handler.advance(500);t.onTouchEvent(one(1,600,560));clean();assert GameActivity.infos==infos+1&&GameActivity.clicks==clicks;
 }
 static void settingsChord(){
  TouchLayer disabled=layer(64,2,0);int settingsBefore=GameActivity.settingsRequests;
  disabled.onTouchEvent(one(0,400,400));disabled.onTouchEvent(two(5,1,400,400,700,400));disabled.onTouchEvent(three(5,2));disabled.onTouchEvent(three(6,2));disabled.onTouchEvent(two(6,1,400,400,700,400));disabled.onTouchEvent(one(1,400,400));
  assert GameActivity.settingsRequests==settingsBefore;noExtra(disabled);

  for(int mode:new int[]{0,16,64}){
   TouchLayer t=layer(mode,0,0);int requests=GameActivity.settingsRequests,clicks=GameActivity.clicks;
   t.onTouchEvent(one(0,400,400));Handler.block(100);t.onTouchEvent(two(5,1,400,400,700,400));Handler.block(100);t.onTouchEvent(three(5,2));Handler.block(100);t.onTouchEvent(three(6,2));t.onTouchEvent(two(6,1,400,400,700,400));t.onTouchEvent(one(1,400,400));assert GameActivity.settingsRequests==requests&&GameActivity.clicks==clicks;clean();
  }
  for(int cancel:new int[]{0,1,2}){
   TouchLayer t=layer(0,0,0);int requests=GameActivity.settingsRequests;t.onTouchEvent(one(0,400,400));t.onTouchEvent(two(5,1,400,400,700,400));t.onTouchEvent(three(5,2));
   if(cancel==0)t.onTouchEvent(one(3,400,400));else{if(cancel==1)Handler.block(751);else t.onTouchEvent(event(2,0,new int[]{7,19,31},new float[]{500,700,1000},new float[]{400,400,400}));t.onTouchEvent(three(6,2));t.onTouchEvent(two(6,1,400,400,700,400));t.onTouchEvent(one(1,400,400));}assert GameActivity.settingsRequests==requests;noExtra(t);
  }
 }
 static void floorGestures(){
  for(int mode:new int[]{128,256,512})for(int direction:new int[]{-1,1})for(int lift=0;lift<3;lift++){
   TouchLayer t=layer(mode,2,1);GameActivity.events.clear();int clicks=GameActivity.clicks;
   t.onTouchEvent(one(0,400,400));Handler.block(40);t.onTouchEvent(two(5,1,400,400,700,400));
   Handler.block(40);t.onTouchEvent(three(5,2));
   MotionEvent move=event(2,0,new int[]{7,19,31},new float[]{400,700,1000},new float[]{400+direction*90,400+direction*100,400+direction*110});
   t.onTouchEvent(move);t.onTouchEvent(move);int key=direction<0?33:34;
   assert GameActivity.events.stream().filter(s->s.equals("key:"+key+":true")).count()==1;
   t.onTouchEvent(event(6,lift,new int[]{7,19,31},new float[]{400,700,1000},new float[]{300,300,300}));
   int[] rest=lift==0?new int[]{19,31}:lift==1?new int[]{7,31}:new int[]{7,19};
   t.onTouchEvent(event(2,0,rest,new float[]{600,900},new float[]{600,600}));
   t.onTouchEvent(event(6,1,rest,new float[]{600,900},new float[]{600,600}));
   t.onTouchEvent(event(1,0,new int[]{rest[0]},new float[]{600},new float[]{600}));clean();
   assert GameActivity.clicks==clicks&&GameActivity.events.stream().noneMatch(s->s.startsWith("button:"));noExtra(t);
  }
  for(int reject=0;reject<8;reject++){
   TouchLayer t=layer(reject==0?64:128,2,1);GameActivity.events.clear();
   t.onTouchEvent(one(0,400,400));if(reject==1)Handler.block(251);
   t.onTouchEvent(two(5,1,400,400,700,400));if(reject==2)t.onTouchEvent(two(2,0,400,420,700,420));
   t.onTouchEvent(three(5,2));
   if(reject==3)GameActivity.layoutEpoch++;
   if(reject==4)GameActivity.context=2;
   if(reject==5)t.onTouchEvent(event(5,3,new int[]{7,19,31,55},new float[]{400,700,1000,1200},new float[]{400,400,400,400}));
   t.onTouchEvent(event(2,0,new int[]{7,19,31},new float[]{reject==6?600:400,700,1000},new float[]{300,reject==7?500:300,300}));
   assert !GameActivity.events.contains("key:33:true")&&!GameActivity.events.contains("key:34:true"):"floor reject "+reject;noExtra(t);
  }
 }
 static void gestureSafety(){
  // A second touch can begin a camera gesture before the delayed first world tap has fired.
  for(int mode:new int[]{64,128}){
   TouchLayer t=layer(mode,2,1);int clicks=GameActivity.clicks;tap(t,500,500);
   t.onTouchEvent(one(0,600,500));t.onTouchEvent(two(5,1,600,500,900,500));
   t.onTouchEvent(two(2,0,500,550,1000,550));t.onTouchEvent(two(6,1,500,550,1000,550));
   t.onTouchEvent(one(1,500,550));clean();assert GameActivity.clicks==clicks;noExtra(t);
  }
  // Native interaction-menu entries and ordinary buttons commit only an unmoved tap.
  for(int kind:new int[]{1,129}){
   TouchLayer t=layer(128,kind,1);int clicks=GameActivity.clicks;
   t.onTouchEvent(one(0,600,500));t.onTouchEvent(one(2,700,550));t.onTouchEvent(one(1,700,550));clean();
   assert GameActivity.clicks==clicks&&!GameActivity.mouse;noExtra(t);
  }
  TouchLayer chooser=layer(8,257,0);int scrolls=GameActivity.scrolls,clicks=GameActivity.clicks;
  chooser.onTouchEvent(one(0,1550,500));
  for(int x=1500;x>=950;x-=50)chooser.onTouchEvent(one(2,x,500));
  chooser.onTouchEvent(one(2,1700,500));chooser.onTouchEvent(one(1,1700,500));clean();
  assert GameActivity.scrolls==scrolls+1&&GameActivity.clicks==clicks;
  chooser.onTouchEvent(one(0,950,500));chooser.onTouchEvent(one(2,1550,500));chooser.onTouchEvent(one(1,1550,500));clean();
  assert GameActivity.scrolls==scrolls+2&&GameActivity.clicks==clicks;noExtra(chooser);
 }
 static void controller(){
  assert GamepadLayer.normalize(Float.NaN,0)==0&&GamepadLayer.normalize(.1f,0)==0;
  assert Math.abs(GamepadLayer.normalize(.58f,0)-.25f)<.0001f;
  assert GamepadLayer.normalize(-1,0)==-1&&GamepadLayer.normalize(5,0)==1;
  TouchLayer t=layer(128,1,0);int clicks=GameActivity.clicks;t.controllerClick(.3f,.4f,false);clean();assert GameActivity.clicks==clicks+1;
  t=layer(128,1,0);clicks=GameActivity.clicks;t.controllerClick(.3f,.4f,true);clean();assert GameActivity.clicks==clicks;
  t=layer(128,2,0);t.controllerClick(.3f,.4f,true);clean();assert GameActivity.clicks==clicks;
  t=layer(256,2,2);t.controllerClick(.3f,.4f,false);clean();assert t.canUseObjectPreview()&&GameActivity.clicks==clicks;
  t.controllerClick(.3f,.4f,false);clean();assert GameActivity.clicks==clicks+1;
  t=layer(64,41,0);int scrolls=GameActivity.scrolls;t.controllerScroll(.3f,.4f,1);assert GameActivity.scrolls==scrolls+1;clean();
 }
 static void actualActivity(){
  GameActivity a=new GameActivity();GameActivity.active=a;a.showTouchSettings();assert a.settingsDialog!=null&&a.settingsDialog.theme==1234&&a.scaleStatus.context!=a&&a.scaleStatus.textColor==0xff000000;int calls=GameActivity.scaleCalls;SeekBar s=SeekBar.latest;
  assert a.settingsDialog.view instanceof ScrollView&&((ScrollView)a.settingsDialog.view).child instanceof LinearLayout&&((ScrollView)a.settingsDialog.view).context==a.scaleStatus.context;
  s.listener.onProgressChanged(s,150,true);s.listener.onProgressChanged(s,180,true);assert GameActivity.scaleCalls==calls;
  s.listener.onStopTrackingTouch(s);assert GameActivity.scaleCalls==calls+1&&GameActivity.uiScale==280;a.settingsDialog.dismiss();assert GameActivity.scaleCalls==calls+1;
  a.showTouchSettings();s=SeekBar.latest;GameActivity.scaleAccept=false;calls=GameActivity.scaleCalls;s.listener.onProgressChanged(s,150,true);s.listener.onStopTrackingTouch(s);a.settingsDialog.dismiss();assert GameActivity.scaleCalls==calls+1;GameActivity.scaleAccept=true;
  GameActivity.uiScale=200;GameActivity.scaleApply=false;a.showTouchSettings();s=SeekBar.latest;calls=GameActivity.scaleCalls;int noticeCount=GameActivity.notices;
  s.listener.onProgressChanged(s,200,true);s.listener.onStopTrackingTouch(s);assert GameActivity.scaleCalls==calls+1&&GameActivity.uiScale==200;
  a.settingsDialog.dismiss();assert GameActivity.scaleCalls==calls+1&&GameActivity.notices==noticeCount+1;assert GameActivity.lastNotice.contains("300%")&&GameActivity.lastNotice.contains("200%");GameActivity.scaleApply=true;
  a.showTouchSettings();noticeCount=GameActivity.notices;a.settingsDialog.dismiss();assert GameActivity.notices==noticeCount;
  // Real full-value editor methods prefill Cyrillic and permit replacing with the empty string.
  byte[] original=GameActivity.original;GameActivity.original=new byte[129];java.util.Arrays.fill(GameActivity.original,(byte)65);a.showTextEditor(.25f,.4f);assert a.textDialog==null;GameActivity.original=original;
  a.showTextEditor(.25f,.4f);assert a.textEditor.getText().equals("Сенсор")&&a.textDialog.theme==1234&&a.textEditor.context!=a&&a.textEditor.textColor==0xff000000;calls=GameActivity.cancels;a.onWindowFocusChanged(false);assert GameActivity.cancels==calls;
  assert (a.textEditor.imeOptions&33554432)!=0;a.textEditor.setText("");GameActivity.replaceReceipt=2;assert a.textEditor.editorAction.action(null,6,null);assert GameActivity.replaced.length==0&&a.textDialog==null;
  a.showTextEditor(.25f,.4f);a.textEditor.setText("Анна");GameActivity.replaceReceipt=(42L<<32)|1;int replaces=GameActivity.replaces;a.textDialog.positive.click();assert a.textSerial==42&&!a.textDialog.positive.enabled;
  a.handleTextReceipt((42L<<32)|1);assert GameActivity.replaces==replaces+1;a.onPause();a.onResume();assert a.textSerial==42;
  a.handleTextReceipt((41L<<32)|2);assert a.textSerial==42;a.handleTextReceipt((42L<<32)|4);assert a.textSerial==0&&a.textEditor.getText().equals("Анна")&&a.textEditor.error!=null;
  a.textDialog.cancel();assert a.textDialog==null;int shows=android.view.inputmethod.InputMethodManager.shows;Handler.advance(0);assert android.view.inputmethod.InputMethodManager.shows==shows;
  // Actual owner callbacks retire A before B, preserving B's native held slider and pending UI pulse.
  GameActivity.context=4;GameActivity.kind=5;GameActivity.tool=0;GameActivity b=new GameActivity();b.claim();GameActivity.active=b;b.touch.setBlocked(false);
  b.touch.onTouchEvent(one(0,600,500));b.touch.onTouchEvent(one(2,700,500));assert GameActivity.mouse;calls=GameActivity.cancels;
  a.onPause();a.onWindowFocusChanged(false);a.onDestroy();a.touch.onTouchEvent(one(3,500,500));a.touch.onSizeChanged(2400,1080,2400,1080);assert GameActivity.mouse&&GameActivity.cancels==calls;
  b.touch.onTouchEvent(one(1,700,500));clean();int beforeClicks=GameActivity.clicks;tap(b.touch,400,300);a.touch.onTouchEvent(one(3,500,500));a.touch.onSizeChanged(2400,1080,2400,1080);Handler.advance(50);assert GameActivity.mouse&&GameActivity.clicks==beforeClicks+1;Handler.advance(90);assert !GameActivity.mouse;
  b.showTextEditor(.25f,.4f);calls=GameActivity.cancels;a.cancelTextSession();a.onWindowFocusChanged(false);assert GameActivity.cancels==calls;b.textDialog.cancel();
  android.view.Surface sa=new android.view.Surface(),sb=new android.view.Surface();a.surfaceCreated(()->sa);b.surfaceCreated(()->sb);assert GameActivity.currentSurface==sb;a.surfaceDestroyed(()->sa);assert GameActivity.currentSurface==sb;b.surfaceDestroyed(()->sb);assert GameActivity.currentSurface==null;
  b.onDestroy();GameActivity.active=null;
  GameActivity failed=new GameActivity();failed.started=false;failed.touch=new TouchLayer(failed);GameActivity.active=failed;
  int nativeCancels=GameActivity.cancels,nativeSurfaces=GameActivity.surfaceCalls,nativePauses=GameActivity.pauseCalls;
  failed.onWindowFocusChanged(false);failed.cancelTextSession();failed.surfaceCreated(()->sa);failed.surfaceDestroyed(()->sa);failed.onPause();failed.onDestroy();
  assert GameActivity.cancels==nativeCancels&&GameActivity.surfaceCalls==nativeSurfaces&&GameActivity.pauseCalls==nativePauses;
 }
 static void platform(){
  GameActivity a=new GameActivity();a.createObjectControls(new FrameLayout(a));
  android.view.WindowInsets safe=new android.view.WindowInsets(80,0,5,12);a.applySafeInsets(safe);
  FrameLayout.LayoutParams p=(FrameLayout.LayoutParams)a.objectControls.getLayoutParams();assert p.leftMargin==80&&p.rightMargin==13&&p.bottomMargin==12;
  FrameLayout.LayoutParams help=(FrameLayout.LayoutParams)a.helpButton.getLayoutParams();assert help.width==52&&help.height==48&&help.gravity==(android.view.Gravity.RIGHT|android.view.Gravity.TOP);
  assert help.leftMargin==80&&help.rightMargin==13&&help.topMargin==8&&help.bottomMargin==12;
  safe=new android.view.WindowInsets(0,2,80,0);safe.corners[0]=new android.view.RoundedCorner(100);safe.corners[3]=new android.view.RoundedCorner(50);a.applySafeInsets(safe);
  assert p.leftMargin==30&&p.topMargin==30&&p.rightMargin==88&&p.bottomMargin==15;
  assert help.leftMargin==30&&help.topMargin==38&&help.rightMargin==88&&help.bottomMargin==15;
  android.os.Build.VERSION.SDK_INT=30;a.applySafeInsets(safe);assert p.leftMargin==0&&p.topMargin==2&&p.bottomMargin==0;android.os.Build.VERSION.SDK_INT=29;safe.cutout=new android.view.DisplayCutout(90,4,1,17);a.applySafeInsets(safe);
  assert p.leftMargin==90&&p.topMargin==4&&p.rightMargin==88&&p.bottomMargin==17;
  assert help.leftMargin==90&&help.topMargin==12&&help.rightMargin==88&&help.bottomMargin==17;
  safe.cutout=null;a.applySafeInsets(safe);assert p.leftMargin==0&&p.topMargin==2&&p.bottomMargin==0;
  android.os.Build.VERSION.SDK_INT=36;
  assert GameActivity.userdataName(null).equals("userdata");assert GameActivity.userdataName("userdata-qa-audit-20261104").equals("userdata-qa-audit-20261104");
  for(String bad:new String[]{"", "userdata", "../userdata", "userdata-qa-../userdata", "userdata-qa-", "userdata-qa-a/b", "userdata-qa-a\\b"}){
   boolean rejected=false;try{GameActivity.userdataName(bad);}catch(IllegalArgumentException e){rejected=true;}assert rejected:bad;
  }
  android.view.Display.Mode m=new android.view.Display.Mode(2400,1080,60);
  android.view.Display display=new android.view.Display(m,m,new android.view.Display.Mode(2400,1080,59.94f),new android.view.Display.Mode(2400,1080,120),new android.view.Display.Mode(2400,1080,40),new android.view.Display.Mode(1920,1080,144),new android.view.Display.Mode(2400,1080,25));
  assert GameActivity.displayRates(display).equals("40,60,120");
 }
 public static void main(String[] args)throws Exception {touchUx();world();navigationMouseMoves();cameraContacts();directTools();ui();multi();contextHold();widgetSwipes();cancelTap();settingsChord();floorGestures();gestureSafety();actualActivity();radialMenu();controller();objectPreview();objectRotation();catalogDrag();areaFill();commandHandoff();platform();System.out.println("Native-first touch PASS: guarded objectpreview/buttons/commit-once, kind6click-only, camera/tap2 distinction, remaining finger/rejoined pinch, UI hold right click and balanced cancellation, natural yaw, Escape order, pinned UI, Town0, CAS model/pan/3xzoom, batched motion, chooser swipes, three-finger floors, no gesture clicks, full text/scale, held-object twist/fill serial, Activity ownership, safe insets/rotation, QA profile validation and display rates");}
}
'''

def method(source,signature):
    start=source.index(signature);opening=source.index('{',start);depth=1;end=opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end].replace('private ','')

def main():
    activity=(ROOT/'android/app/src/org/sims2/port/GameActivity.java').read_text(encoding='utf-8')
    for obsolete in ['touchPanel','addTouchControls','profileButton','HorizontalScrollView','nativeText(','loadControls(','hudScale']:
        assert obsolete not in activity,obsolete
    assert 'root.addView(surface' in activity and 'root.addView(touch' in activity
    assert 'touch.pollCommands();' in activity, 'command polls reuse the existing Activity context timer'
    native=NATIVE
    for signature in ['static void touchObjectControlsChanged(','private Button objectButton(',
                      'private void createObjectControls(','private void refreshObjectControls(',
                      'private boolean toolMenuValid(','private void closeToolMenu(','private void showToolMenu(',
                      'private void applySafeInsets(','static String userdataName(','static String displayRates(',
                      'private boolean ownsInput(','private void submitScale(','private void showTouchSettings(','private void showTimeHint(',
                      'private void showTextEditor(','private void handleTextReceipt(','private void cancelTextSession(',
                      'public void onWindowFocusChanged(','public void onBackPressed(','public void surfaceCreated(',
                      'public void surfaceDestroyed(','protected void onPause(','protected void onResume(','protected void onDestroy(']:
        native+=method(activity,signature)+'\n'
    start=activity.index('if (active != null && active != this)');end=activity.index('active = this;',start)+len('active = this;')
    native+='void claim(){'+activity[start:end]+'}\n}\n'
    STUBS['org/sims2/port/GamepadLayer.java']='package org.sims2.port;class GamepadLayer{void release(){}void setEnabledInput(boolean b){}'+method((ROOT/'android/app/src/org/sims2/port/GamepadLayer.java').read_text(), 'static float normalize(')+'}'
    STUBS['android/hardware/input/InputManager.java']='package android.hardware.input;public class InputManager{public void unregisterInputDeviceListener(Object o){}}'
    STUBS['org/sims2/port/launcher/PortOptions.java']='package org.sims2.port.launcher;public class PortOptions{public static void append(android.content.Context c,android.widget.LinearLayout l,Object launch){}}'
    sources={**STUBS,'org/sims2/port/GameActivity.java':native,'org/sims2/port/TouchCheck.java':CHECK}
    style=ET.parse(ROOT/'android/app/res/values/styles.xml').getroot().find("style[@name='PortDialog']")
    assert style is not None and style.attrib['parent']=='android:style/Theme.Material.Light.Dialog.Alert'
    colors={item.attrib['name']:item.text for item in style}
    assert colors=={'android:forceDarkAllowed':'false','android:textColorPrimary':'#FF111111','android:textColorSecondary':'#FF333333','android:colorBackground':'#FFF7F7F7'}
    translations=[]
    for folder in ('values','values-ru','values-de','values-pl'):
        translations.append({s.attrib['name']:s.text for s in ET.parse(ROOT/('android/app/res/'+folder+'/strings.xml')).getroot()})
    assert all(set(t)==set(translations[0]) for t in translations)
    for key in translations[0]:
        assert all(sorted(re.findall(r'%[12]\$[ds]',t[key]))==sorted(re.findall(r'%[12]\$[ds]',translations[0][key])) for t in translations)
    names=list(translations[1]);constants=''.join('public static final int '+name+'='+str(i)+';' for i,name in enumerate(names))
    sources['org/sims2/port/R.java']='package org.sims2.port;class R{static class drawable{static final int tool_45_degree=1;static final int tool_day_night=2;static final int tool_design=3;static final int tool_eyedropper=4;static final int tool_hand=5;static final int tool_redo=6;static final int tool_sledgehammer=7;static final int tool_top_down=8;static final int tool_undo=9;}static class style{static final int PortDialog=1234;}static class string{'+constants+'}static String[] values={'+','.join(json.dumps(translations[1][name],ensure_ascii=False) for name in names)+'};}'
    with tempfile.TemporaryDirectory(prefix='sims2-native-ui-') as folder:
        base=Path(folder);paths=[]
        for name,source in sources.items():
            p=base/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(source,encoding='utf-8');paths.append(str(p))
        subprocess.run([str(JDK/'bin/javac.exe'),'--release','8','-encoding','UTF-8','-d',str(base),*paths,str(ROOT/'android/app/src/org/sims2/port/TouchLayer.java')],check=True)
        subprocess.run([str(JDK/'bin/java.exe'),'-ea','-cp',str(base),'org.sims2.port.TouchCheck'],check=True)
    print('Selected-object-only controls, actual Activity styling/settings/text/lifecycle methods PASS')

if __name__=='__main__':main()
