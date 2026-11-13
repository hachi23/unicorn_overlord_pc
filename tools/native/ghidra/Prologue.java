import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
public class Prologue extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address a = toAddr(Long.parseUnsignedLong(arg.replace("0x", ""), 16));
            int n = 0; StringBuilder hx = new StringBuilder(); String flag = ""; StringBuilder txt = new StringBuilder();
            Instruction ins = getInstructionAt(a);
            while (ins != null && n < 5) {
                for (byte x : ins.getBytes()) hx.append(String.format("%02x", x));
                String s = ins.toString();
                if (s.contains("RIP") || ins.getFlowType().isJump() || ins.getFlowType().isCall() || ins.getFlowType().isTerminal()) flag += "R";
                for (int i = 0; i < ins.getNumOperands(); i++) for (Object o : ins.getOpObjects(i)) if (o instanceof Address) flag += "A";
                txt.append(s).append("; ");
                n += ins.getLength(); ins = ins.getNext();
            }
            println("P\t" + arg + "\t" + n + "\t" + hx + "\t" + (flag.isEmpty() ? "ok" : flag) + "\t" + txt);
        }
    }
}
