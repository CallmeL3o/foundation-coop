// For each "Name hexaddr count" line in E:/FoundationCoopTest/re/vtables.txt, list vtable slots and decompile them.
// Output: E:/FoundationCoopTest/re/out5/<Name>.txt
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

public class DumpVtables extends GhidraScript {
    @Override
    public void run() throws Exception {
        File out = new File("E:/FoundationCoopTest/re/out5"); out.mkdirs();
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        for (String l : Files.readAllLines(Paths.get("E:/FoundationCoopTest/re/vtables.txt"))) {
            String[] p = l.trim().split("\\s+"); if (p.length < 3) continue;
            Address vt = toAddr(p[1]); int n = Integer.parseInt(p[2]);
            try (PrintWriter w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, p[0] + ".txt")), StandardCharsets.UTF_8))) {
                for (int i = 0; i < n; i++) {
                    long target = getLong(vt.add(8L * i));
                    Function f = getFunctionAt(toAddr(target));
                    w.println("==== slot " + i + " (+0x" + Integer.toHexString(8 * i) + ") -> " + Long.toHexString(target) + " " + (f == null ? "?" : f.getName()));
                    if (f != null && f.getBody().getNumAddresses() < 1500) {
                        DecompileResults res = dec.decompileFunction(f, 60, monitor);
                        if (res.decompileCompleted()) w.println(res.getDecompiledFunction().getC());
                    }
                }
            }
        }
        println("done");
    }
}
