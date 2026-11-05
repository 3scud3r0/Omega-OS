#include "rational.h"
#define I64_MAX 9223372036854775807LL
#define I64_MIN (-I64_MAX-1)
static uint64_t magnitude(int64_t n) { return n<0?0ULL-(uint64_t)n:(uint64_t)n; }
static uint64_t gcd(uint64_t a,uint64_t b) { while(b) { uint64_t t=a%b; a=b; b=t; } return a; }
bool rational_make(int64_t n,int64_t d,omega_rational_t *r) {
 uint64_t g,un,ud; bool neg;
 if(!d) return false;
 neg=(n<0)!=(d<0); un=magnitude(n); ud=magnitude(d); g=gcd(un,ud); un/=g; ud/=g;
 if(ud>(uint64_t)I64_MAX || un>((uint64_t)I64_MAX+(neg?1ULL:0ULL))) return false;
 r->den=(int64_t)ud;
 r->num=neg?(un==(1ULL<<63)?I64_MIN:-(int64_t)un):(int64_t)un; return true;
}
bool rational_valid(omega_rational_t r) { return r.den>0 && gcd(magnitude(r.num),(uint64_t)r.den)==1; }
/* Checked intermediates: representable results may still be rejected on overflow. */
bool rational_add(omega_rational_t a,omega_rational_t b,omega_rational_t *r) {
 int64_t x,y,n,d; uint64_t g;
 if(!rational_valid(a)||!rational_valid(b)) return false;
 g=gcd((uint64_t)a.den,(uint64_t)b.den);
 if(__builtin_mul_overflow(a.num,b.den/(int64_t)g,&x)||__builtin_mul_overflow(b.num,a.den/(int64_t)g,&y)||__builtin_add_overflow(x,y,&n)||__builtin_mul_overflow(a.den,b.den/(int64_t)g,&d)) return false;
 return rational_make(n,d,r);
}
bool rational_sub(omega_rational_t a,omega_rational_t b,omega_rational_t *r) {
 if(b.num==I64_MIN) return false;
 b.num=-b.num; return rational_add(a,b,r);
}
bool rational_mul(omega_rational_t a,omega_rational_t b,omega_rational_t *r) {
 int64_t n,d; uint64_t g,h;
 if(!rational_valid(a)||!rational_valid(b)) return false;
 g=gcd(magnitude(a.num),(uint64_t)b.den); h=gcd(magnitude(b.num),(uint64_t)a.den);
 if(__builtin_mul_overflow(a.num/(int64_t)g,b.num/(int64_t)h,&n)||__builtin_mul_overflow(a.den/(int64_t)h,b.den/(int64_t)g,&d)) return false;
 return rational_make(n,d,r);
}
/* Continued-fraction comparison avoids cross-product overflow. */
bool rational_cmp(omega_rational_t a,omega_rational_t b,int *result) {
 uint64_t an,bn,ad,bd; int sign=1;
 if(!rational_valid(a)||!rational_valid(b)) return false;
 if((a.num<0)!=(b.num<0)) { *result=a.num<0?-1:1; return true; }
 if(a.num<0) sign=-1;
 an=magnitude(a.num); bn=magnitude(b.num); ad=(uint64_t)a.den; bd=(uint64_t)b.den;
 for(;;) { uint64_t aq=an/ad,bq=bn/bd,ar=an%ad,br=bn%bd;
  if(aq!=bq) { *result=sign*(aq<bq?-1:1); return true; }
  if(!ar||!br) { *result=ar==br?0:sign*(!ar?-1:1); return true; }
  an=ad; ad=ar; bn=bd; bd=br; sign=-sign;
 }
}
