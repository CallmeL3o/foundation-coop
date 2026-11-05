// Functions that reference ALL strings listed (one per line) in E:/FoundationCoopTest/re/combo.txt; decompile them.
// Output: E:/FoundationCoopTest/re/out8/
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class FindStrCombo extends GhidraScript {
    @Override
    public void run() throws Exception {
        List<String> want = new ArrayList<>();
        for (String l : Files.readAllLines(Paths.get("E:/FoundationCoopTest/re/combo.txt"))) if (!l.trim().isEmpty()) want.add(l.trim());
        File out = new File("E:/FoundationCoopTest/re/out8"); out.mkdirs();
        Map<Function, Set<String>> hits = new HashMap<>();
        Memory mem = currentProgram.getMemory();
        for (String s : want) {
            byte[] pat = (s + "\0").getBytes(StandardCharsets.US_ASCII);
            Address a = mem.findBytes(currentProgram.getMinAddress(), pat, null, true, monitor);
            while (a != null) {
                // only whole strings (preceded by NUL)
                byte prev = 1; try { prev = mem.getByte(a.subtract(1)); } catch (Exception e) {}
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) hits.computeIfAbsent(f, k -> new HashSet<>()).add(s);
                }
                a = mem.findBytes(a.add(1), pat, null, true, monitor);
            }
        }
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram); for (String s : want) { int c = 0; for (Set<String> v : hits.values()) if (v.contains(s)) c++; println("hits " + s + ": " + c + " functions"); } for (Map.Entry<Function, Set<String>> e2 : hits.entrySet()) println("fn " + e2.getKey().getEntryPoint() + " " + e2.getValue());
        try (PrintWriter sum = new PrintWriter(new File(out, "summary.txt"))) {
            for (Map.Entry<Function, Set<String>> e : hits.entrySet()) {
                if (e.getValue().size() < want.size()) continue;
                Function f = e.getKey();
                sum.println(f.getName() + " @ " + f.getEntryPoint());
                DecompileResults res = dec.decompileFunction(f, 90, monitor);
                try (PrintWriter w = new PrintWriter(new File(out, f.getEntryPoint() + ".c"))) {
                    w.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed");
                }
            }
        }
        println("done");
    }
}
