/*******************************************************************************
 * utest-wl-display.c
 *
 * Display support for unit-test application (Wayland-client)
 *
 * Copyright (c) 2014-2016 Cogent Embedded Inc. ALL RIGHTS RESERVED.
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

#define MODULE_TAG                      DISPLAY

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include "utest-common.h"
#include "utest-wl-display.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <gst/video/video-format.h>
#include "linux-dmabuf-client-protocol.h"
#include "scaler-client-protocol.h"
#include <libdrm/drm_fourcc.h>

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(EVENT, 1);
TRACE_TAG(DEBUG, 0);

/*******************************************************************************
 * Local typedefs
 ******************************************************************************/

/* ...output device data */
typedef struct output_data
{
    /* ...list node */
    struct wl_list              link;

    /* ...Wayland output device handle */
    struct wl_output           *output;

    /* ...current output device width / height */
    u32                         width, height;

    /* ...transformation parameters */
    u32                         transform;

}   output_data_t;

/* ...input device data */
typedef struct input_data
{
    /* ...list node */
    struct wl_list              link;

    /* ...Wayland seat handle */
    struct wl_seat             *seat;

    /* ...seat capabilities */
    u32                         caps;

    /* ...pointer device interface */
    struct wl_pointer          *pointer;

    /* ...current focus for pointer device (should I make them different?) */
    widget_data_t              *pointer_focus;
    
    /* ...latched pointer position */
    int                         pointer_x, pointer_y;

    /* ...keyboard device interface */
    struct wl_keyboard         *keyboard;

    /* ...current focus for keyboard device (should I make them different?) */
    widget_data_t              *keyboard_focus;

    /* ...touch device interface */
    struct wl_touch            *touch;

    /* ...current focus widgets for touchscreen events */
    widget_data_t              *touch_focus;

}   input_data_t;

/* ...dispatch loop source */
typedef struct display_source_cb
{
    /* ...processing function */
    int           (*hook)(display_data_t *, struct display_source_cb *, u32 events);
    
}   display_source_cb_t;

/* ...display data */
struct display_data
{
    /* ...Wayland display handle */
    struct wl_display          *display;

    /* ...Wayland registry handle */
    struct wl_registry         *registry;

    /* ...screen compositor */
    struct wl_compositor       *compositor;

    /* ...subcompositor interface handle (not used) */
    struct wl_subcompositor    *subcompositor;

    /* ...shell interface handle */
    struct wl_shell            *shell;

    /* ...shared memory interface handle (not used?) */
    struct wl_shm              *shm;

    /* ....DMA buffers handling interface */
    struct zlinux_dmabuf       *dmabuf;

    /* ...scaling interface */
    struct wl_scaler           *scaler;

    /* ...input/output device handles */
    struct wl_list              outputs, inputs;

    /* ...set of registered windows */
    struct wl_list              windows;

    /* ...cairo device associated with display */
    cairo_device_t             *cairo;

    /* ...dispatch loop epoll descriptor */
    int                         efd;

    /* ...pending display event status */
    int                         pending;

    /* ...dispatch thread handle */
    pthread_t                   thread;

    /* ...display lock (need that really? - tbd) */
    pthread_mutex_t             lock;
};

/* ...widget data structure */
struct widget_data
{
    /* ...reference to owning window */
    window_data_t              *window;

    /* ...reference to parent widget (not used yet - tbd) */
    widget_data_t              *parent;

    /* ...pointer to the user-provided widget info */
    widget_info_t              *info;

    /* ...widget client data */
    void                       *cdata;

    /* ...surface interface */
    struct wl_surface          *surface;

    /* ...subsurface interface */
    struct wl_subsurface       *subsurface;

    /* ...cairo surfaces associated with this widget */
    cairo_surface_t           **cs;

    /* ...index of current surface */
    int                         index;

    /* ...actual widget dimensions */
    int                         left, top, width, height;

    /* ...surface update request */
    int                         dirty;
};

/* ...output window data */
struct window_data
{
    /* ...root widget data (must be first) */
    widget_data_t               widget;

    /* ...reference to a display data */
    display_data_t             *display;

    /* ...list node in display windows list */
    struct wl_list              link;

    /* ...wayland surface */
    struct wl_surface          *surface;

    /* ...shell surface */
    struct wl_shell_surface    *shell;

    /* ...scaling viewport */
    struct wl_viewport         *viewport;

    /* ...cairo device associated with current window context */
    cairo_device_t             *cairo;

    /* ...window information */
    const window_info_t        *info;

    /* ...client data for a callback */
    void                       *cdata;

    /* ...internal data access lock */
    pthread_mutex_t             lock;

    /* ...conditional variable for rendering thread */
    pthread_cond_t              wait;

    /* ...window rendering thread */
    pthread_t                   thread;

    /* ...processing flags */
    u32                         flags;

    /* ...frame-rate calculation */
    u32                         fps_ts, fps_acc;
};

/*******************************************************************************
 * Window processing flags
 ******************************************************************************/

/* ...redraw command pending */
#define WINDOW_FLAG_REDRAW              (1 << 0)

/* ...buffer busyness flag */
#define WINDOW_FLAG_BUSY                (1 << 1)

/* ...pending redraw request */
#define WINDOW_FLAG_PENDING             (1 << 2)

/* ...termination command pending */
#define WINDOW_FLAG_TERMINATE           (1 << 3)

/*******************************************************************************
 * Local variables
 ******************************************************************************/

/* ...this should be singleton for now - tbd */
static display_data_t   __display;

/* ...thread key to store current window in TLS */
static pthread_key_t    __key_window;

/*******************************************************************************
 * Internal helpers
 ******************************************************************************/

/* ...regroup all that stuff down below - tbd */
static inline window_data_t * __window_lookup(struct wl_surface *surface)
{
    window_data_t  *window;
    
    if (!surface || !(window = wl_surface_get_user_data(surface)))  return NULL;
    if (window->surface != surface)     return NULL;
    return window;
}

/*******************************************************************************
 * Display dispatch thread
 ******************************************************************************/

/* ...number of events expected */
#define DISPLAY_EVENTS_NUM      4

/* ...add handle to a display polling structure */
static inline int display_add_poll_source(display_data_t *display, int fd, display_source_cb_t *cb)
{
    struct epoll_event  event;
    
    event.events = EPOLLIN;
    event.data.ptr = cb;
    return epoll_ctl(display->efd, EPOLL_CTL_ADD, fd, &event);
}

/* ...remove handle from a display polling structure */
static inline int display_remove_poll_source(display_data_t *display, int fd)
{
    return epoll_ctl(display->efd, EPOLL_CTL_DEL, fd, NULL);
}

/* ...display dispatch thread */
static void * dispatch_thread(void *arg)
{
    display_data_t     *display = arg;
    struct epoll_event  event[DISPLAY_EVENTS_NUM];

    /* ...add display file descriptor */
    CHK_ERR(display_add_poll_source(display, wl_display_get_fd(display->display), NULL) == 0, NULL);

    /* ...start waiting loop */
    while (1)
    {
        int     disp = 0;
        int     i, r;

        /* ...as we are preparing to poll Wayland display, add polling prologue */
        while (wl_display_prepare_read(display->display) != 0)
        {
            /* ...dispatch all pending events and repeat attempt */
            wl_display_dispatch_pending(display->display);
        }

        /* ...flush all outstanding commands to a display */
        if (wl_display_flush(display->display) < 0)
        {
            TRACE(ERROR, _x("display flush failed: %m"));
            goto error;
        }

        /* ...wait for an event */
        if ((r = epoll_wait(display->efd, event, DISPLAY_EVENTS_NUM, -1)) < 0)
        {
            /* ...ignore soft interruptions */
            if (errno != EINTR)
            {
                TRACE(ERROR, _x("epoll failed: %m"));
                goto error;
            }
        }

        /* ...process all signalled events */
        for (i = 0; i < r; i++)
        {
            display_source_cb_t *dispatch = event[i].data.ptr;

            /* ...invoke event-processing function (ignore result code) */
            if (dispatch)
            {
                dispatch->hook(display, dispatch, event[i].events);
            }
            else if (event[i].events & EPOLLIN)
            {
                disp = 1;
            }
        }

        /* ...process display event separately */
        if (disp)
        {
            /* ...read display events */
            if (wl_display_read_events(display->display) < 0 && errno != EAGAIN)
            {
                TRACE(ERROR, _x("failed to read display events: %m"));
                goto error;
            }

            /* ...process pending display events (if any) */
            if (wl_display_dispatch_pending(display->display) < 0)
            {
                TRACE(ERROR, _x("failed to dispatch display events: %m"));
                goto error;
            }
        }
        else
        {
            /* ...if nothing was read from display, cancel initiated reading */
            wl_display_cancel_read(display->display);
        }
    }

    TRACE(INIT, _b("display dispatch thread terminated"));
    return NULL;

error:
    return (void *)(intptr_t)-errno;
}

/*******************************************************************************
 * Output device handling
 ******************************************************************************/

/* ...geometry change notification */
static void output_handle_geometry(void *data, struct wl_output *wl_output,
               int32_t x, int32_t y,
               int32_t physical_width, int32_t physical_height,
               int32_t subpixel,
               const char *make, const char *model,
               int32_t output_transform)
{
    output_data_t     *output = data;

    /* ...nothing but printing? */
    TRACE(INFO, _b("output[%p:%p]: %s:%s: x=%d, y=%d, transform=%d"), output, wl_output, make, model, x, y, output_transform);

    /* ...save current transformation parameters */
    output->transform = output_transform;
}

/* ...output device mode reporting processing */
static void output_handle_mode(void *data, struct wl_output *wl_output,
           uint32_t flags, int32_t width, int32_t height,
           int32_t refresh)
{
    output_data_t *output = data;

    /* ...check if the mode is current */
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)    return;

    /* ...set current output device size */
    switch (output->transform)
    {
    case 0:
    case 180:
        output->width = width, output->height = height;
        break;
    case 90:
    case 270:
    default:
        output->width = height, output->height = width;
    }
    
    TRACE(INFO, _b("output[%p:%p] - %d*%d"), output, wl_output, width, height);
}

static const struct wl_output_listener output_listener =
{
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
};

/* ...add output device */
static inline void display_add_output(display_data_t *display, struct wl_registry *registry, uint32_t id)
{
    output_data_t   *output = calloc(1, sizeof(*output));

    BUG(!output, _x("failed to allocate memory"));

    output->output = wl_registry_bind(registry, id, &wl_output_interface, 1);
    wl_output_add_listener(output->output, &output_listener, output);
    wl_list_insert(display->outputs.prev, &output->link);

    /* ...force another round of display initialization */
    display->pending = 1;
}

/* ...get output device by number */
static output_data_t *display_get_output(display_data_t *display, int n)
{
    output_data_t  *output;

    /* ...traverse available outputs list */
    wl_list_for_each(output, &display->outputs, link)
        if (n-- == 0)
            return output;

    /* ...not found */
    return NULL;
}

/*******************************************************************************
 * Input device handling
 ******************************************************************************/

/* ...pointer entrance notification */
static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
            uint32_t serial, struct wl_surface *surface,
            wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    input_data_t   *input = data;
    int             sx = wl_fixed_to_int(sx_w);
    int             sy = wl_fixed_to_int(sy_w);
    window_data_t  *window;
    widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;

    TRACE(1, _b("input[%p]-enter: surface: %p, serial: %u, sx: %d, sy: %d"), input, surface, serial, sx, sy);
    
    /* ...check the surface is valid */
    if (!(window = __window_lookup(surface)))   return;

    /* ...latch pointer position */
    input->pointer_x = sx, input->pointer_y = sy;

    /* ...set current focus */
    focus = &window->widget;

    /* ...drop event if no processing is associated */
    if (!(info = focus->info) || !info->event)      return;

    /* ...respond with a "set-cursor" (use default) */
    wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);

    TRACE(1, _b("hide cursor"));
    
    /* ...pass event to the root widget */
    event.type = WIDGET_EVENT_MOUSE_ENTER;
    event.mouse.x = sx, event.mouse.y = sy;
    input->pointer_focus = info->event(focus, focus->cdata, &event);
}

/* ...pointer leave notification */
static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
            uint32_t serial, struct wl_surface *surface)
{
    input_data_t   *input = data;
    window_data_t  *window;
    widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;

    TRACE(0, _b("input[%p]-leave: surface: %p, serial: %u"), input, surface, serial);

    /* ...check the surface is valid */
    if (!(window = __window_lookup(surface)))   return;

    /* ...drop event if no focus is defined */
    if (!(focus = input->pointer_focus))    return;

    /* ...clear pointer-device focus */
    input->pointer_focus = NULL;

    /* ...drop event if no processing is associated */
    if (!(info = focus->info) || !info->event)  return;

    /* ...pass event to the current widget */
    event.type = WIDGET_EVENT_MOUSE_LEAVE;

    /* ...pass event to active widget */
    input->pointer_focus = info->event(focus, focus->cdata, &event);

    (focus != input->pointer_focus ? TRACE(DEBUG, _b("focus updated: %p"), input->pointer_focus) : 0);
}

/* ...handle pointer motion */
static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
            uint32_t time, wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    input_data_t   *input = data;
    int             sx = wl_fixed_to_int(sx_w);
    int             sy = wl_fixed_to_int(sy_w);
    widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;

    TRACE(0, _b("input[%p]: motion: sx=%d, sy=%d"), input, sx, sy);

    /* ...drop event if no current focus set */
    if (!(focus = input->pointer_focus))    return;

    /* ...latch input position */
    input->pointer_x = sx, input->pointer_y = sy;

    /* ...drop event if no processing hook set */
    if (!(info = focus->info) || !info->event)  return;

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_MOUSE_MOVE;
    event.mouse.x = sx;
    event.mouse.y = sy;
    input->pointer_focus = info->event(focus, focus->cdata, &event);
}

/* ...button press/release processing */
static void pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial,
            uint32_t time, uint32_t button, uint32_t state)
{
    input_data_t   *input = data;
    widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;

    TRACE(0, _b("input[%p]: serial=%u, button=%u, state=%u"), input, serial, button, state);

    /* ...drop event if no current focus set */
    if (!(focus = input->pointer_focus))    return;
    
    /* ...drop event if no processing hook set */
    if (!(info = focus->info) || !info->event)  return;

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_MOUSE_BUTTON;
    event.mouse.x = input->pointer_x;
    event.mouse.y = input->pointer_y;
    event.mouse.button = button;
    event.mouse.state = (state == WL_POINTER_BUTTON_STATE_PRESSED);   
    input->pointer_focus = info->event(focus, focus->cdata, &event);
}

/* ...button wheel (?) processing */
static void pointer_handle_axis(void *data, struct wl_pointer *pointer,
            uint32_t time, uint32_t axis, wl_fixed_t value)
{
    input_data_t   *input = data;
    int             v = wl_fixed_to_int(value);
    widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;

    TRACE(0, _x("input[%p]: axis=%u, value=%d"), input, axis, v);

    /* ...drop event if no current focus set */
    if (!(focus = input->pointer_focus))    return;
    
    /* ...drop event if no processing hook set */
    if (!(info = focus->info) || !info->event)  return;

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_MOUSE_AXIS;
    event.mouse.x = input->pointer_x;
    event.mouse.y = input->pointer_y;
    event.mouse.axis = axis;
    event.mouse.value = v;
    input->pointer_focus = info->event(focus, focus->cdata, &event);
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
};

/*******************************************************************************
 * Touchscreen support
 ******************************************************************************/

static void touch_handle_down(void *data, struct wl_touch *wl_touch,
          uint32_t serial, uint32_t time, struct wl_surface *surface,
          int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    input_data_t   *input = data;
    int             sx = wl_fixed_to_int(x_w);
    int             sy = wl_fixed_to_int(y_w);
    window_data_t  *window;
    widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;
 
    TRACE(0, _b("input[%p]-touch-down: surface=%p, id=%u, sx=%d, sy=%d"), input, surface, id, sx, sy);
    
    /* ...get window associated with a surface */
    if (!(window = __window_lookup(surface)))   return;

    /* ...get touch focus if needed */
    focus = (input->touch_focus ? : &window->widget);

    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)  return;
    
    /* ...pass event to root widget */
    event.type = WIDGET_EVENT_TOUCH_DOWN;
    event.touch.x = sx;
    event.touch.y = sy;
    event.touch.id = id;

    input->touch_focus = info->event(focus, focus->cdata, &event);

    if (!input->touch_focus)    TRACE(DEBUG, _x("touch focus lost!"));
}

/* ...touch removal event notification */
static void touch_handle_up(void *data, struct wl_touch *wl_touch,
        uint32_t serial, uint32_t time, int32_t id)
{
    input_data_t   *input = data;
    widget_data_t  *focus = input->touch_focus;
    widget_info_t  *info;
    widget_event_t  event;
    
    TRACE(0, _b("input[%p]-touch-up: serial=%u, id=%u"), input, serial, id);

    /* ...drop event if no focus defined */
    if (!(focus = input->touch_focus))      return;

    /* ...reset touch focus pointer */
    input->touch_focus = NULL;
    
    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)  return;

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_TOUCH_UP;
    event.touch.id = id;
    input->touch_focus = info->event(focus, focus->cdata, &event);

    if (!input->touch_focus)    TRACE(DEBUG, _x("touch focus lost!"));
}

/* ...touch sliding event processing */
static void touch_handle_motion(void *data, struct wl_touch *wl_touch,
            uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    input_data_t   *input = data;
    int             sx = wl_fixed_to_int(x_w);
    int             sy = wl_fixed_to_int(y_w);
    widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;
    
    TRACE(0, _b("input[%p]-move: id=%u, sx=%d, sy=%d (focus: %p)"), input, id, sx, sy, input->touch_focus);

    /* ...ignore event if no touch focus exists */
    if (!(focus = input->touch_focus))      return;

    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)  return;

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_TOUCH_MOVE;
    event.touch.x = sx;
    event.touch.y = sy;
    event.touch.id = id;
    input->touch_focus = info->event(focus, focus->cdata, &event);

    if (!input->touch_focus)    TRACE(DEBUG, _x("touch focus lost!"));
}

/* ...end of touch frame (gestures recognition?) */
static void touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
    input_data_t   *input = data;

    TRACE(DEBUG, _b("input[%p]-touch-frame"), input);
}

/* ...touch-frame cancellation (gestures recognition?) */
static void touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
    input_data_t   *input = data;

    TRACE(DEBUG, _b("input[%p]-frame-cancel"), input);
}

/* ...wayland touch device listener callbacks */
static const struct wl_touch_listener touch_listener = {
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel,
};

/*******************************************************************************
 * Keyboard events processing
 ******************************************************************************/

/* ...keymap handling */
static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
            uint32_t format, int fd, uint32_t size)
{
    input_data_t   *input = data;

    /* ...here we can remap keycodes - tbd */
    TRACE(DEBUG, _b("input[%p]: keymap format: %X, fd=%d, size=%u"), input, format, fd, size);
}

/* ...keyboard focus receive notification */
static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
            uint32_t serial, struct wl_surface *surface,
            struct wl_array *keys)
{
    input_data_t   *input = data;
    window_data_t  *window;
    widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;

    TRACE(DEBUG, _b("input[%p]: key-enter: surface: %p"), input, surface);

    /* ...get window associated with a surface */
    if (!(window = __window_lookup(surface)))   return;

	/* ...set focus to root widget (? - tbd) */
	input->keyboard_focus = focus = &window->widget;

    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)      return;
    
    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_KEY_ENTER;
    input->keyboard_focus = info->event(focus, focus->cdata, &event);

    /* ...process all pressed keys? modifiers? */
}

/* ...keyboard focus leave notification */
static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
            uint32_t serial, struct wl_surface *surface)
{
    input_data_t   *input = data;
    window_data_t  *window;
    widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;

    TRACE(DEBUG, _b("input[%p]: key-leave: surface: %p"), input, surface);

    /* ...find a target widget */
    if (!(window = __window_lookup(surface)))   return;

    /* ...select active widget (root widget if nothing) */
    focus = (input->keyboard_focus ? : &window->widget);

    /* ...reset keyboard focus */
    input->keyboard_focus = NULL;

    /* ...drop message if no processing is defined */
    if (!(info = focus->info) || !info->event)  return;

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_KEY_LEAVE;
    input->keyboard_focus = info->event(focus, focus->cdata, &event);
}

/* ...key pressing event */
static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
            uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	input_data_t   *input = data;
	widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;

    TRACE(DEBUG, _b("input[%p]: key-press: key=%u, state=%u"), input, key, state);

    /* ...ignore event if no focus defined */
    if (!(focus = input->keyboard_focus))   return;
    
    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)  return;

	/* ...pass notification to the widget */
    event.type = WIDGET_EVENT_KEY_PRESS;
    event.key.code = key;
    event.key.state = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    input->keyboard_focus = info->event(focus, focus->cdata, &event);
}

/* ...modifiers state change */
static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
            uint32_t serial, uint32_t mods_depressed,
            uint32_t mods_latched, uint32_t mods_locked,
            uint32_t group)
{
	input_data_t   *input = data;
	widget_data_t  *focus;
    widget_info_t  *info;
    widget_event_t  event;

    TRACE(DEBUG, _b("input[%p]: mods-press: press=%X, latched=%X, locked=%X, group=%X"), input, mods_depressed, mods_latched, mods_locked, group);

    /* ...ignore event if no focus defined */
    if (!(focus = input->keyboard_focus))   return;
    
    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)  return;

	/* ...pass notification to the widget */
    event.type = WIDGET_EVENT_KEY_MODS;
    event.key.mods_on = mods_latched;
    event.key.mods_off = mods_depressed;
    event.key.mods_locked = mods_locked;
	input->keyboard_focus = info->event(focus, focus->cdata, &event);
}

/* ...keyboard listener callback */
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
};

/*******************************************************************************
 * Input device registration
 ******************************************************************************/

/* ...input device capabilities registering */
static void seat_handle_capabilities(void *data, struct wl_seat *seat, enum wl_seat_capability caps)
{
    input_data_t   *input = data;

    /* ...process pointer device addition/removal */
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer)
    {
        input->pointer = wl_seat_get_pointer(seat);
        wl_pointer_set_user_data(input->pointer, input);
        wl_pointer_add_listener(input->pointer, &pointer_listener, input);
        TRACE(INFO, _b("pointer-device %p added"), input->pointer);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->pointer)
    {
        TRACE(INFO, _b("pointer-device %p removed"), input->pointer);
        wl_pointer_destroy(input->pointer);
        input->pointer = NULL;
    }

    /* ...process keyboard addition/removal */
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->keyboard)
    {
        input->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_set_user_data(input->keyboard, input);
        wl_keyboard_add_listener(input->keyboard, &keyboard_listener, input);
        TRACE(INFO, _b("keyboard-device %p added"), input->keyboard);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && input->keyboard)
    {
        TRACE(INFO, _b("keyboard-device %p removed"), input->keyboard);
        wl_keyboard_destroy(input->keyboard);
        input->keyboard = NULL;
    }

    /* ...process touch device addition/removal */
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !input->touch)
    {
        input->touch = wl_seat_get_touch(seat);
        wl_touch_set_user_data(input->touch, input);
        wl_touch_add_listener(input->touch, &touch_listener, input);
        TRACE(INFO, _b("touch-device %p added"), input->touch);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && input->touch)
    {
        TRACE(INFO, _b("touch-device %p removed"), input->touch);
        wl_touch_destroy(input->touch);
        input->touch = NULL;
    }
}

/* ...input device name (probably, for a mapping to particular output? - tbd) */
static void seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
    input_data_t   *input = data;

    /* ...just output a name */
    TRACE(INFO, _b("input[%p]: device '%s' registered"), input, name);
}

/* ...input device wayland callback */
static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name
};

/* ...register input device */
static inline void display_add_input(display_data_t *display, struct wl_registry *registry, uint32_t id, uint32_t version)
{
    input_data_t   *input = calloc(1, sizeof(*input));

    BUG(!input, _x("failed to allocate memory"));

    /* ...bind seat interface */
    input->seat = wl_registry_bind(registry, id, &wl_seat_interface, MIN(version, 3));
    wl_seat_add_listener(input->seat, &seat_listener, input);
    wl_list_insert(display->inputs.prev, &input->link);

    /* ...force another round of display initialization */
    display->pending = 1;
}

/*******************************************************************************
 * Spacenav 3D-joystick support
 ******************************************************************************/

/* ...spacenav input event processing */
static int input_spacenav_event(display_data_t *display, display_source_cb_t *cb, u32 events)
{
    widget_event_t  event;
    spnav_event     e;
    window_data_t  *window;
    
    /* ...drop event if no reading flag set */
    if ((events & EPOLLIN) == 0)        return 0;
    
    /* ...retrieve poll event */
    if (CHK_API(spnav_poll_event(&e)) == 0)     return 0;

    /* ...preare widget event */
    event.type = WIDGET_EVENT_SPNAV;
    event.spnav.e = &e;

    /* ...pass to all windows */
    wl_list_for_each(window, &display->windows, link)
    {
        widget_data_t  *widget = &window->widget;
        widget_info_t  *info = widget->info;

        /* ...ignore window if no input event is registered */
        if (!info || !info->event)      continue;

        /* ...pass event to root widget (only one consumer?) */
        if (info->event(widget, window->cdata, &event) != NULL)   break;
    }

    return 0;
}

static display_source_cb_t spacenav_source = {
    .hook = input_spacenav_event,
};

/* ...spacenav event initializer */
static inline int input_spacenav_init(display_data_t *display)
{
    int     fd;
    
    /* ...open spacenav device (do not die if not found) */
    if (spnav_open() < 0)
    {
        TRACE(INIT, _b("spacenavd daemon is not running"));
        return 0;
    }
    
    if ((fd = spnav_fd()) < 0)
    {
        TRACE(ERROR, _x("failed to open spacenv connection: %m"));
        goto error;
    }
    
    /* ...add file-descriptor as display poll source */
    if (display_add_poll_source(display, fd, &spacenav_source) < 0)
    {
        TRACE(ERROR, _x("failed to add poll source: %m"));
        goto error;
    }
    
    TRACE(INIT, _b("spacenav input added"));
    
    return 0;

error:
    /* ...destroy connection to a server */
    spnav_close();

    return -errno;
}

/*******************************************************************************
 * Joystick support
 ******************************************************************************/

typedef struct joystick_data
{
    /* ...generic display source handle */
    display_source_cb_t     source;

    /* ...file descriptor */
    int                     fd;

    /* ...any axis? - button maps? - tbd - need to keep latched values */

}   joystick_data_t;

/* ...joystick input event processing */
static int input_joystick_event(display_data_t *display, display_source_cb_t *cb, u32 events)
{
    joystick_data_t    *js = (joystick_data_t *)cb;
    widget_event_t      event;
    struct js_event     e;
    window_data_t      *window;
    
    /* ...drop event if no reading flag set */
    if ((events & EPOLLIN) == 0)        return 0;

    /* ...retrieve poll event */
    CHK_ERR(read(js->fd, &e, sizeof(e)) == sizeof(e), -errno);

    /* ...preare widget event */
    event.type = WIDGET_EVENT_JOYSTICK;
    event.js.e = &e;

    TRACE(DEBUG, _b("joystick event: type=%x, value=%x, number=%x"), e.type & ~JS_EVENT_INIT, e.value, e.number);    

    /* ...pass to all windows */
    wl_list_for_each(window, &display->windows, link)
    {
        widget_data_t  *widget = &window->widget;
        widget_info_t  *info = widget->info;
        
        /* ...ignore window if no input event is registered */
        if (!info || !info->event)      continue;

        /* ...pass event to root widget (only one consumer?) */
        if (info->event(widget, window->cdata, &event) != NULL)   break;
    }

    return 0;
}

static joystick_data_t joystick_source = {
    .source = {
        .hook = input_joystick_event,
    },
};

/* ...spacenav event initializer */
static inline int input_joystick_init(display_data_t *display, const char *devname)
{
    int     fd;
    int     version = 0x800;
    int     axes = 2, buttons = 2;
    char    name[128] = { '\0' };

    /* ...open joystick device */
    if ((joystick_source.fd = fd = open(devname, O_RDONLY)) < 0)
    {
        TRACE(INIT, _b("no joystick connected"));
        return 0;
    }

    ioctl(fd, JSIOCGVERSION, &version);
	ioctl(fd, JSIOCGAXES, &axes);
	ioctl(fd, JSIOCGBUTTONS, &buttons);
	ioctl(fd, JSIOCGNAME(sizeof(name)), name);

    TRACE(INIT, _b("device: %s; version: %X, buttons: %d, axes: %d, name: %s"), devname, version, buttons, axes, name);
    
    /* ...put joystick into non-blocking mode */
    fcntl(fd, F_SETFL, O_NONBLOCK);

    /* ...add file descriptor to display poll set */
    if (display_add_poll_source(display, fd, &joystick_source.source) < 0)
    {
        TRACE(ERROR, _x("failed to add joystick: %m"));
        goto error;
    }

    TRACE(INIT, _b("joystick device '%s' added"), devname);
    
    return 0;

error:
    /* close device descriptor */
    close(fd);
    return -errno;
}

/*******************************************************************************
 * Shared memory handling
 ******************************************************************************/

static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
    display_data_t     __trace__(*display) = data;

    TRACE(DEBUG, _b("shm-format supported: %X"), format);
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format
};

/* ...anonimous file creation (taken from Weston) */
static inline int __create_anonymous_file(off_t length)
{
	static const char template[] = "/weston-shared-XXXXXX";
	static const char *path;
	char       *name;
	int         fd;

    /* ...get server runtime directory */
	CHK_ERR(path || (path = getenv("XDG_RUNTIME_DIR")), -(errno = ENOENT));

    /* ...allocate temporary storage for a file name */
	CHK_ERR(name = malloc(strlen(path) + sizeof(template)), -(errno = ENOMEM));

    /* ...prepare temporary file template */
	strcpy(name, path), strcat(name, template);

    /* ...create temporary file */
	if ((fd = mkostemp(name, O_CLOEXEC)) < 0)
    {
        TRACE(ERROR, _x("failed to create file '%s': %m"), name);
        goto out;
    }
    else
    {
        /* ...remove file from the filesystem */
        unlink(name);
    }

    /* ...reserve size of the file */
    if (ftruncate(fd, length) < 0)
    {
        TRACE(ERROR, _x("failed to reserve %zu bytes: %m"), length);
        close(fd), fd = -1;
        goto out;
    }

    TRACE(DEBUG, _b("reserved %zu bytes (fd=%d)"), length, fd);
    
out:
    /* ...release temporary string name */
    free(name);

    return fd;
}

/* ...shared memory descriptor */
typedef  struct sh_mem
{
    /* ...file descriptor (suitable for exchange between processes) */
    int                 fd;

    /* ...user-space maping */
    void               *data;

    /* ...chunk size */
    u32                 size;

    /* ...wayland buffer pointer */
    struct wl_buffer   *buffer;

}   sh_mem_t;
    
/* ...allocate shared memory chunk */
sh_mem_t * sh_mem_alloc(u32 size)
{
    sh_mem_t   *shm;

    /* ...allocate memory descriptor */
    CHK_ERR(shm = calloc(1, sizeof(*shm)), (errno = ENOMEM, NULL));

    /* ...all chunks must be page-size aligned */
    shm->size = size = (size + 4095) & ~4095;

    /* ...reserve memory in the mapping region */
    if ((shm->fd = __create_anonymous_file(size)) < 0)
    {
        TRACE(ERROR, _x("failed to allocate memory: %m"));
        free(shm);
        return NULL;
    }

    /* ...map memory in the user-space */
    if ((shm->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0)) == MAP_FAILED)
    {
        TRACE(ERROR, _x("failed to map memory: %m"));
        close(shm->fd);
        free(shm);
        return NULL;
    }

    TRACE(DEBUG, _b("allocated shared memory %p[fd=%d, data=%p, size=%u]"), shm, shm->fd, shm->data, shm->size);

    return shm;
}

/* ...release shared memory */
void sh_mem_free(sh_mem_t *shm)
{
    /* ...drop wayland buffer as needed */
    (shm->buffer ? wl_buffer_destroy(shm->buffer) : 0);

    /* ...unmap memory */
    munmap(shm->data, shm->size);

    /* ...release reserved memory */
    close(shm->fd);

    /* ...destroy memory descriptor */
    free(shm);

    TRACE(DEBUG, _b("shared memory %p destroyed"), shm);
}

/*******************************************************************************
 * Shared memory buffers allocation
 ******************************************************************************/

/* ...determine buffer size */
static inline u32 __shmem_pixfmt_size(int w, int h, int format, u32 *shm_format, int *stride)
{
    switch (format)
    {
    case GST_VIDEO_FORMAT_ARGB:
        return *shm_format = WL_SHM_FORMAT_ARGB8888, (*stride = w * 4) * h;
    case GST_VIDEO_FORMAT_RGB16:
        return *shm_format = WL_SHM_FORMAT_RGB565, (*stride = w * 2) * h;
    default:
        return 0;
    }
}

/* ...create shared memory buffer */
int shmem_allocate_buffers(int w, int h, int format, sh_mem_t **output, int num)
{
    display_data_t     *display = &__display;
    u32                 size;
    u32                 shm_format;
    int                 stride;
    int                 i;

    /* ...calculate size of the buffer */
    if ((size = __shmem_pixfmt_size(w, h, format, &shm_format, &stride)) == 0)
    {
        TRACE(ERROR, _x("unsupported format %d"), format);
        return -(errno = EINVAL);
    }

    /* ...reserve shared memory of given size */
    for (i = 0; i < num; i++)
    {
        struct wl_shm_pool     *pool;
        
        /* ...create shared memory chunk */
        if ((output[i] = sh_mem_alloc(size)) == NULL)
        {
            TRACE(ERROR, _x("failed to allocate buffer: %m"));
            goto error;
        }

        /* ...create wayland pool to contain a single buffer */
        if ((pool = wl_shm_create_pool(display->shm, output[i]->fd, size)) == NULL)
        {
            TRACE(ERROR, _x("failed to create shared memory pool"));
            sh_mem_free(output[i]);
            errno = ENOMEM;
            goto error;
        }

        /* ...create shared buffer */
        if ((output[i]->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, shm_format)) == NULL)
        {
            TRACE(ERROR, _x("failed to create shared buffer"));
            wl_shm_pool_destroy(pool);
            sh_mem_free(output[i]);
            errno = ENOMEM;
            goto error;
        }

        /* ...destroy pool object */
        wl_shm_pool_destroy(pool);
    }

    return 0;

error:
    /* ...destroy buffers allocated thus far */
    while (i--)
    {
        sh_mem_free(output[i]);
    }

    return -1;
}

/*******************************************************************************
 * DMA buffers handling
 ******************************************************************************/

static void dmabuf_format(void *data, struct zlinux_dmabuf *dmabuf, uint32_t format)
{
    display_data_t     __trace__(*display) = data;

    TRACE(DEBUG, _b("dmabuf-format supported: %X"), format);
}

static const struct zlinux_dmabuf_listener dmabuf_listener = {
    .format = dmabuf_format
};

/*******************************************************************************
 * Registry listener callbacks
 ******************************************************************************/

/* ...interface registrar */
static void global_registry_handler(void *data, struct wl_registry *registry, uint32_t id,
                                    const char *interface, uint32_t version)
{
    display_data_t     *display = data;

    if (strcmp(interface, "wl_compositor") == 0)
    {
        display->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
    }
    else if (strcmp(interface, "wl_subcompositor") == 0)
    {
        display->subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    }
    else if (strcmp(interface, "wl_shell") == 0)
    {
        display->shell = wl_registry_bind(registry, id, &wl_shell_interface, 1);
    }
    else if (strcmp(interface, "wl_output") == 0)
    {
        display_add_output(display, registry, id);
    }
    else if (strcmp(interface, "wl_seat") == 0)
    {
        display_add_input(display, registry, id, version);
    }
    else if (strcmp(interface, "wl_shm") == 0)
    {
        display->shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
        wl_shm_add_listener(display->shm, &shm_listener, display);
    }
    else if (strcmp(interface, "zlinux_dmabuf") == 0)
    {
        display->dmabuf = wl_registry_bind(registry, id, &zlinux_dmabuf_interface, 1);
        zlinux_dmabuf_add_listener(display->dmabuf, &dmabuf_listener, display);
    }
    else if (strcmp(interface, "wl_scaler") == 0)
    {
        display->scaler = wl_registry_bind(registry, id, &wl_scaler_interface, 2);
    }
}

/* ...interface removal notification callback */
static void global_registry_remove(void *data, struct wl_registry *registry, uint32_t id)
{
    display_data_t     *display = data;

    TRACE(INIT, _b("display[%p]: id removed: %u"), display, id);
}

/* ...registry listener callbacks */
static const struct wl_registry_listener registry_listener =
{
    global_registry_handler,
    global_registry_remove
};

/*******************************************************************************
 * Shell surface interface implementation
 ******************************************************************************/

/* ...shell surface heartbeat callback */
static void handle_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

/* ...shell surface reconfiguration callback */
static void handle_configure(void *data, struct wl_shell_surface *shell_surface,
         uint32_t edges, int32_t width, int32_t height)
{
    TRACE(INFO, _b("shell configuration changed: W=%d, H=%d, E=%u"), width, height, edges);
}

/* ...focus removal notification */
static void handle_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
    TRACE(INFO, _b("focus removed - hmm..."));
}

/* ...shell surface callbacks */
static const struct wl_shell_surface_listener shell_surface_listener = {
    handle_ping,
    handle_configure,
    handle_popup_done
};

/*******************************************************************************
 * Window support
 ******************************************************************************/

/* ...window rendering thread */
static void * window_thread(void *arg)
{
    window_data_t      *window = arg;
    display_data_t     *display = window->display;

    /* ...register current window inside TLS */
    pthread_setspecific(__key_window, window);

    /* ...enter executive loop */
    while (1)
    {
        /* ...serialize access to window state */
        pthread_mutex_lock(&window->lock);

        /* ...wait for a drawing command from an application */
        while (!(window->flags & (WINDOW_FLAG_REDRAW | WINDOW_FLAG_TERMINATE)))
        {
            TRACE(DEBUG, _b("window[%p] wait"), window);
            pthread_cond_wait(&window->wait, &window->lock);
        }

        TRACE(DEBUG, _b("window[%p] redraw (flags=%X)"), window, window->flags);

        /* ...break processing thread if requested to do that */
        if (window->flags & WINDOW_FLAG_TERMINATE)
        {
            pthread_mutex_unlock(&window->lock);
            break;
        }

        /* ...clear window drawing schedule flag */
        window->flags &= ~WINDOW_FLAG_REDRAW;

        /* ...release window access lock */
        pthread_mutex_unlock(&window->lock);

        /* ...invoke user-supplied hook */
        window->info->redraw(display, window->cdata);
    }

    TRACE(INIT, _b("window[%p] thread terminated"), window);

    return NULL;
}

/*******************************************************************************
 * Internal helpers - getting messy - tbd
 ******************************************************************************/

/* ...check surface status */
static inline int __check_surface(cairo_surface_t *cs)
{
    cairo_status_t  status;
    
    switch (status = cairo_surface_status(cs))
    {
    case CAIRO_STATUS_SUCCESS:          return 0;
    case CAIRO_STATUS_READ_ERROR:       errno = EINVAL; break;
    case CAIRO_STATUS_FILE_NOT_FOUND:   errno = ENOENT; break;
    default:                            errno = ENOMEM; break;
    }

    TRACE(ERROR, _b("cairo surface error: '%s'"), cairo_status_to_string(status));
    
    return -errno;
}

/* ...surface status error */
static inline const char * __surface_strerr(cairo_surface_t *cs)
{
    return cairo_status_to_string(cairo_surface_status(cs));
}

/*******************************************************************************
 * Basic widgets support
 ******************************************************************************/

/* ...static key for shared memory pointer */
static cairo_user_data_key_t    __cairo_shmem_key;

/* ...destructor callback */
static void __cairo_shmem_destroy(void *data)
{
    sh_mem_t   *shm = data;
    sh_mem_free(shm);
}

/* ...create cairo surfaces for a widget */
static inline int __widget_create_surfaces(widget_data_t *widget, int num, int w, int h)
{
    sh_mem_t   *shm[num];
    int         i;
    
    /* ...allocate array of cairo surfaces */
    CHK_ERR(widget->cs = calloc(num, sizeof(*widget->cs)), -(errno = ENOMEM));
        
    /* ...create pool of memory buffers */
    if (shmem_allocate_buffers(w, h, GST_VIDEO_FORMAT_ARGB, shm, num) < 0)
    {
        TRACE(ERROR, _x("failed to allocate buffers: %m"));
        goto error;
    }
    
    /* ...create cairo surfaces */
    for (i = 0; i < num; i++)
    {
        /* ...create surface wrapping the memory buffer */
        widget->cs[i] = cairo_image_surface_create_for_data(shm[i]->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);

        /* ...make sure surface has been allocated successfully */
        if (__check_surface(widget->cs[i]) < 0)
        {
            TRACE(ERROR, _x("failed to allocate a surface: %m"));
            goto error_cs;
        }

        /* ...attach user-data to the surface */
        cairo_surface_set_user_data(widget->cs[i], &__cairo_shmem_key, shm[i], __cairo_shmem_destroy);
    }

    TRACE(DEBUG, _b("allocated %d cairo-buffers"), num);

    return 0;
    
error_cs:
    /* ...destroy all surfaces created thus far */
    while (i--)
    {
        cairo_surface_destroy(widget->cs[i]);
    }

error:
    /* ...destroy buffers */
    free(widget->cs), widget->cs = NULL;
    return -1;
}

/* ...destroy cairo surfaces */
static inline void __widget_destroy_surfaces(widget_data_t *widget)
{
    const widget_info_t    *info = widget->info;
    int                     i;

    /* ...destroy all surfaces allocated */
    for (i = 0; i < info->buffers; i++)
    {
        cairo_surface_destroy(widget->cs[i]);
    }

    /* ...release surfaces array */
    free(widget->cs);
}

/* ...internal widget initialization function */
static int __widget_init(widget_data_t *widget, window_data_t *window, int W, int H, widget_info_t *info, void *cdata)
{
    struct wl_region   *region;
    int                 w, h;

    /* ...set user-supplied data */
    widget->info = info, widget->cdata = cdata;

    /* ...set pointer to the owning window */
    widget->window = window;

    /* ...if width/height are not specified, take them from window */
    widget->width = w = (info && info->width ? info->width : W);
    widget->height = h = (info && info->height ? info->height : H);
    widget->top = (info ? info->top : 0);
    widget->left = (info ? info->left : 0);

    /* ...create widget surface */
    widget->surface = wl_compositor_create_surface(window->display->compositor);
    region = wl_compositor_create_region(window->display->compositor);
    wl_region_add(region, 0, 0, 0, 0);
    wl_surface_set_input_region(widget->surface, region);
    wl_region_destroy(region);

    /* ...create sub-surface for a widget (single nesting level? - tbd) */
    widget->subsurface = wl_subcompositor_get_subsurface(window->display->subcompositor, widget->surface, window->surface);
    
    /* ...create cairo surface for a 2D-graphics if required */
    if ((info->buffers ? __widget_create_surfaces(widget, info->buffers, w, h) : 0) < 0)
    {
        TRACE(ERROR, _x("failed to create surfaces: %m"));
        goto error;
    }

    /* ...initialize widget controls as needed */
    if (info && info->init)
    {
        if (info->init(widget, cdata) < 0)
        {
            TRACE(ERROR, _x("widget initialization failed: %m"));
            goto error;
        }
     
        /* ...mark widget is dirty */
        widget->dirty = 1;
    }
    else
    {
        /* ...clear dirty flag */
        widget->dirty = 0;
    }

    TRACE(INIT, _b("widget [%p] initialized"), widget);

    return 0;

error:
    /* ...destroy cairo surfaces */
    (widget->cs ? __widget_destroy_surfaces(widget) : 0);

    /* ...destroy subsurface if set */
    (widget->subsurface ? wl_subsurface_destroy(widget->subsurface) : 0);

    /* ...destroy surface */
    (widget->surface ? wl_surface_destroy(widget->surface) : 0);

    return -1;    
}

/* ...create widget */
widget_data_t * widget_create(window_data_t *window, widget_info_t *info, void *cdata)
{
    int             w = window->widget.width;
    int             h = window->widget.height;;
    widget_data_t  *widget;

    /* ...allocate data handle */
    CHK_ERR(widget = malloc(sizeof(*widget)), (errno = ENOMEM, NULL));

    /* ...initialize widget data */
    if (__widget_init(widget, window, w, h, info, cdata) < 0)
    {
        TRACE(ERROR, _x("widget initialization error: %m"));
        goto error;
    }

    return widget;

error:
    /* ...destroy widget data */
    free(widget);

    return NULL;
}

/* ...widget destructor */
void widget_destroy(widget_data_t *widget)
{
    widget_info_t  *info = widget->info;
    
    /* ...invoke custom destructor function as needed */
    (info && info->destroy ? info->destroy(widget, widget->cdata) : 0);
    
    /* ...destroy cairo surface */
    if (widget->cs)
    {
        int     i;
        
        for (i = 0; i < info->buffers; i++)
        {
            cairo_surface_destroy(widget->cs[i]);
        }
        
        free(widget->cs);
    }

    /* ...release data handle */
    free(widget);

    TRACE(INIT, _b("widget[%p] destroyed"), widget);
}

#if 0
/* ...render widget content into given target context */
void widget_render(widget_data_t *widget, cairo_t *cr, float alpha)
{
    widget_info_t      *info = widget->info;

    /* ...update widget content as needed */
    widget_update(widget, 0);

    /* ...output widget content in current drawing context */
    cairo_save(cr);
    cairo_set_source_surface(cr, widget->cs, info->left, info->top);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

/* ...update widget content */
void widget_update(widget_data_t *widget, int flush)
{
    cairo_t    *cr;

    /* ...do nothing if update is not required */
    if (!widget->dirty)     return;

    /* ...clear dirty flag in advance */
    widget->dirty = 0;

    /* ...get curface drawing context */
    cr = cairo_create(widget->cs);

    /* ...update widget content */
    widget->info->draw(widget, widget->cdata, cr);

    /* ...make sure context is sane */
    if (TRACE_CFG(DEBUG) && cairo_status(cr) != CAIRO_STATUS_SUCCESS)
    {
        TRACE(ERROR, _x("widget[%p]: bad context: '%s'"), widget, cairo_status_to_string(cairo_status(cr)));
    }

    /* ...destroy context */
    cairo_destroy(cr);

    /* ...force widget surface update */
    (0 && flush ? cairo_surface_flush(widget->cs) : 0);
}

/* ...schedule widget redrawing */
void widget_schedule_redraw(widget_data_t *widget)
{
    /* ...mark widget is dirty */
    widget->dirty = 1;

    /* ...schedule redrawing of the parent window */
    window_schedule_redraw(widget->window);
}

/* ...input event processing */
widget_data_t * widget_input_event(widget_data_t *widget, widget_event_t *event)
{
    widget_info_t  *info = widget->info;
    
    return (info && info->event ? info->event(widget, widget->cdata, event) : NULL);
}
#endif

/* ...return current widget width */
int widget_get_width(widget_data_t *widget)
{
    return widget->width;
}

/* ...return current widget height */
int widget_get_height(widget_data_t *widget)
{
    return widget->height;
}

/* ...return left point */
int widget_get_left(widget_data_t *widget)
{
    return widget->left;
}

/* ...return top point */
int widget_get_top(widget_data_t *widget)
{
    return widget->top;
}

/* ...get cairo device associated with widget */
cairo_device_t * widget_get_cairo_device(widget_data_t *widget)
{
    return widget->window->cairo;
}

/* ...get parent window root widget */
widget_data_t * widget_get_parent(widget_data_t *widget)
{
    return &widget->window->widget;
}

/*******************************************************************************
 * Entry points
 ******************************************************************************/

/* ...create native window */
window_data_t * window_create(display_data_t *display, window_info_t *info, widget_info_t *info2, void *cdata)
{
    int                 width = info->width;
    int                 height = info->height;
    output_data_t      *output;
    window_data_t      *window;
    struct wl_region   *region;
    pthread_attr_t      attr;
    int                 r;

    /* ...make sure we have a valid output device */
    if ((output = display_get_output(display, info->output)) == NULL)
    {
        TRACE(ERROR, _b("invalid output device number: %u"), info->output);
        errno = EINVAL;
        return NULL;
    }

    /* ...if width/height are not specified, use output device dimensions */
    (!width ? width = output->width : 0), (!height ? height = output->height : 0);

    /* ...allocate a window data */
    if ((window = calloc(1, sizeof(*window))) == NULL)
    {
        TRACE(ERROR, _x("failed to allocate memory"));
        errno = ENOMEM;
        return NULL;
    }

    /* ...initialize window data access lock */
    pthread_mutex_init(&window->lock, NULL);

    /* ...initialize conditional variable for communication with rendering thread */
    pthread_cond_init(&window->wait, NULL);

    /* ...save display handle */
    window->display = display;

    /* ...save window info data */
    window->info = info, window->cdata = cdata;

    /* ...clear window flags */
    window->flags = 0;

    /* ...reset frame-rate calculator */
    window_frame_rate_reset(window);

    /* ...get wayland surface (subsurface maybe?) */
    window->surface = wl_compositor_create_surface(display->compositor);

    /* ...specify window has the only opaque region */
    region = wl_compositor_create_region(display->compositor);
    wl_region_add(region, 0, 0, width, height);
    wl_surface_set_opaque_region(window->surface, region);
    wl_region_destroy(region);
    
    /* ...get desktop shell surface handle */
    window->shell = wl_shell_get_shell_surface(display->shell, window->surface);
    wl_shell_surface_add_listener(window->shell, &shell_surface_listener, window);
    (info->title ? wl_shell_surface_set_title(window->shell, info->title) : 0);
    wl_shell_surface_set_toplevel(window->shell);
    (info->fullscreen ? wl_shell_surface_set_fullscreen(window->shell, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0, output->output) : 0);

    /* ...get scaling interface */
    window->viewport = wl_scaler_get_viewport(display->scaler, window->surface);

    /* ...set private data poitner */
    wl_surface_set_user_data(window->surface, window);

    /* ...window has no persistent buffer attached (yet) */
   
    /* ...initialize root widget data */
    if (__widget_init(&window->widget, window, width, height, info2, cdata) < 0)
    {
        TRACE(INIT, _b("widget initialization failed: %m"));
        goto error;
    }

    /* ...initialize thread attributes (joinable, default stack size) */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    /* ...create rendering thread */
    r = pthread_create(&window->thread, &attr, window_thread, window);
    pthread_attr_destroy(&attr);
    if (r != 0)
    {
        TRACE(ERROR, _x("thread creation failed: %m"));
        goto error;
    }

    /* ...add window to global display list */
    wl_list_insert(display->windows.prev, &window->link);

    TRACE(INFO, _b("window created: %p, %u*%u, output: %u"), window, width, height, info->output);

    return window;

error:
    /* ...destroy window memory */
    free(window);
    return NULL;
}

/* ...hide window */
int window_set_invisible(window_data_t *window)
{
    /* ...we just pass null buffer */
    wl_surface_attach(window->surface, NULL, 0, 0);
    wl_surface_damage(window->surface, 0, 0, window->widget.width, window->widget.height);
    wl_surface_commit(window->surface);
    
    TRACE(DEBUG, _b("window minimized"));

    return 0;
}

/* ...window destruction completion callback */
static void __destroy_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
    pthread_mutex_t  *wait_lock = data;

    TRACE(DEBUG, _b("release wait lock"));

    /* ...release mutex */
    pthread_mutex_unlock(wait_lock);

    wl_callback_destroy(callback);
}

static const struct wl_callback_listener __destroy_listener = {
    __destroy_callback,
};

/* ...destroy a window */
void window_destroy(window_data_t *window)
{
    display_data_t         *display = window->display;
    const window_info_t    *info = window->info;
    const widget_info_t    *info2 = window->widget.info;
    struct wl_callback     *callback;

    /* ...terminate window rendering thread */
    pthread_mutex_lock(&window->lock);
    window->flags |= WINDOW_FLAG_TERMINATE;
    pthread_cond_signal(&window->wait);
    pthread_mutex_unlock(&window->lock);

    /* ...wait until thread completes */
    pthread_join(window->thread, NULL);

    TRACE(DEBUG, _b("window[%p] thread joined"), window);

    /* ...remove window from global display list */
    wl_list_remove(&window->link);

    /* ...invoke custom widget destructor function as needed */
    (info2 && info2->destroy ? info2->destroy(&window->widget, window->cdata) : 0);

    /* ...destroy root widget cairo surface (I don't have it yet - tbd )*/
    (window->widget.cs ? __widget_destroy_surfaces(&window->widget) : 0);

    /* ...invoke custom window destructor function as needed */
    (info && info->destroy ? info->destroy(window, window->cdata) : 0);

    /* ...destroy cairo device (don't have it yet) */
    (window->cairo ? cairo_device_destroy(window->cairo) : 0);

    /* ...destroy shell surface */
    wl_shell_surface_destroy(window->shell);

    /* ....destroy wayland surface (shell surface gets destroyed automatically) */
    wl_surface_destroy(window->surface);

    /* ...make sure function is complete before we actually proceed */
    if ((callback = wl_display_sync(display->display)) != NULL)
    {
        pthread_mutex_t     wait_lock = PTHREAD_MUTEX_INITIALIZER;

        pthread_mutex_lock(&wait_lock);
        wl_callback_add_listener(callback, &__destroy_listener, &wait_lock);
        wl_display_flush(display->display);

        /* ...mutex will be released in callback function executed from display thread context */
        pthread_mutex_lock(&wait_lock);
    }

    /* ...destroy window lock */
    pthread_mutex_destroy(&window->lock);

    /* ...destroy rendering thread conditional variable */
    pthread_cond_destroy(&window->wait);

    /* ...destroy object */
    free(window);

    TRACE(INFO, _b("window[%p] destroyed"), window);
}

/* ...return current window width */
int window_get_width(window_data_t *window)
{
    return window->widget.width;
}

/* ...return current window height */
int window_get_height(window_data_t *window)
{
    return window->widget.height;
}

/* ...schedule redrawal of the window */
void window_schedule_redraw(window_data_t *window)
{
    int     kick = 0;

    /* ...acquire window lock */
    pthread_mutex_lock(&window->lock);

    /* ...check if we don't have a flag already */
    if ((window->flags & (WINDOW_FLAG_REDRAW | WINDOW_FLAG_PENDING)) == 0)
    {
        /* ...set a flag */
        if ((window->flags & WINDOW_FLAG_BUSY) == 0)
        {
            /* ...schedule drawing operation */
            window->flags ^= WINDOW_FLAG_REDRAW;
            kick = 1;
        }
        else
        {
            /* ...drawing cannot start; put pending request */
            window->flags ^= WINDOW_FLAG_PENDING;
        }

        TRACE(DEBUG, _b("schedule window[%p] redraw"), window);
    }

    /* ...release window access lock */
    pthread_mutex_unlock(&window->lock);

    /* ...and kick processing thread as needed */
    (kick ? pthread_cond_signal(&window->wait) : 0);
}

/* ...window destruction completion callback */
static void __window_sync_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
    window_data_t  *window = data;
    int             kick = 0;
    
    TRACE(DEBUG, _b("window[%p]: frame sync callback"), window);

    /* ...lock window access */
    pthread_mutex_lock(&window->lock);

    BUG((window->flags & WINDOW_FLAG_BUSY) == 0, _x("invalid state: %X"), window->flags);

    /* ...clear busy flag */
    if ((window->flags ^= WINDOW_FLAG_BUSY) & WINDOW_FLAG_PENDING)
    {
        /* ...enable drawing operation */
        window->flags ^= WINDOW_FLAG_PENDING | WINDOW_FLAG_REDRAW;

        /* ...resume window thread */
        kick = 1;
    }

    /* ...release buffer lock */
    pthread_mutex_unlock(&window->lock);

    /* ...kick window thread as needed */
    (kick ? pthread_cond_signal(&window->wait) : 0);

    /* ...do I need to destroy this callback all the time? - tbd */
    wl_callback_destroy(callback);
}

static void __window_sync_callback2(void *data, struct wl_callback *callback, uint32_t serial)
{
    pthread_mutex_t    *lock = data;
    
    TRACE(DEBUG, _b("lock[%p]: frame sync callback"), lock);

    /* ...release waiting lock */
    pthread_mutex_unlock(lock);

    /* ...do I need to destroy this callback all the time? - tbd */
    wl_callback_destroy(callback);
}

/* ...display sync calback */
static const struct wl_callback_listener __window_sync_listener = {
    __window_sync_callback,
};

/* ...display sync calback */
static const struct wl_callback_listener __window_sync_listener2 = {
    __window_sync_callback2,
};

/* ...submit window to a renderer */
void window_draw(window_data_t *window)
{
    display_data_t         *display = window->display;
    widget_data_t          *w = &window->widget;
    struct wl_callback     *callback;
    u32                     t0, t1;
    pthread_mutex_t         __wait_lock = PTHREAD_MUTEX_INITIALIZER;
    
    t0 = __get_time_usec();

    /* ...finalize any pending 2D-drawing? - do it differently - hmm */
#if 0
    if (0 && w->cs)
    {
        cairo_surface_flush(w->cs);
        
        /* ...make sure everything is correct */
        BUG(cairo_surface_status(w->cs) != CAIRO_STATUS_SUCCESS, _x("bad status: %s"), cairo_status_to_string(cairo_surface_status(w->cs)));
    }
#endif

    /* ...submit stuff to the wayland server */
    wl_surface_damage(window->surface, 0, 0, w->width, w->height);

    /* ...mark window is busy */
    pthread_mutex_lock(&window->lock);

    if (0) {
        
    if ((window->flags ^= WINDOW_FLAG_BUSY) & WINDOW_FLAG_REDRAW)
    {
        /* ...put back pending flag */
        window->flags ^= WINDOW_FLAG_REDRAW | WINDOW_FLAG_PENDING;
    }
    }
    
    pthread_mutex_unlock(&window->lock);

    pthread_mutex_lock(&__wait_lock);

    /* ...do I have to wait for a buffer processing? I guess it should be done in window thread */
    if ((callback = wl_surface_frame(window->surface)) != NULL)
    {
        //wl_callback_add_listener(callback, &__window_sync_listener, window);
        wl_callback_add_listener(callback, &__window_sync_listener2, &__wait_lock);
    }
    else
    {
        BUG(1, _x("breakpoint"));
    }
    
    /* ...commit a change */
    wl_surface_commit(window->surface);

    /* ...push pending commands to a server (need that? probably no) */
    wl_display_flush(display->display);

    /* ...wait for a callback completion right here */
    pthread_mutex_lock(&__wait_lock);

    t1 = __get_time_usec();

    TRACE(DEBUG, _b("swap[%p]: %u"), window, t1 - t0);
}

/* ...retrieve associated cairo surface */
cairo_t * window_get_cairo(window_data_t *window)
{
    /* ...check if cairo surface has been created */
    if (window->widget.cs)
    {
        cairo_t    *cr;
        int         i = window->widget.index;
        
        /* ...create new drawing context */
        cr = cairo_create(window->widget.cs[i]);

        /* ...make it a bug for a moment */
        BUG(cairo_status(cr) != CAIRO_STATUS_SUCCESS, _x("invalid status: (%d) - %s"), cairo_status(cr), cairo_status_to_string(cairo_status(cr)));

        /* ...update widget index */
        window->widget.index = (++i == window->widget.info->buffers ? 0 : i);

        return cr;
    }
    else
    {
        return NULL;
    }
}

/* ...release associated cairo surface */
void window_put_cairo(window_data_t *window, cairo_t *cr)
{
    if (cr)
    {
        cairo_surface_t    *cs = cairo_get_target(cr);
        sh_mem_t           *shm = cairo_surface_get_user_data(cs, &__cairo_shmem_key);

        BUG(!shm, _x("invalid cairo context: cr=%p, cs=%p, status=%s"), cr, cs, __surface_strerr(cs));
        
        /* ...destroy cairo drawing interface */
        cairo_destroy(cr);

        /* ...flush surface before switching to native drawing */
        cairo_surface_flush(cs);

        /* ...attach buffer to the root widget subsurface */
        wl_surface_attach(window->widget.surface, shm->buffer, 0, 0);

        /* ...damage surface */
        wl_surface_damage(window->widget.surface, 0, 0, window->widget.width, window->widget.height);

        /* ...commit the changes */
        wl_surface_commit(window->widget.surface);
    }
}

/*******************************************************************************
 * Display module initialization
 ******************************************************************************/

/* ...create display data */
display_data_t * display_create(void)
{
    extern char        *joystick_dev_name;
    display_data_t     *display = &__display;
    pthread_attr_t      attr;
    int                 r;

    /* ...reset display data */
    memset(display, 0, sizeof(*display));

    /* ...connect to Wayland display */
    if ((display->display = wl_display_connect(NULL)) == NULL)
    {
        TRACE(ERROR, _x("failed to connect to Wayland: %m"));
        errno = EBADFD;
        goto error;
    }
    else if ((display->registry = wl_display_get_registry(display->display)) == NULL)
    {
        TRACE(ERROR, _x("failed to get registry: %m"));
        errno = EBADFD;
        goto error_disp;
    }
    else
    {
        /* ...set global registry listener */
        wl_registry_add_listener(display->registry, &registry_listener, display);
    }

    /* ...initialize inputs/outputs lists */
    wl_list_init(&display->outputs);
    wl_list_init(&display->inputs);

    /* ...initialize windows list */
    wl_list_init(&display->windows);

    /* ...create a display command/response lock */
    pthread_mutex_init(&display->lock, NULL);

    /* ...create polling structure */
    if ((display->efd = epoll_create(DISPLAY_EVENTS_NUM)) < 0)
    {
        TRACE(ERROR, _x("failed to create epoll: %m"));
        goto error_disp;
    }

    /* ...create global TLS key for storing current window */
    if (pthread_key_create(&__key_window, NULL) < 0)
    {
        TRACE(ERROR, _x("failed to create TLS key: %m"));
        goto error_epoll;
    }

    /* ...pre-initialize global Wayland interfaces */
    do
    {
        display->pending = 0, wl_display_roundtrip(display->display);
    }
    while (display->pending);

    /* ...create Wayland dispatch thread (joinable, default stack size) */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    r = pthread_create(&display->thread, &attr, dispatch_thread, display);
    pthread_attr_destroy(&attr);
    if (r != 0)
    {
        TRACE(ERROR, _x("thread creation failed: %m"));
        goto error_epoll;
    }

    /* ...wait until display thread starts? */
    TRACE(INIT, _b("Wayland display interface initialized"));

    /* ...initialize extra input devices */
    input_spacenav_init(display);

    /* ...joystick device requires start-up events generation (should be window-specific) */
    input_joystick_init(display, joystick_dev_name);
    
    /* ...doesn't look good, actually - don't want to start thread right here */
    return display;

error_epoll:
    /* ...close poll file-handle */
    close(display->efd);

error_disp:
    /* ...disconnect display */
    wl_display_flush(display->display);
    wl_display_disconnect(display->display);

error:
    return NULL;
}

/*******************************************************************************
 * Textures handling
 ******************************************************************************/

/* ...calculate cropping and viewport parameters for a texture */
void texture_set_view(texture_view_t *vcoord, float x0, float y0, float x1, float y1)
{
    float      *p;

    /* ...adjust coordinates to GL-space */
    x0 = x0 * 2 - 1, y0 = y0 * 2 - 1;
    x1 = x1 * 2 - 1, y1 = y1 * 2 - 1;

    /* ...fill-in vertex coordinates map */
    p = *vcoord;
    *p++ = x0, *p++ = y0;
    *p++ = x1, *p++ = y0;
    *p++ = x0, *p++ = y1;
    *p++ = x0, *p++ = y1;
    *p++ = x1, *p++ = y0;
    *p++ = x1, *p++ = y1;
}

/* ...set texture cropping data */
void texture_set_crop(texture_crop_t *tcoord, float x0, float y0, float x1, float y1)
{
    float      *p = (float *)tcoord;

    /* ...fill-in texture coordinates */
    *p++ = x0, *p++ = y1;
    *p++ = x1, *p++ = y1;
    *p++ = x0, *p++ = y0;
    *p++ = x0, *p++ = y0;
    *p++ = x1, *p++ = y1;
    *p++ = x1, *p++ = y0;
}

/* ...scale texture to fill particular image area */
void texture_set_view_scale(texture_view_t *vcoord, int x, int y, int w, int h, int W, int H, int width, int height)
{
    float   x0 = (float)x / W, x1 = (float)(x + w) / W;
    float   y0 = (float)y / H, y1 = (float)(y + h) / H;
    int     t0 = height * w;
    int     t1 = width * h;
    int     t = t0 - t1;
    float   f;

    if (t > 0)
    {
        /* ...texture fills the area vertically */
        f = (0.5 * (x1 - x0) * t) / t0;

        texture_set_view(vcoord, x0 + f, y0, x1 - f, y1);
    }
    else
    {
        /* ...texture fills the window horizontally */
        f = (-0.5 * (y1 - y0) * t) / t1;

        texture_set_view(vcoord, x0, y0 + f, x1, y1 - f);
    }
}

static inline float texture_view_x0(texture_view_t *v)
{
    return ((*v)[0] + 1) / 2;    
}

static inline float texture_view_y0(texture_view_t *v)
{
    return (1 - (*v)[5]) / 2;    
}

static inline float texture_view_x1(texture_view_t *v)
{
    return ((*v)[2] + 1) / 2;    
}

static inline float texture_view_y1(texture_view_t *v)
{
    return (1 - (*v)[1]) / 2;
}

static inline float texture_view_width(texture_view_t *v)
{
    return ((*v)[2] - (*v)[0]) / 2;    
}

static inline float texture_view_height(texture_view_t *v)
{
    return ((*v)[5] - (*v)[1]) / 2;    
}

/* ...draw external texture in given view-port */
void texture_draw(texture_data_t *texture, texture_view_t *view, texture_crop_t *crop, float alpha)
{
    window_data_t      *window = pthread_getspecific(__key_window);

    if (view)
    {
        float      *p = *view;
        int         i;

        for (i = 0; i < 12; i += 2, p += 2)
        {
            TRACE(0, _b("view[%d] = (%.2f, %.2f)"), i / 2, p[0], p[1]);
        }
    }

    if (crop)
    {
        float      *p = *crop;
        int         i;

        for (i = 0; i < 12; i += 2, p += 2)
        {
            TRACE(0, _b("crop[%d] = (%.2f, %.2f)"), i / 2, p[0], p[1]);
        }
    }

    /* ...check if the viewport is empty (full screen) */
    if (!view)
    {
        /* ...attach buffer to the window surface */
        wl_surface_attach(window->surface, texture->wl_buffer, 0, 0);

        /* ...set scaling interface */
        wl_viewport_set_destination(window->viewport, window->widget.width, window->widget.height);

        /* ...invalidate entire surface data */
        wl_surface_damage(window->surface, 0, 0, window->widget.width, window->widget.height);

        TRACE(DEBUG, _b("wl-buffer %p attached: %d,%d,%d,%d"), texture->wl_buffer, 0, 0, window->widget.width, window->widget.height);
    }
    else
    {
        float   x = texture_view_x0(view);
        float   y = texture_view_y0(view);
        float   w = texture_view_width(view);
        float   h = texture_view_height(view);

        /* ...we have a viewport specified */
        wl_viewport_set_destination(window->viewport, w, h);
        wl_surface_attach(window->surface, texture->wl_buffer, x, y);
        wl_surface_damage(window->surface, x, y, w, h);

        /* ...we need to set up scaler, and set subsurface */
        TRACE(DEBUG, _b("only full-size drawing is supported"));
    }
}

/*******************************************************************************
 * DMA buffer creation / deletion interface
 ******************************************************************************/

/* ...buffer allocation callback */
static void linux_dmabuf_created(void *data, struct zlinux_buffer_params *params, struct wl_buffer *buffer)
{
    texture_data_t     *texture = data;

    /* ...save buffer handle */
    texture->wl_buffer = buffer;

    /* ...release buffer mutex to mark completion of the procedure */
    pthread_mutex_unlock(&texture->lock);
}

/* ...allocation failure callback */
static void linux_dmabuf_failed(void *data, struct zlinux_buffer_params *params)
{
    texture_data_t     *texture = data;

    /* ...mark we have no data available */
    texture->wl_buffer = NULL;

    /* ...complete buffer creation procedure */
    pthread_mutex_unlock(&texture->lock);
}

/* ...buffer creation callbacks */
static const struct zlinux_buffer_params_listener dmabuf_params_listener = {
    .created = linux_dmabuf_created,
    .failed = linux_dmabuf_failed,
};

/* ...translate V4L2 pixel-format into EGL-format */
static inline uint32_t __pixfmt_gst_to_drm(int format, int *n)
{
    switch (format)
    {
    case GST_VIDEO_FORMAT_ARGB:     return *n = 1, DRM_FORMAT_ARGB8888;
    case GST_VIDEO_FORMAT_RGB16:    return *n = 1, DRM_FORMAT_RGB565;
    case GST_VIDEO_FORMAT_NV16:     return *n = 2, DRM_FORMAT_NV16;
    case GST_VIDEO_FORMAT_NV12:     return *n = 2, DRM_FORMAT_NV12;
    case GST_VIDEO_FORMAT_UYVY:     return *n = 1, DRM_FORMAT_UYVY;
    case GST_VIDEO_FORMAT_YUY2:     return *n = 1, DRM_FORMAT_YUYV;
    case GST_VIDEO_FORMAT_YVYU:     return *n = 1, DRM_FORMAT_YVYU;
    case GST_VIDEO_FORMAT_GRAY8:    return *n = 1, DRM_FORMAT_R8;
    default:                        return TRACE(ERROR, _x("unsupported format: %d"), format), 0;
    }
}

/* ...texture creation for given set of DMA-file-descriptors */
texture_data_t * texture_create(int w, int h, int format, int *dmafd, unsigned *offset, unsigned *stride)
{
    display_data_t                 *display = &__display;
    texture_data_t                 *texture;
    struct zlinux_buffer_params    *params;
    int                             n, i;
    uint32_t                        fmt;

    /* ...map format to the internal value */
    CHK_ERR((fmt = __pixfmt_gst_to_drm(format, &n)) > 0, (errno = EINVAL, NULL));

    /* ...allocate texture data */
    CHK_ERR(texture = malloc(sizeof(*texture)), (errno = ENOMEM, NULL));

    /* ...prepare buffer allocation request */
    pthread_mutex_init(&texture->lock, NULL);
    pthread_mutex_lock(&texture->lock);
    params = zlinux_dmabuf_create_params(display->dmabuf);
    zlinux_buffer_params_add_listener(params, &dmabuf_params_listener, texture);
    
    /* ...add planes to the buffer */
    for (i = 0; i < n; i++)
    {
        zlinux_buffer_params_add(params, dmafd[i], i, offset[i], stride[i], 0, 0);
        (fmt == DRM_FORMAT_NV16 ? TRACE(1, _b("#%d: fd=%d, offset=%d, stride=%d"), i, dmafd[i], offset[i], stride[i]) : 0);
    }

    /* ...submit buffer creation request and wait for processing */
    zlinux_buffer_params_create(params, w, h, fmt, ZLINUX_BUFFER_PARAMS_FLAGS_Y_INVERT);
    wl_display_flush(display->display);

    /* ...wait for a buffer creation completion */
    pthread_mutex_lock(&texture->lock);
    zlinux_buffer_params_destroy(params);

    if (texture->wl_buffer != NULL)
    {
        TRACE(INFO, _b("buffer allocated: %d*%d@%d [fd=%d/%d/%d, %p]"), w, h, format, dmafd[0], dmafd[1], dmafd[2], texture->wl_buffer);
        return texture;
    }
    else
    {
        TRACE(ERROR, _b("failed to allocate buffer: %d*%d@%d [fd=%d/%d/%d]"), w, h, format, dmafd[0], dmafd[1], dmafd[2]);
        free(texture);
        return NULL;
    }
}

/* ...destroy texture data */
void texture_destroy(texture_data_t *texture)
{
    /* ...destroy wayland buffer */
    wl_buffer_destroy(texture->wl_buffer);

    /* ...destroy texture structure */
    free(texture);
}

/*******************************************************************************
 * Auxiliary frame-rate calculation functions
 ******************************************************************************/

/* ...reset FPS calculator */
void window_frame_rate_reset(window_data_t *window)
{
    /* ...reset accumulator and timestamp */
    window->fps_acc = 0, window->fps_ts = 0;
}

/* ...update FPS calculator */
float window_frame_rate_update(window_data_t *window)
{
    u32     ts_0, ts_1, delta, acc;
    float   fps;

    /* ...get current timestamp for a window frame-rate calculation */
    delta = (ts_1 = __get_time_usec()) - (ts_0 = window->fps_ts);

    /* ...check if accumulator is initialized */
    if ((acc = window->fps_acc) == 0)
    {
        if (ts_0 != 0)
        {
            /* ...initialize accumulator */
            acc = delta << 4;
        }
    }
    else
    {
        /* ...accumulator is setup already; do exponential averaging */
        acc += delta - ((acc + 8) >> 4);
    }

    /* ...calculate current frame-rate */
    if ((fps = (acc ? 1e+06 / ((acc + 8) >> 4) : 0)) != 0)
    {
        TRACE(DEBUG, _b("delta: %u, acc: %u, fps: %f"), delta, acc, fps);
    }

    /* ...update timestamp and accumulator values */
    window->fps_acc = acc, window->fps_ts = ts_1;

    return fps;
}
