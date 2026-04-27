/* ================================================================
 *  Systrix OS — libc/systrix_libc.h
 *  Unified C library — usable in BOTH kernel and user space.
 *
 *  Rules for this header:
 *   - NO syscall wrappers (those live in user/libc.h only)
 *   - NO freestanding-incompatible headers
 *   - Compiles with:  -ffreestanding -nostdlib -nostdinc
 *   - Compiles with:  plain gcc (for user programs)
 *
 *  Usage (kernel):   #include "libc/systrix_libc.h"  (from root)
 *  Usage (user):     already pulled in by user/libc.h
 * ================================================================ */

#pragma once

/* ── va_list (compiler built-in, always available) ─────────────── */
typedef __builtin_va_list va_list;
#define va_start(v,l)  __builtin_va_start(v,l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v,l)    __builtin_va_arg(v,l)

/* ── Basic types ─────────────────────────────────────────────────
 * Use the kernel's short aliases (u8/u16/u32/u64) when building
 * inside the kernel; expose the full stdint-compatible names for
 * user code.  Both sets are always defined here so code that only
 * includes this header compiles either way.
 * ────────────────────────────────────────────────────────────── */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed   char       int8_t;
typedef signed   short      int16_t;
typedef signed   int        int32_t;
typedef signed   long long  int64_t;
typedef unsigned long long  size_t;
typedef signed   long long  ssize_t;
typedef unsigned long long  uintptr_t;
typedef signed   long long  intptr_t;
typedef signed   long long  ptrdiff_t;

/* Short aliases used throughout the kernel */
#ifndef _SYSTRIX_KERNEL_TYPES_DEFINED
#define _SYSTRIX_KERNEL_TYPES_DEFINED
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int64_t   i64;
typedef size_t    usize;
#endif

#define NULL    ((void*)0)
#define EOF     (-1)
#define true    1
#define false   0

/* ── Limits ──────────────────────────────────────────────────────── */
#define INT8_MIN    (-128)
#define INT8_MAX    (127)
#define UINT8_MAX   (255U)
#define INT16_MIN   (-32768)
#define INT16_MAX   (32767)
#define UINT16_MAX  (65535U)
#define INT32_MIN   (-2147483648)
#define INT32_MAX   (2147483647)
#define UINT32_MAX  (4294967295U)
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   (9223372036854775807LL)
#define UINT64_MAX  (18446744073709551615ULL)
#define INT_MIN     INT32_MIN
#define INT_MAX     INT32_MAX
#define UINT_MAX    UINT32_MAX
#define LONG_MAX    INT64_MAX
#define LONG_MIN    INT64_MIN
#define SIZE_MAX    UINT64_MAX

/* ── Utility macros ──────────────────────────────────────────────── */
#define ALIGN_UP(v, a)     (((v) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(v, a)   ((v) & ~((a) - 1))
#define BIT(n)             (1UL << (n))
#define IS_POWER_OF_2(n)   ((n) && !((n) & ((n) - 1)))
#define UNUSED(x)          ((void)(x))
#define ARRAY_SIZE(a)      (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)          ((a) < (b) ? (a) : (b))
#define MAX(a, b)          ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi)   ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* ================================================================
 *  Function declarations
 *  All implemented in libc/systrix_libc.c
 * ================================================================ */

/* ── Memory ──────────────────────────────────────────────────────── */
void  *memset (void *dst, int c, size_t n);
void  *memcpy (void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp (const void *a, const void *b, size_t n);
void  *memchr (const void *s, int c, size_t n);

/* ── String ──────────────────────────────────────────────────────── */
size_t  strlen (const char *s);
size_t  strnlen(const char *s, size_t maxlen);
int     strcmp (const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
int     strcasecmp (const char *a, const char *b);
int     strncasecmp(const char *a, const char *b, size_t n);
char   *strcpy (char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
size_t  strlcpy(char *dst, const char *src, size_t sz);   /* always NUL-terminates */
char   *strcat (char *dst, const char *src);
char   *strncat(char *dst, const char *src, size_t n);
size_t  strlcat(char *dst, const char *src, size_t sz);   /* always NUL-terminates */
char   *strchr (const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr (const char *haystack, const char *needle);
char   *strpbrk(const char *s, const char *accept);
size_t  strspn (const char *s, const char *accept);
size_t  strcspn(const char *s, const char *reject);
char   *strtok (char *s, const char *delim);               /* NOT re-entrant */
char   *strtok_r(char *s, const char *delim, char **saveptr);

/* ── Character classification ────────────────────────────────────── */
int isdigit (int c);
int isxdigit(int c);
int isalpha (int c);
int isalnum (int c);
int isspace (int c);
int isupper (int c);
int islower (int c);
int isprint (int c);
int ispunct (int c);
int iscntrl (int c);
int toupper (int c);
int tolower (int c);

/* ── Integer conversion ──────────────────────────────────────────── */
int           atoi  (const char *s);
long          atol  (const char *s);
long long     atoll (const char *s);
long          strtol  (const char *s, char **endptr, int base);
long long     strtoll (const char *s, char **endptr, int base);
unsigned long strtoul (const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);

/* ── Numeric helpers ─────────────────────────────────────────────── */
int      abs (int x);
long     labs(long x);
long long llabs(long long x);

/* ── Formatted output (no FP — %f/%e/%g not supported) ──────────── */
/*
 * Supported specifiers:
 *   %d  %i  %u  %x  %X  %o  %c  %s  %p  %%
 *   %ld %lu %lx  %lld %llu %llx
 *   Width (%5d), precision (%.3s), flags (%-10s, %08x, %+d)
 */
int snprintf (char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int sprintf  (char *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vsprintf (char *buf, const char *fmt, va_list ap);

/*
 * slibc_printf_cb — generic "printf with a callback per character"
 * Used internally; exposed so the kernel's kprintf can reuse it
 * without a buffer.  cb(ctx, c) is called once per output character.
 */
typedef void (*slibc_putc_fn)(void *ctx, char c);
int slibc_vprintf_cb(slibc_putc_fn cb, void *ctx,
                     const char *fmt, va_list ap);

/* ── Integer-to-string helpers ───────────────────────────────────── */
/* Write decimal / hex digits of v into buf (must be ≥20 / ≥17 bytes).
 * Returns number of characters written (not including NUL). */
int slibc_u64_to_dec(uint64_t v, char *buf);
int slibc_u64_to_hex(uint64_t v, char *buf);  /* lowercase */
int slibc_u64_to_HEX(uint64_t v, char *buf);  /* uppercase */

/* ── Sorting / searching ─────────────────────────────────────────── */
void   qsort (void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *));
void  *bsearch(const void *key, const void *base,
               size_t nmemb, size_t size,
               int (*cmp)(const void *, const void *));

/* ── setjmp / longjmp ────────────────────────────────────────────── */
/*
 * jmp_buf layout (8 × 8 bytes = 64 bytes, x86-64):
 *   [0] rbx  [1] rbp  [2] r12  [3] r13  [4] r14  [5] r15
 *   [6] rsp  [7] rip  (return address saved by setjmp call site)
 */
typedef unsigned long long jmp_buf[8];

static inline int setjmp(jmp_buf env) {
    int r;
    __asm__ volatile(
        "mov  %%rbx,    (%1)\n\t"
        "mov  %%rbp,   8(%1)\n\t"
        "mov  %%r12,  16(%1)\n\t"
        "mov  %%r13,  24(%1)\n\t"
        "mov  %%r14,  32(%1)\n\t"
        "mov  %%r15,  40(%1)\n\t"
        "lea  8(%%rsp), %%rax\n\t"
        "mov  %%rax,  48(%1)\n\t"
        "mov  (%%rsp), %%rax\n\t"
        "mov  %%rax,  56(%1)\n\t"
        "xor  %0, %0\n\t"
        : "=r"(r) : "r"(env) : "rax", "memory");
    return r;
}

static inline void longjmp(jmp_buf env, int val)
    __attribute__((noreturn));
static inline void longjmp(jmp_buf env, int val) {
    if (val == 0) val = 1;
    __asm__ volatile(
        "mov    (%0), %%rbx\n\t"
        "mov   8(%0), %%rbp\n\t"
        "mov  16(%0), %%r12\n\t"
        "mov  24(%0), %%r13\n\t"
        "mov  32(%0), %%r14\n\t"
        "mov  40(%0), %%r15\n\t"
        "mov  48(%0), %%rsp\n\t"
        "mov  56(%0), %%rax\n\t"
        "mov  %1,     %%edi\n\t"
        "jmp  *%%rax\n\t"
        : : "r"(env), "r"(val) :);
    __builtin_unreachable();
}

/* ── Seek constants (used by FILE* layer in user/libc.h) ────────── */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* ================================================================
 *  Integer math & bit manipulation
 * ================================================================ */

/* Power / log */
uint64_t slibc_pow_u64(uint64_t base, uint32_t exp);
int64_t  slibc_pow_i64(int64_t  base, uint32_t exp);
int      slibc_log2_u64(uint64_t v);    /* floor(log2(v)), -1 if v==0 */
int      slibc_log10_u64(uint64_t v);   /* floor(log10(v)), -1 if v==0 */

/* Rounding */
uint64_t slibc_round_up_pow2(uint64_t v);   /* next power of 2 >= v  */
uint64_t slibc_round_down_pow2(uint64_t v); /* largest power of 2 <= v */

/* Bit operations */
int      slibc_popcount(uint64_t v);         /* count set bits            */
int      slibc_clz(uint64_t v);              /* count leading zeros (63..0) */
int      slibc_ctz(uint64_t v);              /* count trailing zeros      */
int      slibc_parity(uint64_t v);           /* 1 if odd number of set bits */
uint64_t slibc_reverse_bits(uint64_t v);     /* bit-reverse a 64-bit word */
uint8_t  slibc_reverse_byte(uint8_t v);      /* bit-reverse a byte        */
uint64_t slibc_rotl64(uint64_t v, int n);    /* rotate left               */
uint64_t slibc_rotr64(uint64_t v, int n);    /* rotate right              */
uint32_t slibc_rotl32(uint32_t v, int n);
uint32_t slibc_rotr32(uint32_t v, int n);

/* Byte-swap (endian conversion) */
uint16_t slibc_bswap16(uint16_t v);
uint32_t slibc_bswap32(uint32_t v);
uint64_t slibc_bswap64(uint64_t v);

/* Host ↔ big/little endian helpers */
#define slibc_htobe16(x)  slibc_bswap16(x)   /* only correct on LE hosts */
#define slibc_htole16(x)  (x)
#define slibc_be16toh(x)  slibc_bswap16(x)
#define slibc_le16toh(x)  (x)
#define slibc_htobe32(x)  slibc_bswap32(x)
#define slibc_htole32(x)  (x)
#define slibc_be32toh(x)  slibc_bswap32(x)
#define slibc_le32toh(x)  (x)
#define slibc_htobe64(x)  slibc_bswap64(x)
#define slibc_htole64(x)  (x)
#define slibc_be64toh(x)  slibc_bswap64(x)
#define slibc_le64toh(x)  (x)

/* Overflow-safe arithmetic (return 1 on overflow, 0 on ok) */
int slibc_add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out);
int slibc_mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *out);
int slibc_add_overflow_i64(int64_t  a, int64_t  b, int64_t  *out);
int slibc_mul_overflow_i64(int64_t  a, int64_t  b, int64_t  *out);

/* GCD / LCM */
uint64_t slibc_gcd(uint64_t a, uint64_t b);
uint64_t slibc_lcm(uint64_t a, uint64_t b);  /* returns 0 on overflow */

/* Saturating arithmetic */
uint64_t slibc_sat_add_u64(uint64_t a, uint64_t b); /* clamp at UINT64_MAX */
uint64_t slibc_sat_sub_u64(uint64_t a, uint64_t b); /* clamp at 0          */
int64_t  slibc_sat_add_i64(int64_t  a, int64_t  b);
int64_t  slibc_sat_sub_i64(int64_t  a, int64_t  b);

/* Division helpers */
int64_t  slibc_div_round_up(int64_t n, int64_t d);   /* ceil(n/d) for d>0 */
uint64_t slibc_udiv_round_up(uint64_t n, uint64_t d);

/* ================================================================
 *  String extras
 * ================================================================ */

/* Duplication (caller frees with the kernel allocator of their choice) */
/* NOTE: these call malloc() — only safe in user space unless the
 * kernel supplies a malloc underneath.                             */
char *slibc_strdup (const char *s);
char *slibc_strndup(const char *s, size_t n);

/* In-place transformation */
void  slibc_strupr(char *s);          /* ASCII uppercase in-place    */
void  slibc_strlwr(char *s);          /* ASCII lowercase in-place    */
void  slibc_strrev(char *s);          /* reverse string in-place     */
void  slibc_strtrim(char *s);         /* strip leading+trailing space */
char *slibc_ltrim(char *s);           /* return ptr past leading space */
void  slibc_rtrim(char *s);           /* strip trailing space in-place */

/* Predicates */
int   slibc_str_starts_with(const char *s, const char *prefix);
int   slibc_str_ends_with  (const char *s, const char *suffix);
int   slibc_str_is_empty   (const char *s);  /* NULL or "" */
int   slibc_str_is_int     (const char *s);  /* valid signed decimal integer */
int   slibc_str_is_uint    (const char *s);  /* valid unsigned decimal integer */

/* Count occurrences of substring */
size_t slibc_str_count(const char *haystack, const char *needle);

/* Replace first / all occurrences; writes into buf of size bufsz */
int slibc_str_replace(const char *src, const char *from,
                      const char *to, char *buf, size_t bufsz);

/* Split string by delimiter into out_parts[]; returns part count.
 * Modifies 'src' in-place (inserts NULs).  max_parts is array size. */
int slibc_str_split(char *src, char delim,
                    char **out_parts, int max_parts);

/* Join array of strings with separator into buf */
int slibc_str_join(const char **parts, int count,
                   char sep, char *buf, size_t bufsz);

/* Hex encode / decode */
void  slibc_hex_encode(const uint8_t *src, size_t srclen,
                        char *out);               /* out must be 2*srclen+1 */
int   slibc_hex_decode(const char *hex, uint8_t *out, size_t outlen);
                                                   /* returns bytes written, -1 on bad input */

/* ================================================================
 *  Hashing
 * ================================================================ */

/* FNV-1a — fast, non-cryptographic */
uint32_t slibc_fnv1a32(const void *data, size_t len);
uint64_t slibc_fnv1a64(const void *data, size_t len);

/* DJB2 — simple string hash */
uint32_t slibc_djb2(const char *s);

/* MurmurHash3 (32-bit output) */
uint32_t slibc_murmur3_32(const void *data, size_t len, uint32_t seed);

/* CRC-32 (ISO 3309 / Ethernet polynomial 0xEDB88320) */
uint32_t slibc_crc32(const void *data, size_t len);
uint32_t slibc_crc32_update(uint32_t crc, const void *data, size_t len);

/* Adler-32 (zlib compatible) */
uint32_t slibc_adler32(const void *data, size_t len);
uint32_t slibc_adler32_update(uint32_t adler, const void *data, size_t len);

/* ================================================================
 *  Pseudo-random number generation  (xorshift64 + splitmix64)
 * ================================================================ */

typedef struct { uint64_t state; } SRng;

void     slibc_rng_seed(SRng *r, uint64_t seed);
uint64_t slibc_rng_next(SRng *r);                  /* raw 64-bit value   */
uint32_t slibc_rng_u32(SRng *r);                   /* uniform 32-bit     */
uint64_t slibc_rng_range(SRng *r, uint64_t lo, uint64_t hi); /* [lo, hi) */
void     slibc_rng_fill(SRng *r, void *buf, size_t n);       /* random bytes */
void     slibc_rng_shuffle(SRng *r, void *base,
                            size_t nmemb, size_t sz);         /* Fisher-Yates  */

/* Global convenience RNG (not thread-safe) */
void     slibc_srand(uint64_t seed);
uint64_t slibc_rand(void);
uint64_t slibc_rand_range(uint64_t lo, uint64_t hi);

/* ================================================================
 *  Ring buffer  (single-producer / single-consumer, lock-free on x86)
 * ================================================================ */

#define SLIBC_RING_MAX_POW2  (1u << 24)  /* 16M entries max */

typedef struct {
    uint8_t  *buf;      /* storage (must be elem_size * capacity bytes) */
    uint32_t  cap;      /* capacity — must be a power of 2              */
    uint32_t  esize;    /* element size in bytes                        */
    uint32_t  head;     /* producer index (mod cap)                     */
    uint32_t  tail;     /* consumer index (mod cap)                     */
} SRing;

/* init: buf must already be allocated; cap must be a power of 2 */
void slibc_ring_init (SRing *r, void *buf, uint32_t cap, uint32_t elem_size);
int  slibc_ring_push (SRing *r, const void *elem);  /* 0=ok, -1=full  */
int  slibc_ring_pop  (SRing *r, void *elem);         /* 0=ok, -1=empty */
int  slibc_ring_peek (const SRing *r, void *elem);   /* pop without consuming */
void slibc_ring_clear(SRing *r);
static inline int  slibc_ring_empty(const SRing *r) { return r->head == r->tail; }
static inline int  slibc_ring_full (const SRing *r) { return (r->head - r->tail) == r->cap; }
static inline uint32_t slibc_ring_count(const SRing *r) { return r->head - r->tail; }

/* ================================================================
 *  Bitmap  (compact bit array)
 * ================================================================ */

typedef struct {
    uint64_t *words;
    size_t    nbits;
} SBitmap;

/* init: words must point to an array of ceil(nbits/64) uint64_t */
void slibc_bm_init (SBitmap *bm, uint64_t *words, size_t nbits);
void slibc_bm_set  (SBitmap *bm, size_t bit);
void slibc_bm_clear(SBitmap *bm, size_t bit);
int  slibc_bm_test (const SBitmap *bm, size_t bit);
void slibc_bm_toggle(SBitmap *bm, size_t bit);
void slibc_bm_zero (SBitmap *bm);           /* clear all bits */
void slibc_bm_fill (SBitmap *bm);           /* set all bits   */
/* Find first set/clear bit at or after 'start'; returns nbits if none */
size_t slibc_bm_first_set  (const SBitmap *bm, size_t start);
size_t slibc_bm_first_clear(const SBitmap *bm, size_t start);
size_t slibc_bm_count_set  (const SBitmap *bm);

/* ================================================================
 *  Fixed-width formatting helpers (no heap, stack-only)
 * ================================================================ */

/* Format a byte count into a human-readable string: "1.23 MB"
 * buf must be at least 16 bytes. */
void slibc_fmt_bytes(uint64_t bytes, char *buf, size_t bufsz);

/* Format a duration in milliseconds: "2h 03m 47s" or "347ms"
 * buf must be at least 24 bytes. */
void slibc_fmt_duration_ms(uint64_t ms, char *buf, size_t bufsz);

/* Zero-pad an integer to a fixed width into buf:  slibc_fmt_zpad(42,5) -> "00042" */
void slibc_fmt_zpad(uint64_t v, int width, char *buf);

/* Format IPv4 address from 32-bit big-endian word: "192.168.1.1" */
void slibc_fmt_ipv4(uint32_t ip_be, char *buf);   /* buf >= 16 */

/* Format MAC address from 6-byte array: "aa:bb:cc:dd:ee:ff" */
void slibc_fmt_mac(const uint8_t mac[6], char *buf); /* buf >= 18 */

/* Parse IPv4 dotted-decimal; returns 0 on success, -1 on error */
int  slibc_parse_ipv4(const char *s, uint32_t *out_be);

/* ================================================================
 *  Checksum / parity
 * ================================================================ */

/* Internet checksum (RFC 1071) — used by IP, TCP, UDP headers */
uint16_t slibc_inet_checksum(const void *data, size_t len);
uint16_t slibc_inet_checksum_update(uint16_t acc,
                                     const void *data, size_t len);

/* XOR checksum (simple) */
uint8_t slibc_xor_checksum(const void *data, size_t len);

/* ================================================================
 *  Sorting extras
 * ================================================================ */

/* Stable sort (merge sort — O(n log n), uses stack scratch for small arrays) */
void slibc_msort(void *base, size_t nmemb, size_t sz,
                 int (*cmp)(const void *, const void *));

/* Insertion sort (best for n<16, in-place, stable) */
void slibc_isort(void *base, size_t nmemb, size_t sz,
                 int (*cmp)(const void *, const void *));

/* ================================================================
 *  String ↔ number pretty-printers
 * ================================================================ */

/* Print signed/unsigned integer into a stack buffer and return pointer.
 * Buffer must be at least 22 bytes.  Convenience wrappers: */
char *slibc_itoa (int64_t  v, char *buf);  /* decimal */
char *slibc_utoa (uint64_t v, char *buf);  /* decimal */
char *slibc_xtoa (uint64_t v, char *buf);  /* lowercase hex, no prefix */
char *slibc_Xtoa (uint64_t v, char *buf);  /* uppercase hex, no prefix */
char *slibc_otoa (uint64_t v, char *buf);  /* octal */
char *slibc_btoa (uint64_t v, char *buf);  /* binary (buf >= 65 bytes) */

/* ================================================================
 *  Memory pool allocator  (fixed-size slab, no heap fragmentation)
 * ================================================================ */

typedef struct SPool {
    uint8_t  *mem;       /* backing storage                           */
    size_t    obj_size;  /* size of each object (padded to alignment) */
    size_t    capacity;  /* total number of objects                   */
    size_t    free_head; /* index of first free slot (SIZE_MAX = full) */
} SPool;

/* init: mem must be obj_size*capacity bytes, aligned to 8 */
void  slibc_pool_init(SPool *p, void *mem,
                      size_t obj_size, size_t capacity);
void *slibc_pool_alloc(SPool *p);           /* NULL if full     */
void  slibc_pool_free (SPool *p, void *obj);
void  slibc_pool_reset(SPool *p);           /* free all objects */
static inline size_t slibc_pool_used(const SPool *p) {
    size_t free_count = 0;
    size_t i = p->free_head;
    while (i != SIZE_MAX && free_count <= p->capacity) {
        i = *(size_t *)(p->mem + i * p->obj_size);
        free_count++;
    }
    return p->capacity - free_count;
}

/* ================================================================
 *  Simple key=value config parser
 * ================================================================ */

#define SLIBC_CFG_MAX_KEY  64
#define SLIBC_CFG_MAX_VAL  256
#define SLIBC_CFG_MAX_PAIRS 128

typedef struct {
    char key[SLIBC_CFG_MAX_KEY];
    char val[SLIBC_CFG_MAX_VAL];
} SCfgPair;

typedef struct {
    SCfgPair pairs[SLIBC_CFG_MAX_PAIRS];
    int      count;
} SCfg;

/* Parse "key=value\n" text (comments with '#' supported) */
int         slibc_cfg_parse (SCfg *cfg, const char *text);
const char *slibc_cfg_get   (const SCfg *cfg, const char *key);
const char *slibc_cfg_get_or(const SCfg *cfg, const char *key,
                              const char *fallback);
int         slibc_cfg_get_int(const SCfg *cfg, const char *key,
                               int fallback);

/* ================================================================
 *  Base64 encode / decode
 * ================================================================ */

/* Returns number of bytes written.  out must be >= (len+2)/3*4 + 1 */
size_t slibc_base64_encode(const uint8_t *src, size_t len, char *out);

/* Returns decoded byte count, or -1 on bad input.
 * out must be >= len*3/4 bytes. */
ssize_t slibc_base64_decode(const char *src, size_t len, uint8_t *out);

/* ================================================================
 *  UUID (version 4, random)
 * ================================================================ */

typedef struct { uint8_t b[16]; } SUUID;

void slibc_uuid4   (SRng *rng, SUUID *out);
void slibc_uuid_str(const SUUID *u, char buf[37]); /* "xxxxxxxx-xxxx-..." */

/* ================================================================
 *  Compile-time / debug utilities
 * ================================================================ */

/* Static assert — works in C11 and as a statement in C99 via sizeof trick */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define SLIBC_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#  define SLIBC_STATIC_ASSERT(cond, msg) \
       typedef char _slibc_sa_##__LINE__[(cond) ? 1 : -1]
#endif

/* container_of — get outer struct from member pointer */
#define slibc_container_of(ptr, type, member) \
    ((type *)((uint8_t *)(ptr) - __builtin_offsetof(type, member)))

/* Likely / unlikely branch hints */
#define slibc_likely(x)   __builtin_expect(!!(x), 1)
#define slibc_unlikely(x) __builtin_expect(!!(x), 0)

/* Compiler memory barrier (prevents reordering across this point) */
#define slibc_barrier() __asm__ volatile("" ::: "memory")

/* Unreachable marker */
#define slibc_unreachable() __builtin_unreachable()

/* ================================================================
 *  Linked-list macros  (intrusive doubly-linked list)
 *
 *  Embed an SListNode in your struct, then use macros to link/walk.
 *  Example:
 *    typedef struct { int val; SListNode node; } MyItem;
 *    SListNode head; slibc_list_init(&head);
 *    slibc_list_push_back(&head, &item.node);
 *    SListNode *n;
 *    slibc_list_foreach(n, &head) {
 *        MyItem *it = slibc_container_of(n, MyItem, node);
 *    }
 * ================================================================ */

typedef struct SListNode {
    struct SListNode *prev;
    struct SListNode *next;
} SListNode;

static inline void slibc_list_init(SListNode *head) {
    head->prev = head; head->next = head;
}
static inline int slibc_list_empty(const SListNode *head) {
    return head->next == head;
}
static inline void _slibc_list_insert(SListNode *prev, SListNode *next,
                                       SListNode *node) {
    node->prev = prev; node->next = next;
    prev->next = node; next->prev = node;
}
static inline void slibc_list_push_back(SListNode *head, SListNode *node) {
    _slibc_list_insert(head->prev, head, node);
}
static inline void slibc_list_push_front(SListNode *head, SListNode *node) {
    _slibc_list_insert(head, head->next, node);
}
static inline void slibc_list_remove(SListNode *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = node;
}
static inline SListNode *slibc_list_pop_front(SListNode *head) {
    if (slibc_list_empty(head)) return NULL;
    SListNode *n = head->next; slibc_list_remove(n); return n;
}
static inline SListNode *slibc_list_pop_back(SListNode *head) {
    if (slibc_list_empty(head)) return NULL;
    SListNode *n = head->prev; slibc_list_remove(n); return n;
}
static inline size_t slibc_list_count(const SListNode *head) {
    size_t n = 0;
    const SListNode *c = head->next;
    while (c != head) { n++; c = c->next; }
    return n;
}
#define slibc_list_foreach(node, head) \
    for (SListNode *(node) = (head)->next; \
         (node) != (head); \
         (node) = (node)->next)
#define slibc_list_foreach_safe(node, tmp, head) \
    for (SListNode *(node) = (head)->next, \
                   *(tmp)  = (node)->next; \
         (node) != (head); \
         (node) = (tmp), (tmp) = (tmp)->next)

/* ================================================================
 *  Error codes  (POSIX-compatible, negative, matching kernel.h)
 *
 *  Centralised here so user-space code that only includes
 *  systrix_libc.h still has the full errno vocabulary.
 *  The kernel defines these too — the guard prevents redefinition.
 * ================================================================ */

#ifndef _SYSTRIX_ERRNO_DEFINED
#define _SYSTRIX_ERRNO_DEFINED

#define EPERM          (-1LL)   /* Operation not permitted         */
#define ENOENT         (-2LL)   /* No such file or directory       */
#define ESRCH          (-3LL)   /* No such process                 */
#define EINTR          (-4LL)   /* Interrupted system call         */
#define EIO            (-5LL)   /* I/O error                       */
#define EBADF          (-9LL)   /* Bad file descriptor             */
#define ECHILD         (-10LL)  /* No child processes              */
#define EAGAIN         (-11LL)  /* Try again (resource unavailable)*/
#define EWOULDBLOCK    (-11LL)  /* Same as EAGAIN                  */
#define ENOMEM         (-12LL)  /* Out of memory                   */
#define EACCES         (-13LL)  /* Permission denied               */
#define EFAULT         (-14LL)  /* Bad address                     */
#define EBUSY          (-16LL)  /* Device or resource busy         */
#define EEXIST         (-17LL)  /* File exists                     */
#define ENODEV         (-19LL)  /* No such device                  */
#define ENOTDIR        (-20LL)  /* Not a directory                 */
#define EISDIR         (-21LL)  /* Is a directory                  */
#define EINVAL         (-22LL)  /* Invalid argument                */
#define ENFILE         (-23LL)  /* File table overflow             */
#define EMFILE         (-24LL)  /* Too many open files             */
#define ENOSPC         (-28LL)  /* No space left on device         */
#define EROFS          (-30LL)  /* Read-only file system           */
#define EPIPE          (-32LL)  /* Broken pipe                     */
#define ERANGE         (-34LL)  /* Math result out of range        */
#define ENAMETOOLONG   (-36LL)  /* File name too long              */
#define ENOSYS         (-38LL)  /* Function not implemented        */
#define ENOTEMPTY      (-39LL)  /* Directory not empty             */
#define EOVERFLOW      (-75LL)  /* Value too large for data type   */
#define EOPNOTSUPP     (-95LL)  /* Operation not supported         */
#define ETIMEDOUT      (-110LL) /* Connection timed out            */
#define ECONNREFUSED   (-111LL) /* Connection refused              */
#define ENETUNREACH    (-101LL) /* Network unreachable             */
#define EISCONN        (-106LL) /* Transport endpoint connected    */
#define ENOTCONN       (-107LL) /* Transport not connected         */
#define EALREADY       (-114LL) /* Operation already in progress   */
#define EINPROGRESS    (-115LL) /* Operation now in progress       */

#endif /* _SYSTRIX_ERRNO_DEFINED */

/* Convert a negative Systrix error code to a human-readable string.
 * Returns a pointer to a string literal — do NOT free or modify it.
 * Unknown codes return "Unknown error". */
const char *strerror(int errnum);

/* ================================================================
 *  Path utilities  (no heap — all work on caller-supplied buffers)
 *
 *  All functions treat '/' as the path separator and handle:
 *    - Absolute paths (/foo/bar)
 *    - Relative paths (foo/bar)
 *    - Trailing slashes (/foo/bar/)
 *    - Double slashes   (/foo//bar)
 *    - Dot components  (/foo/./bar  →  /foo/bar)
 *    - Dot-dot collapse (/foo/bar/../baz  →  /foo/baz)
 * ================================================================ */

/* Join base_path + "/" + name into dst (size dst_sz).
 * If name is absolute it replaces base_path entirely.
 * Always NUL-terminates dst.  Returns 0 on success, -1 if truncated. */
int  slibc_path_join(char *dst, size_t dst_sz,
                     const char *base, const char *name);

/* In-place normalise: collapse "//" → "/", remove "." components,
 * collapse ".." components.  Modifies path in place.
 * Returns path for convenience. */
char *slibc_path_normalize(char *path);

/* Return pointer to the last component of path (no allocation).
 * "/foo/bar"  → "bar"
 * "/foo/bar/" → "bar"   (trailing slash ignored)
 * "/"         → "/"
 * "foo"       → "foo"   */
const char *slibc_path_basename(const char *path);

/* Copy the directory part of path into dst (size dst_sz).
 * "/foo/bar"  → "/foo"
 * "/foo/bar/" → "/foo"
 * "/foo"      → "/"
 * "foo"       → "."
 * Returns 0 on success, -1 if truncated. */
int  slibc_path_dirname(const char *path, char *dst, size_t dst_sz);

/* Return 1 if path is absolute (starts with '/'), else 0. */
static inline int slibc_path_is_absolute(const char *path) {
    return path && path[0] == '/';
}

/* Return 1 if the path has a given file extension (case-insensitive).
 * slibc_path_has_ext("foo.TXT", "txt")  → 1 */
int slibc_path_has_ext(const char *path, const char *ext);

/* ================================================================
 *  String padding / column formatting  (no heap)
 * ================================================================ */

/* Write 'count' copies of character 'c' into buf, NUL-terminate.
 * buf must be at least count+1 bytes.  Returns count. */
size_t slibc_str_repeat(char *buf, char c, size_t count);

/* Left-pad or right-pad str to width characters using pad_char into dst.
 * dst must be at least width+1 bytes.
 * left_align=0 → right-align (leading pad); left_align≠0 → trailing pad.
 * Strings longer than width are copied truncated.
 * Returns number of characters written (≤ width). */
size_t slibc_str_pad(char *dst, size_t dst_sz,
                     const char *str, size_t width,
                     char pad_char, int left_align);

/* Write a text-mode "progress bar" of total_width chars into buf:
 * "[=====>   ]"  where filled = total_width*numerator/denominator.
 * buf must be ≥ total_width+3 bytes.  */
void slibc_fmt_progress(char *buf, size_t total_width,
                        uint64_t numerator, uint64_t denominator);

/* ================================================================
 *  Simple dynamic string builder  (grows via realloc — user space)
 * ================================================================ */

/* NOTE: SStrBuf uses malloc/free — kernel code must supply those.  */

typedef struct {
    char   *data;    /* NUL-terminated string                  */
    size_t  len;     /* current length (not including NUL)     */
    size_t  cap;     /* allocated capacity (including NUL slot)*/
} SStrBuf;

/* Initialise an empty builder (call slibc_sb_free when done). */
void slibc_sb_init   (SStrBuf *sb);
void slibc_sb_free   (SStrBuf *sb);

/* Append operations — return 0 on success, -1 on alloc failure. */
int  slibc_sb_append (SStrBuf *sb, const char *s);
int  slibc_sb_appendn(SStrBuf *sb, const char *s, size_t n);
int  slibc_sb_appendc(SStrBuf *sb, char c);
int  slibc_sb_appendf(SStrBuf *sb, const char *fmt, ...)
         __attribute__((format(printf, 2, 3)));

/* Reset to empty without freeing storage. */
void slibc_sb_reset  (SStrBuf *sb);

/* Steal the internal buffer (caller owns it; sb is reset). */
char *slibc_sb_steal (SStrBuf *sb);
