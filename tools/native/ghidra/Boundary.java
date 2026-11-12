import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.*;
// args: lo hi  -> functions in [lo,hi) and their callers outside the range
public class Boundary extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long lo = Long.parseUnsignedLong(a[0].replace("0x",""),16), hi = Long.parseUnsignedLong(a[1].replace("0x",""),16);
        Map<String, TreeSet<String>> ext = new TreeMap<>();   // outside caller -> lib fns
        Map<String, Integer> api = new TreeMap<>();            // lib fn -> # outside call sites
        Map<String, Integer> apiSize = new HashMap<>();
        int nlib = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(toAddr(lo), true);
        while (it.hasNext()) {
            Function f = it.next();
            long e = f.getEntryPoint().getOffset();
            if (e >= hi) break;
            nlib++;
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                long from = r.getFromAddress().getOffset();
                if (from >= lo && from < hi) continue;
                Function c = getFunctionContaining(r.getFromAddress());
                String k = c == null ? "data:" + r.getFromAddress() : c.getEntryPoint().toString();
                ext.computeIfAbsent(k, x -> new TreeSet<>()).add(f.getEntryPoint().toString());
                api.merge(f.getEntryPoint().toString(), 1, Integer::sum);
                apiSize.put(f.getEntryPoint().toString(), (int) f.getBody().getNumAddresses());
            }
        }
        println("LIBFNS " + nlib + " API " + api.size() + " EXTCALLERS " + ext.size());
        for (Map.Entry<String,Integer> e : api.entrySet()) println("API\t" + e.getKey() + "\tsize=" + apiSize.get(e.getKey()) + "\tsites=" + e.getValue());
        for (Map.Entry<String,TreeSet<String>> e : ext.entrySet()) println("EXT\t" + e.getKey() + "\t" + e.getValue().size() + "\t" + e.getValue());
    }
}
