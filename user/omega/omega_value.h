#ifndef OMEGA_VALUE_PORT_H
#define OMEGA_VALUE_PORT_H
#include "rational.h"
typedef struct Value *V;
struct Value { unsigned kind; omega_rational_t q; char *s; size_t n,cap; V *a; char **keys; };
void omega_fail(const char *s) __attribute__((noreturn));
void *ov_alloc(size_t n);
#define OV static __attribute__((unused))
OV size_t ov_len(const char *s) { size_t n=0; while(s[n]) n++; return n; }
OV int ov_cmp(const char *a,const char *b) { while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
extern uint64_t ov_gas;
OV void o_tick(void) { if(++ov_gas>2000000) omega_fail("Omega instruction budget"); }
OV void o_enter(void) { o_tick(); }
OV V o_ret(V v) { return v; }
OV V ov_new(unsigned kind) { V v=ov_alloc(sizeof *v); v->kind=kind; v->q.den=1; return v; }
OV V o_int(int64_t n) { V v=ov_new(0);v->q.num=n;return v; }
OV V o_bool(bool b) { return o_int(b?1:0); }
OV V ov_string(const char *s,size_t n) { V v=ov_new(1);v->s=ov_alloc(n+1);memcpy(v->s,s,n);v->s[n]=0;v->n=n;return v; }
OV V o_str(const char *s) { return ov_string(s,ov_len(s)); }
OV bool o_truth(V v) { return v->kind==0?v->q.num!=0:v->n!=0; }
OV int64_t o_long(V v) { if(v->kind||v->q.den!=1) omega_fail("Integer required");return v->q.num; }
OV V o_list(void) { return ov_new(2); }
OV V o_obj(void) { return ov_new(3); }
OV V o_len(V v) { return o_int((int64_t)v->n); }
OV void ov_grow(V v) { if(v->n==v->cap) { size_t cap=v->cap?v->cap*2:4; V *a=ov_alloc(cap*sizeof(V)); if(v->n)memcpy(a,v->a,v->n*sizeof(V));v->a=a;
 if(v->kind==3){char **keys=ov_alloc(cap*sizeof(char *));if(v->n)memcpy(keys,v->keys,v->n*sizeof(char *));v->keys=keys;}v->cap=cap; } }
OV V o_push(V a,V v) { if(a->kind!=2)omega_fail("List push");ov_grow(a);a->a[a->n++]=v;return a; }
OV V o_has(V a,V k) { if(a->kind!=3||k->kind!=1)return o_bool(false);for(size_t i=0;i<a->n;i++)if(!ov_cmp(a->keys[i],k->s))return o_bool(true);return o_bool(false); }
OV V o_get(V a,V k) {
 if(a->kind==3&&k->kind==1){for(size_t i=0;i<a->n;i++)if(!ov_cmp(a->keys[i],k->s))return a->a[i];omega_fail("Missing object key");}
 {int64_t i=o_long(k);if(i<0||(uint64_t)i>=a->n)omega_fail("Sequence index");if(a->kind==2)return a->a[i];if(a->kind==1)return ov_string(a->s+i,1);}
 omega_fail("Object/sequence required");
}
OV V o_put(V a,V k,V v) {
 if(a->kind==3&&k->kind==1){for(size_t i=0;i<a->n;i++)if(!ov_cmp(a->keys[i],k->s)){a->a[i]=v;return v;}ov_grow(a);a->keys[a->n]=k->s;a->a[a->n++]=v;return v;}
 if(a->kind==2){int64_t i=o_long(k);if(i<0||(uint64_t)i>=a->n)omega_fail("List assignment");a->a[i]=v;return v;}omega_fail("Invalid assignment");
}
OV bool ov_equal(V a,V b) { if(a->kind!=b->kind)return false;if(a->kind==0)return a->q.num==b->q.num&&a->q.den==b->q.den;if(a->kind==1)return !ov_cmp(a->s,b->s);if(a->n!=b->n)return false;for(size_t i=0;i<a->n;i++){if(a->kind==3&&ov_cmp(a->keys[i],b->keys[i]))return false;if(!ov_equal(a->a[i],b->a[i]))return false;}return true; }
OV V o_eq(V a,V b){return o_bool(ov_equal(a,b));}
OV V o_ne(V a,V b){return o_bool(!ov_equal(a,b));}
OV int ov_compare(V a,V b) { int c;if(a->kind==1&&b->kind==1)return ov_cmp(a->s,b->s);if(a->kind||b->kind||!rational_cmp(a->q,b->q,&c))omega_fail("Comparison domain");return c; }
OV V o_lt(V a,V b){return o_bool(ov_compare(a,b)<0);}
OV V o_le(V a,V b){return o_bool(ov_compare(a,b)<=0);}
OV V o_gt(V a,V b){return o_bool(ov_compare(a,b)>0);}
OV V o_ge(V a,V b){return o_bool(ov_compare(a,b)>=0);}
OV V ov_q(omega_rational_t q){V v=ov_new(0);v->q=q;return v;}
OV V o_add(V a,V b) {omega_rational_t q;if(a->kind==1&&b->kind==1){char *s=ov_alloc(a->n+b->n+1);memcpy(s,a->s,a->n);memcpy(s+a->n,b->s,b->n);s[a->n+b->n]=0;return o_str(s);}if(a->kind||b->kind||!rational_add(a->q,b->q,&q))omega_fail("Exact add overflow/type");return ov_q(q);}
OV V o_sub(V a,V b){omega_rational_t q;if(a->kind||b->kind||!rational_sub(a->q,b->q,&q))omega_fail("Exact subtract");return ov_q(q);}
OV V o_mul(V a,V b){omega_rational_t q;if(a->kind||b->kind||!rational_mul(a->q,b->q,&q))omega_fail("Exact multiply");return ov_q(q);}
OV V o_div(V a,V b){omega_rational_t inv,q;if(a->kind||b->kind||!rational_make(b->q.den,b->q.num,&inv)||!rational_mul(a->q,inv,&q))omega_fail("Exact division");return ov_q(q);}
OV V o_mod(V a,V b){int64_t n=o_long(a),d=o_long(b);if(!d||(n==(-9223372036854775807LL-1)&&d==-1))omega_fail("Integer modulus");return o_int(n%d);}
OV V o_neg(V a){return o_sub(o_int(0),a);}
OV V o_not(V a){return o_bool(!o_truth(a));}
OV int64_t ov_integer(const char **p){bool neg=false;uint64_t n=0,limit=9223372036854775807ULL;if(**p=='-'){neg=true;(*p)++;limit++;}if(**p<'0'||**p>'9')omega_fail("Numeric literal");while(**p>='0'&&**p<='9'){unsigned d=(unsigned)(*(*p)++-'0');if(n>(limit-d)/10)omega_fail("Literal overflow");n=n*10+d;}return neg?(n==(1ULL<<63)?(-9223372036854775807LL-1):-(int64_t)n):(int64_t)n;}
OV V o_num(const char *s) {const char *p=s;int64_t n=ov_integer(&p),d=1;omega_rational_t q;if(*p=='/'){p++;d=ov_integer(&p);}else if(*p=='.'){p++;while(*p>='0'&&*p<='9'){int64_t t;if(__builtin_mul_overflow(d,10LL,&d)||__builtin_mul_overflow(n,10LL,&t)||__builtin_add_overflow(t,(s[0]=='-'?-(int64_t)(*p++-'0'):(int64_t)(*p++-'0')),&n))omega_fail("Decimal overflow");}}if(*p||!rational_make(n,d,&q))omega_fail("Rational literal");return ov_q(q);}
OV V o_number(V s){if(s->kind!=1)omega_fail("Number text");return o_num(s->s);}
OV V ov_digits(int64_t n) {char out[32],rev[24];size_t m=0,k=0;uint64_t v=n<0?0ULL-(uint64_t)n:(uint64_t)n;if(n<0)out[k++]='-';do{rev[m++]=(char)('0'+v%10);v/=10;}while(v);while(m)out[k++]=rev[--m];return ov_string(out,k);}
OV V o_text(V a){if(a->kind==1)return a;if(a->kind==0){V s=ov_digits(a->q.num);if(a->q.den!=1)s=o_add(o_add(s,o_str("/")),ov_digits(a->q.den));return s;}omega_fail("Scalar text");}
OV V o_isnum(V a){return o_bool(a->kind==0);}
OV V o_isstr(V a){return o_bool(a->kind==1);}
OV V o_islist(V a){return o_bool(a->kind==2);}
OV V o_slice(V a,V l,V h){int64_t lo=o_long(l),hi=o_long(h);if(lo<0||hi<lo||(uint64_t)hi>a->n)omega_fail("Slice range");if(a->kind==1)return ov_string(a->s+lo,(size_t)(hi-lo));if(a->kind==2){V v=o_list();while(lo<hi)o_push(v,a->a[lo++]);return v;}omega_fail("Slice type");}
OV V o_contains(V a,V b){if(a->kind==2){for(size_t i=0;i<a->n;i++)if(ov_equal(a->a[i],b))return o_bool(true);return o_bool(false);}if(a->kind==1&&b->kind==1){if(b->n>a->n)return o_bool(false);for(size_t i=0;i+b->n<=a->n;i++){size_t j=0;while(j<b->n&&a->s[i+j]==b->s[j])j++;if(j==b->n)return o_bool(true);}return o_bool(false);}omega_fail("Contains domain");}
OV V o_ident(V a){if(a->kind!=1||!a->n)return o_bool(false);for(size_t i=0;i<a->n;i++){unsigned char c=(unsigned char)a->s[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||(i&&c>='0'&&c<='9')))return o_bool(false);}return o_bool(true);}
OV V o_decimal(V a,V pos){int64_t i=o_long(pos);return o_int(a->kind==1&&i>=0&&(uint64_t)i<a->n&&a->s[i]>='0'&&a->s[i]<='9'?1:0);}
OV V o_qden(V a){if(a->kind)omega_fail("Rational required");return o_int(a->q.den);}
OV V o_qbits(V a){uint64_t n=a->q.num<0?0ULL-(uint64_t)a->q.num:(uint64_t)a->q.num;unsigned b=1,d=1;while(b<64&&n>>b)b++;n=(uint64_t)a->q.den;while(d<64&&n>>d)d++;return o_int(b>d?b:d);}
OV V o_fail(V s){omega_fail(o_text(s)->s);}

OV V o_keys(V a){if(a->kind!=3)omega_fail("Object keys");V out=o_list();for(size_t i=0;i<a->n;i++)o_push(out,o_str(a->keys[i]));return out;}
OV V o_join(V a,V separator){if(a->kind!=2||separator->kind!=1)omega_fail("Join types");size_t n=0;for(size_t i=0;i<a->n;i++){if(a->a[i]->kind!=1||a->a[i]->n>8*1024*1024-n)omega_fail("Join budget");n+=a->a[i]->n;if(i)n+=separator->n;}char *out=ov_alloc(n+1);size_t at=0;for(size_t i=0;i<a->n;i++){if(i){memcpy(out+at,separator->s,separator->n);at+=separator->n;}memcpy(out+at,a->a[i]->s,a->a[i]->n);at+=a->a[i]->n;}out[at]=0;return o_str(out);}
OV void ov_json(V a,V parts,unsigned depth){if(depth>64)omega_fail("JSON depth");if(a->kind==0){if(a->q.den!=1)omega_fail("JSON requires integer or explicit rational text");o_push(parts,o_text(a));return;}if(a->kind==1){o_push(parts,o_str("\""));for(size_t i=0;i<a->n;i++){unsigned char c=(unsigned char)a->s[i];if(c=='"')o_push(parts,o_str("\\\""));else if(c=='\\')o_push(parts,o_str("\\\\"));else if(c=='\n')o_push(parts,o_str("\\n"));else if(c=='\r')o_push(parts,o_str("\\r"));else if(c=='\t')o_push(parts,o_str("\\t"));else if(c<32){char e[7]={'\\','u','0','0',"0123456789abcdef"[c>>4],"0123456789abcdef"[c&15],0};o_push(parts,o_str(e));}else o_push(parts,ov_string(a->s+i,1));}o_push(parts,o_str("\""));return;}o_push(parts,o_str(a->kind==2?"[":"{"));for(size_t i=0;i<a->n;i++){if(i)o_push(parts,o_str(","));if(a->kind==3){ov_json(o_str(a->keys[i]),parts,depth+1);o_push(parts,o_str(":"));}ov_json(a->a[i],parts,depth+1);}o_push(parts,o_str(a->kind==2?"]":"}"));}
OV V o_json(V a){V parts=o_list();ov_json(a,parts,0);return o_join(parts,o_str(""));}
OV void ov_space(const char **p){while(**p==' '||**p=='\n'||**p=='\r'||**p=='\t')(*p)++;}
OV unsigned ov_hex(char c){if(c>='0'&&c<='9')return (unsigned)(c-'0');if(c>='a'&&c<='f')return (unsigned)(c-'a'+10);if(c>='A'&&c<='F')return (unsigned)(c-'A'+10);omega_fail("JSON hex");}
OV V ov_parse(const char **p,unsigned depth){if(depth>64)omega_fail("JSON depth");ov_space(p);if(**p=='"'){(*p)++;V parts=o_list();while(**p&&**p!='"'){unsigned char c=(unsigned char)*(*p)++;if(c<32)omega_fail("JSON control");if(c=='\\'){c=(unsigned char)*(*p)++;if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='b')c='\b';else if(c=='f')c='\f';else if(c=='u'){unsigned v=0;for(unsigned i=0;i<4;i++){if(!**p)omega_fail("JSON unicode truncated");v=v*16+ov_hex(*(*p)++);}if(v>=0xd800&&v<=0xdfff)omega_fail("JSON surrogate unsupported");char bytes[3];size_t n=1;if(v<128)bytes[0]=(char)v;else if(v<2048){bytes[0]=(char)(0xc0|(v>>6));bytes[1]=(char)(0x80|(v&63));n=2;}else{bytes[0]=(char)(0xe0|(v>>12));bytes[1]=(char)(0x80|((v>>6)&63));bytes[2]=(char)(0x80|(v&63));n=3;}if(!v)omega_fail("Embedded NUL unsupported");o_push(parts,ov_string(bytes,n));continue;}else if(c!='"'&&c!='\\'&&c!='/')omega_fail("JSON escape");}o_push(parts,ov_string((const char *)&c,1));}if(*(*p)++!='"')omega_fail("JSON string");return o_join(parts,o_str(""));}
 if(**p=='['||**p=='{'){bool object=**p=='{';char end=object?'}':']';(*p)++;V a=object?o_obj():o_list();ov_space(p);if(**p==end){(*p)++;return a;}for(;;){if(object){V key=ov_parse(p,depth+1);ov_space(p);if(key->kind!=1||*(*p)++!=':')omega_fail("JSON key");o_put(a,key,ov_parse(p,depth+1));}else o_push(a,ov_parse(p,depth+1));ov_space(p);if(**p==end){(*p)++;return a;}if(*(*p)++!=',')omega_fail("JSON comma");}}
 const char *start=*p;if(**p=='-')(*p)++;while(**p>='0'&&**p<='9')(*p)++;if(*p==start)omega_fail("JSON number");V text=ov_string(start,(size_t)(*p-start));return o_num(text->s);}
OV V o_parsejson(V s){if(s->kind!=1)omega_fail("JSON text");const char *p=s->s;V v=ov_parse(&p,0);ov_space(&p);if(*p)omega_fail("JSON trailing data");return v;}
extern const char *ov_input;
extern V ov_output;
OV V o_argc(void){return o_int(4);}
OV V o_arg(V n){int64_t i=o_long(n);if(i==0)return o_str("omegac");if(i==1)return o_str("mutation.omega");if(i==2)return o_str("mutation.ir");if(i==3)return o_str("--ir");omega_fail("Compiler argument");}
OV V o_read(V path){if(path->kind!=1||ov_cmp(path->s,"mutation.omega"))omega_fail("Compiler input capability");return o_str(ov_input);}
OV V o_write(V path,V text){if(path->kind!=1||ov_cmp(path->s,"mutation.ir")||text->kind!=1)omega_fail("Compiler output capability");ov_output=text;return o_int(0);}
V omega_frontend(const char *source);
V omega_egraph(V request);
#endif
