// Usage: -postScript AsmAround.java <hexaddr> [before] [after]  -- instructions around an address
// @category doax3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;

public class AsmAround extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Instruction p = getInstructionContaining(toAddr(Long.parseUnsignedLong(a[0].replace("0x", ""), 16)));
        int before = a.length > 1 ? Integer.parseInt(a[1]) : 30, after = a.length > 2 ? Integer.parseInt(a[2]) : 40;
        for (int i = 0; i < before && p.getPrevious() != null; i++) p = p.getPrevious();
        for (int i = 0; i < before + after && p != null; i++, p = p.getNext()) {
            Function f = getFunctionContaining(p.getAddress());
            println(p.getAddress() + "  " + p + (f != null && f.getEntryPoint().equals(p.getAddress()) ? "   <== " + f.getName() : ""));
        }
    }
}
