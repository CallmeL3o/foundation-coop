// List symbols whose name contains any pattern in E:/FoundationCoopTest/re/symbols.txt, with references,
// and dump the referencing functions (e.g. constructors that store a vtable).
// Output: E:/FoundationCoopTest/re/out4/
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

public class FindSymbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        File out = new File("E:/FoundationCoopTest/re/out4"); out.mkdirs();
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        List<String> pats = new ArrayList<>();
        for (String l : Files.readAllLines(Paths.get("E:/FoundationCoopTest/re/symbols.txt"))) if (!l.trim().isEmpty()) pats.add(l.trim());
        PrintWriter sum = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, "summary.txt")), StandardCharsets.UTF_8));
        Set<Address> done = new HashSet<>();
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext()) {
            Symbol s = it.next();
            String full = s.getName(true);
            boolean match = false; for (String p : pats) if (full.contains(p)) match = true;
            if (!match) continue;
            sum.println("== " + full + " @ " + s.getAddress());
            int n = 0;
            for (Reference r : getReferencesTo(s.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                sum.println("   <- " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                if (f != null && n++ < 6 && done.add(f.getEntryPoint()) && f.getBody().getNumAddresses() < 4000) {
                    DecompileResults res = dec.decompileFunction(f, 60, monitor);
                    String c = res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed";
                    try (PrintWriter w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, f.getEntryPoint() + ".c")), StandardCharsets.UTF_8))) {
                        w.println("// " + f.getName() + " @ " + f.getEntryPoint() + " (refs " + full + ")"); w.println(c);
                    }
                }
            }
        }
        sum.close();
        println("done, dumped " + done.size());
    }
}
