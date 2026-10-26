// Usage: -postScript DisasmAt.java <hexstart> <count>  -- disassembles from an address Ghidra
// left as data (code reached only through computed jumps) and prints the instructions.
// @category doax3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class DisasmAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Address start = toAddr(Long.parseUnsignedLong(a[0].replace("0x", ""), 16));
        int count = Integer.parseInt(a[1]);
        disassemble(start);
        Instruction p = getInstructionAt(start);
        for (int i = 0; i < count && p != null; i++) {
            println(p.getAddress() + "  " + p);
            Instruction n = p.getNext();
            if (n == null || !n.getAddress().equals(p.getAddress().add(p.getLength()))) {
                disassemble(p.getAddress().add(p.getLength()));
                n = getInstructionAt(p.getAddress().add(p.getLength()));
            }
            p = n;
        }
    }
}
