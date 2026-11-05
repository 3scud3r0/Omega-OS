"""Pinned Omega stage-0 seed; generated code executes entirely inside Ring 3."""
import importlib.util,pathlib,json
root=pathlib.Path('vendor/omega')
spec=importlib.util.spec_from_file_location('omega_seed',root/'omega_selfhost/bootstrap.py'); m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
functions={f[0]:f for path in ['egraph_linked.omega','omega_selfhost/compiler.omega'] for f in m.Bootstrap((root/path).read_text()).parse()}
needed=set();builtins=set()
def scan(x):
    if not isinstance(x,list):return
    if x and x[0]=='call':
        name=x[1]
        if name in m.BUILTINS:builtins.add(name)
        else:collect(name)
    for y in x:scan(y)
def collect(name):
    if name in needed:return
    needed.add(name);scan(functions[name][2])
for name in ['principal','om_mod_4_execute']:collect(name)
lines=['#include "omega_value.h"']
for name in sorted(needed):
    _,args,body=functions[name];lines.append('static V fn_'+name+'('+','.join('V v_'+a for a in args)+');')
for name in sorted(needed):
    _,args,body=functions[name]
    text=m.cblock(body).replace('({','__extension__ ({')
    lines.append('static V fn_'+name+'('+','.join('V v_'+a for a in args)+'){o_enter();\n'+text+'return o_ret(o_int(0));}\n')
lines+=['V omega_frontend(const char *s){ov_input=s;ov_output=0;(void)fn_principal();if(!ov_output)omega_fail("Compiler produced no IR");return o_parsejson(ov_output);}', 'V omega_egraph(V request){return fn_om_mod_4_execute(request);}']
pathlib.Path('build/omega_generated.c').write_text('\n'.join(lines))
pathlib.Path('build/omega_seed_report.json').write_text(json.dumps({'functions':sorted(needed),'builtins':sorted(builtins)},indent=2))
print('Omega seed:',len(needed),'reachable original functions; builtins:',','.join(sorted(builtins)))
