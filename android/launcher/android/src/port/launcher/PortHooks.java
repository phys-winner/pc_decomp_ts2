package port.launcher;

import android.content.Context;
import android.content.Intent;
import java.io.File;
import java.io.IOException;
import java.util.Collections;
import java.util.List;
import java.util.Set;

/** What only the game knows, for the launcher: a class of the game named by port.json's
 *  "app.hooks" implements it (all methods are optional). The launcher's process loads it: it must
 *  not touch the game's activity or native library. */
public interface PortHooks {
    /** A reason an optional choice cannot be used yet; null when its files are available. */
    default String optionProblem(Context context, String key, String value) { return null; }
    /** A warning to confirm before saving a choice; may carry styled text. Null accepts it. */
    default CharSequence optionWarning(Context context, String key, String value) { return null; }

    /** Optional editor belonging to a game-specific launcher option. */
    default android.view.View optionEditor(Context context, String key) { return null; }

    /** Returns true if a game-specific option handled this picker result. */
    default boolean optionResult(android.app.Activity activity, int request, int result, Intent data) { return false; }
    /** Paths of a part, relative to its folder, which the game also needs and which are missing
     *  there now, beyond port.json's "required": what depends on the game's own settings (UT3: the
     *  language chosen in its settings). Empty when nothing is missing. */
    default List<String> missing(Context context, String distribution, String part, File folder) { return Collections.emptyList(); }

    /** Refuses an incompatible installation destination before any files are changed; null accepts it. */
    default String installationProblem(Context context, String distribution, String part, File folder) { return null; }

    /** Full original-input verification, on an import/selection worker before the new folder is
     * committed. A game can hash its small EXE/DLL here; this never runs on the UI thread. */
    default void verifyInstallation(Context context, String distribution, String part, File folder) throws java.io.IOException { }

    /** The renderer the game would start with when the game keeps the choice in its own settings
     *  (an in-game options page); null: the launcher keeps it. */
    default String renderer(Context context) { return null; }

    /** Stores the player's renderer where the game reads it. */
    default void setRenderer(Context context, String mode) { }

    /** Adds the game's own extras to the intent that starts a distribution. */
    default void prepare(Context context, String distribution, Intent intent) { }

    /** A one-time question before this start: title, message, accept, decline. Null starts at once.
     *  The hook records that it asked; the launcher does not. */
    default String[] startOffer(Context context) { return null; }

    /** Saves the answer from startOffer. False leaves the player in the launcher.
     *  accept is the minimal-graphics choice. An ETC2 choice is the launcher's own work. */
    default boolean startOfferAnswer(Context context, boolean accept) { return true; }

    /** True when the plain startOffer dialog should also offer an ETC2 texture conversion.
     *  probed/etc2 come from the GLES probe. nativeBcUnsupported is true only when DXT/BC is
     *  known to be missing. alreadyConverted is a finished scan that found a cache.
     *  converterPresent means this APK has the converter and it can be constructed. */
    default boolean offerEtc2(Context context, boolean probed, boolean etc2, boolean nativeBcUnsupported,
            boolean alreadyConverted, boolean converterPresent) { return false; }

    /** Title, body, continue, graphics choice, ETC2 choice for that dialog. Null keeps the plain one. */
    default String[] startOfferEtc2(Context context) { return null; }

    /** Libraries this build carries packed (its assets) instead of in nativeLibraryDir, named as
     *  port.json's renderer "libraries" name them: they count as present, and the game installs
     *  them itself when a start needs them (Heroes V: the DXVK/Turnip runtime, about 19 MB that
     *  players who never choose DXVK would otherwise have installed). */
    default Set<String> packedLibraries(Context context) { return Collections.emptySet(); }

    /** For the driver check of a Turnip mode, in the check's own process (Probe, not the UI
     *  thread): a folder holding that mode's libraries (port.json "libraries"), unpacked here when
     *  the build packs them; null: nativeLibraryDir. The check loads libadrenotools.so, the hooks
     *  and the driver from it, so whatever they need besides the system must already be loaded or
     *  be in that folder's linker reach (Heroes V loads its libc++_shared.so from it first). */
    default File checkLibraries(Context context, String mode, List<String> libraries) throws IOException { return null; }

    /** The driver check is done with the folder checkLibraries returned (a port may remove what it
     *  unpacked only for the check). Not called when the check crashed: clean up on the next call. */
    default void checkedLibraries(Context context, String mode, File folder) { }

    /** Once per process, before the launcher or Launch.attach reads the player's choices: a game
     *  that had a launcher of its own hands its settings over here with Launch.adopt (Heroes V: the
     *  game folders and options its old launcher kept), so that an update keeps the player's setup. */
    default void migrate(Context context) { }

    /** Loads the hooks of port.json, or the defaults. */
    static PortHooks of(Context context) {
        String name = Spec.get(context).hooks;
        if (!name.isEmpty()) {
            try {
                return (PortHooks) Class.forName(name).getDeclaredConstructor().newInstance();
            } catch (ReflectiveOperationException | ClassCastException e) {
                android.util.Log.e("Launcher", "hooks " + name + ": " + e);
            }
        }
        return new PortHooks() { };
    }
}
