// List functions that reference the named imported functions (names in E:/FoundationCoopTest/re/imports.txt).
// Output: E:/FoundationCoopTest/re/imports_out.txt
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class FindImportRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        List<String> names = Files.readAllLines(Paths.get("E:/FoundationCoopTest/re/imports.txt"));
        try (PrintWriter w = new PrintWriter(new FileWriter("E:/FoundationCoopTest/re/imports_out.txt"))) {
            SymbolTable st = currentProgram.getSymbolTable();
            for (String n : names) {
                n = n.trim(); if (n.isEmpty()) continue;
                w.println("== " + n);
                for (Symbol s : st.getSymbols(n)) {
                    Set<Address> seen = new HashSet<>();
                    for (Reference r : getReferencesTo(s.getAddress())) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        // follow thunks / IAT pointers one level
                        if (f == null) {
                            for (Reference r2 : getReferencesTo(r.getFromAddress())) {
                                Function f2 = getFunctionContaining(r2.getFromAddress());
                                if (f2 != null && seen.add(f2.getEntryPoint())) w.println("   " + f2.getName() + " @ " + f2.getEntryPoint() + " (via " + r.getFromAddress() + ")");
                            }
                        } else if (seen.add(f.getEntryPoint())) w.println("   " + f.getName() + " @ " + f.getEntryPoint());
                    }
                }
            }
        }
        println("done");
    }
}
