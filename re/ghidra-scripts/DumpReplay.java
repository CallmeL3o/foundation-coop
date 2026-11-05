// Dump replay-related functions, their callers, and every instruction writing GamCGame+0x3d8 / +0x3c8.
// Output: E:/FoundationCoopTest/re/out2/
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class DumpReplay extends GhidraScript {
    static final String[][] FUNCS = {
        {"rec_newFrame", "14029eb60"},
        {"rec_command", "140298250"},
        {"play_nextFrame", "140294db0"},
        {"play_readCommand", "140294bf0"},
        {"replay_delete", "1401124e0"},
        {"sim_update", "1401c27d0"},
        {"game_updateFrame", "1401c2bf0"},
        {"game_pushPlayerCommand", "1401b3210"},
    };
    DecompInterface dec;
    File out;
    Set<Address> done = new HashSet<>();

    @Override
    public void run() throws Exception {
        out = new File("E:/FoundationCoopTest/re/out2");
        out.mkdirs();
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        PrintWriter sum = pw("summary.txt");
        for (String[] f : FUNCS) {
            Function fn = getFunctionAt(toAddr(f[1]));
            if (fn == null) { sum.println(f[0] + " " + f[1] + ": no function"); continue; }
            dump(f[0], fn);
            sum.println("== " + f[0] + " @ " + f[1] + " callers:");
            int n = 0;
            for (Reference r : getReferencesTo(fn.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                sum.println("   " + r.getFromAddress() + " in " + (c == null ? "?" : c.getName() + " @ " + c.getEntryPoint()));
                if (c != null && n++ < 12) dump("caller_of_" + f[0], c);
            }
        }
        // instructions that store to [reg + 0x3d8] or [reg + 0x3c8] (replay mode / replay object)
        sum.println("== writes to +0x3d8 / +0x3c8");
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        int hits = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            String m = ins.getMnemonicString();
            if (!m.startsWith("MOV")) continue;
            String op0 = ins.getDefaultOperandRepresentation(0);
            if (!(op0.contains("+ 0x3d8]") || op0.contains("+0x3d8]") || op0.contains("+ 0x3c8]") || op0.contains("+0x3c8]"))) continue;
            Function c = getFunctionContaining(ins.getAddress());
            sum.println("   " + ins.getAddress() + "  " + ins + "  in " + (c == null ? "?" : c.getName() + " @ " + c.getEntryPoint()));
            if (c != null && hits++ < 40) dump("writes_mode", c);
        }
        sum.close();
        println("done, dumped " + done.size() + " functions");
    }

    PrintWriter pw(String name) throws Exception {
        return new PrintWriter(new OutputStreamWriter(new FileOutputStream(new File(out, name)), StandardCharsets.UTF_8));
    }

    void dump(String tag, Function fn) throws Exception {
        if (!done.add(fn.getEntryPoint())) return;
        DecompileResults res = dec.decompileFunction(fn, 120, monitor);
        String c = res.decompileCompleted() ? res.getDecompiledFunction().getC() : ("// decompile failed: " + res.getErrorMessage());
        try (PrintWriter w = pw(tag + "_" + fn.getEntryPoint() + ".c")) {
            w.println("// " + fn.getName() + " @ " + fn.getEntryPoint() + " size " + fn.getBody().getNumAddresses());
            w.println(c);
        }
    }
}
