package io.github.mindbound.cclj.natives;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.*;
import java.nio.file.attribute.FileTime;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Duration;
import java.time.Instant;
import java.util.HexFormat;
import java.util.Locale;

/**
 * Prototype loader for a single, self-contained JNI library per platform (LuaJIT statically linked in).
 * <p>
 * Jar layout:  natives/&lt;os&gt;-&lt;arch&gt;/&lt;libname&gt;   e.g. natives/windows-x86_64/cclj.dll,
 * natives/linux-x86_64/libcclj.so, natives/linux-aarch64/libcclj.so, natives/macos-x86_64/libcclj.dylib,
 * natives/macos-aarch64/libcclj.dylib.
 * <p>
 * Extraction: &lt;cacheRoot&gt;/&lt;first 16 hex digits of the sha256 of the library bytes&gt;/&lt;libname&gt;.
 * The directory name is derived from the content hash, so
 * <ul>
 *   <li>different mod versions never collide and never overwrite a DLL another process has loaded
 *       (Windows cannot delete or replace a loaded DLL);</li>
 *   <li>an existing file is re-hashed and reused only if it matches (detects truncated/corrupted extractions);</li>
 *   <li>concurrent JVMs (several servers sharing one game dir, client + integrated server) each write to a
 *       private temp file and publish it with an atomic rename; the loser of the race just loads the winner's
 *       identical file.</li>
 * </ul>
 * Stale hash directories from other versions are deleted best effort, once no JVM has loaded from them or extracted
 * into them for a day: the directory's modification time, which each load sets before it checks or extracts the
 * file. A JVM that keeps the library loaded longer does not keep its directory fresh; on Windows the deletion then
 * fails and is ignored, and on Linux and macOS it succeeds without harm to that JVM (its mapping stays). The mark
 * keeps another version that shares the cache from deleting the directory between the check and {@code System.load};
 * a file that is gone at the check is extracted again. What is left is a cleanup that read the old time just before
 * the mark and deletes just after it (two different builds sharing one cache root; best effort).
 */
public final class NativeLoader {
    private NativeLoader() {}

    /**
     * How long ago a hash directory must last have been loaded from or extracted into (its modification time) before
     * another version's cleanup deletes it.
     */
    static final Duration STALE_AFTER = Duration.ofDays(1);

    /** This build of the mod contains no native library for the current platform (an expected, permanent state). */
    public static final class NotBundledException extends IOException {
        public NotBundledException(String message) {
            super(message);
        }
    }

    public static String platformId() {
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        String arch = System.getProperty("os.arch").toLowerCase(Locale.ROOT);
        String o = os.startsWith("windows") ? "windows" : os.startsWith("mac") || os.startsWith("darwin") ? "macos"
                 : os.startsWith("linux") ? "linux" : null;
        String a = switch (arch) {
            case "amd64", "x86_64", "x64" -> "x86_64";
            case "aarch64", "arm64" -> "aarch64";
            default -> null;
        };
        if (o == null || a == null) throw new UnsupportedOperationException("Unsupported platform " + os + "/" + arch);
        return o + "-" + a;
    }

    public static String libraryFileName(String platformId) {
        return platformId.startsWith("windows") ? "cclj.dll"
            : platformId.startsWith("macos") ? "libcclj.dylib" : "libcclj.so";
    }

    /** Extract (if needed) and System.load the library for the current platform. Returns the loaded path. */
    public static Path load(Path cacheRoot) throws IOException {
        String pid = platformId();
        String name = libraryFileName(pid);
        String resource = "natives/" + pid + "/" + name;
        byte[] bytes;
        // Class#getResourceAsStream resolves against this mod's own jar/module, which also works under NeoForge's
        // module layer (a ClassLoader lookup can be subject to JPMS resource encapsulation).
        try (InputStream in = NativeLoader.class.getResourceAsStream("/" + resource)) {
            if (in == null) {
                throw new NotBundledException("No native library bundled for " + pid + " (" + resource + ")");
            }
            bytes = in.readAllBytes();
        }
        String hash = sha256(bytes);
        Path dir = cacheRoot.resolve(hash.substring(0, 16));
        Path target = dir.resolve(name);

        try {
            Files.setLastModifiedTime(dir, FileTime.from(Instant.now()));   // in use: see the class comment
        } catch (IOException | RuntimeException ignored) {
            // Not extracted yet (the extraction below makes the time fresh), or a cache this JVM may not change the
            // time of: it still loads, and another version's cleanup may delete it a day after its last mark.
        }
        if (!intact(target, hash)) {
            Files.createDirectories(dir);
            Path tmp = Files.createTempFile(dir, name, ".tmp");
            try {
                Files.write(tmp, bytes);
                try {
                    Files.move(tmp, target, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                } catch (IOException e) {
                    // Windows: target exists and is loaded by another JVM -> cannot replace. Fine if it is identical.
                    if (!intact(target, hash)) throw e;
                }
            } finally {
                Files.deleteIfExists(tmp);
            }
        }
        System.load(target.toAbsolutePath().toString());
        cleanupStale(cacheRoot, dir, Files::isDirectory, Instant.now());
        return target;
    }

    /**
     * Deletes the directories below {@code cacheRoot} that {@code dirs} accepts (load passes Files::isDirectory:
     * every directory, a link to one included; only hash directories should be there), except {@code keep} and any
     * whose modification time (which each load sets) is within {@link #STALE_AFTER} before {@code now}. Best effort:
     * the library is loaded by now, so nothing here may throw, or NativeLibrary would keep the exception as a
     * permanent failure. An I/O error while a directory is listed comes out of its iterator as the unchecked
     * {@link DirectoryIteratorException}, so {@link RuntimeException} is caught too. Package-private, with the filter
     * and the time, for BridgeProbe.
     */
    static void cleanupStale(Path cacheRoot, Path keep, DirectoryStream.Filter<? super Path> dirs, Instant now) {
        try (var stream = Files.newDirectoryStream(cacheRoot, dirs)) {
            for (Path d : stream) {
                if (d.equals(keep)) continue;
                try {
                    if (Files.getLastModifiedTime(d).toInstant().isAfter(now.minus(STALE_AFTER))) continue;
                    try (var files = Files.newDirectoryStream(d)) {
                        for (Path f : files) Files.deleteIfExists(f);
                    }
                    Files.deleteIfExists(d);
                } catch (IOException | RuntimeException ignored) {
                    // Still loaded by another process (Windows), or unreadable: left for next time.
                }
            }
        } catch (IOException | RuntimeException ignored) {
            // The cache root cannot be listed: nothing more to clean this time.
        }
    }

    /** True if {@code target} is a file with the sha256 {@code hash}. An I/O error (the file went away) is false. */
    private static boolean intact(Path target, String hash) {
        try {
            return Files.isRegularFile(target) && sha256(Files.readAllBytes(target)).equals(hash);
        } catch (IOException e) {
            return false;
        }
    }

    private static String sha256(byte[] data) {
        try {
            return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(data));
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }
}
