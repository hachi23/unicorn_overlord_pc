// Usage: -postScript DecompAt.java <hexaddr>...  -- like Decomp.java, but disassembles and creates
// the function at each address first when the analysis has none there (read-only projects too).
// @category vw
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;

public class DecompAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Address a = toAddr(Long.parseUnsignedLong(arg.replace("0x", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null) {
                disassemble(a);
                f = createFunction(a, null);
            }
            if (f == null) { println("no function at " + arg); continue; }
            println("##### " + f.getName() + " @ " + f.getEntryPoint());
            DecompileResults res = di.decompileFunction(f, 300, monitor);
            if (res.decompileCompleted()) println(res.getDecompiledFunction().getC());
            else println("decompile failed: " + res.getErrorMessage());
        }
    }
}
