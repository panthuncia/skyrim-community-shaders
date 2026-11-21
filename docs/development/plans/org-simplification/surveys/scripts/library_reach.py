import os,re,sys
root=sys.argv[1] if len(sys.argv)>1 else "."
libs={'ORG':root+"/extern/OpenRenderGraph/include",'OMS':root+"/extern/ORGModuleServices/include"}
inc=re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
def hdrs(base):
    out={}
    for d,_,fs in os.walk(base):
        for f in fs:
            if f.endswith(('.h','.hpp','.inl')):
                p=os.path.join(d,f); out[os.path.relpath(p,base).replace(chr(92),'/')]=p
    return out
H={}
for k,b in libs.items():
    for r,p in hdrs(b).items(): H[r]=(k,p)
def incs(p):
    try: t=open(p,encoding='utf-8',errors='ignore').read()
    except: return []
    return inc.findall(t)
seeds=set()
for d in ['src','tests']:
    for dd,_,fs in os.walk(os.path.join(root,d)):
        for f in fs:
            if f.endswith(('.h','.cpp','.hpp','.inl')):
                for i in incs(os.path.join(dd,f)):
                    if i in H: seeds.add(i)
direct=set(seeds)
seen=set(); st=list(seeds)
while st:
    h=st.pop()
    if h in seen: continue
    seen.add(h)
    base=os.path.dirname(h)
    for i in incs(H[h][1]):
        cand=[i, os.path.normpath(os.path.join(base,i)).replace(chr(92),'/')]
        for c in cand:
            if c in H and c not in seen: st.append(c)
def loc(p): return sum(1 for _ in open(p,encoding='utf-8',errors='ignore'))
for lib in ['ORG','OMS']:
    un=[(r,loc(p)) for r,(k,p) in H.items() if k==lib and r not in seen]
    print(lib,'headers',sum(1 for v in H.values() if v[0]==lib),'direct',sum(1 for r in direct if H[r][0]==lib),'reach',sum(1 for r in seen if H[r][0]==lib))
    for r,n in sorted(un): print('  UNREACHED',r,n)
