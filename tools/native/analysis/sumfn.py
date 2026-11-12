import re, sys
t=open(sys.argv[1]).read()
for blk in t.split('##### ')[1:]:
    head=blk.split('\n',1)[0]
    m=re.search(r'\n(\w[^\n]*\n?[^\n]*\))\s*\n\s*\{',blk)
    sig=re.sub(r'\s+',' ',m.group(1)) if m else '?'
    sig=re.sub(r'undefined(\d)','u\\1',sig); sig=re.sub(r'param_','p',sig)
    consts=sorted(set(re.findall(r'0x[0-9a-f]{3,}',blk)),key=lambda x:int(x,16))
    calls=sorted(set(re.findall(r'(FUN_00[0-9a-f]+|sce\w+|libSce\w+)\(',blk)))
    calls=[c for c in calls if c not in head]
    print(head.split(' @')[0].replace('FUN_00',''), head.split('size=')[1], '|', sig[:100], '|', ' '.join(consts)[:110], '|', ' '.join(c.replace('FUN_00','') for c in calls)[:80])
