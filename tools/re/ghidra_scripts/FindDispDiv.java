// Usage: -postScript FindDispDiv.java <hex displacement>...  -- float loads from [reg+disp]
// followed within 8 instructions by a divide, grouped by function (finds size-based ratios).
// @category doax3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.util.*;

public class FindDispDiv extends GhidraScript {
    @Override
    public void run() throws Exception {
        Set<Long> disps = new HashSet<>();
        for (String a : getScriptArgs()) disps.add(Long.parseUnsignedLong(a.replace("0x", ""), 16));
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            String m = ins.getMnemonicString();
            if (!(m.contains("MOVSS") || m.contains("CVT"))) continue;
            String s = ins.toString();
            boolean hit = false;
            for (long d : disps) if (s.contains(String.format("+ 0x%x]", d))) hit = true;
            if (!hit) continue;
            Instruction p = ins.getNext();
            boolean div = false;
            for (int i = 0; i < 8 && p != null && !div; i++, p = p.getNext())
                if (p.getMnemonicString().contains("DIV") || p.getMnemonicString().contains("RCP")) div = true;
            if (!div) continue;
            Function f = getFunctionContaining(ins.getAddress());
            println(ins.getAddress() + " " + (f == null ? "?" : f.getName()) + "  " + s);
        }
    }
}
