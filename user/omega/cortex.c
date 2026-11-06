/* Omega Cortex: streaming GGUF/Llama inference in a private Ring-3 ELF.
 * No libc, heap, BLAS, CUDA or floating-point kernel dependency.
 * Weights and FP16 KV blocks are acquired as SLS page capabilities. */
#include "cortex.h"
#include "jit.h"
#include "crypto/monocypher.h"
#define LOCAL static __attribute__((unused))
#define MAX_DIM 8192U
#define MAX_HIDDEN 32768U
#define MAX_VOCAB 262144U
#define MAX_TENSORS 4096U
#define PI 3.14159265358979323846f
LOCAL bool vector_available(void) {
    uint32_t a, b, c, d, lo, hi;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if ((c & ((1U << 27) | (1U << 28) | (1U << 12))) != ((1U << 27) | (1U << 28) | (1U << 12)))
        return false;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 7) != 7)
        return false;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    return (b & (1U << 5)) != 0;
}
LOCAL bool finite(float x) {
    uint32_t u;
    memcpy(&u, &x, 4);
    return (u & 0x7f800000U) != 0x7f800000U;
}
float cortex_fp16(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 1023, u;
    if (e == 31)
        u = sign | 0x7f800000U | (m << 13);
    else if (!e) {
        if (!m)
            u = sign;
        else {
            unsigned shift = 0;
            while (!(m & 1024)) {
                m <<= 1;
                shift++;
            }
            u = sign | ((127 - 14 - shift) << 23) | ((m & 1023) << 13);
        }
    } else
        u = sign | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
LOCAL uint16_t half(float x) {
    uint32_t u;
    memcpy(&u, &x, 4);
    uint32_t sign = (u >> 16) & 0x8000, e = (u >> 23) & 255, m = u & 0x7fffff;
    if (e == 255)
        return (uint16_t)(sign | 0x7c00 | (m ? 0x200 : 0));
    int exponent = (int)e - 127 + 15;
    if (exponent >= 31)
        return (uint16_t)(sign | 0x7c00);
    if (exponent <= 0) {
        if (exponent < -10)
            return (uint16_t)sign;
        m |= 0x800000;
        unsigned shift = (unsigned)(14 - exponent);
        uint32_t value = m >> shift, rem = m & ((1U << shift) - 1), mid = 1U << (shift - 1);
        if (rem > mid || (rem == mid && (value & 1)))
            value++;
        return (uint16_t)(sign | value);
    }
    uint32_t value = m >> 13, rem = m & 8191;
    if (rem > 4096 || (rem == 4096 && (value & 1))) {
        value++;
        if (value == 1024) {
            value = 0;
            exponent++;
        }
    }
    return (uint16_t)(sign | ((uint32_t)exponent << 10) | value);
}
LOCAL float load_float(const uint8_t *p) {
    float f;
    memcpy(&f, p, 4);
    return f;
}
LOCAL float load_half(const uint8_t *p) {
    uint16_t h;
    memcpy(&h, p, 2);
    return cortex_fp16(h);
}
float cortex_dequant_value(unsigned type, const uint8_t *p, unsigned index) {
    if (type == 0)
        return load_float(p + 4 * index);
    if (type == 1)
        return load_half(p + 2 * index);
    if (type == 8)
        return load_half(p) * (float)((const signed char *)(p + 2))[index];
    if (type == 12) {
        unsigned group = index / 32, j = index % 32, scale, min;
        const uint8_t *s = p + 4;
        if (group < 4) {
            scale = s[group] & 63;
            min = s[group + 4] & 63;
        } else {
            scale = (s[group + 4] & 15) | ((s[group - 4] >> 6) << 4);
            min = (s[group + 4] >> 4) | ((s[group] >> 6) << 4);
        }
        unsigned q = p[16 + (group / 2) * 32 + j];
        q = (group & 1) ? q >> 4 : q & 15;
        return load_half(p) * (float)(scale * q) - load_half(p + 2) * (float)min;
    }
    if (type == 14) {
        unsigned part = index / 128, i = index % 128, l = i % 32, group = i / 32;
        const uint8_t *ql = p + part * 64, *qh = p + 128 + part * 32;
        unsigned low = ql[l + (group & 1) * 32];
        low = group < 2 ? low & 15 : low >> 4;
        int q = (int)(low | (((qh[l] >> (group * 2)) & 3) << 4)) - 32;
        int scale = ((const signed char *)(p + 192))[part * 8 + group * 2 + l / 16];
        return load_half(p + 208) * (float)(scale * q);
    }
    return 0.0f / 0.0f;
}
LOCAL __attribute__((target("avx2,fma"))) float dot_avx(const float *a, const float *b,
                                                        unsigned n) {
    float lanes[8] ALIGN_64;
    unsigned i = 0;
    __asm__ volatile("vxorps %%ymm0,%%ymm0,%%ymm0" ::: "ymm0");
    for (; i + 8 <= n; i += 8)
        __asm__ volatile("vmovups (%0),%%ymm1; vfmadd231ps (%1),%%ymm1,%%ymm0" ::"r"(a + i),
                         "r"(b + i)
                         : "ymm0", "ymm1", "memory");
    __asm__ volatile("vmovups %%ymm0,%0; vzeroupper" : "=m"(lanes)::"ymm0", "memory");
    float s = 0;
    for (unsigned j = 0; j < 8; j++)
        s += lanes[j];
    for (; i < n; i++)
        s += a[i] * b[i];
    return s;
}
LOCAL float dot_scalar(const float *a, const float *b, unsigned n) {
    float s = 0;
    for (unsigned i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}
LOCAL __attribute__((target("avx2,fma"))) float quant_dot_avx(unsigned type, const uint8_t *quant,
                                                              const float *input) {
    float total = 0;
    unsigned groups = type == 8 ? 1 : 8;
    for (unsigned group = 0; group < groups; group++) {
        float scale, minimum = 0;
        unsigned shift = 0;
        const uint8_t *p;
        if (type == 8) {
            scale = load_half(quant);
            p = quant + 2;
        } else {
            const uint8_t *s = quant + 4;
            unsigned sc, mn;
            if (group < 4) {
                sc = s[group] & 63;
                mn = s[group + 4] & 63;
            } else {
                sc = (s[group + 4] & 15) | ((s[group - 4] >> 6) << 4);
                mn = (s[group + 4] >> 4) | ((s[group] >> 6) << 4);
            }
            scale = load_half(quant) * (float)sc;
            minimum = load_half(quant + 2) * (float)mn;
            shift = (group & 1) ? 4 : 0;
            p = quant + 16 + group / 2 * 32;
        }
        float lanes[8] ALIGN_64;
        const float *x = input + group * 32;
        unsigned count = 4, mask = 15;
        if (type == 8) {
            __asm__ volatile("vxorps %%ymm0,%%ymm0,%%ymm0; vbroadcastss %4,%%ymm2; 1: vpmovsxbd "
                             "(%1),%%ymm1; vcvtdq2ps %%ymm1,%%ymm1; vmulps %%ymm2,%%ymm1,%%ymm1; "
                             "vfmadd231ps (%2),%%ymm1,%%ymm0; addq $8,%1; addq $32,%2; decl %3; "
                             "jnz 1b; vmovups %%ymm0,%0; vzeroupper"
                             : "=m"(lanes), "+&r"(p), "+&r"(x), "+&r"(count)
                             : "m"(scale)
                             : "ymm0", "ymm1", "ymm2", "memory", "cc");
        } else {
            __asm__ volatile(
                "vxorps %%ymm0,%%ymm0,%%ymm0; vbroadcastss %4,%%ymm2; vbroadcastss %5,%%ymm3; "
                "vpbroadcastd %6,%%ymm4; 1: vpmovzxbd (%1),%%ymm1; testl %7,%7; jz 2f; vpsrld "
                "$4,%%ymm1,%%ymm1; 2: vpand %%ymm4,%%ymm1,%%ymm1; vcvtdq2ps %%ymm1,%%ymm1; vmulps "
                "%%ymm2,%%ymm1,%%ymm1; vsubps %%ymm3,%%ymm1,%%ymm1; vfmadd231ps "
                "(%2),%%ymm1,%%ymm0; addq $8,%1; addq $32,%2; decl %3; jnz 1b; vmovups %%ymm0,%0; "
                "vzeroupper"
                : "=m"(lanes), "+&r"(p), "+&r"(x), "+&r"(count)
                : "m"(scale), "m"(minimum), "m"(mask), "r"(shift)
                : "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "memory", "cc");
        }
        for (unsigned j = 0; j < 8; j++)
            total += lanes[j];
    }
    return total;
}
LOCAL __attribute__((target("avx2"))) int64_t i8_avx(const signed char *a, const signed char *b,
                                                     unsigned n) {
    int32_t lanes[8] ALIGN_64;
    int64_t sum = 0;
    unsigned i = 0;
    for (; i + 16 <= n; i += 16) {
        __asm__ volatile("vpmovsxbw (%1),%%ymm0; vpmovsxbw (%2),%%ymm1; vpmullw "
                         "%%ymm1,%%ymm0,%%ymm0; vpcmpeqw %%ymm1,%%ymm1,%%ymm1; vpsrlw "
                         "$15,%%ymm1,%%ymm1; vpmaddwd %%ymm1,%%ymm0,%%ymm0; vmovdqu %%ymm0,%0"
                         : "=m"(lanes)
                         : "r"(a + i), "r"(b + i)
                         : "ymm0", "ymm1", "memory");
        for (unsigned j = 0; j < 8; j++)
            sum += lanes[j];
    }
    __asm__ volatile("vzeroupper" ::: "memory");
    for (; i < n; i++)
        sum += (int64_t)a[i] * b[i];
    return sum;
}
bool cortex_gemm_i8(const signed char *a, const signed char *b, int64_t *c, uint32_t m, uint32_t n,
                    uint32_t k, bool vector) {
    if (!m || !n || !k || k > 1048576 || m > 65536 || n > 65536)
        return false;
    bool simd = vector && vector_available();
    /* A and B panels: 4 x K and 32 x K. Accumulation never saturates int16. */
    for (unsigned ii = 0; ii < m; ii += 4)
        for (unsigned jj = 0; jj < n; jj += 32)
            for (unsigned i = ii; i < m && i < ii + 4; i++)
                for (unsigned j = jj; j < n && j < jj + 32; j++) {
                    int64_t total = 0;
                    for (unsigned kk = 0; kk < k; kk += 256) {
                        unsigned count = k - kk;
                        if (count > 256)
                            count = 256;
                        if (simd)
                            total += i8_avx(a + (size_t)i * k + kk, b + (size_t)j * k + kk, count);
                        else {
                            /* 64-bit packed loads, explicitly signed lanes, exact int64
                             * accumulation. */
                            unsigned t = 0;
                            for (; t + 8 <= count; t += 8) {
                                uint64_t x, y;
                                memcpy(&x, a + (size_t)i * k + kk + t, 8);
                                memcpy(&y, b + (size_t)j * k + kk + t, 8);
                                for (unsigned lane = 0; lane < 8; lane++)
                                    total += (int64_t)(signed char)(x >> (lane * 8)) *
                                             (signed char)(y >> (lane * 8));
                            }
                            for (; t < count; t++)
                                total +=
                                    (int64_t)a[(size_t)i * k + kk + t] * b[(size_t)j * k + kk + t];
                        }
                    }
                    c[(size_t)i * n + j] = total;
                }
    return true;
}
unsigned cortex_symbolic_prune(float *scores, unsigned vocabulary, const cortex_contract_t *rules,
                               unsigned count) {
    unsigned rejected = 0;
    omega_rational_t one = {1, 1};
    uint32_t neginf = 0xff800000U;
    for (unsigned token = 0; token < vocabulary; token++) {
        bool allowed = false;
        for (unsigned i = 0; i < count; i++)
            if (rules[i].token == token) {
                omega_rational_t square;
                int cmp = 1;
                allowed = rational_valid(rules[i].value) &&
                          rational_mul(rules[i].value, rules[i].value, &square) &&
                          rational_cmp(square, one, &cmp) && cmp <= 0;
                break;
            }
        if (!allowed) {
            memcpy(scores + token, &neginf, 4);
            rejected++;
        }
    }
    return rejected;
}
LOCAL float root(float x) {
    if (x <= 0)
        return 0;
    uint32_t u;
    memcpy(&u, &x, 4);
    u = (u >> 1) + 0x1fc00000;
    float r;
    memcpy(&r, &u, 4);
    for (unsigned i = 0; i < 6; i++)
        r = 0.5f * (r + x / r);
    return r;
}
LOCAL float exponential(float x) {
    if (x < -80)
        return 0;
    if (x > 80)
        x = 80;
    float y = x * 1.4426950408889634f;
    int k = (int)y;
    if ((float)k > y)
        k--;
    float r = x - (float)k * 0.6931471805599453f;
    float p = 1 + r * (1 + r * (0.5f + r * (0.1666666667f +
                                            r * (0.0416666667f +
                                                 r * (0.0083333333f +
                                                      r * (0.0013888889f + r * 0.0001984127f))))));
    uint32_t bits = (uint32_t)(k + 127) << 23;
    float scale;
    memcpy(&scale, &bits, 4);
    return p * scale;
}
LOCAL float logarithm(float x) {
    uint32_t b;
    memcpy(&b, &x, 4);
    int k = (int)((b >> 23) & 255) - 127;
    b = (b & 0x7fffff) | 0x3f800000;
    float m;
    memcpy(&m, &b, 4);
    float z = (m - 1) / (m + 1), q = z * z, p = z, sum = 0;
    for (unsigned j = 1; j < 24; j += 2) {
        sum += p / (float)j;
        p *= q;
    }
    return 2 * sum + (float)k * 0.69314718056f;
}
LOCAL void sincosine(float x, float *s, float *c) {
    double angle = x;
    int64_t turns = (int64_t)(angle / 6.283185307179586476925);
    x = (float)(angle - (double)turns * 6.283185307179586476925);
    if (x > PI)
        x -= 2 * PI;
    if (x < -PI)
        x += 2 * PI;
    float cosine_sign = 1;
    if (x > PI / 2) {
        x = PI - x;
        cosine_sign = -1;
    }
    if (x < -PI / 2) {
        x = -PI - x;
        cosine_sign = -1;
    }
    float q = x * x;
    *s = x *
         (1 +
          q * (-0.16666666667f +
               q * (0.00833333333f +
                    q * (-0.00019841270f +
                         q * (0.00000275573f + q * (-0.00000002505f + q * 0.00000000016059f))))));
    *c = 1 + q * (-0.5f +
                  q * (0.04166666667f +
                       q * (-0.00138888889f +
                            q * (0.00002480159f + q * (-0.00000027557f + q * 0.00000000208768f)))));
    *c *= cosine_sign;
}
#ifdef CORTEX_HOST_TEST
float cortex_test_sin(float x) {
    float s, c;
    sincosine(x, &s, &c);
    return s;
}
float cortex_test_exp(float x) { return exponential(x); }
#endif
#ifndef CORTEX_HOST_TEST
extern void omega_fail(const char *s);
extern bool omega_cortex_contract(omega_rational_t value);
LOCAL uint64_t sys(uint64_t n, uint64_t a, uint64_t b) {
    uint64_t r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
LOCAL void fail(const char *s) { omega_fail(s); }
LOCAL void log_line(const char *s) {
    char *p = (char *)0x80003000;
    size_t n = 0;
    while (s[n] && n < 255) {
        p[n] = s[n];
        n++;
    }
    p[n] = 0;
    (void)sys(43, 0, 0);
}
LOCAL uint64_t mapped_id = ~0ULL, mapped_va;
LOCAL bool mapped_write;
LOCAL uint8_t *page(uint64_t id, bool write) {
    if (id == mapped_id && (!write || mapped_write))
        return (uint8_t *)mapped_va;
    if (mapped_id != ~0ULL && sys(41, mapped_va, 0))
        fail("page release");
    mapped_id = ~0ULL;
    uint64_t va = sys(40, id, write);
    if (va == ~0ULL)
        fail("SLS page capability unavailable");
    mapped_id = id;
    mapped_va = va;
    mapped_write = write;
    return (uint8_t *)va;
}
LOCAL void unmap(void) {
    if (mapped_id != ~0ULL) {
        if (sys(41, mapped_va, 0))
            fail("pager unpin");
        mapped_id = ~0ULL;
    }
}
LOCAL uint64_t file_bytes;
LOCAL void read_file(uint64_t offset, void *out, size_t n) {
    if (offset > file_bytes || n > file_bytes - offset)
        fail("GGUF file bounds");
    uint8_t *d = out;
    while (n) {
        size_t count = 4096 - (size_t)(offset & 4095);
        if (count > n)
            count = n;
        memcpy(d, page(CORTEX_MODEL + offset / 4096, false) + (offset & 4095), count);
        d += count;
        offset += count;
        n -= count;
    }
}
LOCAL uint32_t read32(uint64_t *at) {
    uint32_t x;
    read_file(*at, &x, 4);
    *at += 4;
    return x;
}
LOCAL uint64_t read64(uint64_t *at) {
    uint64_t x;
    read_file(*at, &x, 8);
    *at += 8;
    return x;
}
LOCAL bool equal(const char *a, const char *b) {
    unsigned i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    return a[i] == b[i];
}
LOCAL void string(uint64_t *at, char *out, size_t cap) {
    uint64_t n = read64(at);
    if (n > file_bytes - *at)
        fail("GGUF string length");
    if (out) {
        if (n >= cap)
            fail("GGUF identifier length");
        read_file(*at, out, (size_t)n);
        out[n] = 0;
    }
    *at += n;
}
LOCAL void skip_value(uint64_t *at, unsigned type, unsigned depth) {
    static const unsigned widths[13] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
    if (type >= 13 || depth > 4)
        fail("GGUF metadata type");
    if (type == 8) {
        string(at, (void *)0, 0);
        return;
    }
    if (type == 9) {
        unsigned subtype = read32(at);
        uint64_t n = read64(at);
        if (n > 16777216ULL || subtype == 9)
            fail("GGUF array bounds");
        for (uint64_t i = 0; i < n; i++)
            skip_value(at, subtype, depth + 1);
        return;
    }
    if (*at > file_bytes || widths[type] > file_bytes - *at)
        fail("GGUF scalar bounds");
    *at += widths[type];
}
typedef struct {
    char name[96];
    uint64_t offset, bytes;
    uint32_t columns, rows, type;
} tensor_t;
LOCAL tensor_t tensors[MAX_TENSORS];
LOCAL unsigned tensor_count;
typedef struct {
    unsigned dim, hidden, layers, heads, kv_heads, vocab, context;
    float epsilon, rope, rope_factor, rope_low, rope_high;
    unsigned rope_scaling, original_context;
} config_t;
LOCAL config_t cfg;
LOCAL bool simd;
LOCAL cortex_contract_t contracts[16];
LOCAL unsigned contract_count;
LOCAL uint64_t accepted_tokens, rejected_tokens;
LOCAL uint8_t model_hash[32];
LOCAL void gguf(void) {
    uint64_t at = 0;
    unsigned alignment = 32;
    if (read32(&at) != 0x46554747 || read32(&at) != 3)
        fail("GGUF v3 required");
    uint64_t count = read64(&at), metadata = read64(&at);
    if (!count || count > MAX_TENSORS || metadata > 1048576)
        fail("GGUF table limits");
    tensor_count = (unsigned)count;
    cfg.epsilon = 0.00001f;
    cfg.rope = 10000;
    cfg.rope_factor = 1;
    cfg.rope_low = 1;
    cfg.rope_high = 4;
    cfg.original_context = 8192;
    for (uint64_t i = 0; i < metadata; i++) {
        char name[192];
        string(&at, name, sizeof name);
        unsigned type = read32(&at);
        uint64_t start = at;
        if (equal(name, "general.architecture")) {
            char architecture[32];
            if (type != 8)
                fail("architecture metadata type");
            string(&at, architecture, sizeof architecture);
            if (!equal(architecture, "llama"))
                fail("GGUF architecture requires Llama tensors");
            continue;
        }
        if (equal(name, "llama.rope.scaling.type")) {
            char method[32];
            if (type != 8)
                fail("RoPE scaling type");
            string(&at, method, sizeof method);
            if (equal(method, "none"))
                cfg.rope_scaling = 0;
            else if (equal(method, "linear"))
                cfg.rope_scaling = 1;
            else if (equal(method, "llama3"))
                cfg.rope_scaling = 2;
            else
                fail("unsupported RoPE scaling method");
            continue;
        }
        if (equal(name, "llama.rope.scaling.factor") ||
            equal(name, "llama.rope.scaling.low_freq_factor") ||
            equal(name, "llama.rope.scaling.high_freq_factor")) {
            float value;
            if (type != 6)
                fail("RoPE scaling numeric type");
            read_file(at, &value, 4);
            at += 4;
            if (!finite(value) || value <= 0)
                fail("RoPE scaling range");
            if (equal(name, "llama.rope.scaling.factor"))
                cfg.rope_factor = value;
            else if (equal(name, "llama.rope.scaling.low_freq_factor"))
                cfg.rope_low = value;
            else
                cfg.rope_high = value;
            continue;
        }
        unsigned *field = (void *)0;
        if (equal(name, "llama.embedding_length"))
            field = &cfg.dim;
        if (equal(name, "llama.feed_forward_length"))
            field = &cfg.hidden;
        if (equal(name, "llama.block_count"))
            field = &cfg.layers;
        if (equal(name, "llama.attention.head_count"))
            field = &cfg.heads;
        if (equal(name, "llama.attention.head_count_kv"))
            field = &cfg.kv_heads;
        if (equal(name, "llama.context_length"))
            field = &cfg.context;
        if (equal(name, "llama.rope.scaling.original_context_length"))
            field = &cfg.original_context;
        if (equal(name, "general.alignment"))
            field = &alignment;
        if (field) {
            if (type != 4)
                fail("GGUF integer configuration type");
            *field = read32(&at);
            continue;
        }
        if (equal(name, "llama.attention.layer_norm_rms_epsilon") ||
            equal(name, "llama.rope.freq_base")) {
            if (type != 6)
                fail("GGUF float configuration type");
            float value;
            read_file(at, &value, 4);
            at += 4;
            if (!finite(value) || value <= 0)
                fail("GGUF float configuration");
            if (equal(name, "llama.rope.freq_base"))
                cfg.rope = value;
            else
                cfg.epsilon = value;
            continue;
        }
        at = start;
        skip_value(&at, type, 0);
    }
    if (cfg.rope < 1 || cfg.rope_high <= cfg.rope_low || !cfg.original_context)
        fail("RoPE scaling configuration");
    if (!cfg.kv_heads)
        cfg.kv_heads = cfg.heads;
    if (!cfg.dim || cfg.dim > MAX_DIM || !cfg.hidden || cfg.hidden > MAX_HIDDEN || !cfg.layers ||
        cfg.layers > 128 || !cfg.heads || cfg.heads > 128 || !cfg.kv_heads ||
        cfg.heads % cfg.kv_heads || cfg.dim % cfg.heads || (cfg.dim / cfg.heads) & 1 ||
        !cfg.context || cfg.context > 1048576 || !alignment || alignment > 4096 ||
        (alignment & (alignment - 1)))
        fail("GGUF model configuration");
    for (unsigned i = 0; i < tensor_count; i++) {
        tensor_t *t = &tensors[i];
        string(&at, t->name, sizeof t->name);
        unsigned rank = read32(&at);
        if (!rank || rank > 2)
            fail("Llama tensor rank");
        uint64_t cols = read64(&at), rows = rank == 2 ? read64(&at) : 1;
        if (cols > MAX_HIDDEN || rows > MAX_VOCAB || !cols || !rows)
            fail("tensor shape bounds");
        t->columns = (unsigned)cols;
        t->rows = (unsigned)rows;
        t->type = read32(&at);
        t->offset = read64(&at);
        unsigned block = t->type == 8 ? 32 : (t->type == 12 || t->type == 14 ? 256 : 1),
                 bytes =
                     t->type == 8
                         ? 34
                         : (t->type == 12 ? 144 : (t->type == 14 ? 210 : (t->type == 0 ? 4 : 2)));
        if (t->type != 0 && t->type != 1 && t->type != 8 && t->type != 12 && t->type != 14)
            fail("unsupported GGUF tensor quantization");
        if (cols % block)
            fail("quantized row alignment");
        t->bytes = (cols * rows / block) * bytes;
    }
    at = (at + alignment - 1) & ~((uint64_t)alignment - 1);
    if (at > file_bytes)
        fail("GGUF aligned tensor area bounds");
    for (unsigned i = 0; i < tensor_count; i++) {
        tensor_t *t = &tensors[i];
        if (t->offset > file_bytes - at || t->bytes > file_bytes - at - t->offset)
            fail("tensor data bounds");
        t->offset += at;
    }
}
LOCAL tensor_t *find(const char *name) {
    for (unsigned i = 0; i < tensor_count; i++)
        if (equal(tensors[i].name, name))
            return &tensors[i];
    return (void *)0;
}
LOCAL tensor_t *required(const char *name) {
    tensor_t *t = find(name);
    if (!t)
        fail("required Llama tensor missing");
    return t;
}
LOCAL tensor_t *layer(unsigned index, const char *suffix) {
    char name[96];
    unsigned n = 0;
    const char prefix[] = "blk.";
    memcpy(name, prefix, 4);
    n = 4;
    if (index >= 100)
        name[n++] = (char)('0' + index / 100);
    if (index >= 10)
        name[n++] = (char)('0' + index / 10 % 10);
    name[n++] = (char)('0' + index % 10);
    name[n++] = '.';
    for (unsigned i = 0; suffix[i]; i++)
        name[n++] = suffix[i];
    name[n] = 0;
    return required(name);
}
LOCAL unsigned block_values(unsigned type) {
    return type == 8 ? 32 : (type == 12 || type == 14 ? 256 : 1);
}
LOCAL unsigned block_bytes(unsigned type) {
    return type == 8 ? 34 : (type == 12 ? 144 : (type == 14 ? 210 : (type == 0 ? 4 : 2)));
}
LOCAL const uint8_t *weight_block(const tensor_t *t, uint64_t block, uint8_t temporary[210]) {
    uint64_t off = t->offset + block * block_bytes(t->type);
    size_t n = block_bytes(t->type);
    if ((off & 4095) + n <= 4096)
        return page(CORTEX_MODEL + off / 4096, false) + (off & 4095);
    read_file(off, temporary, n);
    return temporary;
}
LOCAL float weight(const tensor_t *t, uint64_t index) {
    uint8_t temporary[210];
    unsigned count = block_values(t->type);
    return cortex_dequant_value(t->type, weight_block(t, index / count, temporary),
                                (unsigned)(index % count));
}
LOCAL void matvec(float *out, const tensor_t *t, const float *input, unsigned rows,
                  unsigned columns) {
    if (t->rows != rows || t->columns != columns)
        fail("Llama matrix shape");
    unsigned block = block_values(t->type);
    for (unsigned base = 0; base < rows; base += 4)
        for (unsigned row = base; row < rows && row < base + 4; row++) {
            float value = 0;
            for (unsigned k = 0; k < columns; k += block) {
                uint8_t temporary[210];
                const uint8_t *q =
                    weight_block(t, ((uint64_t)row * columns + k) / block, temporary);
                float decoded[256] ALIGN_64;
                if (simd && (t->type == 8 || t->type == 12)) {
                    value += quant_dot_avx(t->type, q, input + k);
                    continue;
                }
                for (unsigned j = 0; j < block; j++)
                    decoded[j] = cortex_dequant_value(t->type, q, j);
                value += simd ? dot_avx(input + k, decoded, block)
                              : dot_scalar(input + k, decoded, block);
            }
            if (!finite(value))
                fail("nonfinite matrix output");
            out[row] = value;
        }
}
LOCAL void rms(float *out, const float *in, const tensor_t *scale) {
    if (scale->columns != cfg.dim || scale->rows != 1)
        fail("RMS shape");
    float sum = 0;
    for (unsigned i = 0; i < cfg.dim; i++)
        sum += in[i] * in[i];
    float inv = 1 / root(sum / (float)cfg.dim + cfg.epsilon);
    for (unsigned i = 0; i < cfg.dim; i++)
        out[i] = in[i] * inv * weight(scale, i);
}
LOCAL float x[MAX_DIM] ALIGN_64, xb[MAX_DIM] ALIGN_64, q[MAX_DIM] ALIGN_64, k[MAX_DIM] ALIGN_64,
    v[MAX_DIM] ALIGN_64, attention[MAX_DIM] ALIGN_64, projection[MAX_DIM] ALIGN_64,
    ffn[MAX_HIDDEN] ALIGN_64, gate[MAX_HIDDEN] ALIGN_64, logits[MAX_VOCAB] ALIGN_64;
LOCAL uint64_t kv_dimension(void) { return (uint64_t)cfg.dim * cfg.kv_heads / cfg.heads; }
LOCAL uint64_t layer_pages(void) { return (4ULL * kv_dimension() * cfg.context + 4095) / 4096; }
LOCAL void kv_write(unsigned layer_id, uint64_t position) {
    uint64_t dim = kv_dimension(), start = position * dim * 2;
    for (unsigned j = 0; j < dim * 2; j++) {
        uint64_t element = start + j;
        uint16_t h = half(j < dim ? k[j] : v[j - dim]);
        if ((h & 0x7c00) == 0x7c00)
            fail("FP16 KV overflow");
        uint8_t *p = page(CORTEX_KV + (uint64_t)layer_id * layer_pages() + element / 2048, true);
        memcpy(p + (element % 2048) * 2, &h, 2);
    }
}
LOCAL float kv_value(unsigned layer_id, uint64_t position, bool value, unsigned index) {
    uint64_t dim = kv_dimension(), element = position * dim * 2 + (value ? dim : 0) + index;
    uint16_t h;
    memcpy(&h,
           page(CORTEX_KV + (uint64_t)layer_id * layer_pages() + element / 2048, false) +
               (element % 2048) * 2,
           2);
    return cortex_fp16(h);
}
LOCAL void rope(float *vector, unsigned heads, uint64_t position) {
    tensor_t *factors = find("rope_freqs.weight");
    unsigned size = cfg.dim / cfg.heads;
    float logarithm_base = logarithm(cfg.rope);
    for (unsigned h = 0; h < heads; h++)
        for (unsigned j = 0; j < size; j += 2) {
            float frequency = exponential(-(float)j / (float)size * logarithm_base), s, c;
            if (factors) {
                if (factors->columns != size / 2 || factors->rows != 1)
                    fail("RoPE factor shape");
                float f = weight(factors, j / 2);
                if (!finite(f) || f <= 0)
                    fail("RoPE frequency factor");
                frequency /= f;
            } else if (cfg.rope_scaling == 1)
                frequency /= cfg.rope_factor;
            else if (cfg.rope_scaling == 2) {
                float wavelength = 2 * PI / frequency;
                if (wavelength > (float)cfg.original_context / cfg.rope_low)
                    frequency /= cfg.rope_factor;
                else if (wavelength > (float)cfg.original_context / cfg.rope_high) {
                    float smooth = ((float)cfg.original_context / wavelength - cfg.rope_low) /
                                   (cfg.rope_high - cfg.rope_low);
                    frequency = frequency * (smooth + (1 - smooth) / cfg.rope_factor);
                }
            }
            sincosine((float)position * frequency, &s, &c);
            float a = vector[h * size + j], b = vector[h * size + j + 1];
            vector[h * size + j] = a * c - b * s;
            vector[h * size + j + 1] = a * s + b * c;
        }
}
LOCAL float head_acc[MAX_DIM] ALIGN_64;
LOCAL void paged_attention(unsigned layer_id, uint64_t position) {
    unsigned size = cfg.dim / cfg.heads;
    float inv = 1 / root((float)size);
    for (unsigned head = 0; head < cfg.heads; head++) {
        float sum = 0, maximum = -3.402823466e38f;
        float *acc = head_acc;
        memset(acc, 0, size * sizeof(float));
        unsigned kv_head = head / (cfg.heads / cfg.kv_heads);
        /* Online softmax: no context-sized score buffer. K/V pages may be evicted
         * between every head/token; the block table is the SLS radix plus pager. */
        for (uint64_t t = 0; t <= position; t++) {
            float score = 0;
            for (unsigned j = 0; j < size; j++)
                score += q[head * size + j] * kv_value(layer_id, t, false, kv_head * size + j);
            score *= inv;
            float next = score > maximum ? score : maximum, old = exponential(maximum - next),
                  now = exponential(score - next);
            for (unsigned j = 0; j < size; j++)
                acc[j] = acc[j] * old + now * kv_value(layer_id, t, true, kv_head * size + j);
            sum = sum * old + now;
            maximum = next;
        }
        if (!finite(sum) || sum <= 0)
            fail("attention normalization");
        for (unsigned j = 0; j < size; j++)
            attention[head * size + j] = acc[j] / sum;
    }
}
LOCAL unsigned forward(unsigned token, uint64_t position) {
    tensor_t *embedding = required("token_embd.weight");
    if (embedding->columns != cfg.dim || token >= embedding->rows)
        fail("embedding token bounds");
    cfg.vocab = embedding->rows;
    for (unsigned j = 0; j < cfg.dim; j++)
        x[j] = weight(embedding, (uint64_t)token * cfg.dim + j);
    for (unsigned l = 0; l < cfg.layers; l++) {
        rms(xb, x, layer(l, "attn_norm.weight"));
        unsigned kv = (unsigned)kv_dimension();
        matvec(q, layer(l, "attn_q.weight"), xb, cfg.dim, cfg.dim);
        matvec(k, layer(l, "attn_k.weight"), xb, kv, cfg.dim);
        matvec(v, layer(l, "attn_v.weight"), xb, kv, cfg.dim);
        rope(q, cfg.heads, position);
        rope(k, cfg.kv_heads, position);
        kv_write(l, position);
        paged_attention(l, position);
        matvec(projection, layer(l, "attn_output.weight"), attention, cfg.dim, cfg.dim);
        for (unsigned j = 0; j < cfg.dim; j++)
            x[j] += projection[j];
        rms(xb, x, layer(l, "ffn_norm.weight"));
        matvec(ffn, layer(l, "ffn_up.weight"), xb, cfg.hidden, cfg.dim);
        matvec(gate, layer(l, "ffn_gate.weight"), xb, cfg.hidden, cfg.dim);
        for (unsigned j = 0; j < cfg.hidden; j++)
            ffn[j] *= gate[j] / (1 + exponential(-gate[j]));
        matvec(projection, layer(l, "ffn_down.weight"), ffn, cfg.dim, cfg.hidden);
        for (unsigned j = 0; j < cfg.dim; j++)
            x[j] += projection[j];
    }
    rms(xb, x, required("output_norm.weight"));
    tensor_t *output = find("output.weight");
    if (!output)
        output = embedding;
    matvec(logits, output, xb, cfg.vocab, cfg.dim);
    if (contract_count) {
        rejected_tokens += cortex_symbolic_prune(logits, cfg.vocab, contracts, contract_count);
        for (unsigned i = 0; i < contract_count; i++)
            if (contracts[i].token < cfg.vocab && finite(logits[contracts[i].token]) &&
                !omega_cortex_contract(contracts[i].value)) {
                uint32_t neginf = 0xff800000U;
                memcpy(logits + contracts[i].token, &neginf, 4);
                rejected_tokens++;
            }
    }
    unsigned best = 0;
    for (unsigned j = 1; j < cfg.vocab; j++)
        if (logits[j] > logits[best])
            best = j;
    if (!finite(logits[best]))
        fail("no token satisfies symbolic contract");
    accepted_tokens++;
    return best;
}
LOCAL void native_synthesis(cortex_checkpoint_t *checkpoint) {
    uint8_t *certificate = (uint8_t *)0x80003000;
    uint64_t magic = BYTECODE_MAGIC, length = 35;
    omega_rational_t a, b = {0, 1}, got;
    if (!rational_make((int64_t)checkpoint->token, cfg.vocab, &a))
        fail("native neural candidate rational");
    for (unsigned i = 0; i < contract_count; i++)
        if (contracts[i].token == checkpoint->token)
            a = contracts[i].value;
    if (!omega_cortex_contract(a))
        fail("native candidate equivalence");
    memcpy(certificate, &magic, 8);
    memcpy(certificate + 8, &length, 8);
    certificate[16] = 1;
    memcpy(certificate + 17, &a, 16);
    certificate[33] = 1;
    memcpy(certificate + 34, &b, 16);
    certificate[50] = 2;
    certificate[51] = 0;
    length = 36;
    memcpy(certificate + 8, &length, 8);
    size_t bytes;
    if (sys(19, 0, 0) || !jit_compile(certificate, 52, (uint8_t *)JIT_VA, &bytes) ||
        sys(11, JIT_VA, bytes) || !((bool (*)(omega_rational_t *))JIT_VA)(&got) ||
        got.num != a.num || got.den != a.den)
        fail("cortex native rational synthesis");
    crypto_blake2b(checkpoint->jit_hash, 32, (void *)JIT_VA, bytes);
}
LOCAL void hash_kv(uint64_t position, uint8_t digest[32]) {
    crypto_blake2b_ctx hash;
    crypto_blake2b_init(&hash, 32);
    for (unsigned l = 0; l < cfg.layers; l++) {
        uint64_t bytes = position * kv_dimension() * 4;
        crypto_blake2b_update(&hash, (const uint8_t *)&l, sizeof l);
        crypto_blake2b_update(&hash, (const uint8_t *)&bytes, 8);
        for (uint64_t off = 0; off < bytes; off += 4096) {
            size_t n = bytes - off < 4096 ? (size_t)(bytes - off) : 4096;
            crypto_blake2b_update(
                &hash, page(CORTEX_KV + (uint64_t)l * layer_pages() + off / 4096, false), n);
        }
    }
    crypto_blake2b_final(&hash, digest);
}
/* Optional destructive workload in an unused KV namespace. It exercises the
 * same public page capabilities as attention; no special kernel allocator. */
LOCAL void pager_scale_test(unsigned pages) {
    if (!pages)
        return;
    if (pages > 32768 || layer_pages() * cfg.layers > 0x400000ULL)
        fail("pager stress namespace bounds");
    uint64_t base = CORTEX_KV + 0x400000ULL;
    for (unsigned i = 0; i < pages; i++) {
        uint64_t *p = (uint64_t *)page(base + i, true);
        for (unsigned j = 0; j < 512; j++)
            p[j] = 0x8c7a350000000000ULL ^ ((uint64_t)i << 20) ^ j;
    }
    for (unsigned i = 0; i < pages; i++) {
        const uint64_t *p = (const uint64_t *)page(base + i, false);
        for (unsigned j = 0; j < 512; j++)
            if (p[j] != (0x8c7a350000000000ULL ^ ((uint64_t)i << 20) ^ j))
                fail("out-of-core page verification");
    }
    log_line("CORTEX out-of-core KV stress >64MiB data verified PASS");
}
LOCAL void vector_probe(unsigned role) {
    uint64_t expected[4] ALIGN_64 = {0x1234567800000000ULL + role, 0xfedcba9800000000ULL + role,
                                     0x2233445500000000ULL + role, 0x5544332200000000ULL + role},
                         got[4] ALIGN_64;
    uint64_t features = sys(44, 0, 0);
    bool avx = (features & 7) == 7;
    for (unsigned sample = 0; sample < 8; sample++) {
        if (avx)
            __asm__ volatile("vmovdqu %0,%%ymm15" ::"m"(expected) : "xmm15", "memory");
        else
            __asm__ volatile("movdqu %0,%%xmm15" ::"m"(expected) : "xmm15", "memory");
        /* Keep x87 state live as well; execute no floating C until captured. */
        __asm__ volatile("fld1" ::: "memory");
        for (volatile unsigned i = 0; i < 1000000; i++)
            __asm__ volatile("pause");
        (void)sys(0, 0, 0);
        (void)sys(46, 0, 0);
        if (avx)
            __asm__ volatile("vmovdqu %%ymm15,%0" : "=m"(got)::"memory");
        else
            __asm__ volatile("movdqu %%xmm15,%0" : "=m"(got)::"memory");
        double f;
        __asm__ volatile("fstpl %0" : "=m"(f)::"memory");
        if (f != 1.0)
            fail("x87 context corruption");
        for (unsigned j = 0; j < (avx ? 4U : 2U); j++)
            if (got[j] != expected[j])
                fail("SSE/AVX context corruption");
    }
    log_line(avx ? "CORTEX XSAVE x87/SSE/YMM integrity PASS"
                 : (features ? "CORTEX XSAVE x87/SSE integrity PASS"
                             : "CORTEX FXSAVE x87/SSE integrity PASS"));
}
void _start(void) {
    volatile uint64_t *data = (volatile uint64_t *)0x80002000;
    unsigned role = (unsigned)data[0];
    simd = vector_available();
    vector_probe(role);
    if (role == 8) {
        (void)sys(45, 0, 0);
        fail("probe exit returned");
    }
    log_line(simd ? "CORTEX Ring3 AVX2/FMA dispatch" : "CORTEX Ring3 scalar/SWAR dispatch");
    uint64_t meta = sys(40, CORTEX_META, 0);
    if (meta == ~0ULL) {
        log_line("CORTEX no GGUF provisioned; awaiting model pages");
        (void)sys(45, 0, 0);
        fail("cortex exit returned");
    }
    uint8_t header[1024];
    memcpy(header, (void *)meta, sizeof header);
    if (sys(41, meta, 0))
        fail("metadata unpin");
    const char domain[16] = "OMEGA-CORTEX-01";
    for (unsigned i = 0; i < 16; i++)
        if (header[i] != (uint8_t)domain[i])
            fail("cortex model descriptor");
    memcpy(&file_bytes, header + 16, 8);
    memcpy(model_hash, header + 24, 32);
    uint32_t steps, prompt_size;
    memcpy(&steps, header + 56, 4);
    memcpy(&prompt_size, header + 60, 4);
    if (!file_bytes || file_bytes > (CORTEX_KV - CORTEX_MODEL) * 4096 || !steps ||
        steps > 1048576 || prompt_size > 128)
        fail("model descriptor bounds");
    memcpy(&contract_count, header + 576, 4);
    if (contract_count > 16)
        fail("logical contract count");
    memcpy(contracts, header + 580, contract_count * sizeof *contracts);
    gguf();
    tensor_t *embedding = required("token_embd.weight");
    cfg.vocab = embedding->rows;
    if (cfg.vocab < 2 || cfg.vocab > MAX_VOCAB || layer_pages() * cfg.layers > 0x800000ULL)
        fail("model logical address capacity");
    cortex_checkpoint_t checkpoint;
    memset(&checkpoint, 0, sizeof checkpoint);
    uint64_t saved = sys(40, CORTEX_CHECKPOINT, 0);
    uint64_t position = 0;
    unsigned token = 1;
    if (saved != ~0ULL) {
        memcpy(&checkpoint, (void *)saved, sizeof checkpoint);
        (void)sys(41, saved, 0);
        if (sys(49, 0, 0) == 0) {
            cortex_checkpoint_t approved;
            memcpy(&approved, (void *)0x80003000, sizeof approved);
            if (approved.magic == CORTEX_MAGIC)
                checkpoint = approved;
        }
        if (checkpoint.magic != CORTEX_MAGIC ||
            crypto_verify32(checkpoint.model_hash, model_hash) || checkpoint.dim != cfg.dim ||
            checkpoint.context != cfg.context || checkpoint.position > cfg.context)
            fail("checkpoint model binding");
        uint8_t digest[32];
        hash_kv(checkpoint.position, digest);
        if (crypto_verify32(digest, checkpoint.kv_hash))
            fail("persisted KV hash mismatch");
        position = checkpoint.position;
        token = (unsigned)checkpoint.token;
        log_line("CORTEX restored SLS KV-cache hash PASS");
    }
    uint64_t end = data[30] ? data[30] : position + steps;
    if (end < position)
        end = position;
    if (end > cfg.context)
        end = cfg.context;
    for (; position < end; position++) {
        if (position < prompt_size)
            memcpy(&token, header + 64 + 4 * position, 4);
        token = forward(token, position);
    }
    bool computed = accepted_tokens != 0;
    checkpoint.magic = CORTEX_MAGIC;
    if (computed)
        checkpoint.generation++;
    checkpoint.position = position;
    checkpoint.token = token;
    checkpoint.model_bytes = file_bytes;
    checkpoint.kv_pages = layer_pages() * cfg.layers;
    checkpoint.logical_bytes = checkpoint.kv_pages * 4096;
    checkpoint.dim = cfg.dim;
    checkpoint.hidden = cfg.hidden;
    checkpoint.layers = cfg.layers;
    checkpoint.heads = cfg.heads;
    checkpoint.kv_heads = cfg.kv_heads;
    checkpoint.vocab = cfg.vocab;
    checkpoint.context = cfg.context;
    checkpoint.flags = simd ? 1 : 0;
    memcpy(checkpoint.model_hash, model_hash, 32);
    hash_kv(position, checkpoint.kv_hash);
    if (computed)
        crypto_blake2b(checkpoint.logit_hash, 32, (uint8_t *)logits, cfg.vocab * sizeof(float));
    checkpoint.rejected += rejected_tokens;
    checkpoint.accepted += accepted_tokens;
    native_synthesis(&checkpoint);
    uint32_t stress;
    memcpy(&stress, header + 1000, 4);
    pager_scale_test(stress);
    unmap();
    /* The kernel commits dirty KV pages and this descriptor in one CoW root.
     * Cluster publication later signs its content in the lineage manifest. */
    if (computed) {
        memset((void *)0x80003000, 0, 4096);
        memcpy((void *)0x80003000, logits, (cfg.vocab < 1024 ? cfg.vocab : 1024) * sizeof(float));
        if (sys(47, 0, 0))
            fail("logit checkpoint staging");
    }
    memcpy((void *)0x80003000, &checkpoint, sizeof checkpoint);
    if (sys(42, 0, 0))
        fail("cortex SLS checkpoint transaction");
    log_line("CORTEX GGUF inference + PagedAttention + symbolic pruning + native JIT PASS");
    (void)sys(45, 0, 0);
    fail("cortex exit returned");
}
#endif
