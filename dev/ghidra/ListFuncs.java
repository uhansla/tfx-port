// Headless helper: list functions (entry, size, name) and defined strings with references.
//   args: OUTFILE
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ListFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(getScriptArgs()[0]));
        for (Function f : currentProgram.getFunctionManager().getFunctions(true))
            out.printf("func %s %d %s%n", f.getEntryPoint(), f.getBody().getNumAddresses(), f.getName());
        for (Data d : currentProgram.getListing().getDefinedData(true)) {
            if (!d.hasStringValue()) continue;
            StringBuilder refs = new StringBuilder();
            for (Reference r : currentProgram.getReferenceManager().getReferencesTo(d.getAddress()))
                refs.append(' ').append(r.getFromAddress());
            out.printf("str %s %s <-%s%n", d.getAddress(), d.getDefaultValueRepresentation(), refs);
        }
        out.close();
    }
}
