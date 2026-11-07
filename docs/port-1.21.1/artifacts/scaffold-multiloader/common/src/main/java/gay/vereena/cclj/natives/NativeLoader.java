package gay.vereena.cclj.natives;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.*;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Locale;

/**
 * Prototype loader for a single, self-contained JNI library per platform (LuaJIT statically linked in).
 * <p>
 * Jar layout:  natives/&lt;os&gt;-&lt;arch&gt;/&lt;libname&gt;   e.g. natives/windows-x86_64/cclj.dll,
 * natives/linux-x86_64/libcclj.so, natives/linux-aarch64/libcclj.so, natives/macos-x86_64/libcclj.dylib,
 * natives/macos-aarch64/libcclj.dylib.
 * <p>
 * Extraction: &lt;cacheRoot&gt;/&lt;sha256-of-library-bytes&gt;/&lt;libname&gt;. The directory name is the content hash, so
 * <ul>
 *   <li>different mod versions never collide and never overwrite a DLL another process has loaded
 *       (Windows cannot delete or replace a loaded DLL);</li>
 *   <li>an existing file is re-hashed and reused only if it matches (detects truncated/corrupted extractions);</li>
 *   <li>concurrent JVMs (several servers sharing one game dir, client + integrated server) each write to a private
 *       temp file and publish it with an atomic rename; the loser of the race just loads the winner's identical file.</li>
 * </ul>
 * Stale hash directories from older versions are deleted best-effort (failure on Windows is expected while another
 * process still has them loaded, and is ignored).
 */
public final class NativeLoader {
    private NativeLoader() {}

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
        return platformId.startsWith("windows") ? "cclj.dll" : platformId.startsWith("macos") ? "libcclj.dylib" : "libcclj.so";
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
            if (in == null) throw new IOException("No native library bundled for " + pid + " (" + resource + ")");
            bytes = in.readAllBytes();
        }
        String hash = sha256(bytes);
        Path dir = cacheRoot.resolve(hash.substring(0, 16));
        Path target = dir.resolve(name);

        if (!(Files.isRegularFile(target) && sha256(Files.readAllBytes(target)).equals(hash))) {
            Files.createDirectories(dir);
            Path tmp = Files.createTempFile(dir, name, ".tmp");
            try {
                Files.write(tmp, bytes);
                try {
                    Files.move(tmp, target, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                } catch (IOException e) {
                    // Windows: target exists and is loaded by another JVM -> cannot replace. Fine if it is identical.
                    if (!(Files.isRegularFile(target) && sha256(Files.readAllBytes(target)).equals(hash))) throw e;
                }
            } finally {
                Files.deleteIfExists(tmp);
            }
        }
        System.load(target.toAbsolutePath().toString());
        cleanupStale(cacheRoot, dir);
        return target;
    }

    private static void cleanupStale(Path cacheRoot, Path keep) {
        try (var dirs = Files.newDirectoryStream(cacheRoot, Files::isDirectory)) {
            for (Path d : dirs) {
                if (d.equals(keep)) continue;
                try (var files = Files.newDirectoryStream(d)) {
                    for (Path f : files) Files.deleteIfExists(f);
                    Files.deleteIfExists(d);
                } catch (IOException ignored) {
                    // Still loaded by another process (Windows) - leave it for next time.
                }
            }
        } catch (IOException ignored) {
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
