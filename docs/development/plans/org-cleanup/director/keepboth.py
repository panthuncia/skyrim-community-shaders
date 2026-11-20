import sys,re
for p in sys.argv[1:]:
    s=open(p,encoding='utf-8').read()
    def f(m):
        a,b=m.group(1),m.group(2)
        if p.endswith('.md'): return a+'\n'+b
        # A struct body cut at the shared closing brace: close the first one.
        if a.rstrip().endswith('= default;') and a.lstrip().startswith(('/**','struct')):
            return a+'\t};\n\n'+b
        return a+b
    s=re.sub(r'<<<<<<< [^\n]*\n(.*?)=======\n(.*?)>>>>>>> [^\n]*\n', f, s, flags=re.S)
    open(p,'w',encoding='utf-8',newline='').write(s)
