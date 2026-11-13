import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
public class DecompC extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Function f = getFunctionContaining(toAddr(Long.parseUnsignedLong(arg.replace("0x", ""), 16)));
            if (f == null) { println("no function at " + arg); continue; }
            println("##### " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
            DecompileResults res = di.decompileFunction(f, 120, monitor);
            if (res.decompileCompleted()) println(res.getDecompiledFunction().getC());
        }
    }
}
