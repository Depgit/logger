#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include "zlog.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASYNC_LOG_QUEUE_SIZE  100
#define ASYNC_LOG_MSG_MAX     4096

/* ---- NanoLog-style encoded argument buffer ------------------------- */
/* Fixed-size byte blob. Args are copied in as raw binary (like NanoLog's
 * compressed log records). 128 bytes fits ~8-16 args, which is far
 * more than typical log calls use. */
#define NANO_ARG_BUF_SIZE     128

typedef enum {
    NANO_TYPE_END    = 0,
    NANO_TYPE_INT    = 1,   /* int32_t    */
    NANO_TYPE_LONG   = 2,   /* int64_t    */
    NANO_TYPE_UINT   = 3,   /* uint32_t   */
    NANO_TYPE_ULONG  = 4,   /* uint64_t   */
    NANO_TYPE_DOUBLE = 5,
    NANO_TYPE_STR    = 6,   /* length-prefixed char* */
    NANO_TYPE_PTR    = 7    /* void*      */
} nano_arg_type_t;

typedef struct {
    const char  *fmt;                     /* pointer to static literal */
    uint8_t      args[NANO_ARG_BUF_SIZE]; /* packed type-tagged args   */
    uint16_t     args_len;                /* bytes used                */
} nano_payload_t;

/* ---- Ring buffer entry -------------------------------------------- */
/* A single entry carries either a fully-formatted message (legacy path)
 * or a NanoLog-style payload (fast path). */
typedef struct {
    int level;
    int proc;
    int facility;

    /* legacy path */
    char msg[ASYNC_LOG_MSG_MAX];

    /* nano path */
    nano_payload_t nano;
    int is_nano;

    zlog_category_t *target_category;
} async_log_entry_t;

typedef struct {
    async_log_entry_t queue[ASYNC_LOG_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;

    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_t       worker;
    int             running;

    uint64_t written;
    uint64_t dropped;
    uint64_t total_calls;

    zlog_category_t *category;
} async_logger_t;

extern zlog_category_t *g_zlog_category;
extern async_logger_t g_async_logger;

int logger_init(const char *config_file, const char *category);
void logger_shutdown(void);
int logger_reload_filter_config(const char *category);

/* ---- Legacy runtime-formatting API -------------------------------- */
void zlog_write(
    zlog_category_t *category,
    int proc,
    int level,
    int facility,
    const char *fmt,
    ...
) __attribute__((format(printf, 5, 6)));

/* ---- NanoLog-style producer --------------------------------------- */
int nano_log_enqueue(int proc, int level, int facility,
                     const nano_payload_t *payload);

/* Filter check exposed to the macro (defined in logger.c). */
int nano_should_log(int proc, int level);

/* ---- Argument packers (called by ZLOG_NANO macros) ---------------- */
static inline void nano_pack_int(nano_payload_t *p, int32_t v) {
    if (p->args_len + 5 > NANO_ARG_BUF_SIZE) return;
    p->args[p->args_len++] = NANO_TYPE_INT;
    memcpy(&p->args[p->args_len], &v, 4); p->args_len += 4;
}
static inline void nano_pack_long(nano_payload_t *p, int64_t v) {
    if (p->args_len + 9 > NANO_ARG_BUF_SIZE) return;
    p->args[p->args_len++] = NANO_TYPE_LONG;
    memcpy(&p->args[p->args_len], &v, 8); p->args_len += 8;
}
static inline void nano_pack_uint(nano_payload_t *p, uint32_t v) {
    if (p->args_len + 5 > NANO_ARG_BUF_SIZE) return;
    p->args[p->args_len++] = NANO_TYPE_UINT;
    memcpy(&p->args[p->args_len], &v, 4); p->args_len += 4;
}
static inline void nano_pack_ulong(nano_payload_t *p, uint64_t v) {
    if (p->args_len + 9 > NANO_ARG_BUF_SIZE) return;
    p->args[p->args_len++] = NANO_TYPE_ULONG;
    memcpy(&p->args[p->args_len], &v, 8); p->args_len += 8;
}
static inline void nano_pack_double(nano_payload_t *p, double v) {
    if (p->args_len + 9 > NANO_ARG_BUF_SIZE) return;
    p->args[p->args_len++] = NANO_TYPE_DOUBLE;
    memcpy(&p->args[p->args_len], &v, 8); p->args_len += 8;
}
static inline void nano_pack_ptr(nano_payload_t *p, const void *v) {
    if (p->args_len + 9 > NANO_ARG_BUF_SIZE) return;
    p->args[p->args_len++] = NANO_TYPE_PTR;
    memcpy(&p->args[p->args_len], &v, 8); p->args_len += 8;
}
static inline void nano_pack_str(nano_payload_t *p, const char *s) {
    if (!s) s = "(null)";
    size_t len = strlen(s);
    if (len > 255) len = 255;
    if (p->args_len + 3 + len > NANO_ARG_BUF_SIZE) return;
    p->args[p->args_len++] = NANO_TYPE_STR;
    p->args[p->args_len++] = (uint8_t)(len & 0xFF);
    p->args[p->args_len++] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(&p->args[p->args_len], s, len);
    p->args_len = (uint16_t)(p->args_len + len);
}

/* ---- Argument type dispatch --------------------------------------- */
/* C11 _Generic selects the right packer based on the argument's type. */
#define NANO_PACK_ONE(p, x) _Generic((x),          \
        int:                nano_pack_int,         \
        long:               nano_pack_long,        \
        long long:          nano_pack_long,        \
        unsigned int:       nano_pack_uint,        \
        unsigned long:      nano_pack_ulong,       \
        unsigned long long: nano_pack_ulong,       \
        double:             nano_pack_double,      \
        float:              nano_pack_double,      \
        char *:             nano_pack_str,         \
        const char *:       nano_pack_str,         \
        void *:             nano_pack_ptr,         \
        default:            nano_pack_ptr           \
    )((p), (x))

/* Count args using the classic preprocessor trick. */
#define NANO_ARG_N(_1,_2,_3,_4,_5,_6,_7,_8,N,...) N
#define NANO_NARGS(...) \
    NANO_ARG_N(__VA_ARGS__,8,7,6,5,4,3,2,1,0)

/* Pack N args by expanding to N explicit calls. */
#define NANO_PACK_1(p,a)               NANO_PACK_ONE(p,a)
#define NANO_PACK_2(p,a,b)             NANO_PACK_1(p,a);  NANO_PACK_1(p,b)
#define NANO_PACK_3(p,a,b,c)           NANO_PACK_2(p,a,b);NANO_PACK_1(p,c)
#define NANO_PACK_4(p,a,b,c,d)         NANO_PACK_3(p,a,b,c);NANO_PACK_1(p,d)
#define NANO_PACK_5(p,a,b,c,d,e)       NANO_PACK_4(p,a,b,c,d);NANO_PACK_1(p,e)
#define NANO_PACK_6(p,a,b,c,d,e,f)     NANO_PACK_5(p,a,b,c,d,e);NANO_PACK_1(p,f)
#define NANO_PACK_7(p,a,b,c,d,e,f,g)   NANO_PACK_6(p,a,b,c,d,e,f);NANO_PACK_1(p,g)
#define NANO_PACK_8(p,a,b,c,d,e,f,g,h) NANO_PACK_7(p,a,b,c,d,e,f,g);NANO_PACK_1(p,h)

#define NANO_PACK_N_(n,p,...) NANO_PACK_##n(p,__VA_ARGS__)

/* ---- The fast-path macro ------------------------------------------ */
/* Usage:  ZLOG_NANO(proc, level, facility, "value=%d name=%s", 42, name);
 * fmt MUST be a string literal -- its address is what gets stored.      */
#define ZLOG_NANO(proc, level, facility, fmt, ...) do {             \
        if (nano_should_log((proc), (level))) {                      \
            nano_payload_t _np;                                      \
            _np.fmt = (fmt);                                         \
            _np.args_len = 0;                                        \
            NANO_PACK_N_(NANO_NARGS(__VA_ARGS__), &_np, __VA_ARGS__);\
            nano_log_enqueue((proc), (level), (facility), &_np);     \
        }                                                            \
    } while (0)

/* Zero-arg version */
#define ZLOG_NANO0(proc, level, facility, fmt) do {                 \
        if (nano_should_log((proc), (level))) {                      \
            nano_payload_t _np;                                      \
            _np.fmt = (fmt);                                         \
            _np.args_len = 0;                                        \
            nano_log_enqueue((proc), (level), (facility), &_np);     \
        }                                                            \
    } while (0)

/* Keep the old API unchanged. */
#define ZLOG(proc, level, facility, fmt, ...) \
    zlog_write(g_zlog_category, (proc), (level), (facility), (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* LOGGER_H */
