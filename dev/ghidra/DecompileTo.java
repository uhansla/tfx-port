// Headless helper: decompile functions to a text file.
//   args: OUTFILE ADDR [ADDR...]   (hex, Ghidra program addresses)
//   args: OUTFILE all              (every function)
// Creates a function at an address that has none yet.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*;
import java.util.*;

public class DecompileTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        DecompInterface di = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        di.setOptions(opts);
        di.openProgram(currentProgram);
        List<Function> funcs = new ArrayList<>();
        if (args.length > 1 && args[1].equals("all")) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) funcs.add(f);
        } else {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(Long.parseLong(args[i].replace("0x", ""), 16));
                Function f = getFunctionAt(a);
                if (f == null) f = getFunctionContaining(a);
                if (f == null) {
                    disassemble(a);
                    new CreateFunctionCmd(a).applyTo(currentProgram);
                    f = getFunctionAt(a);
                }
                if (f == null) f = getFunctionContaining(a);
                if (f == null) { out.println("// no function at " + a); continue; }
                funcs.add(f);
            }
        }
        for (Function f : funcs) {
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
            if (r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            else out.println("// decompile failed: " + r.getErrorMessage());
        }
        out.close();
        di.dispose();
    }
}
