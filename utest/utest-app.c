/*******************************************************************************
 * utest-app.c
 *
 * IMR unit test application
 *
 * Copyright (c) 2016 Cogent Embedded Inc. ALL RIGHTS RESERVED.
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

#define MODULE_TAG                      APP

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include "utest-common.h"
#include "utest-app.h"
#include "utest-vsink.h"
#include "utest-png.h"
#include "utest-vin.h"
#include "utest-imr-sv.h"
#include <linux/videodev2.h>
#include <pango/pangocairo.h>
#include <math.h>

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 1);

/*******************************************************************************
 * Local constants definitions
 ******************************************************************************/

/* ...size of compositor buffers pool */
#define VSP_POOL_SIZE                   2

/* ...number of cameras */
#define VIN_NUMBER                      4

/*******************************************************************************
 * Local types definitions
 ******************************************************************************/

struct app_data
{
    /* ...main window handle */
    window_data_t      *window;

    /* ...main execution loop */
    GMainLoop          *loop;

    /* ...GStreamer pipeline */
    GstElement         *pipe;

    /* ...camera-set container */
    GstElement         *camera;

    /* ...input stream dimensions */
    int                 width, height;

    /* ...miscellaneous control flags */
    u32                 flags;

    /* ...input (camera) buffers readiness flag */
    u32                 input_ready;

    /* ...pending input buffers (waiting for IMR processing start) */
    GQueue              input[CAMERAS_NUMBER];

    /* ...rendering queue for main window (buffers waiting for visualization) */
    GQueue              render;
    
    /* ...data access lock */
    pthread_mutex_t     lock;

    /* ...VIN handle */
    vin_data_t         *vin;

    /* ...IMR engine handle */
    imr_sview_t        *imr_sv;
    
    /* ...frame number */
    u32                 frame_num;
};

/*******************************************************************************
 * Operation control flags
 ******************************************************************************/

/* ...output debugging info */
#define APP_FLAG_DEBUG                  (1 << 0)

/* ...renderer flushing condition */
#define APP_FLAG_EOS                    (1 << 1)

/* ...switching to next track */
#define APP_FLAG_NEXT                   (1 << 2)

/* ...switching to previous track */
#define APP_FLAG_PREV                   (1 << 3)

/* ...application termination request */
#define APP_FLAG_EXIT                   (1 << 4)

/* ...output alpha-plane mesh */
#define APP_FLAG_DEBUG_ALPHA_MESH       (1 << 5)

/* ...output camera-planes meshes */
#define APP_FLAG_DEBUG_CAMERA_MESH      (1 << 6)

/* ...active set of alpha/car images */
#define APP_FLAG_SET_INDEX              (1 << 10)

/* ...on-going update sequence status */
#define APP_FLAG_UPDATE                 (1 << 12)

/* ...mesh configuration update condition */
#define APP_FLAG_MAP_UPDATE             (1 << 13)

/* ...car model update condition  */
#define APP_FLAG_CAR_UPDATE             (1 << 14)

/* ...buffer clearing mask */
#define APP_FLAG_CLEAR_BUFFER           (1 << 16)

/*******************************************************************************
 * Mesh processing
 ******************************************************************************/

/* ...default projection matrix */
static const __mat4x4 __p_matrix = {
    __MATH_FLOAT(1.0083325),    __MATH_FLOAT(0),        __MATH_FLOAT(0),            __MATH_FLOAT(0),
    __MATH_FLOAT(0),            __MATH_FLOAT(1.792591), __MATH_FLOAT(0),            __MATH_FLOAT(0),
    __MATH_FLOAT(0),            __MATH_FLOAT(0),        __MATH_FLOAT(-1.020202),    __MATH_FLOAT(-1),
    __MATH_FLOAT(0),            __MATH_FLOAT(0),        __MATH_FLOAT(-0.20202021),  __MATH_FLOAT(0),
};

/* ...default view matrix */
static const __mat4x4 __v_matrix = {
    __MATH_FLOAT(1),    __MATH_FLOAT(0),    __MATH_FLOAT(0),    __MATH_FLOAT(0),
    __MATH_FLOAT(0),    __MATH_FLOAT(1),    __MATH_FLOAT(0),    __MATH_FLOAT(0),
    __MATH_FLOAT(0),    __MATH_FLOAT(0),    __MATH_FLOAT(1),    __MATH_FLOAT(0),
    __MATH_FLOAT(0),    __MATH_FLOAT(0),    __MATH_FLOAT(-1),   __MATH_FLOAT(1),
};

/*******************************************************************************
 * Interface exposed to the camera backend
 ******************************************************************************/

/* ...deallocate texture data */
static void __destroy_vsink_texture(gpointer data, GstMiniObject *obj)
{
    GstBuffer      *buffer = (GstBuffer *)obj;
    vsink_meta_t   *meta = gst_buffer_get_vsink_meta(buffer);

    TRACE(DEBUG, _b("destroy texture referenced by meta: %p:%p"), meta, meta->priv);

    /* ...destroy texture */
    texture_destroy(meta->priv);
}

/* ...input buffer allocation */
static int app_input_alloc(void *data, int i, GstBuffer *buffer)
{
    app_data_t     *app = data;
    vsink_meta_t   *vmeta = gst_buffer_get_vsink_meta(buffer);
    int             w = vmeta->width, h = vmeta->height;

    if (app->width)
    {
        /* ...verify buffer dimensions are valid */
        CHK_ERR(w == app->width && h == app->height, -EINVAL);
    }
    else
    {
        /* ...check dimensions are valid */
        CHK_ERR(w && h, -EINVAL);
        
        /* ...set buffer dimensions */
        app->width = w, app->height = h;
    }

    TRACE(DEBUG, _b("dmafd: %d/%d/%d, offset: %d/%d/%d, stride: %u/%u/%u"),
          vmeta->dmafd[0], vmeta->dmafd[1], vmeta->dmafd[2],
          vmeta->offset[0], vmeta->offset[1], vmeta->offset[2],
          vmeta->stride[0], vmeta->stride[1], vmeta->stride[2]);

    /* ...allocate texture to wrap the buffer */
    CHK_ERR(vmeta->priv = texture_create(w, h, vmeta->format, vmeta->dmafd, vmeta->offset, vmeta->stride), -errno);

    /* ...add custom destructor to the buffer */
    gst_mini_object_weak_ref(GST_MINI_OBJECT(buffer), __destroy_vsink_texture, app);

    /* ...do not need to do anything with the buffer allocation? */
    TRACE(INFO, _b("camera-%d: input buffer allocated (%p)"), i, buffer);

    return 0;
}

/* ...process new input buffer submitted from camera */
static int app_input_process(void *data, int i, GstBuffer *buffer)
{
    app_data_t     *app = data;
    vsink_meta_t   *vmeta = gst_buffer_get_vsink_meta(buffer);

    TRACE(DEBUG, _b("camera-%d: input buffer received"), i);

    /* ...make sure camera index is valid */
    CHK_ERR(i >= 0 && i < VIN_NUMBER, -EINVAL);

    /* ...make sure buffer dimensions are valid */
    CHK_ERR(vmeta && vmeta->width == app->width && vmeta->height == app->height, -EINVAL);

    /* ...lock access to the internal queue */
    pthread_mutex_lock(&app->lock);
    
    /* ...collect buffers in a pending input queue */
    g_queue_push_tail(&app->input[i], gst_buffer_ref(buffer));

    /* ...submit a job only when all buffers have been collected */
    if ((app->input_ready &= ~(1 << i)) == 0)
    {
        GstBuffer  *buf[4];
            
        /* ...collect buffers from input queue (it is a common function) */
        for (i = 0; i < 4; i++)
        {
            buf[i] = g_queue_pop_head(&app->input[i]);

            /* ...update readiness flag */
            (g_queue_is_empty(&app->input[i]) ? app->input_ready |= (1 << i) : 0);
        }

        /* ...submit buffers to the engine */
        CHK_API(imr_sview_submit(app->imr_sv, buf));

        /* ...release buffers ownership */
        for (i = 0; i < 4; i++)
        {
            gst_buffer_unref(buf[i]);
        }
    }
    else
    {
        TRACE(DEBUG, _b("buffer queued: %X"), app->input_ready);
    }

    /* ...unlock internal data */
    pthread_mutex_unlock(&app->lock);

    return 0;
}

/* ...callbacks for camera back-end */
static camera_callback_t camera_cb = {
    .allocate = app_input_alloc,
    .process = app_input_process,
};

/*******************************************************************************
 * IMR-based surround view interface
 ******************************************************************************/

/* ...buffers processing callback */
static void imr_sv_ready(void *cdata, GstBuffer **buf)
{
    app_data_t     *app = cdata;
    GstBuffer      *buffer = imr_sview_buf_output(buf);
    
    TRACE(DEBUG, _b("imr-sv-engine buffer ready"));

    /* ...lock internal application data */
    pthread_mutex_lock(&app->lock);

    /* ...put buffer into rendering queue */
    g_queue_push_tail(&app->render, gst_buffer_ref(buffer));

    /* ...all other buffers are just dropped - tbd */

    /* ...rendering is available */
    window_schedule_redraw(app->window);

    /* ...release application lock */
    pthread_mutex_unlock(&app->lock);
}

/* ...engine processing callback */
static imr_sview_cb_t   imr_sv_callback = {
    .ready = imr_sv_ready,
};

/*******************************************************************************
 * Drawing functions
 ******************************************************************************/

__attribute__((format (printf, 5, 6), unused))
static void draw_text(cairo_t *cr, const char *font, int x, int y, const char *fmt, ...)
{
    PangoLayout            *layout;
    PangoLayoutLine        *line;
    PangoFontDescription   *desc;
    cairo_font_options_t   *font_options;
	char                    buffer[256];
	va_list                 argp;

	va_start(argp, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, argp);
	va_end(argp);

    font_options = cairo_font_options_create();

    cairo_font_options_set_hint_style(font_options, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics(font_options, CAIRO_HINT_METRICS_OFF);

    cairo_set_font_options(cr, font_options);
    cairo_font_options_destroy(font_options);

    layout = pango_cairo_create_layout(cr);

    desc = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_text(layout, buffer, -1);

    /* Use pango_layout_get_line() instead of pango_layout_get_line_readonly()
     * for older versions of pango
     */
    line = pango_layout_get_line_readonly(layout, 0);

    cairo_move_to(cr, x, y);

    /* ...draw line of text */
    pango_cairo_layout_line_path(cr, line);
    pango_cairo_show_layout (cr, layout);

    g_object_unref(layout);
}

/* ...redraw main application window */
static void app_redraw(display_data_t *display, void *data)
{
    app_data_t     *app = data;
    window_data_t  *window = app->window;

    /* ...lock internal data */
    pthread_mutex_lock(&app->lock);
    
    /* ...retrieve pending buffers from rendering queue */
    while (!g_queue_is_empty(&app->render))
    {
        float           fps = window_frame_rate_update(window);
        GstBuffer      *buffer = g_queue_pop_head(&app->render);
        imr_meta_t     *meta = gst_buffer_get_imr_meta(buffer);

        /* ...release data access lock */
        pthread_mutex_unlock(&app->lock);

        /* ...add some performance monitors here - tbd */
        TRACE(INFO, _b("redraw frame: %u"), app->frame_num++);

        /* ...output texture on the screen (stretch to entire viewable area) */
        if (meta)
        {
            texture_draw(meta->priv2, NULL, NULL, 1.0);
        }
        else
        {
            vsink_meta_t *vmeta = gst_buffer_get_vsink_meta(buffer);
            texture_draw(vmeta->priv, NULL, NULL, 1.0);
        }

        if (LOG_LEVEL > 0)
        {
            cairo_t    *cr;

            if ((cr = window_get_cairo(window)) != NULL)
            {
                /* ...clear surface */
                cairo_set_source_rgba(cr, 0, 0, 0, 0.0);
                cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
                cairo_paint(cr);
                cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

                /* ...output frame-rate in the upper-left corner */
                cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
                draw_text(cr, "sans 18", 40, 80, "%.1f FPS", fps);

                /* ...release cairo drawing context */
                window_put_cairo(window, cr);
            }
        }
        else
        {
            TRACE(DEBUG, _b("fps: %.2f"), fps);
        }

        /* ...submit window to display renderer */
        window_draw(window);

        /* ...return output buffer to the pool */
        gst_buffer_unref(buffer);

        /* ...reacquire data access lock */
        pthread_mutex_lock(&app->lock);
    }

    /* ...release processing lock */
    pthread_mutex_unlock(&app->lock);    

    TRACE(DEBUG, _b("drawing complete.."));
}

/* ...initialize GL-processing context */
static int app_context_init(widget_data_t *widget, void *data)
{
    app_data_t     *app = data;
    window_data_t  *window = (window_data_t *)widget;
    int             w = window_get_width(window);
    int             h = window_get_height(window);
    int             i;

    /* ...create VIN engine */
    CHK_ERR(app->vin = vin_init(vin_dev_name, VIN_NUMBER, &camera_cb, app), -errno);

    /* ...setup IMR-based surround-view engine (FullHD is a maximal possible resolution) */
    CHK_ERR(app->imr_sv = imr_sview_init(&imr_sv_callback, app, __vin_width, __vin_height, __vin_format, __vsp_width, __vsp_height, __car_width, __car_height, __shadow_rect), -errno);

    /* ...setup VINs */
    for (i = 0; i < VIN_NUMBER; i++)
    {
        /* ...use 1280*800 UYVY configuration; use pool of 5 buffers */
        CHK_API(vin_device_init(app->vin, i, __vin_width, __vin_height, __vin_format, __vin_buffers_num));

    }

    /* ...buffers not ready */
    app->input_ready = (1 << VIN_NUMBER) - 1;

    TRACE(INFO, _b("run-time initialized: VIN: %d*%d@%c%c%c%c, VSP: %d*%d, DISP: %d*%d"), __vin_width, __vin_height, __v4l2_fmt(__vin_format), __vsp_width, __vsp_height, w, h);

    return 0;
}

/*******************************************************************************
 * Gstreamer thread (separated from decoding)
 ******************************************************************************/

void * app_thread(void *arg)
{
    app_data_t     *app = arg;

    g_main_loop_run(app->loop);
    
    return NULL;
}

/*******************************************************************************
 * Pipeline control flow callback
 ******************************************************************************/

static gboolean app_bus_callback(GstBus *bus, GstMessage *message, gpointer data)
{
    app_data_t     *app = data;
    GMainLoop      *loop = app->loop;

    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_ERROR:
    {
        GError     *err;
        gchar      *debug;

        /* ...dump error-message reported by the GStreamer */
        gst_message_parse_error (message, &err, &debug);
        TRACE(ERROR, _b("execution failed: %s"), err->message);
        g_error_free(err);
        g_free(debug);

        /* ...and terminate the loop */
        g_main_loop_quit(loop);
        BUG(1, _x("breakpoint"));
        return FALSE;
    }

    case GST_MESSAGE_EOS:
    {
        /* ...end-of-stream encountered; break the loop */
        TRACE(INFO, _b("execution completed"));
        g_main_loop_quit(loop);
        return TRUE;
    }

    default:
        /* ...ignore message */
        TRACE(0, _b("ignore message: %s"), gst_message_type_get_name(GST_MESSAGE_TYPE(message)));

        /* ...dump the pipeline? */
        //gst_debug_bin_to_dot_file_with_ts(GST_BIN(app->pipe), GST_DEBUG_GRAPH_SHOW_ALL, "app");
        break;
    }

    /* ...remove message from the queue */
    return TRUE;
}

/*******************************************************************************
 * Interface to backend camera
 ******************************************************************************/

/* ...end-of-stream signalization (for offline playback) */
void app_eos(app_data_t *app)
{
    GstMessage     *message = gst_message_new_eos(GST_OBJECT(app->pipe));
    
    gst_element_post_message(GST_ELEMENT_CAST(app->pipe), message);;
}

/*******************************************************************************
 * Input events processing
 ******************************************************************************/

/* ...3D-joystick input processing */
static inline widget_data_t * app_spnav_event(app_data_t *app, widget_data_t *widget, widget_spnav_event_t *event)
{
    /* ...pass to the IMR engine directly */
    pthread_mutex_lock(&app->lock);
    imr_sview_input_event(app->imr_sv, container_of(event, widget_event_t, spnav));
    pthread_mutex_unlock(&app->lock);

    return widget;
}

/* ...touchscreen input processing */
static inline widget_data_t * app_touch_event(app_data_t *app, widget_data_t *widget, widget_touch_event_t *event)
{
    /* ...pass to the IMR engine directly */
    pthread_mutex_lock(&app->lock);
    imr_sview_input_event(app->imr_sv, container_of(event, widget_event_t, touch));
    pthread_mutex_unlock(&app->lock);
    
    return widget;
}

/* ...touchscreen input processing */
static inline widget_data_t * app_kbd_event(app_data_t *app, widget_data_t *widget, widget_key_event_t *event)
{
    if (event->type == WIDGET_EVENT_KEY_PRESS && event->state)
    {
        switch (event->code)
        {
        case KEY_ESC:
            /* ...terminate application (for the moment, just exit - tbd) */
            TRACE(INIT, _b("terminate application"));
            exit(0);

        default:
            /* ...pass to IMR engine */
            imr_sview_input_event(app->imr_sv, container_of(event, widget_event_t, key));
        }        
    }

    /* ...always keep focus */
    return widget;
}

/* ...event-processing function */
static widget_data_t * app_input_event(widget_data_t *widget, void *cdata, widget_event_t *event)
{
    app_data_t     *app = cdata;

    /* ...pass event to GUI layer first */
    switch (WIDGET_EVENT_TYPE(event->type))
    {
    case WIDGET_EVENT_SPNAV:
        return app_spnav_event(app, widget, &event->spnav);

    case WIDGET_EVENT_TOUCH:
        return app_touch_event(app, widget, &event->touch);

    case WIDGET_EVENT_KEY:
        return app_kbd_event(app, widget, &event->key);

    default:
        return NULL;
    }
}

/*******************************************************************************
 * Module initialization
 ******************************************************************************/

/* ...module destructor */
static void app_destroy(gpointer data, GObject *obj)
{
    app_data_t  *app = data;

    TRACE(INIT, _b("destruct application data"));

    /* ...destroy main loop */
    g_main_loop_unref(app->loop);

    /* ...destroy main application window */
    (app->window ? window_destroy(app->window) : 0);
    
    /* ...free application data structure */
    free(app);

    TRACE(INIT, _b("module destroyed"));
}

/*******************************************************************************
 * Window parameters
 ******************************************************************************/

/* ...processing window parameters */
static window_info_t app_main_info = {
    .fullscreen = 1,
    .redraw = app_redraw,
};

/* ...main window widget parameters (input-interface + GUI?) */
static widget_info_t app_main_info2 = {
    .buffers = 2,
    .init = app_context_init,
    .event = app_input_event,
};


/* ...set camera interface */
int app_camera_init(app_data_t *app, camera_init_func_t camera_init)
{
    GstElement     *bin;

    /* ...clear input stream dimensions (force engine reinitialization) */
    app->width = app->height = 0;
    
    /* ...create camera interface (it may be network camera or file on disk) */
    CHK_ERR(bin = camera_init(&camera_cb, app), -errno);

    /* ...add cameras to a pipe */
    gst_bin_add(GST_BIN(app->pipe), bin);

    /* ...synchronize state with a parent */
    gst_element_sync_state_with_parent(bin);

    /* ...save camera-set container */
    app->camera = bin;

    TRACE(INIT, _b("camera-set initialized"));

    return 0;
}

/*******************************************************************************
 * Entry point
 ******************************************************************************/

/* ...module initialization function */
app_data_t * app_init(display_data_t *display)
{
    app_data_t            *app;
    pthread_mutexattr_t    attr;

    /* ...sanity check - output device shall be positive */
    CHK_ERR(__output_main >= 0, (errno = EINVAL, NULL));

    /* ...create local data handle */
    CHK_ERR(app = calloc(1, sizeof(*app)), (errno = ENOMEM, NULL));

    /* ...set output device number for a main window */
    app_main_info.output = __output_main;

    /* ...initialize data access lock */
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&app->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    /* ...create main loop object (use default context) */
    if ((app->loop = g_main_loop_new(NULL, FALSE)) == NULL)
    {
        TRACE(ERROR, _x("failed to create main loop object"));
        errno = ENOMEM;
        goto error;
    }
    else
    {
        /* ...push default thread context for all subsequent sources */
        g_main_context_push_thread_default(g_main_loop_get_context(app->loop));
    }

    /* ...create a pipeline (not used yet) */
    if ((app->pipe = gst_pipeline_new("app::pipe")) == NULL)
    {
        TRACE(ERROR, _x("pipeline creation failed"));
        errno = ENOMEM;
        goto error;
    }
    else
    {
        GstBus  *bus = gst_pipeline_get_bus(GST_PIPELINE(app->pipe));
        gst_bus_add_watch(bus, app_bus_callback, app);
        gst_object_unref(bus);
    }

    /* ...create full-screen window for processing results visualization */        
    if ((app->window = window_create(display, &app_main_info, &app_main_info2, app)) == NULL)
    {
        TRACE(ERROR, _x("failed to create main window: %m"));
        goto error;
    }

    /* ...start VIN interface */
    if (vin_start(app->vin) < 0)
    {
        TRACE(ERROR, _x("failed to start VIN: %m"));
        goto error;
    }
    
    /* ...add destructor to the pipe */
    g_object_weak_ref(G_OBJECT(app->pipe), app_destroy, app);
    
    TRACE(INIT, _b("application initialized"));

    return app;

error:
    /* ...destroy main loop */
    (app->loop ? g_main_loop_unref(app->loop) : 0);

    /* ...destroy main application window */
    (app->window ? window_destroy(app->window) : 0);

    /* ...destroy data handle */
    free(app);

    return NULL;
}
