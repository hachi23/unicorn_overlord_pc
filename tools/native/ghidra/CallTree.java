import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.*;

// For each address: list caller functions (count of sites), up N levels as a summary.
public class CallTree extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int depth = Integer.parseInt(args[0]);
        Set<Function> level = new LinkedHashSet<>();
        for (int i = 1; i < args.length; i++) {
            Function f = getFunctionContaining(toAddr(Long.parseUnsignedLong(args[i].replace("0x", ""), 16)));
            if (f != null) level.add(f);
        }
        Set<Function> seen = new HashSet<>(level);
        for (int d = 0; d < depth; d++) {
            Set<Function> next = new LinkedHashSet<>();
            for (Function f : level) {
                Map<String,Integer> callers = new TreeMap<>();
                int data = 0;
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    Function c = getFunctionContaining(r.getFromAddress());
                    if (c == null) { data++; continue; }
                    callers.merge(c.getEntryPoint().toString(), 1, Integer::sum);
                    if (seen.add(c)) next.add(c);
                }
                println("L" + d + "\t" + f.getEntryPoint() + "\tsize=" + f.getBody().getNumAddresses() + "\tncallers=" + callers.size() + "\tdataRefs=" + data + "\t" + (callers.size() > 14 ? "(many)" : callers.toString()));
            }
            level = next;
        }
    }
}
