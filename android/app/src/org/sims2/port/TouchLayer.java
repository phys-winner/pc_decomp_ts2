package org.sims2.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.VelocityTracker;
import android.view.ViewConfiguration;
import android.widget.OverScroller;
import java.io.File;
import java.util.HashMap;
import java.util.Map;

/** Native UI owns its gestures; selected objects use an explicit preview/placement step. */
final class TouchLayer extends View {
    static final int MODAL=1,OPTIONS=2,CAS=4,CHOOSER=8,LOADING=16,LOT_INFO=32;
    static final int TOWN=64,LIVE=128,BUY=256,BUILD=512,TEXT_FOCUS=1024;
    private static final int BLOCKERS=MODAL|OPTIONS|CHOOSER|LOADING|LOT_INFO;
    private static final int ESC=0x1B;
    private static final int ROTATE=1,FILL=2;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final Map<Integer,Integer> keys=new HashMap<>();
    private final Map<Integer,float[]> chordPoints=new HashMap<>();
    private final float threshold,density;
    private final OverScroller fling;
    private final int minFling,maxFling;
    private VelocityTracker velocity;
    private boolean flinging,swipeBaseline;
    private int flingX,flingY;
    private long scrollOwner,seenSwipe,swipeExpires;
    private float gestureX,gestureY;
    private String swipeNotice;
    private final Runnable hideSwipe=()->{swipeNotice=null;invalidate();};
    private final Runnable flingTick=new Runnable(){public void run(){
        if(!flinging||!stableOrigin()||!fling.computeScrollOffset()){stopFling();return;}
        int x=fling.getCurrX(),y=fling.getCurrY();
        scrollBy(x-flingX,y-flingY,true);flingX=x;flingY=y;
        handler.postDelayed(this,32);
    }};
    private boolean blocked,retired,held,heldWorld,dragging,consumed,longPressed,multi;
    private boolean camera,uiPan,chordPossible,stream,fillArmed,cameraContact;
    private boolean cancelTap;
    private boolean objectTwist,catalogDrag,singleCamera,tapMoved,floorSwipe,floorDone;
    private int pointer=-1,second=-1,originKind,context,originContext,tool;
    private int optionReturnContext=LIVE;
    private int originFillKind,originWidth,originHeight,commandKind;
    private boolean scrollHorizontal,widgetSwipe,chooserPaged;
    static final int MORE_BUTTON=128,SWIPE_HORIZONTAL=256,SWIPE_VERTICAL=512;
    private long token,originToken,chordTime;
    private long originSize,originEpoch;
    private int commandSerial;
    private float downX,downY,lastX,lastY,camX,camY,camDistance,camAngle,zoomRest,turnRest,scrollRest;
    private Click pending;
    private Preview preview;
    private final Paint timePaint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private boolean timeVisible;
    private int timeSpeed;
    private static final String[] TIME_LABELS={"Ⅱ","1×","2×","3×"};
    private final Runnable longPress=()->{
        if(retired||blocked||pointer<0||dragging||multi||consumed||commandSerial!=0)return;
        longPressed=true;chordPossible=false;
        if((originKind&MORE_BUTTON)!=0&&stableOrigin()){
            consumed=true;GameActivity.touchCheatsRequested();return;
        }
        if((originKind&65)==65&&(originContext&(BUY|BUILD))!=0&&stableOrigin()){
            consumed=true;
            GameActivity.nativeTouchCatalogInfo(downX/originWidth,downY/originHeight,originContext,originSize,originEpoch);
        }
        if(!consumed&&(originKind&3)==1&&(originKind&(4|16))==0&&stableOrigin()){
            consumed=true;pending=new Click(downX,downY,originKind,originContext,originToken);
            pending.button=1;handler.postDelayed(pending.press,0);
        }
        if((originFillKind==1||originFillKind==2)&&(originKind&3)==2&&(int)originToken==3
                &&originContext==BUILD&&!held&&pending==null&&keys.isEmpty()&&stableOrigin()){
            fillArmed=true;consumed=true;
            GameActivity.touchNotice(GameActivity.text(R.string.fill_armed));
        }
        performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
    };
    TouchLayer(Context c){
        super(c);density=c.getResources().getDisplayMetrics().density;threshold=8*density;fling=new OverScroller(c);
        ViewConfiguration config=ViewConfiguration.get(c);minFling=config.getScaledMinimumFlingVelocity();
        maxFling=Math.min(config.getScaledMaximumFlingVelocity(),(int)(4000*density));
    }
    static String readText(File f){
        try(java.io.FileInputStream in=new java.io.FileInputStream(f)){
            byte[] b=new byte[(int)Math.min(f.length(),4096)];int n=in.read(b);
            return n>0?new String(b,0,n,java.nio.charset.StandardCharsets.UTF_8):"";
        }catch(java.io.IOException e){return "";}
    }
    void syncContext(int value){
        if((context&~TEXT_FOCUS)!=(value&~TEXT_FOCUS)){swipeNotice=null;handler.removeCallbacks(hideSwipe);invalidate();}
        if(context!=value&&flinging)stopFling();
        if((value&OPTIONS)!=0&&(context&OPTIONS)==0&&(context&(TOWN|LIVE|BUY|BUILD))!=0)
            optionReturnContext=context&(TOWN|LIVE|BUY|BUILD);
        if(preview!=null&&preview.mode!=value)preview=null;
        if(fillArmed&&value!=originContext)releaseAll();
        if(cameraContact&&(value&(BLOCKERS|TEXT_FOCUS|CAS))!=0)releaseAll();
        if((context&~TEXT_FOCUS)!=(value&~TEXT_FOCUS)&&(held||pending!=null||camera||uiPan||singleCamera||cameraContact))releaseAll();
        context=value;
    }
    void syncToolToken(long value){
        if(preview!=null&&preview.expected!=value)preview=null;
        if(token!=value&&(fillArmed||(held&&(originKind&3)==2)||(pending!=null&&(pending.kind&3)==2&&(!pending.down||pending.capturesWorld()))||(cameraContact&&pointer>=0)))releaseAll();
        token=value;tool=(int)value;
    }
    void setBlocked(boolean value){if(value){swipeNotice=null;releaseAll();}blocked=retired||value;GameActivity.touchObjectControlsChanged();}
    void pollCommands(){
        if(retired)return;
        if(flinging&&!stableOrigin())stopFling();
        long receipt=GameActivity.nativeTouchSwipeFeedback();
        if(!swipeBaseline){seenSwipe=receipt;swipeBaseline=true;}
        if(receipt!=seenSwipe){
            seenSwipe=receipt;
            if(receipt==0){swipeNotice=null;handler.removeCallbacks(hideSwipe);invalidate();}
            String label=swipeLabel(receipt);int position=(int)(receipt>>>32)&15,count=(int)(receipt>>>36)&15;
            if(!blocked&&label!=null&&position>0&&position<=count){
                swipeNotice=label+" · "+position+"/"+count;handler.removeCallbacks(hideSwipe);
                swipeExpires=android.os.SystemClock.uptimeMillis()+1400;
                handler.postDelayed(hideSwipe,1400);invalidate();announceForAccessibility(swipeNotice);
            }
        }
        if(preview!=null&&!preview.matches())preview=null;
        if(fillArmed&&!stableOrigin())releaseAll();
        if(commandSerial!=0)commandReceipt(commandKind==FILL?GameActivity.nativeTouchFillRoomStatus(commandSerial)
                :GameActivity.nativeTouchRotateObjectStatus(commandSerial),commandKind);
    }
    private boolean stableOrigin(){
        return !retired&&!blocked&&getWidth()==originWidth&&getHeight()==originHeight
                &&GameActivity.nativeTouchContext()==originContext&&GameActivity.nativeTouchToolToken()==originToken
                &&GameActivity.nativeTouchGameSize()==originSize&&GameActivity.nativeTouchLayoutEpoch()==originEpoch;
    }
    private void commandReceipt(long receipt,int kind){
        int status=(int)receipt&255,serial=(int)(receipt>>>32);
        if(commandSerial!=0&&serial!=commandSerial)return;
        if(status==1){commandSerial=serial;commandKind=kind;return;}
        commandSerial=0;
        if(kind==ROTATE&&(status==0||status==3||status==7))preview=null;
        if(status==2||retired||blocked)return;
        if(kind==FILL){
            GameActivity.touchNotice(status==4?GameActivity.text(R.string.fill_rejected):
                    status==5?GameActivity.text(R.string.fill_cancelled):status==6?GameActivity.text(R.string.fill_busy):
                    GameActivity.text(R.string.fill_unknown));return;
        }
        GameActivity.touchNotice(status==4?GameActivity.text(R.string.rotate_rejected):
                status==5?GameActivity.text(R.string.rotate_cancelled):status==6?GameActivity.text(R.string.rotate_busy):
                GameActivity.text(R.string.rotate_unknown));
    }
    void takePendingCommand(TouchLayer previous){
        commandSerial=previous.commandSerial;commandKind=previous.commandKind;previous.commandSerial=0;
        optionReturnContext=previous.optionReturnContext;
    }
    void releaseLocal(){swipeNotice=null;clear(false,false);retired=true;blocked=true;}
    void releaseAll(){clear(!retired,heldWorld||(pending!=null&&pending.down&&pending.capturesWorld()));}
    private void clear(boolean send,boolean escape){
        stopFling();releaseVelocity();
        handler.removeCallbacksAndMessages(null);
        if(swipeNotice!=null){
            long remaining=swipeExpires-android.os.SystemClock.uptimeMillis();
            if(remaining>0)handler.postDelayed(hideSwipe,remaining);else swipeNotice=null;
        }
        if(send)cameraContact(false);else cameraContact=false;
        if(send){
            if(escape)GameActivity.nativeKey(ESC,true);
            if(held)GameActivity.nativeMouseButton(0,false);
            if(pending!=null&&pending.down)GameActivity.nativeMouseButton(pending.button,false);
            for(int vk:keys.keySet())GameActivity.nativeKey(vk,false);
            if(escape)GameActivity.nativeKey(ESC,false);
        }
        keys.clear();pending=null;preview=null;held=heldWorld=dragging=camera=uiPan=false;
        timeVisible=false;invalidate();
        pointer=second=-1;consumed=multi=longPressed=false;
        chordPossible=stream=fillArmed=cancelTap=objectTwist=catalogDrag=singleCamera=widgetSwipe=chooserPaged=floorSwipe=floorDone=false;chordPoints.clear();scrollRest=0;
    }
    void back(){
        if(retired||blocked)return;
        stopFling();
        cameraContact(false);
        if(commandSerial!=0){releaseAll();GameActivity.touchNotice(GameActivity.text(R.string.wait_command));return;}
        if(fillArmed){releaseAll();GameActivity.touchNotice(GameActivity.text(R.string.fill_disarmed));return;}
        handler.removeCallbacksAndMessages(null);
        // F5 only opens Options; return with the original world-mode shortcut.
        int mode=GameActivity.nativeTouchContext();final int backKey;boolean clearCatalog=false;
        if((mode&(OPTIONS|MODAL|TEXT_FOCUS))==OPTIONS){
            int target=mode&(TOWN|LIVE|BUY|BUILD);if(target==0)target=optionReturnContext;
            backKey=target==TOWN?ESC:target==BUY?0x71:target==BUILD?0x72:0x70;
        }else{
            int selected=(int)GameActivity.nativeTouchToolToken();
            // Hand leaves the tool; its catalog highlight has a separate owner-thread clear.
            backKey=(mode==BUY||mode==BUILD)&&selected>=2&&selected<=6&&!GameActivity.nativeTouchRewards()?0x48:ESC;
            clearCatalog=(mode==BUY||mode==BUILD)&&selected==2&&backKey==0x48;
        }
        boolean abortCapture=backKey!=ESC&&(heldWorld||(pending!=null&&pending.down&&pending.capturesWorld()));
        GameActivity.nativeKey(abortCapture?ESC:backKey,true);
        if(held)GameActivity.nativeMouseButton(0,false);
        if(pending!=null&&pending.down)GameActivity.nativeMouseButton(pending.button,false);
        for(int vk:keys.keySet())if(vk!=backKey)GameActivity.nativeKey(vk,false);
        if(abortCapture){GameActivity.nativeKey(ESC,false);GameActivity.nativeKey(backKey,true);}
        if(clearCatalog)GameActivity.nativeTouchClearCatalog();
        clear(false,false);keys.put(backKey,1);handler.postDelayed(()->key(backKey,false),90);
    }
    private void key(int vk,boolean down){
        if(retired)return;
        int before=keys.containsKey(vk)?keys.get(vk):0,after=Math.max(0,before+(down?1:-1));
        if(after==0)keys.remove(vk);else keys.put(vk,after);
        if((before==0)!=(after==0))GameActivity.nativeKey(vk,down);
    }
    private void point(float x,float y){GameActivity.nativeMouseMove(x/getWidth(),y/getHeight(),false);}
    private void warp(float x,float y){GameActivity.nativeMouseWarp(x/getWidth(),y/getHeight());}
    private void cameraContact(boolean active){
        if(cameraContact==active)return;
        cameraContact=active;GameActivity.nativeTouchCameraContact(active);
    }
    private int hit(float x,float y){return GameActivity.nativeTouchHitKind(x/getWidth(),y/getHeight());}
    private boolean validate(float x,float y,long expected,int mode){
        int h=hit(x,y),c=GameActivity.nativeTouchContext();long t=GameActivity.nativeTouchToolToken();
        return !retired&&!blocked&&(h&3)==2&&(c&~TEXT_FOCUS)==(mode&~TEXT_FOCUS)
                &&(c&BLOCKERS)==0&&t==expected&&((c&TEXT_FOCUS)==0||(c&CAS)!=0);
    }
    private boolean navigation(){return(originContext&(TOWN|CAS))!=0||tool==1||((originContext&LIVE)!=0&&tool==0);}
    private boolean cameraAllowed(){return navigation()||((originContext&(BUY|BUILD))!=0&&tool>=2&&originToken!=0);}
    boolean objectControlsVisible(){
        int mode=GameActivity.nativeTouchContext();
        return !retired&&!blocked&&(mode==BUY||mode==BUILD)&&(int)GameActivity.nativeTouchToolToken()==2;
    }
    boolean canUseObjectPreview(){
        if(preview!=null&&!preview.matches())preview=null;
        return objectControlsVisible()&&preview!=null&&!camera&&!singleCamera&&pending==null&&commandSerial==0&&keys.isEmpty();
    }
    boolean canCancelObject(){return objectControlsVisible()&&commandSerial==0&&!stream&&keys.isEmpty();}
    private final class Preview{
        final float x,y;final long expected,size,epoch;final int mode,width,height;
        Preview(float x,float y){
            this.x=x;this.y=y;expected=originToken;size=originSize;epoch=originEpoch;
            mode=originContext;width=originWidth;height=originHeight;
        }
        boolean environmentMatches(){
            return !retired&&!blocked&&(mode==BUY||mode==BUILD)
                    &&getWidth()==width&&getHeight()==height&&GameActivity.nativeTouchContext()==mode
                    &&GameActivity.nativeTouchGameSize()==size&&GameActivity.nativeTouchLayoutEpoch()==epoch;
        }
        boolean matches(){return environmentMatches()&&(int)expected==2&&GameActivity.nativeTouchToolToken()==expected;}
    }
    private void previewObject(float x,float y){
        preview=null;
        if(!stableOrigin()||!objectControlsVisible())return;
        if(!GameActivity.nativeTouchObjectPreview(originToken))return;
        point(x,y);
        if(validate(x,y,originToken,originContext)&&stableOrigin())preview=new Preview(x,y);
    }
    void placeObjectPreview(){
        if(!canUseObjectPreview())return;
        Preview chosen=preview;preview=null;consumed=true;
        if(!validate(chosen.x,chosen.y,chosen.expected,chosen.mode)||!chosen.matches())return;
        pending=new Click(chosen.x,chosen.y,2,chosen.mode,chosen.expected,chosen);
        handler.postDelayed(pending.press,50);
    }
    void rotateObjectPreview(){rotateObjectPreview(-1);}
    private void rotateObjectPreview(int direction){
        if(!canUseObjectPreview())return;
        Preview chosen=preview;
        if(!validate(chosen.x,chosen.y,chosen.expected,chosen.mode)||!chosen.matches()){preview=null;return;}
        commandReceipt(GameActivity.nativeTouchRotateObject(chosen.x/chosen.width,chosen.y/chosen.height,
                chosen.expected,chosen.mode,chosen.size,chosen.epoch,direction),ROTATE);
    }
    void cancelObjectPreview(){
        if(!canCancelObject())return;
        releaseAll();controllerKey(ESC);GameActivity.nativeTouchClearCatalog();
    }
    private final class Click{
        final float x,y;final int kind,mode;final long expected;boolean down;int button;
        final long size=originSize,epoch=originEpoch;
        final int width=getWidth(),height=getHeight();
        final Preview placement;
        final Runnable press,release;
        boolean capturesWorld(){return(kind&3)==2&&(mode&(BUY|BUILD))!=0&&(int)expected>=2;}
        Click(float x,float y,int kind,int mode,long expected){
            this(x,y,kind,mode,expected,null);
        }
        Click(float x,float y,int kind,int mode,long expected,Preview placement){
            this.x=x;this.y=y;this.kind=kind;this.mode=mode;this.expected=expected;
            this.placement=placement;
            release=()->{
                if(pending!=this)return;
                if(placement!=null&&!placement.environmentMatches()){releaseAll();return;}
                GameActivity.nativeMouseButton(button,false);down=false;pending=null;
                if((kind&16)!=0&&!blocked&&!retired)GameActivity.touchTextRequested(x/getWidth(),y/getHeight());
            };
            press=()->{
                if(pending!=this||blocked||retired)return;
                if(getWidth()!=width||getHeight()!=height||GameActivity.nativeTouchGameSize()!=size
                        ||GameActivity.nativeTouchLayoutEpoch()!=epoch){pending=null;return;}
                cameraContact(false);
                boolean worldTap=(kind&3)==2&&mode==LIVE&&((int)expected==0||(int)expected==1);
                if((kind&3)==2){
                    if((placement!=null&&!placement.matches())||(!worldTap&&!validate(x,y,expected,mode))
                            ||(placement!=null&&!placement.matches())){pending=null;return;}
                }
                else if((GameActivity.nativeTouchContext()&~TEXT_FOCUS)!=(mode&~TEXT_FOCUS)){pending=null;return;}
                if(worldTap){
                    long target=GameActivity.nativeTouchTarget(x/getWidth(),y/getHeight(),18*density/getWidth());
                    if(target==-1||GameActivity.nativeTouchContext()!=mode||GameActivity.nativeTouchToolToken()!=expected||GameActivity.nativeTouchGameSize()!=size
                            ||GameActivity.nativeTouchLayoutEpoch()!=epoch){pending=null;return;}
                    int w=(int)size,h=(int)(size>>>32);float scale=Math.min(width/(float)w,height/(float)h);
                    GameActivity.nativeMouseMove(((int)target*scale+(width-w*scale)*.5f)/width,
                            ((int)(target>>>32)*scale+(height-h*scale)*.5f)/height,false);
                }else point(x,y);
                GameActivity.nativeMouseButton(button,true);down=true;handler.postDelayed(release,90);
            };
        }
        void finishUi(){
            handler.removeCallbacks(press);handler.removeCallbacks(release);if(!down)press.run();
            handler.removeCallbacks(release);if(pending==this&&down)release.run();
        }
    }
    private void click(float x,float y){pending=new Click(x,y,originKind,originContext,originToken);handler.postDelayed(pending.press,50);}
    void controllerKey(int vk){if(!blocked&&!retired){key(vk,true);handler.postDelayed(()->key(vk,false),90);}}
    void controllerRotate(int direction){rotateObjectPreview(direction);}
    void controllerClick(float x,float y,boolean right){
        if(blocked||retired||pointer>=0||pending!=null||commandSerial!=0)return;
        float px=x*getWidth(),py=y*getHeight();
        if(!right&&canUseObjectPreview()&&Math.hypot(px-preview.x,py-preview.y)<threshold){placeObjectPreview();return;}
        long now=android.os.SystemClock.uptimeMillis();
        MotionEvent down=MotionEvent.obtain(now,now,MotionEvent.ACTION_DOWN,px,py,0);onTouchEvent(down);down.recycle();
        if(right){handler.removeCallbacks(longPress);longPress.run();consumed=true;}
        MotionEvent up=MotionEvent.obtain(now,now+1,MotionEvent.ACTION_UP,px,py,0);onTouchEvent(up);up.recycle();
    }
    void controllerScroll(float x,float y,int amount){
        if(blocked||retired||pointer>=0)return;
        stopFling();
        int kind=GameActivity.nativeTouchHitKind(x,y),mode=GameActivity.nativeTouchContext();
        if((kind&(32|64))!=0)GameActivity.nativeTouchScroll(x,y,amount,mode,GameActivity.nativeTouchGameSize(),GameActivity.nativeTouchLayoutEpoch());
        else if((kind&3)==1){GameActivity.nativeMouseWarp(x,y);GameActivity.nativeMouseWheel(amount*120);}
    }
    private void startCapture(float x,float y){
        cameraContact(false);
        point(x,y);GameActivity.nativeMouseButton(0,true);held=true;
        // CAS model rotation is a native mouse drag, not cancellable build geometry.
        heldWorld=(originKind&3)==2&&(originContext&CAS)==0;
    }
    private void cancelWorldCapture(){
        cameraContact(false);
        handler.removeCallbacks(longPress);
        if(heldWorld){key(ESC,true);GameActivity.nativeMouseButton(0,false);held=heldWorld=false;handler.postDelayed(()->key(ESC,false),90);}
        consumed=multi=true;camera=uiPan=false;
    }
    private void pan(float dx,float dy){
        if((GameActivity.nativeTouchContext()&(BLOCKERS|TEXT_FOCUS|CAS))!=0||(dx==0&&dy==0))return;
        GameActivity.nativeTouchPan(lastX/getWidth(),lastY/getHeight(),(lastX+dx)/getWidth(),(lastY+dy)/getHeight());
    }
    private float[] pair(MotionEvent e){
        int a=e.findPointerIndex(pointer),b=e.findPointerIndex(second);if(a<0||b<0)return null;
        float x=e.getX(a),y=e.getY(a),xx=e.getX(b),yy=e.getY(b);
        return new float[]{(x+xx)/2,(y+yy)/2,(float)Math.hypot(xx-x,yy-y),(float)Math.atan2(yy-y,xx-x)};
    }
    private void movePair(MotionEvent e){
        float[] p=pair(e);if(p==null)return;int c=GameActivity.nativeTouchContext();
        if((c&~TEXT_FOCUS)!=(originContext&~TEXT_FOCUS)||(c&BLOCKERS)!=0||((c&TEXT_FOCUS)!=0&&(c&CAS)==0)){cancelWorldCapture();return;}
        if(cancelTap){
            if(e.getEventTime()-chordTime>450||!chordPossible)cancelTap=false;
            else return;
        }
        if(objectTwist){
            float turn=p[3]-camAngle;if(turn>Math.PI)turn-=2*Math.PI;if(turn<-Math.PI)turn+=2*Math.PI;
            turnRest+=turn;
            if(Math.abs(turnRest)>Math.PI/4&&canUseObjectPreview()){
                rotateObjectPreview(turnRest>0?1:-1);turnRest=0;
            }
            camAngle=p[3];return;
        }
        preview=null;
        if(uiPan){if(Math.abs(p[1]-camY)>0)GameActivity.nativeTouchPanCas(Math.max(-1,Math.min(1,(p[1]-camY)/getHeight())));}
        else{
            float zoom=camDistance>0&&p[2]>0?(float)Math.log(p[2]/camDistance):0;
            if((c&CAS)!=0&&camDistance>0&&p[2]>0){
                zoomRest+=Math.log(p[2]/camDistance)*((c&CAS)!=0?3:1);float step=(float)Math.log(1.15);
                while(Math.abs(zoomRest)>=step){GameActivity.nativeMouseWheel(zoomRest>0?120:-120);zoomRest+=zoomRest>0?-step:step;}
            }
            float turn=p[3]-camAngle;if(turn>Math.PI)turn-=2*Math.PI;if(turn<-Math.PI)turn+=2*Math.PI;
            if(p[2]<threshold*2||camDistance<threshold*2){turn=zoom=0;}
            if((c&(CAS|TEXT_FOCUS))==0){
                float dy=p[1]-camY,spread=(p[2]*(float)Math.sin(p[3])-camDistance*(float)Math.sin(camAngle))*.5f;
                float pitch=(dy-spread)*(dy+spread)>0&&Math.abs(dy)>Math.abs(p[0]-camX)
                        ?Math.max(-(float)Math.PI/2,Math.min((float)Math.PI/2,dy/getHeight()*(float)Math.PI)):0;
                GameActivity.nativeTouchCamera(0,0,turn,Math.max(-1,Math.min(1,zoom)),pitch);turnRest=0;
            }
        }
        camX=p[0];camY=p[1];camDistance=p[2];camAngle=p[3];
    }
    private void trackChord(MotionEvent e){
        int a=e.getActionMasked(),i=e.getActionIndex(),id=e.getPointerId(i);
        if(a==MotionEvent.ACTION_DOWN){chordPoints.clear();chordPossible=true;chordTime=e.getEventTime();}
        if(a==MotionEvent.ACTION_DOWN||a==MotionEvent.ACTION_POINTER_DOWN){chordPoints.put(id,new float[]{e.getX(i),e.getY(i)});if(chordPoints.size()>3)chordPossible=false;}
        // DOWN starts a new gesture; drag/hold flags and an unpressed UI click belong to the previous one.
        if(e.getEventTime()-chordTime>750||held||!keys.isEmpty()
                ||(a!=MotionEvent.ACTION_DOWN&&(dragging||longPressed||pending!=null)))chordPossible=false;
        for(int n=0;n<e.getPointerCount()&&chordPossible;n++){
            float[] p=chordPoints.get(e.getPointerId(n));if(p==null)continue;
            if(Math.hypot(e.getX(n)-p[0],e.getY(n)-p[1])>threshold*2)chordPossible=false;
            for(int h=0;h<e.getHistorySize()&&chordPossible;h++)
                if(Math.hypot(e.getHistoricalX(n,h)-p[0],e.getHistoricalY(n,h)-p[1])>threshold*2)chordPossible=false;
        }
    }
    private void stopFling(){
        boolean cancel=flinging;flinging=false;fling.forceFinished(true);handler.removeCallbacks(flingTick);
        if(cancel)GameActivity.nativeTouchStopFling();
    }
    private static String swipeLabel(long receipt){
        int family=(int)(receipt>>>40)&15,id=(int)receipt,index=-1;
        int[][] labels={
            {R.string.swipe_paused,R.string.swipe_speed1,R.string.swipe_speed2,R.string.swipe_speed3},
            {R.string.swipe_needs,R.string.swipe_relationships,R.string.swipe_career,R.string.swipe_simology,R.string.swipe_rewards,R.string.swipe_roommates},
            {R.string.swipe_personality,R.string.swipe_interests,R.string.swipe_memories,R.string.swipe_chemistry,R.string.swipe_hobbies},
            {R.string.swipe_personality,R.string.swipe_interests,R.string.swipe_behaviors,R.string.swipe_memories},
            {R.string.swipe_basics,R.string.swipe_head,R.string.swipe_hair,R.string.swipe_cosmetics,R.string.swipe_clothing,R.string.swipe_personality},
            {R.string.swipe_face_preset,R.string.swipe_brows,R.string.swipe_eyes,R.string.swipe_nose,R.string.swipe_mouth,R.string.swipe_chin,R.string.swipe_face_shape},
            {R.string.swipe_brows,R.string.swipe_glasses,R.string.swipe_makeup,R.string.swipe_costume,R.string.swipe_stubble,R.string.swipe_facial_hair,R.string.swipe_jewelry},
            {R.string.swipe_basics,R.string.swipe_breed,R.string.swipe_personality,R.string.swipe_collar,R.string.swipe_coat,R.string.swipe_body,R.string.swipe_personality}
        };
        if(family==0||family==1){
            int[] ids=family==0?new int[]{0x27D7258F,0x57E01CC1,0x37E01CC3,0x47E01CC5}
                    :new int[]{0x0D8AB291,0x17DEFBF5,0x57DEFBF4,0x47DEFBF2,0x8B85BA4E,0x8B85BADD};
            for(int n=0;n<ids.length;n++)if(ids[n]==id)index=n;
        }else if(family==2)index=(id>>>28)-4;
        else if(family==3)index=id-0xB1;
        else if(family==4)index=((int)(receipt>>>32)&15)-1;
        else index=id&0xFFFF;
        return family<labels.length&&index>=0&&index<labels[family].length?GameActivity.text(labels[family][index]):null;
    }
    private void releaseVelocity(){if(velocity!=null){velocity.recycle();velocity=null;}}
    private void startFling(){
        if(velocity==null||scrollOwner==0||!stableOrigin())return;
        velocity.computeCurrentVelocity(1000,maxFling);
        float speed=scrollHorizontal?velocity.getXVelocity(pointer):velocity.getYVelocity(pointer);
        if(Math.abs(speed)<minFling)return;
        flingX=flingY=0;flinging=true;
        fling.fling(0,0,scrollHorizontal?(int)speed:0,scrollHorizontal?0:(int)speed,
                -getWidth()*2,getWidth()*2,-getHeight()*2,getHeight()*2);
        handler.postDelayed(flingTick,32);
    }
    private void scrollBy(float dx,float dy,boolean inertia){
        if(!stableOrigin()){stopFling();return;}
        scrollRest+=(scrollHorizontal?dx:dy)*4/density;
        float step=(originKind&32)!=0?256:120;
        scrollRest=Math.max(-8*step,Math.min(8*step,scrollRest));
        int limit=inertia?2:8,lines=Math.max(-limit,Math.min(limit,(int)(scrollRest/step)));
        if(lines==0)return;
        boolean accepted;
        if(inertia)accepted=GameActivity.nativeTouchFling(downX/originWidth,downY/originHeight,
                lines,originContext,originSize,originEpoch,scrollOwner);
        else if((originKind&32)!=0)accepted=GameActivity.nativeTouchScroll(downX/originWidth,downY/originHeight,
                lines,originContext,originSize,originEpoch);
        else{
            point(downX,downY);
            for(int n=0;n<Math.abs(lines);n++)GameActivity.nativeMouseWheel(lines>0?120:-120);
            accepted=true;
        }
        if(accepted)scrollRest-=lines*step;
    }

    private void trackTap(MotionEvent e){
        int i=e.findPointerIndex(pointer);if(i<0)return;
        if(Math.hypot(e.getX(i)-gestureX,e.getY(i)-gestureY)>threshold)tapMoved=true;
        for(int h=0;h<e.getHistorySize()&&!tapMoved;h++)
            if(Math.hypot(e.getHistoricalX(i,h)-gestureX,e.getHistoricalY(i,h)-gestureY)>threshold)tapMoved=true;
    }
    @Override public boolean onTouchEvent(MotionEvent e){
        int a=e.getActionMasked(),idx=e.getActionIndex(),id=e.getPointerId(idx);
        if(a==MotionEvent.ACTION_CANCEL){releaseAll();return true;}
        if(retired||blocked)return true;
        // A STARTED owner command finishes once. Discard whole streams received while awaiting it.
        if(commandSerial!=0){releaseAll();return true;}
        if(a!=MotionEvent.ACTION_DOWN&&!stream)return true;
        if(a!=MotionEvent.ACTION_DOWN&&velocity!=null)velocity.addMovement(e);
        if(a!=MotionEvent.ACTION_DOWN){trackChord(e);trackTap(e);}
        if(a==MotionEvent.ACTION_DOWN){
            if(pending!=null){if((pending.kind&3)==1)pending.finishUi();else releaseAll();}
            // Finishing a text-field UI click may have opened the Android editor synchronously.
            if(retired||blocked)return true;
            if(pointer>=0)releaseAll();
            boolean stopOnly=flinging;stopFling();releaseVelocity();velocity=VelocityTracker.obtain();velocity.addMovement(e);
            preview=null;
            // Snapshots must follow the owner-thread FIFO fence.
            cameraContact((GameActivity.nativeTouchContext()&(BLOCKERS|CAS|TEXT_FOCUS))==0);
            float[] picked=GameActivity.nativeTouchUiPick(e.getX()/getWidth(),e.getY()/getHeight(),48*density/getWidth());
            originKind=picked==null?0:(int)picked[0];context=GameActivity.nativeTouchContext();syncToolToken(GameActivity.nativeTouchToolToken());
            if(retired||blocked){cameraContact(false);return true;}
            originContext=context;originToken=token;pointer=id;second=-1;
            originSize=GameActivity.nativeTouchGameSize();originEpoch=GameActivity.nativeTouchLayoutEpoch();
            originWidth=getWidth();originHeight=getHeight();originFillKind=GameActivity.nativeTouchFillKind();
            gestureX=lastX=e.getX();gestureY=lastY=e.getY();
            downX=picked==null?gestureX:picked[1]*getWidth();downY=picked==null?gestureY:picked[2]*getHeight();
            scrollOwner=GameActivity.nativeTouchScrollOwner();
            seenSwipe=GameActivity.nativeTouchSwipeFeedback();swipeBaseline=true;
            tapMoved=chooserPaged=floorSwipe=floorDone=false;dragging=consumed=multi=longPressed=camera=uiPan=fillArmed=cancelTap=objectTwist=catalogDrag=singleCamera=false;scrollRest=0;
            stream=true;trackChord(e);
            cameraContact((originKind&3)==2&&navigation()&&(originContext&(BLOCKERS|CAS|TEXT_FOCUS))==0);
            widgetSwipe=GamePreferences.uiSwipeControls()&&(originKind&(SWIPE_HORIZONTAL|SWIPE_VERTICAL))!=0;
            timeSpeed=(originKind>>>11)&3;timeVisible=false;invalidate();
            if(stopOnly){cameraContact(false);consumed=true;return true;}
            if((originKind&3)==0){consumed=true;return true;}
            if((originKind&3)==2&&!navigation()&&(tool<2||token==0)){consumed=true;GameActivity.touchNotice(GameActivity.text(R.string.choose_tool));return true;}
            // Navigation has explicit finger coordinates; mouse motion would run object-interaction tests.
            if((originKind&3)!=2||!navigation()||(originContext&CAS)!=0)warp(downX,downY);
            if((originKind&3)==2&&tool==2&&(originContext==BUY||originContext==BUILD))previewObject(downX,downY);
            handler.postDelayed(longPress,450);GameActivity.touchObjectControlsChanged();return true;
        }
        if(a==MotionEvent.ACTION_POINTER_DOWN){
            timeVisible=false;invalidate();
            handler.removeCallbacks(longPress);multi=true;
            singleCamera=false;
            if(floorSwipe){floorSwipe=false;consumed=true;return true;}
            if(fillArmed){fillArmed=false;consumed=true;GameActivity.touchNotice(GameActivity.text(R.string.fill_disarmed));return true;}
            if(pointer<0||e.findPointerIndex(pointer)<0){consumed=true;return true;}
            if(e.getPointerCount()==3&&chordPossible&&e.getEventTime()-chordTime<=250&&!held&&pending==null
                    &&(originKind&3)==2&&(originContext==LIVE||originContext==BUY||originContext==BUILD)&&stableOrigin()){
                boolean world=true;
                for(int n=0;n<3;n++)if((hit(e.getX(n),e.getY(n))&3)!=2)world=false;
                if(world&&stableOrigin()){
                    cameraContact(false);preview=null;camera=uiPan=objectTwist=cancelTap=false;floorSwipe=consumed=true;
                    for(int n=0;n<3;n++)chordPoints.put(e.getPointerId(n),new float[]{e.getX(n),e.getY(n)});
                    return true;
                }
            }
            if(e.getPointerCount()!=2){cancelWorldCapture();return true;}
            if(preview!=null&&(originKind&3)==2&&tool==2&&(longPressed||dragging)&&stableOrigin()){
                second=id;objectTwist=true;cancelTap=false;consumed=true;
                float[] p=pair(e);if(p!=null){camAngle=p[3];turnRest=0;}return true;
            }
            if(held){
                if(heldWorld)cancelWorldCapture();
                else if((originKind&3)==2){GameActivity.nativeMouseButton(0,false);held=false;consumed=true;}
                return true;
            }
            if(consumed||dragging||longPressed){consumed=true;return true;}
            if((originKind&3)==2&&cameraAllowed()&&(originContext&CAS)==0)cameraContact(true);
            int h=hit(e.getX(idx),e.getY(idx)),c=GameActivity.nativeTouchContext();
            if((c&~TEXT_FOCUS)!=(originContext&~TEXT_FOCUS)||(c&BLOCKERS)!=0){cameraContact(false);consumed=true;return true;}
            second=id;
            cancelTap=(originContext&(BUY|BUILD))!=0&&(originKind&3)==2&&(h&3)==2
                    &&tool>=2&&originToken!=0&&chordPossible&&e.getEventTime()-chordTime<=220;
            if((originKind&3)==2&&(h&3)==2&&cameraAllowed()&&((c&TEXT_FOCUS)==0||(c&CAS)!=0)){camera=true;if(!navigation()||(originContext&CAS)!=0)warp(downX,downY);}
            else if((c&CAS)!=0&&(originKind&3)==1&&(h&3)==1)uiPan=true;
            else{cameraContact(false);consumed=true;if(!cancelTap)return true;}
            cameraContact(camera&&(c&CAS)==0);
            float[] p=pair(e);if(p==null){cameraContact(false);consumed=true;camera=uiPan=false;return true;}
            camX=p[0];camY=p[1];camDistance=p[2];camAngle=p[3];zoomRest=turnRest=0;return true;
        }
        if(a==MotionEvent.ACTION_MOVE){
            if(floorSwipe){
                if(!stableOrigin()||e.getPointerCount()!=3){floorSwipe=false;return true;}
                if(floorDone)return true;
                int direction=0;
                for(int n=0;n<3;n++){
                    float[] p=chordPoints.get(e.getPointerId(n));if(p==null)return true;
                    float dx=e.getX(n)-p[0],dy=e.getY(n)-p[1];
                    if(Math.abs(dy)<48*density||Math.abs(dx)>Math.abs(dy)*.5f)return true;
                    int next=dy<0?-1:1;if(direction!=0&&direction!=next)return true;direction=next;
                }
                controllerKey(direction<0?0x21:0x22);floorDone=true;return true;
            }
            if(singleCamera){
                int i=e.findPointerIndex(pointer);if(i<0)return true;
                float x=e.getX(i),y=e.getY(i);if(stableOrigin())pan(x-lastX,y-lastY);else releaseAll();
                lastX=x;lastY=y;return true;
            }
            if(camera||uiPan||objectTwist){movePair(e);return true;}if(consumed)return true;
            int i=e.findPointerIndex(pointer);if(i<0)return true;float x=e.getX(i),y=e.getY(i);
            if(!dragging&&Math.hypot(x-gestureX,y-gestureY)>threshold){
                dragging=true;chordPossible=false;handler.removeCallbacks(longPress);
                scrollHorizontal=Math.abs(x-gestureX)>Math.abs(y-gestureY);
                if(widgetSwipe||((originKind&3)==2&&navigation()&&(originContext&CAS)==0)){lastX=gestureX;lastY=gestureY;}
                if((originKind&3)==1){if(!widgetSwipe&&(originKind&4)!=0)startCapture(downX,downY);}
                else if(tool==6){consumed=true;return true;}
                else if((originContext&CAS)!=0||tool>=3){if(!validate(downX,downY,originToken,originContext)){consumed=true;return true;}startCapture(downX,downY);}
            }
            if(dragging){
                if(widgetSwipe){
                    boolean horizontal=(originKind&SWIPE_HORIZONTAL)!=0;
                    if(!stableOrigin()){releaseAll();return true;}
                    scrollRest+=(horizontal?x-lastX:y-lastY)/density;
                    float step=horizontal?64:40;
                    int steps=Math.max(-8,Math.min(8,(int)Math.copySign((int)(Math.abs(scrollRest)/step+.5f),scrollRest)));
                    if(steps!=0&&!chooserPaged&&GameActivity.nativeTouchScroll(downX/originWidth,downY/originHeight,
                            steps,originContext,originSize,originEpoch)){
                        chooserPaged=originContext==CHOOSER;
                        scrollRest-=steps*step;timeSpeed=Math.max(0,Math.min(3,timeSpeed+steps));
                    }
                    timeVisible=horizontal&&(originKind&1024)!=0;invalidate();
                    lastX=x;lastY=y;return true;
                }
                if((originKind&65)==65&&(originContext==BUY||originContext==BUILD)
                        &&(catalogDrag||y<downY-threshold||(hit(x,y)&3)==2)){
                    if(!catalogDrag){
                        if(pending==null){click(downX,downY);pending.finishUi();}
                        catalogDrag=true;hit(x,y);
                    }
                    syncToolToken(GameActivity.nativeTouchToolToken());
                    if(tool==2&&(hit(x,y)&3)==2){originToken=token;originKind=2;originContext=GameActivity.nativeTouchContext();previewObject(x,y);}
                    lastX=x;lastY=y;return true;
                }
                if(held)point(x,(originContext&CAS)!=0&&(originKind&3)==2?downY:y);
                else if((originKind&3)==1&&(originKind&8)!=0){
                    scrollBy(x-lastX,y-lastY,false);
                }
                else if(tool==2)previewObject(x,y);else if((originKind&3)==2)pan(x-lastX,y-lastY);
            }
            lastX=x;lastY=y;return true;
        }
        if(a==MotionEvent.ACTION_POINTER_UP||a==MotionEvent.ACTION_UP){
            if(floorSwipe){floorSwipe=false;consumed=true;}
            timeVisible=false;invalidate();
            if(a==MotionEvent.ACTION_UP)cameraContact(false);
            handler.removeCallbacks(longPress);
            if(cancelTap&&(!chordPossible||e.getEventTime()-chordTime>450))cancelTap=false;
            if(cancelTap&&(a==MotionEvent.ACTION_POINTER_UP||a==MotionEvent.ACTION_UP)){
                boolean valid=stableOrigin();releaseAll();if(valid)back();return true;
            }
            if(a==MotionEvent.ACTION_POINTER_UP&&camera&&!cancelTap&&e.getPointerCount()==2&&(originContext&CAS)==0){
                int remaining=idx==0?1:0;pointer=e.getPointerId(remaining);second=-1;
                gestureX=downX=lastX=e.getX(remaining);gestureY=downY=lastY=e.getY(remaining);
                camera=multi=consumed=dragging=longPressed=cancelTap=chordPossible=false;singleCamera=true;
                chordPoints.clear();chordPoints.put(pointer,new float[]{downX,downY});
                return true;
            }
            if(singleCamera){singleCamera=false;consumed=true;}
            if(objectTwist){objectTwist=false;consumed=true;}
            if(camera||uiPan){camera=uiPan=false;consumed=true;}
            if(id==pointer){
                if(a==MotionEvent.ACTION_UP&&dragging&&!held&&!multi&&!consumed&&!widgetSwipe
                        &&(originKind&9)==9&&(originKind&(4|64))==0)startFling();
                if(fillArmed){
                    fillArmed=false;consumed=true;
                    if(a==MotionEvent.ACTION_UP&&!multi&&stableOrigin())
                        commandReceipt(GameActivity.nativeTouchFillRoom(downX/originWidth,downY/originHeight,
                                originToken,originContext,originSize,originEpoch),FILL);
                    else GameActivity.touchNotice(GameActivity.text(R.string.fill_changed));
                }
                else if(held){
                    point(e.getX(idx),(originContext&CAS)!=0&&(originKind&3)==2?downY:e.getY(idx));
                    GameActivity.nativeMouseButton(0,false);held=heldWorld=false;
                }
                else if(!consumed&&!longPressed&&!multi&&(!tapMoved&&e.getEventTime()-chordTime<450||tool==2&&(originKind&3)==2)&&(!dragging||(tool==2&&(originKind&3)==2))){
                    if((originKind&3)==1)click(downX,downY);
                    else if(tool==2&&(originContext==BUY||originContext==BUILD))previewObject(e.getX(idx),e.getY(idx));
                    else click(downX,downY);
                }
                pointer=-1;consumed=true;
            }
            if(a==MotionEvent.ACTION_UP){releaseVelocity();pointer=second=-1;camera=uiPan=false;chordPossible=stream=false;GameActivity.touchObjectControlsChanged();}
            return true;
        }
        return true;
    }
    @Override protected void onDraw(Canvas canvas){
        if(swipeNotice!=null){
            float cx=getWidth()*.5f,top=24*density,half=Math.min(180*density,getWidth()*.5f-16*density);
            timePaint.setColor(0xee14232d);canvas.drawRoundRect(cx-half,top,cx+half,top+48*density,10*density,10*density,timePaint);
            timePaint.setColor(0xffd7edf6);timePaint.setTextSize(16*density);timePaint.setTextAlign(Paint.Align.CENTER);
            canvas.drawText(swipeNotice,cx,top+30*density,timePaint);
        }
        if(!timeVisible)return;
        float cx=getWidth()*.5f,cy=getHeight()*.5f,half=126*density,y=cy-10*density;
        timePaint.setColor(0xee14232d);
        canvas.drawRoundRect(cx-half-18*density,cy-38*density,cx+half+18*density,cy+42*density,10*density,10*density,timePaint);
        timePaint.setStrokeWidth(2*density);timePaint.setColor(0xff82909a);
        canvas.drawLine(cx-half,y,cx+half,y,timePaint);
        float progress=Math.max(0,Math.min(3,timeSpeed+scrollRest/64));
        timePaint.setColor(0xff70d7ff);canvas.drawLine(cx-half,y,cx-half+84*density*progress,y,timePaint);
        timePaint.setTextSize(16*density);timePaint.setTextAlign(Paint.Align.CENTER);
        for(int i=0;i<4;i++){
            float x=cx-half+84*density*i;
            timePaint.setColor(i==timeSpeed?0xffffd36b:0xffd7edf6);
            canvas.drawCircle(x,y,(i==timeSpeed?7:4)*density,timePaint);
            canvas.drawText(TIME_LABELS[i],x,cy+24*density,timePaint);
        }
    }
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh){
        super.onSizeChanged(w,h,oldw,oldh);swipeNotice=null;releaseAll();
        setSystemGestureExclusionRects(java.util.Collections.singletonList(new android.graphics.Rect(0,0,w,h)));
    }
    @Override protected void onDetachedFromWindow(){swipeNotice=null;releaseAll();super.onDetachedFromWindow();}
}
