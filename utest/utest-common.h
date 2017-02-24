/*******************************************************************************
 * utest-common.h
 *
 * ADAS unit-test common definitions
 *
 * Copyright (c) 2015 Cogent Embedded Inc. ALL RIGHTS RESERVED.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *******************************************************************************/

#ifndef __UTEST_COMMON_H
#define __UTEST_COMMON_H

/*******************************************************************************
 * Includes
 ******************************************************************************/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <gst/gst.h>
#include <gst/video/video-info.h>
#include <linux/videodev2.h>

/*******************************************************************************
 * Primitive typedefs
 ******************************************************************************/

typedef uint8_t         u8;
typedef uint16_t        u16;
typedef uint32_t        u32;
typedef uint64_t        u64;

typedef int8_t          s8;
typedef int16_t         s16;
typedef int32_t         s32;
typedef int64_t         s64;

/*******************************************************************************
 * Auxiliary macros
 ******************************************************************************/

#ifndef offset_of
#define offset_of(type, member)         \
    ((int)(intptr_t)&(((const type *)(0))->member))
#endif

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((void *)(ptr) - offset_of(type, member)))
#endif

/*******************************************************************************
 * Bug check for constant conditions (file scope)
 ******************************************************************************/

#define __C_BUG(n)      __C_BUG2(n)
#define __C_BUG2(n)     __c_bug_##n
#define C_BUG(expr)     typedef char __C_BUG(__LINE__)[(expr) ? -1 : 1]

/*******************************************************************************
 * Unused variable
 ******************************************************************************/

#define C_UNUSED(v)                     (void)(0 ? (v) = (v), 1 : 0)

/*******************************************************************************
 * Configuration options
 ******************************************************************************/

/* ...tracing facility is on */
#define INTERN_TRACE                    1

/* ...debugging facility is on */
#define INTERN_DEBUG                    1

/*******************************************************************************
 * Auxiliary macros
 ******************************************************************************/

/* ...define a stub for unused declarator */
#define __intern_stub(tag, line)        __intern_stub2(tag, line)
#define __intern_stub2(tag, line)       typedef int __intern_##tag##_##line

/* ...convert anything into string */
#define __intern_string(x)              __intern_string2(x)
#define __intern_string2(x)             #x

/*******************************************************************************
 * Tracing facility
 ******************************************************************************/

extern int LOG_LEVEL;

enum
{
	LOG_1	 		= 0,
	LOG_ERROR 		= 0,
	LOG_INIT 		= 1,
	LOG_INFO 		= 2,
	LOG_WARNING		= 2,
	LOG_PROCESS		= 3,
	LOG_EVENT 		= 4,
	LOG_PERFORMANCE = 4,
	LOG_BUFFER 		= 5,
	LOG_DEBUG 		= 5,
	LOG_BMCA 		= 6,
	LOG_RX	 		= 6,
	LOG_SM 			= 6,
	LOG_TIME 		= 6,
	LOG_TX	 		= 6,
	LOG_SYNC 		= 6,
	LOG_PDELAY 		= 6,
	LOG_INFLIGHT	= 6,
	LOG_DUMP		= 6,

	LOG_0 		= INT_MAX,
};

/* ...variable definition specifier */
#define __trace__(var)                  __attribute__ ((unused)) var

#if INTERN_TRACE

/* ...tracing to communication processor */
extern int  intern_trace(const char *format, ...) __attribute__((format (printf, 1, 2)));

/* ...tracing facility initialization */
extern void intern_trace_init(const char *banner);

/* ...initialize tracing facility */
#define TRACE_INIT(banner)              (intern_trace_init(banner))

/* ...trace tag definition */
#define TRACE_TAG(tag, on)              enum { __intern_trace_##tag = on }

/* ...check if the trace tag is enabled */
#define TRACE_CFG(tag)                  (__intern_trace_##tag)

/* ...tagged tracing primitive */
//#define TRACE(tag, fmt, ...)            (void)(__intern_trace_##tag ? __intern_trace(tag, __intern_format##fmt, ## __VA_ARGS__), 1 : 0)
#define TRACE(tag, fmt, ...)            (void)((__intern_trace_##tag && LOG_##tag <= LOG_LEVEL)  ? __intern_trace2(tag, __intern_format##fmt, ## __VA_ARGS__), 1 : 0)

#define __intern_trace2(fmt, ...)                                   \
({                                                                  \
    int  __oldstate;                                                \
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &__oldstate);    \
    __intern_trace(fmt, ##__VA_ARGS__);                             \
    pthread_setcancelstate(__oldstate, NULL);                       \
})
    
/*******************************************************************************
 * Tagged tracing formats
 ******************************************************************************/

/* ...tracing primitive */
#define __intern_trace(tag, fmt, ...)   \
    ({ __attribute__((unused)) const char *__intern_tag = #tag; intern_trace(fmt, ## __VA_ARGS__); })

/* ...just a format string */
#define __intern_format_n(fmt)          fmt

/* ...module tag and trace tag shown */
#define __intern_format_b(fmt)          "%x:[%s.%s] " fmt, (unsigned)pthread_self(), __intern_string(MODULE_TAG), __intern_tag

/* ...module tag, trace tag, file name and line shown */
#define __intern_format_x(fmt)          "%x:[%s.%s] - %s@%d - " fmt, (unsigned)pthread_self(), __intern_string(MODULE_TAG), __intern_tag, __FILE__, __LINE__

/*******************************************************************************
 * Globally defined tags
 ******************************************************************************/

/* ...unconditionally OFF */
TRACE_TAG(0, 0);

/* ...unconditionally ON */
TRACE_TAG(1, 1);

/* ...error output - on by default */
TRACE_TAG(ERROR, 1);

/* ...warning output - on by default */
TRACE_TAG(WARNING, 1);

#else

#define TRACE_INIT(banner)              (void)0
#define TRACE_TAG(tag, on)              __intern_stub(trace_##tag, __LINE__)
#define TRACE(tag, fmt, ...)            (void)0
#define __intern_trace(tag, fmt, ...)   (void)0

#endif  /* INTERN_TRACE */

/*******************************************************************************
 * Bugchecks
 ******************************************************************************/

#if INTERN_DEBUG

/* ...run-time bugcheck */
#define BUG(cond, fmt, ...)                                         \
do                                                                  \
{                                                                   \
    if (cond)                                                       \
    {                                                               \
        /* ...output message */                                     \
        __intern_trace(BUG, __intern_format##fmt, ## __VA_ARGS__);  \
                                                                    \
        /* ...and die (tbd) */                                      \
        abort();                                                    \
    }                                                               \
}                                                                   \
while (0)

#else
#define BUG(cond, fmt, ...)             (void)0
#endif  /* INTERN_DEBUG */

/*******************************************************************************
 * Performance counters
 ******************************************************************************/

/* ...retrieve CPU cycles count - due to thread migration, use generic clock interface */
static inline u32 __get_cpu_cycles(void)
{
    struct timespec     ts;

    /* ...retrieve value of monotonic clock */
    clock_gettime(CLOCK_MONOTONIC, &ts);

    /* ...translate value into milliseconds (ignore wrap-around) */
    return (u32)((u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec);
}

static inline u32 __get_time_usec(void)
{
    struct timespec     ts;

    /* ...retrieve value of monotonic clock */
    clock_gettime(CLOCK_MONOTONIC, &ts);

    /* ...translate value into milliseconds (ignore wrap-around) */
    return (u32)((u64)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000);
}

/*******************************************************************************
 * Global constants definitions
 ******************************************************************************/

/* ...total number of cameras */
#define CAMERAS_NUMBER          4

/*******************************************************************************
 * Forward types declarations
 ******************************************************************************/

/* ...forward declaration */
typedef struct fd_source        fd_source_t;
typedef struct timer_source     timer_source_t;

/*******************************************************************************
 * External functions
 ******************************************************************************/

/* ...file source operations */
extern fd_source_t * fd_source_create(const char *filename, gint prio, GSourceFunc func, gpointer user_data, GDestroyNotify notify, GMainContext *context);
extern int fd_source_get_fd(fd_source_t *fsrc);
extern void fd_source_suspend(fd_source_t *fsrc);
extern void fd_source_resume(fd_source_t *fsrc);
extern int fd_source_is_active(fd_source_t *fsrc);

/* ...timer source operations */
extern timer_source_t * timer_source_create(GSourceFunc func, gpointer user_data, GDestroyNotify notify, GMainContext *context);
extern int timer_source_get_fd(timer_source_t *tsrc);
extern void timer_source_start(timer_source_t *tsrc, u32 interval, u32 period);
extern void timer_source_stop(timer_source_t *tsrc);
extern int timer_source_is_active(timer_source_t *tsrc);

/*******************************************************************************
 * Camera support
 ******************************************************************************/

/* ...opaque declaration */
typedef struct camera_data  camera_data_t;

/*******************************************************************************
 * Surround-view application API
 ******************************************************************************/

typedef struct display_data     display_data_t;
typedef struct window_data      window_data_t;
typedef struct texture_data     texture_data_t;

/*******************************************************************************
 * Video formats conversion helpers
 ******************************************************************************/

/* ...V4L2 format output */
#define __v4l2_fmt(f)     ((f) >> 0) & 0xFF, ((f) >> 8) & 0xFF, ((f) >> 16) & 0xFF, ((f) >> 24) & 0xFF

/* ...mapping between Gstreamer and V4L2 pixel-formats */
static inline int __pixfmt_v4l2_to_gst(u32 format)
{
    switch (format)
    {
    case V4L2_PIX_FMT_ARGB32:           return GST_VIDEO_FORMAT_ARGB;
    case V4L2_PIX_FMT_RGB565:           return GST_VIDEO_FORMAT_RGB16;
    case V4L2_PIX_FMT_RGB555:           return GST_VIDEO_FORMAT_RGB15;
    case V4L2_PIX_FMT_NV16:             return GST_VIDEO_FORMAT_NV16;
    case V4L2_PIX_FMT_NV12:             return GST_VIDEO_FORMAT_NV12;
    case V4L2_PIX_FMT_UYVY:             return GST_VIDEO_FORMAT_UYVY;
    case V4L2_PIX_FMT_YUYV:             return GST_VIDEO_FORMAT_YUY2;
    case V4L2_PIX_FMT_YVYU:             return GST_VIDEO_FORMAT_YVYU;
    case V4L2_PIX_FMT_GREY:             return GST_VIDEO_FORMAT_GRAY8;
    case V4L2_PIX_FMT_Y10:              return GST_VIDEO_FORMAT_GRAY16_BE;
    default:                            return -1;
    }
}

/*******************************************************************************
 * Auxiliary helpers (to be moved somewhere to "utest-debug.h" - tbd)
 ******************************************************************************/

#define CHK_API(cond)                           \
({                                              \
    int  __r = (cond);                          \
    if (__r < 0)                                \
        return TRACE(ERROR, _x("%m")), __r;     \
    __r;                                        \
})

#define CHK_ERR(cond, err)                                  \
({                                                          \
    if (!(cond))                                            \
        return TRACE(ERROR, _x("condition failed")), (err); \
    1;                                                      \
})

#define CHK_GL(expr)                                                        \
({                                                                          \
    GLuint  _err;                                                           \
    (expr);                                                                 \
    _err = glGetError();                                                    \
    (_err != GL_NO_ERROR ? TRACE(ERROR, _x("GL error: %X"), _err), 0 : 1);  \
})
    
#define xmalloc(size, err)                                                                  \
({                                                                                          \
    void *__p = malloc((size));                                                             \
    if (!__p)                                                                               \
        return errno = ENOMEM, TRACE(ERROR, _x("alloc failed (%u bytes)"), (size)), (err);  \
    __p;                                                                                    \
})

#endif  /* __UTEST_COMMON_H */
