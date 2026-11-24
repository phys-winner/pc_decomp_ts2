package org.sims2.port;

import android.app.AlertDialog;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Environment;
import android.text.InputType;
import android.util.AtomicFile;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Toast;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import org.sims2.port.launcher.FileBrowserActivity;
import org.sims2.port.launcher.Launch;

/** Edits the same profile INI that the game reads at startup. */
final class ExtenderSettings {
    private static final int MAX_BYTES = 64 * 1024;
    private static final int PICK_LC_ZIP = 17244;
    private static final String DEFAULTS =
            "[General]\nConsole = false\nSkipIntro = true\n\n"
            + "[Enhancements]\nSeparates4All = false\nFreeZodiac = false\nUIScale = false\n"
            + "UIScaleResolution = 1080\nShadowQuality = 0\nImposterQuality = 0\nDesignToolPrice = 15\n\n"
            + "[Sims 3 Camera]\nEnabled = false\nXSensitivity = 1.0\nYSensitivity = 1.0\n\n"
            + "[Fixes]\nFixRNG = true\nFixOFBUniform = true\nFixPinkFlashing = true\nFixMakeupLag = true\n"
            + "FixPoolShadows = true\nFixOutdoorShadows = true\nFixSun = true\n\n"
            + "[Misc]\nGenderedThumbnails = true\n\n"
            + "[Splash]\nEnabled = true\nVerticalCoverage = 0.5\n\n"
            + "[Legacy]\nOceanReflections = false\n\n"
            + "[Advanced]\nExtendedLua = true\nExtendedSimAntics = true\nSingleCore = false\nDisableGroupsCache = true\n";

    private ExtenderSettings() {}

    private static File profile() {
        return new File(Environment.getExternalStorageDirectory(), "Sims2/userdata");
    }

    static View button(Context context) {
        LinearLayout controls = new LinearLayout(context);
        controls.setOrientation(LinearLayout.VERTICAL);
        Button button = new Button(context);
        button.setText(ExtenderAssets.INI_NAME);
        button.setAllCaps(false);
        button.setOnClickListener(view -> open(context));
        controls.addView(button);
        if (context instanceof Activity) {
            Button archive = new Button(context);
            boolean russian = "ru".equals(Launch.language(context));
            archive.setText(russian ? "Импортировать LC ZIP" : "Import LC ZIP");
            archive.setAllCaps(false);
            archive.setOnClickListener(view -> {
                try {
                    FileBrowserActivity.pickZip((Activity)context, PICK_LC_ZIP,
                            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS));
                }
                catch (ActivityNotFoundException failure) {
                    Toast.makeText(context, russian ? "Выбор файла недоступен" : "File picker unavailable", Toast.LENGTH_LONG).show();
                }
            });
            controls.addView(archive);
        }
        return controls;
    }

    static boolean importResult(Activity activity, int request, int result, Intent data) {
        if (request != PICK_LC_ZIP) return false;
        String path = result == Activity.RESULT_OK ? FileBrowserActivity.pickedPath(data) : null;
        if (path == null) return true;
        boolean russian = "ru".equals(Launch.language(activity));
        new Thread(() -> {
            File temporary = null;
            String message;
            try {
                File directory = profile();
                if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create game profile");
                temporary = File.createTempFile("ts2extender-", ".zip", directory);
                try (InputStream stream = new FileInputStream(path);
                     OutputStream output = Files.newOutputStream(temporary.toPath())) {
                    byte[] buffer = new byte[8192];
                    int bytes = 0, count;
                    while ((count = stream.read(buffer)) != -1) {
                        bytes += count;
                        if (bytes > 32 * 1024 * 1024) throw new IOException("ZIP too large");
                        output.write(buffer, 0, count);
                    }
                }
                ExtenderAssets.zipIni(temporary);
                Files.move(temporary.toPath(), new File(directory, ExtenderAssets.ZIP_NAME).toPath(),
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                message = russian ? "Архив TS2 Extender импортирован" : "TS2 Extender ZIP imported";
            } catch (IOException | SecurityException failure) {
                android.util.Log.w("Sims2", "TS2 Extender import", failure);
                message = russian ? "Не удалось импортировать LC ZIP" : "Could not import the LC ZIP";
            } finally {
                if (temporary != null && temporary.exists() && !temporary.delete())
                    android.util.Log.w("Sims2", "Cannot remove temporary Extender ZIP");
            }
            String resultMessage = message;
            activity.runOnUiThread(() -> Toast.makeText(activity, resultMessage, Toast.LENGTH_LONG).show());
        }, "ts2-extender-import").start();
        return true;
    }

    private static void open(Context context) {
        File path = new File(profile(), ExtenderAssets.INI_NAME);
        AtomicFile file = new AtomicFile(path);
        boolean russian = "ru".equals(Launch.language(context));
        String error = russian ? "Не удалось открыть или сохранить TS2Extender.ini"
                : "Could not open or save TS2Extender.ini";
        AlertDialog.Builder builder = new AlertDialog.Builder(context, R.style.PortDialog);
        EditText input = new EditText(builder.getContext());
        input.setTypeface(Typeface.MONOSPACE);
        input.setTextSize(14);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setMinLines(6);
        input.setMaxLines(10);
        input.setGravity(Gravity.START | Gravity.TOP);
        input.setHorizontallyScrolling(false);
        try (FileInputStream stream = file.openRead()) {
            long size = stream.getChannel().size();
            if (size > MAX_BYTES) throw new IOException("INI too large");
            byte[] bytes = new byte[(int)size];
            new DataInputStream(stream).readFully(bytes);
            input.setText(new String(bytes, StandardCharsets.UTF_8));
        } catch (FileNotFoundException missing) {
            if (path.exists()) {
                Toast.makeText(context, error, Toast.LENGTH_LONG).show();
                return;
            }
            File archive = new File(profile(), ExtenderAssets.ZIP_NAME);
            try {
                input.setText(archive.isFile() ? ExtenderAssets.zipIni(archive) : DEFAULTS);
            } catch (IOException failure) {
                Toast.makeText(context, error, Toast.LENGTH_LONG).show();
                input.setText(DEFAULTS);
            }
        } catch (IOException failure) {
            Toast.makeText(context, error, Toast.LENGTH_LONG).show();
            return;
        }
        AlertDialog dialog = builder.setTitle("TS2Extender.ini").setView(input)
                .setNegativeButton(R.string.cancel, null)
                .setPositiveButton(russian ? "Сохранить" : "Save", null).create();
        dialog.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN
                | android.view.WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        dialog.show();
        dialog.getWindow().getDecorView().setForceDarkAllowed(false);
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
            byte[] bytes = input.getText().toString().getBytes(StandardCharsets.UTF_8);
            FileOutputStream out = null;
            try {
                if (bytes.length > MAX_BYTES) throw new IOException("INI too large");
                out = file.startWrite();
                out.write(bytes);
                file.finishWrite(out);
                dialog.dismiss();
            } catch (IOException failure) {
                if (out != null) file.failWrite(out);
                Toast.makeText(context, error, Toast.LENGTH_LONG).show();
            }
        });
    }
}
