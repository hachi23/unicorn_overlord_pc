import re, sys
t=open(sys.argv[1]).read()
want=[w.lower().replace('0x','').zfill(8) for w in sys.argv[2:]]
drop=re.compile(r'^\s*$|stack_chk|invalidInstruction|Does not return|^\s*(undefined|long|ulong|int|uint|code|char|byte|float|bool|ushort|short)[0-9]* \*?\*?[a-zA-Z_0-9]+( \[[0-9]+\])?;$|^\s*(undefined|u?long|u?int|code|char|byte) \(?\*')
for blk in t.split('##### ')[1:]:
    a=blk.split(' @ ')[1][:8]
    if a in want:
        for l in blk.split('\n'):
            if not drop.search(l): print(l[:170])
