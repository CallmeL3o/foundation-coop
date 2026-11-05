// Dump functions listed in E:/FoundationCoopTest/re/addrs.txt ("tag hexaddr" per line), plus their callers (decompiled).
// Output: E:/FoundationCoopTest/re/out3/
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class DumpAddrs extends GhidraScript {
    DecompInterface dec;
    File out;
    Set<Address> done = new HashSet<>();

    @Override
    public void run() throws Exception {
        out = new File("E:/FoundationCoopTest/re/out3");
        out.mkdirs();
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        PrintWriter sum = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, "summary.txt"), true), StandardCharsets.UTF_8));
        for (String line : Files.readAllLines(Paths.get("E:/FoundationCoopTest/re/addrs.txt"))) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] p = line.split("\\s+");
            Function fn = getFunctionContaining(toAddr(p[1]));
            if (fn == null) {
                // data address (e.g. a vtable slot): walk back to the start of the pointer table and list who references it
                Address a = toAddr(p[1]);
                sum.println("== " + p[0] + " " + a + " is data; refs to nearby addresses:");
                for (int off = -0x200; off <= 0x40; off += 8) {
                    Address b = a.add(off);
                    for (Reference r : getReferencesTo(b)) {
                        Function c = getFunctionContaining(r.getFromAddress());
                        sum.println("   " + b + " <- " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (c == null ? "?" : c.getName() + " @ " + c.getEntryPoint()));
                        if (c != null) dump("ref_" + p[0], c);
                    }
                }
                Symbol s = getSymbolAt(a);
                sum.println("   symbol at addr: " + s);
                continue;
            }
            dump(p[0], fn);
            sum.println("== " + p[0] + " " + fn.getName() + " @ " + fn.getEntryPoint() + " callers:");
            for (Reference r : getReferencesTo(fn.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                sum.println("   " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (c == null ? "? (data: vtable?)" : c.getName() + " @ " + c.getEntryPoint()));
                if (c != null) dump("caller_of_" + p[0], c);
            }
        }
        sum.close();
        println("done, dumped " + done.size() + " functions");
    }

    void dump(String tag, Function fn) throws Exception {
        if (!done.add(fn.getEntryPoint())) return;
        DecompileResults res = dec.decompileFunction(fn, 120, monitor);
        String c = res.decompileCompleted() ? res.getDecompiledFunction().getC() : ("// decompile failed: " + res.getErrorMessage());
        try (PrintWriter w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, tag + "_" + fn.getEntryPoint() + ".c")), StandardCharsets.UTF_8))) {
            w.println("// " + fn.getName() + " @ " + fn.getEntryPoint() + " size " + fn.getBody().getNumAddresses());
            w.println(c);
        }
    }
}
