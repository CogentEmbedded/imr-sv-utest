/*******************************************************************************
 * utest-app.h
 *
 * IMR unit-test application common definitions
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

#ifndef __UTEST_APP_H
#define __UTEST_APP_H

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include "utest-wl-display.h"
#include "utest-camera.h"
#include "utest-math.h"

/*******************************************************************************
 * Forward declarations
 ******************************************************************************/

typedef struct app_data   app_data_t;

/*******************************************************************************
 * Cameras mapping
 ******************************************************************************/

#define CAMERA_RIGHT                    0
#define CAMERA_LEFT                     1
#define CAMERA_FRONT                    2
#define CAMERA_REAR                     3

/* ...mapping of cameras into texture indices (the order if left/right/front/rear) */
static inline int camera_id(int i)
{
    return (i < 2 ? i ^ 1 : i);
}

static inline int camera_idx(int id)
{
    return (id < 2 ? id ^ 1 : id);
}

/*******************************************************************************
 * Global configuration options
 ******************************************************************************/

/* ...joystick device name */
extern char  *joystick_dev_name;

/* ...output devices for main window */
extern int __output_main;

/* ...VIN device names */
extern char * vin_dev_name[];

/* ...IMR device names */
extern char * imr_dev_name[];

/* ...mesh data (tbd - move to track configuration) */
extern char * __mesh_file_name;

/* ...camera format */
extern u32  __vin_format;

/* ...camera dimensions */
extern int  __vin_width, __vin_height;

/* ...number of buffers to allocate */
extern int  __vin_buffers_num;

/* ...output buffer dimensions */
extern int  __vsp_width, __vsp_height;

/* ...car buffer dimensions */
extern int  __car_width, __car_height;

/* ...car shadow region */
extern __vec4   __shadow_rect;

/* ...steps for rotation parameters */
extern int  __steps[];

/* ...model file prefix */
extern char * __model;

/*******************************************************************************
 * Public module API
 ******************************************************************************/

/* ...application data initialization */
extern app_data_t * app_init(display_data_t *display);

/* ...main application thread */
extern void * app_thread(void *arg);

#endif  /* __UTEST_APP_H */
