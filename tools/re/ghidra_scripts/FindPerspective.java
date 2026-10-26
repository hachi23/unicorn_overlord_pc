// Usage: -postScript FindPerspective.java  -- functions that look like a perspective matrix build
// (FUN_000396a0 inlined): they read the sine table at 0xa4b980 and store -1.0f (m[11]).
// Prints the 1920.0/1080.0 constants and screen-size fields each one also reads.
// @category doax3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import java.util.*;

public class FindPerspective extends GhidraScript {
    @Override
    public void run() throws Exception {
        Address table = toAddr(0xa4b980L);
        Set<Function> sine = new HashSet<>();
        for (Reference r : getReferencesTo(table)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) sine.add(f);
        }
        long[] sizes = {0xaf3290L, 0xaf3294L, 0xaf28d0L, 0xaf28d4L, 0x74d094L};
        for (Function f : sine) {
            boolean minusOne = false;
            List<String> extra = new ArrayList<>();
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                for (int i = 0; i < ins.getNumOperands(); i++)
                    for (Object o : ins.getOpObjects(i))
                        if (o instanceof Scalar && ((Scalar) o).getUnsignedValue() == 0xbf800000L) minusOne = true;
                for (Reference r : ins.getReferencesFrom())
                    for (long s : sizes)
                        if (r.getToAddress().getOffset() == s) extra.add(String.format("%s->%x", ins.getAddress(), s));
                String t = ins.toString();
                if ((t.contains("+ 0x38]") || t.contains("+ 0x3c]")) && ins.getMnemonicString().contains("MOVSS")) extra.add(ins.getAddress() + ":" + t);
            }
            if (minusOne) println(f.getName() + " size=" + f.getBody().getNumAddresses() + " " + extra);
        }
    }
}
