// Headless helper: list references to address ranges.
//   args: OUTFILE LO-HI [LO-HI...]   (hex; HI exclusive; single ADDR also allowed)
// Prints every reference whose target falls in a range, with the source
// instruction, plus instructions whose scalar operands fall in a range.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.scalar.Scalar;
import java.io.*;
import java.util.*;

public class RefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        List<long[]> ranges = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            String[] p = args[i].split("-");
            long lo = Long.parseLong(p[0], 16);
            long hi = p.length > 1 ? Long.parseLong(p[1], 16) : lo + 1;
            ranges.add(new long[]{lo, hi});
        }
        ReferenceManager rm = currentProgram.getReferenceManager();
        Listing listing = currentProgram.getListing();
        for (long[] r : ranges) {
            for (long a = r[0]; a < r[1]; a++) {
                for (Reference ref : rm.getReferencesTo(toAddr(a))) {
                    Address from = ref.getFromAddress();
                    Instruction ins = listing.getInstructionAt(from);
                    Function f = getFunctionContaining(from);
                    out.printf("ref %08x <- %s %s [%s] in %s%n", a, from, ref.getReferenceType(),
                               ins != null ? ins.toString() : "data", f != null ? f.getName() : "-");
                }
            }
        }
        // scalar operands (immediates) that point into a range
        for (Instruction ins : listing.getInstructions(true)) {
            for (int op = 0; op < ins.getNumOperands(); op++) {
                for (Object o : ins.getOpObjects(op)) {
                    if (!(o instanceof Scalar)) continue;
                    long v = ((Scalar) o).getUnsignedValue();
                    for (long[] r : ranges) if (v >= r[0] && v < r[1]) {
                        Function f = getFunctionContaining(ins.getAddress());
                        out.printf("imm %08x in %s [%s] in %s%n", v, ins.getAddress(), ins, f != null ? f.getName() : "-");
                    }
                }
            }
        }
        out.close();
    }
}
