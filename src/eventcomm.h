/*
 * Copyright © 2004 Peter Osterlund
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *      Peter Osterlund (petero2@telia.com)
 */

#ifndef _EVENTCOMM_H_
#define _EVENTCOMM_H_

#include <linux/input.h>
#include <linux/version.h>
#include <X11/Xdefs.h>
#include <xorg/input.h>
#include <stdint.h>
#include <xorg/xf86Xinput.h>
#include "synproto.h"

/* for auto-dev: */
#define DEV_INPUT_EVENT "/dev/input"
#define EVENT_DEV_NAME "event"

#define HIST_SLOT_MAX 5

struct mtdev;
struct grail;

typedef struct {
    BOOL need_grab;
    unsigned long absbits[ABS_CNT];
    struct input_absinfo absinfo[ABS_CNT];
    int mt_axis_map[ABS_MT_DISTANCE - ABS_MT_TOUCH_MAJOR];
    int cur_slot;
    uint32_t *mt_slot_map;
    Bool close_slot;
    uint32_t next_touch_id;
    ValuatorMask *touch_mask;
    ValuatorMask *cur_vals;
    Bool new_touch;
    int num_mt_axes;
    int num_touches;
    struct mtdev *mtdev;
    struct grail *grail;
    int active_touches;
    Bool semi_mt;
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int contact_x[HIST_SLOT_MAX];
    int contact_y[HIST_SLOT_MAX];
} EventcommPrivate;

extern Bool EventProcessEvent(InputInfoPtr pInfo, struct CommData *comm,
                              struct SynapticsHwState *hwRet,
                              const struct input_event *ev);
extern int GrailOpen(InputInfoPtr pInfo);
extern void GrailClose(InputInfoPtr pInfo);

#endif /* _EVENTCOMM_H_ */
