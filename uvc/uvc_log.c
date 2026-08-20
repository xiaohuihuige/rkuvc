#define UVC_LOG_IMPLEMENTATION
#include "uvc_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>

#include "rkuvc.h"

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static uvc_log_callback g_log_callback;
static void *g_log_userdata;

void register_uvc_log_callback(uvc_log_callback cb, void *userdata)
{
    pthread_mutex_lock(&g_log_lock);
    g_log_callback = cb;
    g_log_userdata = userdata;
    pthread_mutex_unlock(&g_log_lock);
}

static int uvc_log_vfprintf(FILE *stream, const char *format, va_list args)
{
    if (!stream || !format)
        return -1;

    pthread_mutex_lock(&g_log_lock);
    uvc_log_callback callback = g_log_callback;
    void *userdata = g_log_userdata;
    pthread_mutex_unlock(&g_log_lock);

    if (!callback)
        return vfprintf(stream, format, args);

    va_list size_args;
    va_copy(size_args, args);
    int length = vsnprintf(NULL, 0, format, size_args);
    va_end(size_args);
    if (length < 0)
        return length;

    char *message = malloc((size_t)length + 1);
    if (!message)
        return vfprintf(stream, format, args);

    vsnprintf(message, (size_t)length + 1, format, args);

    callback(userdata, message);
    free(message);
    return length;
}

int uvc_log_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int ret = uvc_log_vfprintf(stdout, format, args);
    va_end(args);
    return ret;
}

int uvc_log_fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int ret = uvc_log_vfprintf(stream, format, args);
    va_end(args);
    return ret;
}
