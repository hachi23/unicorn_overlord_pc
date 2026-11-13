import sys, re, collections
vs=ps=None; pairs=collections.Counter(); cur_t=None
tex={}; blend=None; pairinfo={}
for l in open(sys.argv[1]):
    w=l.split()
    if w[0]=='vs': vs=w[2]
    elif w[0]=='ps': ps=w[2]
    elif w[0]=='blend': blend=w[2]
    elif w[0]=='texture':
        tex[(w[2],w[4])] = None if 'none' in l else re.search(r'fmt (\d+)',l).group(1)
    elif w[0]=='draw':
        k=(vs,ps); pairs[k]+=1
        slots=sorted(re.findall(r's(\d+)\(r\d+\+\d+ stride \d+ fmt (\d+/\d+)\)', l), key=lambda x:int(x[0]))
        pairinfo.setdefault(k,set()).add((' '.join(a+':'+b for a,b in slots), blend))
print('shader pairs', len(pairs), 'vs', len({k[0] for k in pairs}), 'ps', len({k[1] for k in pairs}))
for k,c in pairs.most_common():
    print(c, k[0], k[1], '|', ' || '.join(a+' blend '+str(b) for a,b in sorted(pairinfo[k]))[:150])
