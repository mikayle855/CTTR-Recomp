// Decompile the functions containing the given addresses into one text file (headless helper).
//   analyzeHeadless <proj dir> <proj> -process <program> -noanalysis -scriptPath ps2xRecomp/tools/ghidra
//       -postScript DecompileAt.java <out.c> <hexaddr> [<hexaddr> ...]
// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
public class DecompileAt extends GhidraScript {
    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        java.io.PrintWriter w = new java.io.PrintWriter(args[0]);
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        java.util.Set<Long> done = new java.util.HashSet<>();
        for (int i = 1; i < args.length; i++) {
            long a = Long.parseLong(args[i].replaceFirst("^0[xX]", ""), 16);
            Address ad = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
            Function fn = getFunctionContaining(ad);
            if (fn == null) { w.println("// no function contains 0x" + Long.toHexString(a)); continue; }
            if (!done.add(fn.getEntryPoint().getOffset())) continue;
            w.println("// ===== " + fn.getName() + " @0x" + Long.toHexString(fn.getEntryPoint().getOffset())
                      + " (asked 0x" + Long.toHexString(a) + ") =====");
            DecompileResults r = dec.decompileFunction(fn, 90, monitor);
            w.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "//fail");
        }
        w.close();
    }
}
