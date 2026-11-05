// Print disassembly around addresses listed in E:/FoundationCoopTest/re/disasm.txt ("hexaddr before after" per line).
// Output: E:/FoundationCoopTest/re/disasm_out.txt
// @category coop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

public class DisasmAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        try (PrintWriter w = new PrintWriter(new OutputStreamWriter(new FileOutputStream("E:/FoundationCoopTest/re/disasm_out.txt"), StandardCharsets.UTF_8))) {
            for (String l : Files.readAllLines(Paths.get("E:/FoundationCoopTest/re/disasm.txt"))) {
                String[] p = l.trim().split("\\s+"); if (p.length < 3) continue;
                Address a = toAddr(p[0]);
                Function f = getFunctionContaining(a);
                w.println("==== " + a + " in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                Instruction ins = getInstructionContaining(a);
                for (int i = 0; i < Integer.parseInt(p[1]) && ins != null && ins.getPrevious() != null; i++) ins = ins.getPrevious();
                int total = Integer.parseInt(p[1]) + Integer.parseInt(p[2]);
                for (int i = 0; i <= total && ins != null; i++, ins = ins.getNext())
                    w.println((ins.getAddress().equals(a) || ins.getMaxAddress().compareTo(a) >= 0 && ins.getAddress().compareTo(a) <= 0 ? ">> " : "   ") + ins.getAddress() + "  " + ins);
            }
        }
        println("done");
    }
}
