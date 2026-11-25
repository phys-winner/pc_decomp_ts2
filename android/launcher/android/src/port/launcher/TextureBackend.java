package port.launcher;

import android.content.Context;
import java.io.File;
import java.util.Collections;
import java.util.List;

/** A game's texture converter (port.json "textures.backend"): re-encodes the game's DXT textures
 *  into a format the GPU samples (ETC2 or ASTC) in the game's own container. The launcher shows the
 *  card, runs the work in the background and keeps the player informed; how the textures are found
 *  and stored stays the game's (Heroes V: zz_port_textures.pak per data bank; The Void: one .tvt). */
public interface TextureBackend {
    int ETC2 = 1, ASTC = 2;

    /** False when this build of the converter cannot write the format (The Sims 2: no ASTC encoder in its
     *  32-bit library); the launcher then does not offer that conversion. */
    default boolean canEncode(int format) { return format == ETC2 || format == ASTC; }

    /** Selective conversion: true when convert(..., sources) converts only the textures of the given source
     *  formats and the game's renderer reads the cache per texture, keeping the originals of all the others
     *  (and adds to an existing cache of the same target format). Only then does the launcher convert just what
     *  the selected renderer lacks. False (the default): a conversion re-encodes the whole data set, as before,
     *  and the launcher says so in its offer. A port whose conversion replaces the game's files as a whole
     *  must keep false. */
    default boolean selective() { return false; }

    final class Scan {
        public long textures, bytes;          /* convertible originals and their size */
        public boolean sampled;              /* one package checked; full discovery happens on conversion */
        public boolean estimated;            /* the game supplied an estimate of the full corpus */
        /** Exact original sampling formats from a complete scan, or null to use textures.formats. */
        public List<String> formats;
        public int converted;                 /* 0, ETC2 or ASTC: an existing conversion */
        public long convertedTextures, convertedBytes;
        public String failure;                /* why the game's files could not be read; null */
        public boolean unreadableArchive;
        /** What removeOriginals() would free after a conversion; 0: nothing (or not offered). */
        public long removable;
        /** The originals were removed after the conversion: it can no longer be undone. */
        public boolean originalsRemoved;
        /** The source formats (TextureFormats names) the existing conversion covers, when the backend knows them
         *  (a selective backend reads them from its cache); null: the launcher's record of the conversions it ran,
         *  else all of them. */
        public List<String> convertedFormats;
    }

    /** Blocking: what there is to convert in the game folder. */
    Scan scan(File game);

    /** Stored ZIP: the read-only descriptor remains open until this call returns. */
    default Scan scan(File game, File index, int archiveFd) { return scan(game); }

    /** The cache root can be on a different volume from the original files. */
    default Scan scan(File game, File cache, File index, int archiveFd) { return scan(game, index, archiveFd); }

    /** Blocking: converts, reporting through progress(); 0 when done, else message() says why. */
    int convert(File game, int format, int threads);

    default int convert(File game, int format, int threads, File index, int archiveFd) {
        return convert(game, format, threads);
    }

    default int convert(File game, File cache, int format, int threads, File index, int archiveFd) {
        return convert(game, format, threads, index, archiveFd);
    }

    /** Blocking: converts the textures whose source format is one of `sources` (TextureFormats names: "DXT1",
     *  "BC5", …; null: all of them). The launcher passes sources only to a selective() backend; the default
     *  converts everything. */
    default int convert(File game, File cache, int format, int threads, File index, int archiveFd, List<String> sources) {
        return convert(game, cache, format, threads, index, archiveFd);
    }

    /** Blocking: removes a conversion; 0 when done. */
    int undo(File game);

    default int undo(File game, File cache) { return undo(game); }

    /** Blocking, optional (Scan.removable offers it): removes the original textures a conversion
     *  replaced from the game's files, for space; the conversion can no longer be undone then.
     *  0 when done. */
    default int removeOriginals(File game) { return -1; }

    final class Mod {
        public final File archive;
        public final String key;
        public final long bytes;
        public final boolean offered;

        public Mod(File archive, String key, long bytes, boolean offered) {
            this.archive = archive;
            this.key = key;
            this.bytes = bytes;
            this.offered = offered;
        }
    }

    /** Optional, metadata only. A play request may call this before any archive scan. */
    default List<Mod> pendingMods(Context context, String distribution, File game) { return Collections.emptyList(); }
    default void markMods(Context context, List<Mod> mods, boolean ignore) { }
    default int convertMods(Context context, List<Mod> mods) { return 1; }
    default String currentMod() { return ""; }
    default boolean unreadableArchive() { return false; }

    void cancel();

    /** {done, total, bytesDone, bytesTotal} of the running operation. */
    long[] progress();

    String message();
}
