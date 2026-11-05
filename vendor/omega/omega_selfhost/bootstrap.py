"""Auditable stage-0 bootstrap for Omega's procedural compiler dialect.

This file is not used by the stage-1 or stage-2 compiler. It deliberately only
compiles the procedural language, not the historical tensor frontend.
"""
from __future__ import annotations
import argparse
import json
import re
from pathlib import Path

BUILTINS = {'list': 'list', 'obj': 'obj', 'len': 'len', 'get': 'get', 'put': 'put',
 'push': 'push', 'has': 'has', 'keys': 'keys', 'text': 'text', 'number': 'number',
 'ident':'ident', 'decimal':'decimal', 'isnum': 'isnum', 'isstr': 'isstr', 'islist': 'islist', 'slice': 'slice', 'join': 'join',
 'contains': 'contains', 'arg': 'arg', 'argc': 'argc', 'read': 'read', 'write': 'write',
 'print': 'print', 'fail': 'fail', 'json': 'json', 'parsejson': 'parsejson',
 'qnum':'qnum','qden':'qden','qfloor':'qfloor','qbits':'qbits','monotonic_ns':'monotonic_ns','readline':'readline','flush':'flush',
 'root_read':'root_read','root_write':'root_write','root_exists':'root_exists','sha256':'sha256','file_sha256':'file_sha256',
 'append_cas':'append_cas','ed25519_verify':'ed25519_verify','ed25519_sign':'ed25519_sign','unix_time':'unix_time'}
OPS = {'+':'add','-':'sub','*':'mul','/':'div','%':'mod','==':'eq','!=':'ne',
       '<':'lt','<=':'le','>':'gt','>=':'ge'}
TOKEN = re.compile(r'\s+|\#[^\n]*|//[^\n]*|"(?:[^"\\]|\\.)*"|\d+(?:\.\d+)?|[^\W\d]\w*|==|!=|<=|>=|&&|\|\||[{}();,\[\]+*/%<>=!:.\-]', re.UNICODE)


def tokenize(source):
    out=[];pos=0
    for m in TOKEN.finditer(source):
        if m.start()!=pos: raise ValueError(f'Invalid source at byte {pos}')
        pos=m.end();s=m.group()
        if not s.isspace() and not s.startswith(('#','//')): out.append(s)
    if pos!=len(source): raise ValueError('Trailing source data')
    return out+['<fim>']


class Bootstrap:
    def __init__(self, source):
        self.t=tokenize(source);self.i=0
    def take(self):
        s=self.t[self.i];self.i+=1;return s
    def accept(self,s):
        if self.t[self.i]==s:self.i+=1;return True
        return False
    def need(self,s):
        if not self.accept(s):raise ValueError(f'Expected {s}, found {self.t[self.i]} at token {self.i}')
    def name(self):
        s=self.take()
        if not s.isidentifier():raise ValueError('Identifier required')
        return s
    def expr(self,level=0):
        levels=[['||'],['&&'],['==','!='],['<','<=','>','>='],['+','-'],['*','/','%']]
        if level<len(levels):
            a=self.expr(level+1)
            while self.t[self.i] in levels[level]:
                op=self.take();a=['bin',op,a,self.expr(level+1)]
            return a
        if self.t[self.i] in ('!','-'):
            return ['un',self.take(),self.expr(level)]
        s=self.take()
        if s=='(':
            a=self.expr();self.need(')')
        elif s.startswith('"'):
            value=json.loads(s)
            if '\0' in value:raise ValueError('Embedded NUL is not supported')
            a=['s',value]
        elif s[0].isdigit():a=['n',s]
        elif s=='[':
            items=[]
            if not self.accept(']'):
                items.append(self.expr())
                while self.accept(','):items.append(self.expr())
                self.need(']')
            a=['list',items]
        elif s.isidentifier():
            if self.accept('('):
                args=[]
                if not self.accept(')'):
                    args.append(self.expr())
                    while self.accept(','):args.append(self.expr())
                    self.need(')')
                a=['call',s,args]
            else:a=['v',s]
        else:raise ValueError(f'Invalid expression token {s}')
        while self.accept('['):
            a=['index',a,self.expr()];self.need(']')
        return a
    def block(self):
        self.need('{');out=[]
        while not self.accept('}'):
            word=self.t[self.i]
            if self.accept('var'):
                n=self.name();self.need('=');out.append(['var',n,self.expr()]);self.need(';')
            elif self.accept('retornar'):
                out.append(['return',self.expr()]);self.need(';')
            elif self.accept('parar'):
                out.append(['break']);self.need(';')
            elif word in ('se','enquanto'):
                self.take();self.need('(');c=self.expr();self.need(')');body=self.block()
                other=self.block() if word=='se' and self.accept('senao') else []
                out.append([word,c,body,other])
            elif self.t[self.i+1]=='=':
                n=self.name();self.need('=');out.append(['set',n,self.expr()]);self.need(';')
            else:
                out.append(['expr',self.expr()]);self.need(';')
        return out
    def parse(self):
        funcs=[]
        while self.t[self.i]!='<fim>':
            self.need('funcao');n=self.name();self.need('(');args=[]
            if not self.accept(')'):
                args.append(self.name())
                while self.accept(','):args.append(self.name())
                self.need(')')
            funcs.append([n,args,self.block()])
        return funcs


def apply_c(name,args):
    return '({' + ''.join('V a'+str(i)+'='+cexpr(a)+';' for i,a in enumerate(args)) + name+'('+','.join('a'+str(i) for i in range(len(args)))+');})'


def c_literal(text):
    """C string syntax differs from JSON for ASCII control characters."""
    if '\0' in text:raise ValueError('Embedded NUL is not supported')
    text.encode('utf-8')
    special={'"':'\\"','\\':'\\\\','\n':'\\n','\r':'\\r','\t':'\\t'}
    return '"'+''.join(special.get(c,('\\'+format(ord(c),'03o')) if ord(c)<32 or ord(c)==127 else c)
                       for c in text)+'"'


def cexpr(e):
    kind=e[0]
    if kind in ('s','n'):return ('o_str' if kind=='s' else 'o_num')+'('+c_literal(e[1])+')'
    if kind=='v':return 'v_'+e[1]
    if kind=='index':return apply_c('o_get',e[1:])
    if kind=='call':
        name=('o_'+BUILTINS[e[1]]) if e[1] in BUILTINS else 'fn_'+e[1]
        return apply_c(name,e[2])
    if kind=='list':
        return '({V r=o_list();'+''.join('o_push(r,'+cexpr(a)+');' for a in e[1])+'r;})'
    if kind=='un':return ('o_not' if e[1]=='!' else 'o_neg')+'('+cexpr(e[2])+')'
    if e[1] in ('&&','||'):return 'o_bool(o_truth('+cexpr(e[2])+')'+e[1]+'o_truth('+cexpr(e[3])+'))'
    return apply_c('o_'+OPS[e[1]],e[2:])


def cblock(stmts):
    out=[]
    for s in stmts:
        op=s[0]
        if op in ('var','set'):out.append(('V ' if op=='var' else '')+'v_'+s[1]+'='+cexpr(s[2])+';')
        elif op=='expr':out.append(cexpr(s[1])+';')
        elif op=='return':out.append('return o_ret('+cexpr(s[1])+');')
        elif op=='break':out.append('break;')
        else:
            out.append(('if' if op=='se' else 'while')+'(o_truth('+cexpr(s[1])+')){o_tick();\n'+cblock(s[2])+'}\n')
            if s[3]:out.append('else{\n'+cblock(s[3])+'}\n')
    return '\n'.join(out)+'\n'


def compile_source(source):
    fs=Bootstrap(source).parse()
    lines=['#include "runtime.h"']
    for n,args,_ in fs:lines.append('static V fn_'+n+'('+','.join('V v_'+a for a in args)+');')
    for n,args,body in fs:
        lines.append('static V fn_'+n+'('+','.join('V v_'+a for a in args)+'){o_enter();\n'+cblock(body)+'return o_ret(o_int(0));}\n')
    lines.append('int main(int argc,char **argv){o_init(argc,argv);return (int)o_long(fn_principal());}\n')
    return '\n'.join(lines)


def main():
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    a.output.write_text(compile_source(a.source.read_text()),encoding='utf-8')
if __name__=='__main__':main()
