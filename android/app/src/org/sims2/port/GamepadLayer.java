package org.sims2.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.hardware.input.InputManager;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.HashMap;

/** A controller pointer uses the same validated selection/placement path as touch. */
final class GamepadLayer extends View implements InputManager.InputDeviceListener {
    private final TouchLayer touch;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final HashMap<Integer,Pad> pads=new HashMap<>();
    private float x=.5f,y=.5f;
    private long lastTick;
    private boolean ticking,enabled=true;
    private static final class Pad {float x,y,rx,ry,lt,rt;boolean left,right,tilt,slow,leftTrigger,rightTrigger;int hx,hy;long scrolled;}
    GamepadLayer(Context context,TouchLayer touch){super(context);this.touch=touch;setVisibility(GONE);setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);}
    static float normalize(float value,float flat){
        if(!Float.isFinite(value)||!Float.isFinite(flat))return 0;
        flat=Math.min(.95f,Math.max(.16f,flat));
        float magnitude=Math.min(1,Math.max(0,(Math.abs(value)-flat)/(1-flat)));
        return Math.copySign(magnitude*magnitude,value);
    }
    private float axis(MotionEvent event,int axis){
        InputDevice device=event.getDevice();InputDevice.MotionRange range=device==null?null:device.getMotionRange(axis,event.getSource());
        return normalize(event.getAxisValue(axis),range==null?0:range.getFlat());
    }
    private Pad pad(int id){Pad result=pads.get(id);if(result==null){result=new Pad();pads.put(id,result);}return result;}
    private void start(){if(!enabled)return;setVisibility(VISIBLE);if(!ticking){ticking=true;lastTick=SystemClock.uptimeMillis();postDelayed(tick,16);}}
    void setEnabledInput(boolean value){enabled=value;if(!value)release();}
    void hidePointer(){setVisibility(GONE);}
    void release(){pads.clear();removeCallbacks(tick);ticking=false;setVisibility(GONE);}
    @Override public void onInputDeviceAdded(int id){}
    @Override public void onInputDeviceChanged(int id){onInputDeviceRemoved(id);}
    @Override public void onInputDeviceRemoved(int id){pads.remove(id);if(pads.isEmpty())release();}
    boolean key(KeyEvent event){
        if(!enabled||(!event.isFromSource(InputDevice.SOURCE_GAMEPAD)&&!event.isFromSource(InputDevice.SOURCE_JOYSTICK)))return false;
        if(event.getAction()!=KeyEvent.ACTION_DOWN&&event.getAction()!=KeyEvent.ACTION_UP)return false;
        boolean down=event.getAction()==KeyEvent.ACTION_DOWN;if(down&&event.getRepeatCount()!=0)return true;
        Pad p=pad(event.getDeviceId());start();
        switch(event.getKeyCode()){
        case KeyEvent.KEYCODE_BUTTON_A:if(down)touch.controllerClick(x,y,false);break;
        case KeyEvent.KEYCODE_BUTTON_X:if(down)touch.controllerClick(x,y,true);break;
        case KeyEvent.KEYCODE_BUTTON_B:if(down)touch.back();break;
        case KeyEvent.KEYCODE_BUTTON_Y:if(down)touch.controllerKey(0x50);break;
        case KeyEvent.KEYCODE_BUTTON_START:if(down)GameActivity.touchSettingsRequested();break;
        case KeyEvent.KEYCODE_BUTTON_SELECT:if(down)GameActivity.touchNotice(GameActivity.text(R.string.gamepad_help));break;
        case KeyEvent.KEYCODE_BUTTON_L1:case KeyEvent.KEYCODE_BUTTON_R1:
            if(down&&touch.canUseObjectPreview())touch.controllerRotate(event.getKeyCode()==KeyEvent.KEYCODE_BUTTON_L1?-1:1);
            else if(event.getKeyCode()==KeyEvent.KEYCODE_BUTTON_L1)p.left=down;else p.right=down;
            break;
        case KeyEvent.KEYCODE_BUTTON_L2:p.leftTrigger=down;break;
        case KeyEvent.KEYCODE_BUTTON_R2:p.rightTrigger=down;break;
        case KeyEvent.KEYCODE_BUTTON_THUMBR:p.tilt=down;break;
        case KeyEvent.KEYCODE_BUTTON_THUMBL:p.slow=down;break;
        case KeyEvent.KEYCODE_DPAD_UP:case KeyEvent.KEYCODE_DPAD_LEFT:if(down)scroll(p,1);break;
        case KeyEvent.KEYCODE_DPAD_DOWN:case KeyEvent.KEYCODE_DPAD_RIGHT:if(down)scroll(p,-1);break;
        default:return false;
        }
        return true;
    }
    private void scroll(Pad p,int amount){long now=SystemClock.uptimeMillis();if(now-p.scrolled<150)return;p.scrolled=now;touch.controllerScroll(x,y,amount);}
    boolean motion(MotionEvent event){
        if(!enabled||!event.isFromSource(InputDevice.SOURCE_JOYSTICK))return false;
        if(event.getActionMasked()==MotionEvent.ACTION_CANCEL){onInputDeviceRemoved(event.getDeviceId());return true;}
        if(event.getActionMasked()!=MotionEvent.ACTION_MOVE)return false;
        Pad p=pad(event.getDeviceId());p.x=axis(event,MotionEvent.AXIS_X);p.y=axis(event,MotionEvent.AXIS_Y);
        InputDevice device=event.getDevice();boolean z=device!=null&&device.getMotionRange(MotionEvent.AXIS_Z,event.getSource())!=null;
        p.rx=axis(event,z?MotionEvent.AXIS_Z:MotionEvent.AXIS_RX);p.ry=axis(event,z?MotionEvent.AXIS_RZ:MotionEvent.AXIS_RY);
        p.lt=Math.max(0,Math.max(axis(event,MotionEvent.AXIS_LTRIGGER),axis(event,MotionEvent.AXIS_BRAKE)));
        p.rt=Math.max(0,Math.max(axis(event,MotionEvent.AXIS_RTRIGGER),axis(event,MotionEvent.AXIS_GAS)));
        int hx=(int)Math.signum(event.getAxisValue(MotionEvent.AXIS_HAT_X)),hy=(int)Math.signum(event.getAxisValue(MotionEvent.AXIS_HAT_Y));
        if(hx!=p.hx&&hx!=0)scroll(p,-hx);if(hy!=p.hy&&hy!=0)scroll(p,-hy);p.hx=hx;p.hy=hy;
        start();return true;
    }
    private final Runnable tick=new Runnable(){public void run(){
        if(!enabled){release();return;}long now=SystemClock.uptimeMillis();float dt=Math.min(.05f,Math.max(0,(now-lastTick)/1000f));lastTick=now;
        boolean moving=false;
        for(Pad p:pads.values()){
            float speed=p.slow?.12f:.55f;
            x=Math.max(.002f,Math.min(.998f,x+p.x*speed*dt));y=Math.max(.002f,Math.min(.998f,y+p.y*speed*dt));
            float turn=((p.right?1:0)-(p.left?1:0))*.9f*dt,zoom=(Math.max(p.rt,p.rightTrigger?1:0)-Math.max(p.lt,p.leftTrigger?1:0))*1.2f*dt;
            if(p.rx!=0||p.ry!=0||turn!=0||zoom!=0)GameActivity.nativeTouchCamera(p.tilt?0:-p.rx*.45f*dt,p.tilt?0:-p.ry*.45f*dt,turn,zoom,p.tilt?p.ry*dt:0);
            moving|=p.x!=0||p.y!=0||p.rx!=0||p.ry!=0||p.left||p.right||p.lt!=0||p.rt!=0||p.leftTrigger||p.rightTrigger;
        }
        if(moving)GameActivity.nativeMouseWarp(x,y);
        invalidate();ticking=moving;if(moving)postDelayed(this,16);
    }};
    @Override protected void onDraw(Canvas canvas){
        float xx=x*getWidth(),yy=y*getHeight(),radius=9*getResources().getDisplayMetrics().density;
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(5);paint.setColor(Color.BLACK);canvas.drawCircle(xx,yy,radius,paint);
        paint.setStrokeWidth(2);paint.setColor(Color.WHITE);canvas.drawCircle(xx,yy,radius,paint);
        canvas.drawLine(xx-radius*.5f,yy,xx+radius*.5f,yy,paint);canvas.drawLine(xx,yy-radius*.5f,xx,yy+radius*.5f,paint);
    }
}
