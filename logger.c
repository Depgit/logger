#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>
#include <unistd.h>
#include "logger.h"

zlog_category_t *g_zlog_category = NULL;
async_logger_t   g_async_logger;

/* ------------------------------------------------------------------ */
/* Log-level filtering (application independent)                       */
/*                                                                     */
/* Log.conf format, one "name:value" per line, '#' = comment:          */
/*                                                                     */
/*     <Category>Log:255       category-wide level mask (all procs)    */
/*     <Category><proc>Log:63  optional override for ONE proc number   */
/*                                                                     */
/* Example for category "Billing":                                     */
/*     BillingLog:255          every proc logs everything              */
/*     Billing3Log:0           ...except proc 3, which is silent       */
/*                                                                     */
/* The SAME rule is used for every application. No category name is    */
/* hard-coded anywhere in this file.                                   */
/* ------------------------------------------------------------------ */

#define LOG_FILTER_CONFIG_PATH "Log.conf"

#define MAX_LOG_FILTERS 64
#define FILTER_KEY_MAX  128

#define ACT_LOG_EMERGENCY               1
#define ACT_LOG_ALERT                   2
#define ACT_LOG_CRITICAL                4
#define ACT_LOG_ERROR                   8
#define ACT_LOG_WARNING                 16
#define ACT_LOG_NOTICE                  32
#define ACT_LOG_INFORMATION             64
#define ACT_LOG_DEBUG                   128

typedef struct {
    char name[64];
    int  value;
} log_filter_entry_t;

static log_filter_entry_t g_log_filters[MAX_LOG_FILTERS];
static int g_log_filter_count = 0;

static int g_log_filter_mask = 0;

static char g_category_name[64];

static pthread_mutex_t g_filter_mutex = PTHREAD_MUTEX_INITIALIZER;


/* ------------------------------------------------------------------ */
/* Config loading                                                      */
/* ------------------------------------------------------------------ */

int logger_load_filter_config(const char *path)
{
    FILE *fp;
    char line[256];
    log_filter_entry_t local[MAX_LOG_FILTERS];
    int local_count = 0;

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open log filter config '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *colon;
        char *name;
        size_t len;

        if (local_count >= MAX_LOG_FILTERS) {
            fprintf(stderr, "Log filter table full (%d), ignoring the rest of %s\n",
                    MAX_LOG_FILTERS, path);
            break;
        }

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') {
            continue;
        }

        colon = strchr(p, ':');
        if (!colon) {
            continue;
        }

        *colon = '\0';
        name = p;

        len = strlen(name);
        while (len > 0 && isspace((unsigned char)name[len - 1])) {
            name[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        strncpy(local[local_count].name, name, sizeof(local[0].name) - 1);
        local[local_count].name[sizeof(local[0].name) - 1] = '\0';
        local[local_count].value = atoi(colon + 1);
        local_count++;
    }

    fclose(fp);

    pthread_mutex_lock(&g_filter_mutex);
    memcpy(g_log_filters, local, sizeof(log_filter_entry_t) * (size_t)local_count);
    g_log_filter_count = local_count;
    pthread_mutex_unlock(&g_filter_mutex);

    fprintf(stderr, "Loaded %d log filter entries from %s\n", local_count, path);
    return 0;
}

/* Exact-name lookup. Returns the value, or -1 if not present.
 * CALLER MUST HOLD g_filter_mutex. */
static int find_filter_locked(const char *name)
{
    int i;

    for (i = 0; i < g_log_filter_count; i++) {
        if (strcmp(g_log_filters[i].name, name) == 0) {
            return g_log_filters[i].value;
        }
    }
    return -1;
}

static void refresh_category_mask(void)
{
    char key[FILTER_KEY_MAX];
    int value;

    snprintf(key, sizeof(key), "%sLog", g_category_name);

    pthread_mutex_lock(&g_filter_mutex);
    value = find_filter_locked(key);
    if (value < 0) {
        value = find_filter_locked(g_category_name);
    }
    g_log_filter_mask = (value < 0) ? 0 : value;
    pthread_mutex_unlock(&g_filter_mutex);

    if (value < 0) {
        fprintf(stderr,
                "logger: no entry for category '%s' (tried '%s') in %s, defaulting to 0\n",
                g_category_name, key, LOG_FILTER_CONFIG_PATH);
    } else {
        fprintf(stderr, "logger: category '%s' mask = %d\n", g_category_name, value);
    }
}


/* ------------------------------------------------------------------ */
/* Level handling                                                      */
/* ------------------------------------------------------------------ */

static int severity_bit(int level)
{
    switch (level) {
    case ACT_LOG_EMERGENCY:   return 0;
    case ACT_LOG_ALERT:       return 1;
    case ACT_LOG_CRITICAL:    return 2;
    case ACT_LOG_ERROR:       return 3;
    case ACT_LOG_WARNING:     return 4;
    case ACT_LOG_NOTICE:      return 5;
    case ACT_LOG_INFORMATION: return 6;
    case ACT_LOG_DEBUG:       return 7;
    default:                  return 6;
    }
}

static int should_log(int proc, int level)
{
    char key[FILTER_KEY_MAX];
    int mask;

    snprintf(key, sizeof(key), "%s%dLog", g_category_name, proc);

    pthread_mutex_lock(&g_filter_mutex);
    mask = find_filter_locked(key);
    if (mask < 0) {
        mask = g_log_filter_mask;
    }
    pthread_mutex_unlock(&g_filter_mutex);

    if (mask == 0) {
        return 0;
    }

    if (level == ACT_LOG_EMERGENCY || level == ACT_LOG_CRITICAL) {
        return 1;
    }

    return (mask & (1 << severity_bit(level))) != 0;
}

int nano_should_log(int proc, int level)
{
    return should_log(proc, level);
}

static int convert_log_level(int level)
{
    switch (level) {
    case ACT_LOG_EMERGENCY:   return ZLOG_LEVEL_FATAL;
    case ACT_LOG_ALERT:       return ZLOG_LEVEL_ERROR;
    case ACT_LOG_CRITICAL:    return ZLOG_LEVEL_ERROR;
    case ACT_LOG_ERROR:       return ZLOG_LEVEL_ERROR;
    case ACT_LOG_WARNING:     return ZLOG_LEVEL_WARN;
    case ACT_LOG_NOTICE:      return ZLOG_LEVEL_NOTICE;
    case ACT_LOG_INFORMATION: return ZLOG_LEVEL_INFO;
    case ACT_LOG_DEBUG:       return ZLOG_LEVEL_DEBUG;
    default:                  return ZLOG_LEVEL_INFO;
    }
}


/* ------------------------------------------------------------------ */
/* Async pipeline: producer -> ring buffer -> worker                   */
/* ------------------------------------------------------------------ */

static void write_to_zlog(async_logger_t *logger, const async_log_entry_t *entry)
{
    switch (convert_log_level(entry->level)) {
    case ZLOG_LEVEL_DEBUG:
        zlog_debug(logger->category, "[process no:%d]  %s", entry->proc, entry->msg);
        break;
    case ZLOG_LEVEL_NOTICE:
        zlog_notice(logger->category, "[process no:%d]  %s", entry->proc, entry->msg);
        break;
    case ZLOG_LEVEL_WARN:
        zlog_warn(logger->category, "[process no:%d]  %s", entry->proc, entry->msg);
        break;
    case ZLOG_LEVEL_ERROR:
        zlog_error(logger->category, "[process no:%d] %s", entry->proc, entry->msg);
        break;
    case ZLOG_LEVEL_FATAL:
        zlog_fatal(logger->category, "[process no:%d]  %s", entry->proc, entry->msg);
        break;
    case ZLOG_LEVEL_INFO:
    default:
        zlog_info(logger->category, "[process no:%d]  %s", entry->proc, entry->msg);
        break;
    }
}

static void nano_decode_and_format(const async_log_entry_t *entry,
                                   char *out, size_t out_sz)
{
    const nano_payload_t *p = &entry->nano;
    const uint8_t *a = p->args;
    size_t         oi = 0;
    const char    *f  = p->fmt;

    if (!f) { out[0] = '\0'; return; }

    while (*f && oi + 1 < out_sz) {
        if (*f != '%') {
            out[oi++] = *f++;
            continue;
        }
        f++; 
        if (*f == '%') { out[oi++] = '%'; f++; continue; }

        char spec[32];
        int  si = 0;
        spec[si++] = '%';

        while (si < (int)sizeof(spec) - 2 && *f &&
               strchr("-+ #0.123456789", *f)) {
            spec[si++] = *f++;
        }

        if (*f == 'l') {
            spec[si++] = *f++;
            if (*f == 'l' && si < (int)sizeof(spec) - 2) {
                spec[si++] = *f++;
            }
        } else if (*f == 'h') {
            spec[si++] = *f++;
            if (*f == 'h' && si < (int)sizeof(spec) - 2) {
                spec[si++] = *f++;
            }
        } else if (*f == 'z' || *f == 'j' || *f == 't' || *f == 'L') {
            spec[si++] = *f++;
        }

        char conv = *f ? *f++ : 's';
        spec[si++] = conv;
        spec[si]   = '\0';

        if (a >= p->args + p->args_len) {
            /* ran out of args -- emit placeholder */
            const char *ph = "(?)";
            while (*ph && oi + 1 < out_sz) out[oi++] = *ph++;
            continue;
        }

        uint8_t type = *a++;
        int wrote = 0;

        switch (type) {
        case NANO_TYPE_INT: {
            int32_t v;
            if (a + 4 > p->args + p->args_len) { wrote = -1; break; }
            memcpy(&v, a, 4); a += 4;
            wrote = snprintf(out + oi, out_sz - oi, spec, (int)v);
            break;
        }
        case NANO_TYPE_LONG: {
            int64_t v;
            if (a + 8 > p->args + p->args_len) { wrote = -1; break; }
            memcpy(&v, a, 8); a += 8;
            wrote = snprintf(out + oi, out_sz - oi, spec, (long long)v);
            break;
        }
        case NANO_TYPE_UINT: {
            uint32_t v;
            if (a + 4 > p->args + p->args_len) { wrote = -1; break; }
            memcpy(&v, a, 4); a += 4;
            wrote = snprintf(out + oi, out_sz - oi, spec, (unsigned)v);
            break;
        }
        case NANO_TYPE_ULONG: {
            uint64_t v;
            if (a + 8 > p->args + p->args_len) { wrote = -1; break; }
            memcpy(&v, a, 8); a += 8;
            wrote = snprintf(out + oi, out_sz - oi, spec,
                             (unsigned long long)v);
            break;
        }
        case NANO_TYPE_DOUBLE: {
            double v;
            if (a + 8 > p->args + p->args_len) { wrote = -1; break; }
            memcpy(&v, a, 8); a += 8;
            wrote = snprintf(out + oi, out_sz - oi, spec, v);
            break;
        }
        case NANO_TYPE_PTR: {
            const void *v;
            if (a + 8 > p->args + p->args_len) { wrote = -1; break; }
            memcpy(&v, a, 8); a += 8;
            wrote = snprintf(out + oi, out_sz - oi, spec, v);
            break;
        }
        case NANO_TYPE_STR: {
            if (a + 2 > p->args + p->args_len) { wrote = -1; break; }
            uint16_t len = (uint16_t)a[0] | ((uint16_t)a[1] << 8);
            a += 2;
            if (a + len > p->args + p->args_len) { wrote = -1; break; }
            char tmp[256];
            uint16_t cp = len < 255 ? len : 255;
            memcpy(tmp, a, cp);
            tmp[cp] = '\0';
            a += len;
            wrote = snprintf(out + oi, out_sz - oi, spec, tmp);
            break;
        }
        default:
            wrote = snprintf(out + oi, out_sz - oi, "(badtype)");
            break;
        }

        if (wrote < 0) {
            const char *ph = "(?)";
            while (*ph && oi + 1 < out_sz) out[oi++] = *ph++;
        } else if ((size_t)wrote >= out_sz - oi) {
            oi = out_sz - 1;
            break;
        } else {
            oi += (size_t)wrote;
        }
    }
    out[oi] = '\0';
}

static void *logger_worker(void *arg)
{
    async_logger_t *logger = (async_logger_t *)arg;

    while (1) {
        async_log_entry_t entry;

        pthread_mutex_lock(&logger->lock);

        while (logger->count == 0 && logger->running) {
            pthread_cond_wait(&logger->cond, &logger->lock);
        }

        if (logger->count == 0 && !logger->running) {
            pthread_mutex_unlock(&logger->lock);
            break;
        }

        entry = logger->queue[logger->tail];
        logger->tail = (logger->tail + 1) % ASYNC_LOG_QUEUE_SIZE;
        logger->count--;

        pthread_mutex_unlock(&logger->lock);

        if (should_log(entry.proc, entry.level)) {
            if (entry.is_nano) {
                char fmtbuf[ASYNC_LOG_MSG_MAX];
                nano_decode_and_format(&entry, fmtbuf, sizeof(fmtbuf));
                async_log_entry_t tmp = entry;
                memcpy(tmp.msg, fmtbuf, sizeof(tmp.msg));
                tmp.msg[ASYNC_LOG_MSG_MAX - 1] = '\0';
                write_to_zlog(logger, &tmp);
            } else {
                write_to_zlog(logger, &entry);
            }
            logger->written++;
        }
    }

    return NULL;
}

static int async_logger_enqueue(
    async_logger_t *logger,
    int proc,
    int level,
    int facility,
    const char *message
)
{
    pthread_mutex_lock(&logger->lock);

    logger->total_calls++;

    if (logger->count >= ASYNC_LOG_QUEUE_SIZE) {
        logger->dropped++;
        pthread_mutex_unlock(&logger->lock);
        return -1;
    }

    async_log_entry_t *entry = &logger->queue[logger->head];

    entry->proc     = proc;
    entry->level    = level;
    entry->facility = facility;
    entry->is_nano  = 0;
    entry->nano.args_len = 0;

    strncpy(entry->msg, message, ASYNC_LOG_MSG_MAX - 1);
    entry->msg[ASYNC_LOG_MSG_MAX - 1] = '\0';

    logger->head = (logger->head + 1) % ASYNC_LOG_QUEUE_SIZE;
    logger->count++;

    pthread_cond_signal(&logger->cond);
    pthread_mutex_unlock(&logger->lock);

    return 0;
}

int nano_log_enqueue(int proc, int level, int facility,
                     const nano_payload_t *payload)
{
    async_logger_t *logger = &g_async_logger;

    pthread_mutex_lock(&logger->lock);

    logger->total_calls++;

    if (logger->count >= ASYNC_LOG_QUEUE_SIZE) {
        logger->dropped++;
        pthread_mutex_unlock(&logger->lock);
        return 0;
    }

    async_log_entry_t *entry = &logger->queue[logger->head];

    entry->proc     = proc;
    entry->level    = level;
    entry->facility = facility;
    entry->is_nano  = 1;
    entry->msg[0]   = '\0';
    entry->nano     = *payload; 

    logger->head = (logger->head + 1) % ASYNC_LOG_QUEUE_SIZE;
    logger->count++;

    pthread_cond_signal(&logger->cond);
    pthread_mutex_unlock(&logger->lock);
    return 1;
}

void zlog_write(
    zlog_category_t *category,
    int proc,
    int level,
    int facility,
    const char *fmt,
    ...
)
{
    char message[ASYNC_LOG_MSG_MAX];
    va_list args;

    (void)category; 

    if (!should_log(proc, level)) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    async_logger_enqueue(&g_async_logger, proc, level, facility, message);
}


// config_file should like during runtime it will check the zlog.conf for log rotate and all the config 
// category is like binary name also the name you mention in Log.conf during runtime
int logger_init(const char *config_file, const char *category)
{
    int rc;

    if (category == NULL || category[0] == '\0') {
        fprintf(stderr, "logger_init: empty category\n");
        return -1;
    }

    memset(&g_async_logger, 0, sizeof(g_async_logger));

    rc = zlog_init(config_file);
    if (rc != 0) {
        fprintf(stderr, "Failed to initialize zlog: %s\n", config_file);
        return -1;
    }

    g_zlog_category = zlog_get_category(category);
    if (g_zlog_category == NULL) {
        fprintf(stderr, "Failed to get zlog category\n");
        zlog_fini();
        return -1;
    }

    strncpy(g_category_name, category, sizeof(g_category_name) - 1);
    g_category_name[sizeof(g_category_name) - 1] = '\0';

    logger_load_filter_config(LOG_FILTER_CONFIG_PATH);
    refresh_category_mask();

    g_async_logger.category = g_zlog_category;
    g_async_logger.head = 0;
    g_async_logger.tail = 0;
    g_async_logger.count = 0;
    g_async_logger.running = 1;
    g_async_logger.written = 0;
    g_async_logger.dropped = 0;
    g_async_logger.total_calls = 0;

    if (pthread_mutex_init(&g_async_logger.lock, NULL) != 0) {
        fprintf(stderr, "pthread_mutex_init failed\n");
        zlog_fini();
        return -1;
    }

    if (pthread_cond_init(&g_async_logger.cond, NULL) != 0) {
        fprintf(stderr, "pthread_cond_init failed\n");
        pthread_mutex_destroy(&g_async_logger.lock);
        zlog_fini();
        return -1;
    }

    if (pthread_create(&g_async_logger.worker, NULL, logger_worker,
                       &g_async_logger) != 0) {
        fprintf(stderr, "Failed to create logger thread\n");
        pthread_cond_destroy(&g_async_logger.cond);
        pthread_mutex_destroy(&g_async_logger.lock);
        zlog_fini();
        return -1;
    }

    fprintf(stderr, "Async logger initialized successfully\n");
    return 0;
}

int logger_reload_filter_config(const char *category)
{
    (void)category;

    sleep(1);

    if (logger_load_filter_config(LOG_FILTER_CONFIG_PATH) != 0) {
        fprintf(stderr,
                "logger_reload_filter_config: reload failed, keeping previous state\n");
        return -1;
    }

    refresh_category_mask();
    return 0;
}

void logger_shutdown(void)
{
    pthread_mutex_lock(&g_async_logger.lock);
    g_async_logger.running = 0;
    pthread_cond_signal(&g_async_logger.cond);
    pthread_mutex_unlock(&g_async_logger.lock);

    pthread_join(g_async_logger.worker, NULL);

    fprintf(stderr,
            "Logger stats: total=%lu written=%lu dropped=%lu\n",
            (unsigned long)g_async_logger.total_calls,
            (unsigned long)g_async_logger.written,
            (unsigned long)g_async_logger.dropped);

    pthread_cond_destroy(&g_async_logger.cond);
    pthread_mutex_destroy(&g_async_logger.lock);

    zlog_fini();
    g_zlog_category = NULL;
    memset(&g_async_logger, 0, sizeof(g_async_logger));
}
