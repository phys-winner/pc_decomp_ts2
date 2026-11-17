"""Run the real session logger against files, process-exit records and Java exceptions."""
import subprocess
import tempfile
from pathlib import Path

from check_touch import JDK, ROOT


def main():
    stubs = {
        'android/content/Context.java': '''package android.content;
public class Context {public static final String ACTIVITY_SERVICE="activity";public static final int MODE_PRIVATE=0;
public Object getSystemService(String s){return android.app.ActivityManager.instance;}
public String getPackageName(){return "org.sims2.port";}
public android.content.pm.PackageManager getPackageManager(){return new android.content.pm.PackageManager();}
public SharedPreferences getSharedPreferences(String name,int mode){return SharedPreferences.instance;}}''',
        'android/content/SharedPreferences.java': '''package android.content;
public class SharedPreferences {static final SharedPreferences instance=new SharedPreferences();java.util.Map<String,Long> values=new java.util.HashMap<>();
public long getLong(String key,long value){return values.getOrDefault(key,value);}public Editor edit(){return new Editor();}
public class Editor{public Editor putLong(String key,long value){values.put(key,value);return this;}public void apply(){}}}''',
        'android/content/pm/PackageManager.java': '''package android.content.pm;public class PackageManager {public PackageInfo getPackageInfo(String n,int f){return new PackageInfo();}}''',
        'android/content/pm/PackageInfo.java': '''package android.content.pm;public class PackageInfo {public String versionName="audit";public int versionCode=2;public long getLongVersionCode(){return versionCode;}}''',
        'android/os/Build.java': '''package android.os;public class Build {public static String SOC_MANUFACTURER="Test",SOC_MODEL="QA",HARDWARE="QA",MANUFACTURER="Test",MODEL="QA",DEVICE="qa";public static String[] SUPPORTED_ABIS={"arm64-v8a"};public static class VERSION{public static int SDK_INT=30;public static String RELEASE="11";}}''',
        'android/app/Application.java': '''package android.app;public class Application {public static String getProcessName(){return "org.sims2.port";}}''',
        'android/app/ActivityManager.java': '''package android.app;public class ActivityManager {public static ActivityManager instance=new ActivityManager();public java.util.List<ApplicationExitInfo> exits=new java.util.ArrayList<>();public java.util.List<ApplicationExitInfo> getHistoricalProcessExitReasons(String p,int id,int count){return exits;}}''',
        'android/util/Log.java': '''package android.util;public class Log {public static int w(String t,String s){return 0;}public static String getStackTraceString(Throwable e){return e.toString();}}''',
        'android/app/ApplicationExitInfo.java': '''package android.app;
public class ApplicationExitInfo {
public static final int REASON_EXIT_SELF=1,REASON_SIGNALED=2,REASON_LOW_MEMORY=3,REASON_CRASH=4,REASON_CRASH_NATIVE=5,REASON_ANR=6,REASON_INITIALIZATION_FAILURE=7,REASON_PERMISSION_CHANGE=8,REASON_EXCESSIVE_RESOURCE_USAGE=9,REASON_USER_REQUESTED=10,REASON_USER_STOPPED=11,REASON_DEPENDENCY_DIED=12,REASON_OTHER=13;
public String process="org.sims2.port",description="QA exit",trace="QA ANR trace";public int reason=REASON_CRASH_NATIVE,status=11;public long timestamp=100;
public String getProcessName(){return process;}public long getTimestamp(){return timestamp;}public int getReason(){return reason;}public int getStatus(){return status;}public String getDescription(){return description;}public long getPss(){return 1024;}public long getRss(){return 2048;}public java.io.InputStream getTraceInputStream() throws java.io.IOException{return new java.io.ByteArrayInputStream(trace.getBytes(java.nio.charset.StandardCharsets.UTF_8));}}
''',
        'org/sims2/port/SessionLogCheck.java': r'''package org.sims2.port;
import java.nio.file.*;import java.nio.charset.StandardCharsets;import android.app.*;
public class SessionLogCheck {
static String read(Path p)throws Exception{return new String(Files.readAllBytes(p),StandardCharsets.UTF_8);}
static void restart()throws Exception{java.lang.reflect.Field f=SessionLog.class.getDeclaredField("begun");f.setAccessible(true);f.setBoolean(null,false);}
public static void main(String[] args)throws Exception{
 Path data=Paths.get(args[0]);Files.createDirectories(data);Path log=data.resolve("sims2.log"),old=data.resolve("sims2.1.log"),older=data.resolve("sims2.2.log");
 Files.write(log,"crashed session\n".getBytes(StandardCharsets.UTF_8));Files.write(old,"previous session\n".getBytes(StandardCharsets.UTF_8));Files.write(older,new byte[]{1});
 final int[] exceptions={0};Thread.setDefaultUncaughtExceptionHandler((t,e)->exceptions[0]++);
 ApplicationExitInfo unrelated=new ApplicationExitInfo();unrelated.process="org.sims2.port:launcher";unrelated.timestamp=200;ActivityManager.instance.exits.add(unrelated);
 ApplicationExitInfo exit=new ApplicationExitInfo();ActivityManager.instance.exits.add(exit);
 android.content.Context c=new android.content.Context();SessionLog.begin(c,data.toFile());
 assert read(old).contains("crashed session")&&read(old).contains("native crash")&&read(old).contains("signal 11");
 assert read(older).equals("previous session\n");assert read(log).contains("The Sims 2 audit (2)")&&read(log).contains("Android 11");
 byte[] snapshot=Files.readAllBytes(old);SessionLog.begin(c,data.toFile());assert java.util.Arrays.equals(snapshot,Files.readAllBytes(old));
 Thread.getDefaultUncaughtExceptionHandler().uncaughtException(Thread.currentThread(),new IllegalStateException("QA Java crash"));
 assert exceptions[0]==1&&read(log).contains("QA Java crash");
 restart();SessionLog.begin(c,data.toFile());assert !read(log).contains("signal 11"):"same exit reported twice";
 restart();exit.reason=ApplicationExitInfo.REASON_ANR;exit.timestamp=300;SessionLog.begin(c,data.toFile());
 assert read(old).contains("QA ANR trace")&&read(old).contains("not responding");
 System.out.println("PASS real SessionLog: rotation, native crash, process isolation, duplicate exit suppression, Java fallback and ANR trace");
}}
''',
    }
    with tempfile.TemporaryDirectory(prefix='sims2-session-log-') as directory:
        out = Path(directory)
        paths = []
        for name, text in stubs.items():
            path = out/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
            paths.append(str(path))
        paths.append(str(ROOT/'android/app/src/org/sims2/port/SessionLog.java'))
        subprocess.run([str(JDK/'bin/javac.exe'), '--release', '8', '-encoding', 'UTF-8', '-d', str(out), *paths], check=True)
        subprocess.run([str(JDK/'bin/java.exe'), '-ea', '-cp', str(out), 'org.sims2.port.SessionLogCheck', str(out/'data')], check=True)


if __name__ == '__main__':
    main()
