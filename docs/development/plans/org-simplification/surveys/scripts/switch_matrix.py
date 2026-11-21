import re,pathlib,sys,tomllib,collections,subprocess
root=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2])
names=set(l.strip() for l in open(sys.argv[3]) if l.strip())
tok=re.compile(r'\b[A-Z][A-Z0-9_]{3,}\b')
def scan(paths):
    hits=collections.defaultdict(set)
    for p in paths:
        try: t=p.read_text(errors='ignore')
        except: continue
        for m in set(tok.findall(t)):
            if m in names: hits[m].add(p.name)
    return hits
schema=tomllib.load(open(root/'tools/cs-test/schema.toml','rb'))
env2key={v['env']:k for k,v in schema.get('key',{}).items() if isinstance(v,dict) and 'env' in v}
cfgs=list((root/'tools/cs-test/configs').glob('*.toml'))
cfghits=scan(cfgs)
for p in cfgs:
    t=p.read_text(errors='ignore'); tbl=None
    for line in t.splitlines():
        s=line.strip()
        if s.startswith('#') or not s: continue
        m=re.match(r'^\[([A-Za-z0-9_."\-]+)\]$',s)
        if m: tbl=m.group(1); continue
        m=re.match(r'^([A-Za-z0-9_]+)\s*=',s)
        if m and tbl:
            k=f"{tbl}.{m.group(1)}"
            for e,kk in env2key.items():
                if kk==k: cfghits[e].add(p.name)
pyhits=scan([p for p in (root/'tools/cs-test').rglob('*.py') if p.name!='gen_schema.py'])
abhits=scan([p for p in (root/'tools/cs-ab').rglob('*') if p.is_file()])
files=subprocess.run(['git','ls-files','src','extern','package'],cwd=root,capture_output=True,text=True).stdout.split()
srcp=[root/f for f in files if f.endswith(('.cpp','.h','.inl','.hlsl','.hlsli')) and 'SwitchRegistry' not in f and 'Settings/Switches.cpp' not in f]
srchits=scan(srcp)
rows=[]
for n in sorted(names):
    rows.append(f"{n}\tkey={env2key.get(n)}\tcfg={len(cfghits[n])}:{','.join(sorted(cfghits[n]))[:160]}\tpy={len(pyhits[n])}\tab={len(abhits[n])}\tsrc={len(srchits[n])}:{','.join(sorted(srchits[n]))[:120]}")
out.write_text('\n'.join(rows))
