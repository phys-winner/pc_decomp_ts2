package org.sims2.port.launcher;

import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Color;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.Toast;

/** The in-game port dialog edits the same validated choices as the launcher. */
public final class PortOptions {
    private PortOptions() {}

    /** FSR 3.1 left the port (too slow on phones): a saved choice of it ("3") becomes FSR 1, which
     *  the game also takes "3" for (sims2_settings.c), so the option shows what the game uses. */
    public static void migrateUpscaler(Context context) {
        if ("3".equals(State.get(context, State.CHOICES, "option.render_upscaler", null)))
            State.put(context, State.CHOICES, "option.render_upscaler", "1");
    }

    public static void append(Context context, LinearLayout content, Launch launch) {
        Spec spec = Spec.get(context);
        for (Spec.Option option : spec.options(launch.distribution())) {
            if (option.rendererSetting || (option.arm64Only && !android.os.Process.is64Bit())) continue;
            if ("bool".equals(option.type)) {
                android.widget.CheckBox check = new android.widget.CheckBox(context);
                check.setText(Texts.of(context, option.title));
                check.setTextColor(Color.BLACK);
                String value = Launch.option(context, option);
                check.setChecked("1".equals(value) || "true".equals(value));
                check.setOnClickListener(v -> {
                    if (!State.put(context, State.CHOICES, "option." + option.key, check.isChecked() ? "1" : "0")) {
                        check.setChecked(!check.isChecked());
                        Toast.makeText(context, context.getString(org.sims2.port.R.string.settings_save_failed), Toast.LENGTH_LONG).show();
                    }
                });
                content.addView(check);
                continue;
            }
            if (!"choice".equals(option.type)) continue;
            Button button = new Button(context);
            button.setAllCaps(false);
            button.setTextColor(Color.BLACK);
            Runnable refresh = () -> {
                String value = Launch.option(context, option);
                int index = option.choices.indexOf(value);
                button.setText(Texts.of(context, option.title) + ": " +
                        (index < 0 ? value : Texts.of(context, option.choiceTitles.get(index))));
            };
            refresh.run();
            button.setOnClickListener(v -> {
                String[] labels = new String[option.choices.size()];
                for (int i = 0; i < labels.length; i++) labels[i] = Texts.of(context, option.choiceTitles.get(i));
                AlertDialog dialog = new AlertDialog.Builder(context, org.sims2.port.R.style.PortDialog)
                        .setTitle(Texts.of(context, option.title))
                        .setSingleChoiceItems(labels, option.choices.indexOf(Launch.option(context, option)), (d, which) -> {
                            if (which < 0 || which >= option.choices.size()) return;
                            String value = option.choices.get(which);
                            Runnable save = () -> {
                                String problem = PortHooks.of(context).optionProblem(context, option.key, value);
                                if (problem != null || !State.put(context, State.CHOICES, "option." + option.key, value)) {
                                    Toast.makeText(context, problem != null ? problem : context.getString(org.sims2.port.R.string.settings_save_failed), Toast.LENGTH_LONG).show();
                                    return;
                                }
                                refresh.run();
                                d.dismiss();
                            };
                            CharSequence warning = PortHooks.of(context).optionWarning(context, option.key, value);
                            if (warning == null) save.run();
                            else {
                                d.dismiss();
                                new AlertDialog.Builder(context, org.sims2.port.R.style.PortDialog)
                                    .setTitle(Texts.of(context, option.title)).setMessage(warning)
                                    .setPositiveButton(context.getString(org.sims2.port.R.string.enable), (confirm, buttonId) -> save.run())
                                    .setNegativeButton(context.getString(org.sims2.port.R.string.cancel), null).show();
                            }
                        }).setNegativeButton(context.getString(org.sims2.port.R.string.cancel), null).create();
                dialog.show();
                dialog.getWindow().getDecorView().setForceDarkAllowed(false);
            });
            content.addView(button);
        }
    }
}
