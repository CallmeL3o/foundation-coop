// List instructions that write to [reg + OFFSET] for each "tag offsetHex" line in E:/FoundationCoopTest/re/writes.txt,
// restricted to the VA range given on the first line ("range startHex endHex"); decompile the containing functions.
// Output: E:/FoundationCoopTest/re/out7/
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class FindWrites extends GhidraScript {
    @Override
    public void run() throws Exception {
        File out = new File("E:/FoundationCoopTest/re/out7"); out.mkdirs();
        List<String> lines = Files.readAllLines(Paths.get("E:/FoundationCoopTest/re/writes.txt"));
        String[] r = lines.get(0).trim().split("\\s+");
        Address lo = toAddr(r[1]), hi = toAddr(r[2]);
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        PrintWriter sum = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, "summary.txt")), StandardCharsets.UTF_8));
        Set<Address> done = new HashSet<>();
        for (String l : lines.subList(1, lines.size())) {
            String[] p = l.trim().split("\\s+"); if (p.length < 2) continue;
            String pat = "+ 0x" + p[1].toLowerCase() + "]";
            sum.println("== " + p[0] + " (" + pat + ")");
            InstructionIterator it = currentProgram.getListing().getInstructions(new AddressSet(lo, hi), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (!ins.getMnemonicString().startsWith("MOV")) continue;
                if (!ins.getDefaultOperandRepresentation(0).contains(pat)) continue;
                Function f = getFunctionContaining(ins.getAddress());
                sum.println("   " + ins.getAddress() + "  " + ins + "  in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                if (f != null && done.add(f.getEntryPoint()) && f.getBody().getNumAddresses() < 3000) {
                    DecompileResults res = dec.decompileFunction(f, 60, monitor);
                    try (PrintWriter w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, p[0] + "_" + f.getEntryPoint() + ".c")), StandardCharsets.UTF_8))) {
                        w.println("// " + f.getName() + " @ " + f.getEntryPoint());
                        w.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed");
                    }
                }
            }
        }
        sum.close();
        println("done, dumped " + done.size());
    }
}
