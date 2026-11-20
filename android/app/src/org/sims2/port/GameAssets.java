package org.sims2.port;

import android.content.res.AssetManager;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** APK-owned resources are installed once per version, before the engine starts. */
final class GameAssets {
    static synchronized File prepare(AssetManager assets,File files) throws IOException {
        String version;
        try(java.io.DataInputStream input=new java.io.DataInputStream(assets.open("runtime.version"))){
            byte[] bytes=new byte[64];input.readFully(bytes);
            if(input.read()!=-1)throw new IOException("Invalid port resource version length");
            version=new String(bytes,StandardCharsets.US_ASCII);
        }
        if(!version.matches("[a-f0-9]{64}"))throw new IOException("Invalid port resource version");
        File target=new File(files,"runtime"),ready=new File(target,".ready");
        if(ready.isFile()&&version.equals(new String(Files.readAllBytes(ready.toPath()),StandardCharsets.US_ASCII)))return target;
        File staging=new File(files,"runtime.new");
        remove(staging);
        if(!staging.mkdirs())throw new IOException("Cannot create port resource directory");
        String prefix=staging.getCanonicalPath()+File.separator;
        try(ZipInputStream archive=new ZipInputStream(assets.open("runtime.zip"))){
            byte[] buffer=new byte[65536];ZipEntry entry;
            while((entry=archive.getNextEntry())!=null){
                File output=new File(staging,entry.getName());
                /* overlay/: the port's resource overlays; shaders/: the native renderers' shader library */
                if(!output.getCanonicalPath().startsWith(prefix)||!(entry.getName().startsWith("overlay/")||entry.getName().startsWith("shaders/")))
                    throw new IOException("Invalid port resource path");
                File directory=entry.isDirectory()?output:output.getParentFile();
                if(!directory.isDirectory()&&!directory.mkdirs())throw new IOException("Cannot create "+directory);
                if(!entry.isDirectory())try(FileOutputStream stream=new FileOutputStream(output)){
                    int count;while((count=archive.read(buffer))!=-1)stream.write(buffer,0,count);
                }
                archive.closeEntry();
            }
        }
        Files.write(new File(staging,".ready").toPath(),version.getBytes(StandardCharsets.US_ASCII));
        remove(target);
        if(!staging.renameTo(target))throw new IOException("Cannot install port resources");
        return target;
    }
    private static void remove(File file) throws IOException {
        if(file.isDirectory()){
            File[] children=file.listFiles();if(children==null)throw new IOException("Cannot read "+file);
            for(File child:children)remove(child);
        }
        if(file.exists()&&!file.delete())throw new IOException("Cannot remove "+file);
    }
}
