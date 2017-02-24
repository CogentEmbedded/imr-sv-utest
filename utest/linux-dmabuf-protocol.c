/* 
 * Copyright © 2014, 2015 Collabora, Ltd.
 * 
 * Permission to use, copy, modify, distribute, and sell this
 * software and its documentation for any purpose is hereby granted
 * without fee, provided that the above copyright notice appear in
 * all copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of
 * the copyright holders not be used in advertising or publicity
 * pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 * 
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 * ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
 * THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

extern const struct wl_interface wl_buffer_interface;
extern const struct wl_interface zlinux_buffer_params_interface;

static const struct wl_interface *types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&zlinux_buffer_params_interface,
	&wl_buffer_interface,
};

static const struct wl_message zlinux_dmabuf_requests[] = {
	{ "destroy", "", types + 0 },
	{ "create_params", "n", types + 6 },
};

static const struct wl_message zlinux_dmabuf_events[] = {
	{ "format", "u", types + 0 },
};

WL_EXPORT const struct wl_interface zlinux_dmabuf_interface = {
	"zlinux_dmabuf", 1,
	2, zlinux_dmabuf_requests,
	1, zlinux_dmabuf_events,
};

static const struct wl_message zlinux_buffer_params_requests[] = {
	{ "destroy", "", types + 0 },
	{ "add", "huuuuu", types + 0 },
	{ "create", "iiuu", types + 0 },
};

static const struct wl_message zlinux_buffer_params_events[] = {
	{ "created", "n", types + 7 },
	{ "failed", "", types + 0 },
};

WL_EXPORT const struct wl_interface zlinux_buffer_params_interface = {
	"zlinux_buffer_params", 1,
	3, zlinux_buffer_params_requests,
	2, zlinux_buffer_params_events,
};

