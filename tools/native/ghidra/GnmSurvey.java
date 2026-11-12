import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.*;
import java.nio.file.*;

public class GnmSurvey extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        Map<Function, TreeSet<String>> byFn = new HashMap<>();
        for (String l : lines) {
            if (l.startsWith("#")) continue;
            String[] c = l.split("\t");
            if (c.length < 6) continue;
            String lib = c[5];
            if (!lib.matches(args[1])) continue;
            String name = c[4].equals("?") ? lib + "_nid_" + c[3] : c[4];
            Set<Address> sites = new TreeSet<>();
            Deque<Address> work = new ArrayDeque<>();
            for (String g : c[2].split(",")) work.add(toAddr(Long.parseUnsignedLong(g.trim().replace("0x", ""), 16)));
            work.add(toAddr(Long.parseUnsignedLong(c[1].replace("0x", ""), 16)));
            Set<Address> seen = new HashSet<>();
            while (!work.isEmpty()) {
                Address t = work.poll();
                if (!seen.add(t)) continue;
                for (Reference r : getReferencesTo(t)) {
                    Address a = r.getFromAddress();
                    Instruction ins = getInstructionAt(a);
                    if (ins == null) continue;
                    Function f = getFunctionContaining(a);
                    String m = ins.getMnemonicString();
                    if (m.equals("JMP") && f != null && f.getBody().getNumAddresses() <= 16) { work.add(f.getEntryPoint()); continue; }
                    sites.add(a);
                }
            }
            Map<String,Integer> fns = new TreeMap<>();
            for (Address a : sites) {
                Function f = getFunctionContaining(a);
                String k = f == null ? "?" + a : f.getEntryPoint().toString();
                fns.merge(k, 1, Integer::sum);
                if (f != null) byFn.computeIfAbsent(f, x -> new TreeSet<>()).add(name);
            }
            println("IMPORT\t" + name + "\t" + sites.size() + "\t" + fns);
        }
        List<Function> fl = new ArrayList<>(byFn.keySet());
        fl.sort(Comparator.comparing(Function::getEntryPoint));
        for (Function f : fl) {
            int callers = 0;
            for (Reference r : getReferencesTo(f.getEntryPoint())) if (r.getReferenceType().isCall()) callers++;
            println("FN\t" + f.getEntryPoint() + "\tsize=" + f.getBody().getNumAddresses() + "\tcallers=" + callers + "\t" + byFn.get(f));
        }
    }
}
