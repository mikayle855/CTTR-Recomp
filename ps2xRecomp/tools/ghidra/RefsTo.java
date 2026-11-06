// Print every reference to the given addresses with the function it comes from (headless helper).
//   analyzeHeadless <proj dir> <proj> -process <program> -noanalysis -readOnly -scriptPath ps2xRecomp/tools/ghidra
//       -postScript RefsTo.java <out.txt> <hexaddr> [<hexaddr> ...]
// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
public class RefsTo extends GhidraScript {
    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        java.io.PrintWriter w = new java.io.PrintWriter(args[0]);
        for (int i = 1; i < args.length; i++) {
            Address to = toAddr(Long.parseLong(args[i].replaceFirst("^0[xX]", ""), 16));
            w.println("== refs to " + to);
            for (Reference r : getReferencesTo(to)) {
                Function f = getFunctionContaining(r.getFromAddress());
                w.println("  " + r.getFromAddress() + " " + r.getReferenceType() + " in "
                          + (f == null ? "?" : f.getName() + " @" + f.getEntryPoint()));
            }
        }
        w.close();
    }
}
