// Third oracle for the differential parse test: compile each case of a pack with Cobalt (CC:T's VM).
// usage: java -cp cobalt-0.9.9.jar CobaltParse.java <pack-file> <out-file>
// Output lines: <id>\tOK  or  <id>\tERR\t<message>  (Cobalt shortens "=name" chunk names to 29 chars).
import org.squiddev.cobalt.LuaState;
import org.squiddev.cobalt.compiler.CompileException;
import org.squiddev.cobalt.compiler.LuaC;

import java.io.ByteArrayInputStream;
import java.io.FileOutputStream;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public class CobaltParse {
    public static void main(String[] args) throws Exception {
        byte[] data = Files.readAllBytes(Path.of(args[0]));
        try (PrintStream out = new PrintStream(new FileOutputStream(args[1]), false, StandardCharsets.ISO_8859_1)) {
            int pos = 0, n = 0;
            while (pos < data.length) {
                int nl = pos;
                while (data[nl] != '\n') nl++;
                String[] hdr = new String(data, pos, nl - pos, StandardCharsets.ISO_8859_1).split(" ");
                String id = hdr[1];
                int len = Integer.parseInt(hdr[2]);
                byte[] src = java.util.Arrays.copyOfRange(data, nl + 1, nl + 1 + len);
                pos = nl + 1 + len + 1;
                n++;
                LuaState state = LuaState.builder().build();
                String res;
                try {
                    LuaC.compile(state, new ByteArrayInputStream(src), "=" + id);
                    res = "OK";
                } catch (CompileException e) {
                    res = "ERR\t" + e.getMessage().replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t");
                } catch (Throwable e) {
                    res = "THROW\t" + e;
                }
                out.print(id + "\t" + res + "\n");
            }
            out.print("@@END\t" + n + "\n");
        }
    }
}
