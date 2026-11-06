// Create functions in the parts of the executable blocks that auto-analysis left outside every function.
// Code reached only through pointers (C++ vtables, interrupt/alarm/thread callbacks) is often missed, and the
// recompiler then has no entry for it. For each gap: skip zero padding, disassemble, and create a function at the
// first instruction if it disassembles cleanly. Repeats until a pass adds nothing. Run before ExportPS2Functions.
//   analyzeHeadless <proj dir> <proj> -process <program> -scriptPath ps2xRecomp/tools/ghidra -postScript FillCodeGaps.java
// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
public class FillCodeGaps extends GhidraScript {
    @Override public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        int total = 0;
        for (int pass = 1; pass <= 8; pass++) {
            int created = 0;
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                if (!b.isExecute() || !b.isInitialized() || b.getName().startsWith(".vu")) continue;
                Address a = b.getStart();
                while (a != null && a.compareTo(b.getEnd()) < 0 && !monitor.isCancelled()) {
                    Function f = fm.getFunctionContaining(a);
                    if (f != null) { a = f.getBody().getMaxAddress().next(); continue; }
                    long mis = a.getOffset() % 4;
                    if (mis != 0) { a = a.add(4 - mis); continue; }
                    if (getInt(a) == 0) { a = a.add(4); continue; }   // padding
                    if (listing.getInstructionAt(a) == null) disassemble(a);
                    Instruction ins = listing.getInstructionAt(a);
                    if (ins != null && createFunction(a, null) != null) {
                        created++;
                        Function nf = fm.getFunctionAt(a);
                        a = (nf != null) ? nf.getBody().getMaxAddress().next() : a.add(4);
                    } else {
                        a = a.add(4);
                    }
                }
            }
            println("pass " + pass + ": created " + created + " functions");
            total += created;
            if (created == 0) break;
        }
        println("FillCodeGaps: " + total + " functions created");
    }
}
