// Find functions that reference given strings and dump their decompiled C.
// Output: E:/FoundationCoopTest/re/out/<tag>.c and a summary in E:/FoundationCoopTest/re/out/summary.txt
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class DumpStrRefs extends GhidraScript {
    static String[][] TARGETS;
    static { try { java.util.List<String> l = java.nio.file.Files.readAllLines(java.nio.file.Paths.get("E:/FoundationCoopTest/re/strings.txt"));
        java.util.List<String[]> t = new java.util.ArrayList<>(); for (String x : l) { x = x.trim(); if (x.isEmpty()) continue; int i = x.indexOf(' '); t.add(new String[]{x.substring(0, i), x.substring(i + 1)}); }
        TARGETS = t.toArray(new String[0][]); } catch (Exception e) { TARGETS = new String[0][]; } }

    @Override
    public void run() throws Exception {
        File outDir = new File("E:/FoundationCoopTest/re/out6");
        outDir.mkdirs();
        PrintWriter sum = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(outDir, "summary.txt")), StandardCharsets.UTF_8));
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Function> dumped = new HashSet<>();
        for (String[] t : TARGETS) {
            String tag = t[0], needle = t[1];
            List<Address> hits = findAscii(needle);
            sum.println("== " + tag + " '" + needle + "' string hits: " + hits);
            for (Address s : hits) {
                for (Reference r : getReferencesTo(s)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    sum.println("   ref from " + r.getFromAddress() + " in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                    if (f != null && dumped.add(f)) {
                        DecompileResults res = dec.decompileFunction(f, 120, monitor);
                        String c = res.decompileCompleted() ? res.getDecompiledFunction().getC() : ("// decompile failed: " + res.getErrorMessage());
                        try (PrintWriter w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(outDir, tag + "_" + f.getEntryPoint() + ".c")), StandardCharsets.UTF_8))) {
                            w.println("// " + f.getName() + " @ " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses());
                            w.println(c);
                        }
                    }
                }
            }
        }
        sum.close();
        println("done, dumped " + dumped.size() + " functions");
    }

    List<Address> findAscii(String needle) throws Exception {
        List<Address> out = new ArrayList<>();
        byte[] pat = needle.getBytes(StandardCharsets.US_ASCII);
        Memory mem = currentProgram.getMemory();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized()) continue;
            Address a = b.getStart();
            while (a != null && a.compareTo(b.getEnd()) <= 0) {
                Address hit = mem.findBytes(a, b.getEnd(), pat, null, true, monitor);
                if (hit == null) break;
                // walk back to the string start (the needle may be in the middle)
                Address start = hit;
                while (start.compareTo(b.getStart()) > 0) {
                    byte prev = mem.getByte(start.subtract(1));
                    if (prev == 0 || prev < 0x20) break;
                    start = start.subtract(1);
                }
                out.add(start);
                a = hit.add(pat.length);
            }
        }
        return out;
    }
}
