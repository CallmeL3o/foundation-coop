# Generates dwmapi_fwd.S (jump stubs) and dwmapi.def (exports incl. ordinal-only) for the proxy dwmapi.dll.
import sys, importlib.util
spec = importlib.util.spec_from_file_location('pe', sys.argv[1]); pe = importlib.util.module_from_spec(spec); spec.loader.exec_module(pe)
exps = pe.exports(r'C:\Windows\System32\dwmapi.dll')
outdir = sys.argv[2]
with open(outdir + '/dwmapi_fwd.S', 'w', newline='\n') as f:
    f.write('# generated: one jump stub per dwmapi export, through fcoop_dwm_tab (filled by DllMain)\n.text\n')
    for i, (o, n) in enumerate(exps):
        f.write('.globl fwd_%d\nfwd_%d:\n    jmp *fcoop_dwm_tab+%d(%%rip)\n' % (o, o, 8 * i))
with open(outdir + '/dwmapi.def', 'w', newline='\n') as f:
    f.write('LIBRARY dwmapi\nEXPORTS\n')
    for o, n in exps:
        if n: f.write('    %s = fwd_%d @%d\n' % (n, o, o))
        else: f.write('    fwd_%d @%d NONAME\n' % (o, o))
with open(outdir + '/dwmapi_tab.h', 'w', newline='\n') as f:
    f.write('// generated: dwmapi exports (ordinal, name or NULL)\n#define FCOOP_DWM_N %d\n' % len(exps))
    f.write('static const struct { unsigned short ord; const char *name; } fcoop_dwm_exports[FCOOP_DWM_N] = {\n')
    for o, n in exps:
        f.write('    { %d, %s },\n' % (o, '"%s"' % n if n else 'NULL'))
    f.write('};\n')
print(len(exps), 'exports')
