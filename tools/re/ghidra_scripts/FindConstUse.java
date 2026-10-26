// Usage: -postScript FindConstUse.java <hexaddr>...  -- every instruction reading the given
// rodata addresses, with its function (Ghidra misses some references; this scans operands).
// @category doax3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.util.*;

public class FindConstUse extends GhidraScript {
    @Override
    public void run() throws Exception {
        Set<Long> want = new HashSet<>();
        for (String a : getScriptArgs()) want.add(Long.parseUnsignedLong(a.replace("0x", ""), 16));
        for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
            for (int i = 0; i < ins.getNumOperands(); i++)
                for (Object o : ins.getOpObjects(i)) {
                    long v = o instanceof Address ? ((Address) o).getOffset() : o instanceof Scalar ? ((Scalar) o).getUnsignedValue() : -1;
                    if (want.contains(v)) {
                        Function f = getFunctionContaining(ins.getAddress());
                        println(String.format("%x %s  %s", v, ins.getAddress(), (f == null ? "?" : f.getName()) + "  " + ins));
                    }
                }
        }
    }
}
