#ifndef UVC_LOG_H
#define UVC_LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int uvc_log_printf(const char *format, ...);
int uvc_log_fprintf(FILE *stream, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* UVC_LOG_H */
