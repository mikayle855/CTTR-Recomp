// Create a function at every address listed in a file (one hex address per line), disassembling first.
//   analyzeHeadless <proj dir> <proj> -process <program> -scriptPath ps2xRecomp/tools/ghidra
//       -postScript CreateFunctionsAt.java <addresses.txt>
// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import java.nio.file.*;
public class CreateFunctionsAt extends GhidraScript {
    @Override public void run() throws Exception {
        int created = 0, skipped = 0;
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            line = line.trim();
            if (line.isEmpty()) continue;
            Address a = toAddr(Long.parseLong(line.replaceFirst("^0[xX]", ""), 16));
            if (getFunctionAt(a) != null) { skipped++; continue; }
            if (getInstructionAt(a) == null) disassemble(a);
            if (getInstructionAt(a) != null && createFunction(a, null) != null) created++; else skipped++;
        }
        println("CreateFunctionsAt: created " + created + ", skipped " + skipped);
    }
}
