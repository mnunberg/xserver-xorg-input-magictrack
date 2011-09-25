/*
 * Copyright © 2004-2007 Peter Osterlund
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xorg-server.h>
#include "eventcomm.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include "synproto.h"
#include "synaptics.h"
#include "synapticsstr.h"
#include <xf86.h>
#include <xorg/xserver-properties.h>
#include <mtdev.h>
#include <grail.h>
#include "yolog.h"

YOLOG_STATIC_INIT("eventcomm.c", YOLOG_WARN);


#define SYSCALL(call) while (((call) == -1) && (errno == EINTR))

#define LONG_BITS (sizeof(long) * 8)
#define NBITS(x) (((x) + LONG_BITS - 1) / LONG_BITS)
#define OFF(x)   ((x) % LONG_BITS)
#define LONG(x)  ((x) / LONG_BITS)
#define TEST_BIT(bit, array) ((array[LONG(bit)] >> OFF(bit)) & 1)

#define SLOT_INACTIVE (uint32_t)-1
#define SCROLL_INACTIVE -1
#define scroll_2f_active(ecp) \
	((ecp->active_touches == 2 && ecp->depressed == FALSE ) \
			|| (ecp->active_touches == 3 && ecp->depressed == TRUE))

/*****************************************************************************
 *	Function Definitions
 ****************************************************************************/

static int
EventDevicePreInitHook(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    EventcommPrivate *ecpriv;
    struct input_absinfo abs;
    int rc;

    priv->proto_data = calloc(1, sizeof(EventcommPrivate));
    if (!priv->proto_data)
        return !Success;

    ecpriv = priv->proto_data;
    ecpriv->need_grab = TRUE;
    ecpriv->num_touches = 10;
    ecpriv->cur_slot = -1;

    SYSCALL(rc = ioctl(pInfo->fd, EVIOCGABS(ABS_MT_SLOT), &abs));
    if (rc >= 0 && abs.maximum > 0)
        ecpriv->num_touches = abs.maximum + 1;
    else {
        SYSCALL(rc = ioctl(pInfo->fd, EVIOCGABS(ABS_MT_TRACKING_ID), &abs));
        if (rc >= 0 && abs.maximum > 0)
            ecpriv->num_touches = abs.maximum + 1;
    }
        
    return Success;
}

static Bool
EventDeviceInitHook(DeviceIntPtr dev)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;
    int i;

    if (!priv->has_touch)
        return Success;

    ecpriv->slot_info = malloc(ecpriv->num_touches * sizeof(SynapticsFinger));
    if (!ecpriv->slot_info)
        goto err;

    memset(ecpriv->slot_info, 0, sizeof(SynapticsFinger));

    for (i = 0; i < ecpriv->num_touches; i++) {
        ecpriv->slot_info[i].tracking_id = -1;
    }

    ecpriv->touch_mask = valuator_mask_new(ecpriv->num_mt_axes);
    ecpriv->cur_vals = valuator_mask_new(ecpriv->num_mt_axes);
    if (!ecpriv->touch_mask || !ecpriv->cur_vals)
        goto err;

    if (!InitTouchClassDeviceStruct(pInfo->dev, ecpriv->num_touches,
                                    XIDependentTouch, ecpriv->num_mt_axes))
        goto err;

    for (i = ABS_MT_TOUCH_MAJOR; i <= ABS_MT_DISTANCE; i++) {
        int axnum, resolution = 10000;
        char *name;
        Atom atom;
        static char * const names[] = {
            AXIS_LABEL_PROP_ABS_MT_TOUCH_MAJOR,
            AXIS_LABEL_PROP_ABS_MT_TOUCH_MINOR,
            AXIS_LABEL_PROP_ABS_MT_WIDTH_MAJOR,
            AXIS_LABEL_PROP_ABS_MT_WIDTH_MINOR,
            AXIS_LABEL_PROP_ABS_MT_ORIENTATION,
            AXIS_LABEL_PROP_ABS_MT_POSITION_X,
            AXIS_LABEL_PROP_ABS_MT_POSITION_Y,
            AXIS_LABEL_PROP_ABS_MT_TOOL_TYPE,
            AXIS_LABEL_PROP_ABS_MT_BLOB_ID,
            AXIS_LABEL_PROP_ABS_MT_TRACKING_ID,
            AXIS_LABEL_PROP_ABS_MT_PRESSURE,
        };

        if (!BitIsOn(ecpriv->absbits, i))
            continue;

        axnum = ecpriv->mt_axis_map[i - ABS_MT_TOUCH_MAJOR];
        name = names[i - ABS_MT_TOUCH_MAJOR];

        if (ecpriv->absinfo[i].resolution)
            resolution = ecpriv->absinfo[i].resolution * 1000;

        atom = MakeAtom(name, strlen(name), TRUE);

        xf86InitTouchValuatorAxisStruct(pInfo->dev, axnum, atom,
                                        ecpriv->absinfo[i].minimum,
                                        ecpriv->absinfo[i].maximum,
                                        ecpriv->absinfo[i].resolution);
    }

    return Success;

err:
    free(ecpriv->slot_info);
    ecpriv->slot_info = NULL;
    free(ecpriv->cur_vals);
    ecpriv->cur_vals = NULL;
    free(ecpriv->touch_mask);
    ecpriv->touch_mask = NULL;
    return !Success;
}

static void
EventDeviceOnHook(InputInfoPtr pInfo, SynapticsParameters *para)
{
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;
    struct input_absinfo abs;
    int rc;

    if (para->grab_event_device) {
	/* Try to grab the event device so that data don't leak to /dev/input/mice */
	int ret;
	SYSCALL(ret = ioctl(pInfo->fd, EVIOCGRAB, (pointer)1));
	if (ret < 0) {
	    xf86Msg(X_WARNING, "%s can't grab event device, errno=%d\n",
		    pInfo->name, errno);
	}
    }

    ecpriv->need_grab = FALSE;

    if (priv->has_touch) {
        SYSCALL(rc = ioctl(pInfo->fd, EVIOCGABS(ABS_MT_SLOT), &abs));
        if (rc >= 0)
            ecpriv->cur_slot = abs.value;
    }

    GrailOpen(pInfo);
}

static void
EventDeviceOffHook(InputInfoPtr pInfo)
{
    GrailClose(pInfo);
}

static void
EventDeviceCloseHook(DeviceIntPtr dev)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    SynapticsPrivate *priv = (SynapticsPrivate *) pInfo->private;
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;

    free(ecpriv->slot_info);
    free(ecpriv->cur_vals);
    free(ecpriv->touch_mask);
    free(ecpriv);
    priv->proto_data = NULL;
}

static Bool
event_query_is_touchpad(int fd, BOOL grab)
{
    int ret = FALSE, rc;
    unsigned long evbits[NBITS(EV_MAX)] = {0};
    unsigned long absbits[NBITS(ABS_MAX)] = {0};
    unsigned long keybits[NBITS(KEY_MAX)] = {0};

    if (grab)
    {
        SYSCALL(rc = ioctl(fd, EVIOCGRAB, (pointer)1));
        if (rc < 0)
            return FALSE;
    }

    /* Check for ABS_X, ABS_Y, ABS_PRESSURE and BTN_TOOL_FINGER */

    SYSCALL(rc = ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits));
    if (rc < 0)
	goto unwind;
    if (!TEST_BIT(EV_SYN, evbits) ||
	!TEST_BIT(EV_ABS, evbits) ||
	!TEST_BIT(EV_KEY, evbits))
	goto unwind;

    SYSCALL(rc = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits));
    if (rc < 0)
	goto unwind;
    if (!TEST_BIT(ABS_X, absbits) ||
	!TEST_BIT(ABS_Y, absbits))
	goto unwind;

    SYSCALL(rc = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits));
    if (rc < 0)
	goto unwind;

    /* we expect touchpad either report raw pressure or touches */
    if (!TEST_BIT(ABS_PRESSURE, absbits) && !TEST_BIT(BTN_TOUCH, keybits))
	goto unwind;
    /* all Synaptics-like touchpad report BTN_TOOL_FINGER */
    if (!TEST_BIT(BTN_TOOL_FINGER, keybits))
	goto unwind;
    if (TEST_BIT(BTN_TOOL_PEN, keybits))
	goto unwind;			    /* Don't match wacom tablets */

    ret = TRUE;

unwind:
    if (grab)
        SYSCALL(ioctl(fd, EVIOCGRAB, (pointer)0));

    return (ret == TRUE);
}

typedef struct {
	short vendor;
	short product;
	enum TouchpadModel model;
} model_lookup_t;
#define PRODUCT_ANY 0x0000

static model_lookup_t model_lookup_table[] = {
	{0x0002, 0x0007, MODEL_SYNAPTICS},
	{0x0002, 0x0008, MODEL_ALPS},
	{0x05ac, PRODUCT_ANY, MODEL_APPLETOUCH},
	{0x0, 0x0, 0x0}
};

static void
event_query_info(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    short id[4];
    int rc;
    model_lookup_t *model_lookup;

    SYSCALL(rc = ioctl(pInfo->fd, EVIOCGID, id));
    if (rc < 0)
        return;

    for(model_lookup = model_lookup_table; model_lookup->vendor; model_lookup++) {
        if(model_lookup->vendor == id[ID_VENDOR] &&
           (model_lookup->product == id[ID_PRODUCT] || model_lookup->product == PRODUCT_ANY))
            priv->model = model_lookup->model;
    }
}

/* Query device for axis ranges */
static void
event_query_axis_ranges(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;
    struct input_absinfo abs = {0};
    unsigned long keybits[NBITS(KEY_MAX)] = {0};
    char buf[256];
    int i, rc;
    uint8_t prop;

    memset(ecpriv->absbits, 0, sizeof(ecpriv->absbits));

    SYSCALL(rc = ioctl(pInfo->fd, EVIOCGABS(ABS_X), &abs));
    if (rc >= 0)
    {
	xf86Msg(X_PROBED, "%s: x-axis range %d - %d\n", pInfo->name,
		abs.minimum, abs.maximum);
	priv->minx = abs.minimum;
	priv->maxx = abs.maximum;
	/* The kernel's fuzziness concept seems a bit weird, but it can more or
	 * less be applied as hysteresis directly, i.e. no factor here. Though,
	 * we don't trust a zero fuzz as it probably is just a lazy value. */
	if (abs.fuzz > 0)
	    priv->synpara.hyst_x = abs.fuzz;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,30)
	priv->resx = abs.resolution;
#endif
    } else
	xf86Msg(X_ERROR, "%s: failed to query axis range (%s)\n", pInfo->name,
		strerror(errno));

    SYSCALL(rc = ioctl(pInfo->fd, EVIOCGABS(ABS_Y), &abs));
    if (rc >= 0)
    {
	xf86Msg(X_PROBED, "%s: y-axis range %d - %d\n", pInfo->name,
		abs.minimum, abs.maximum);
	priv->miny = abs.minimum;
	priv->maxy = abs.maximum;
	/* don't trust a zero fuzz */
	if (abs.fuzz > 0)
	    priv->synpara.hyst_y = abs.fuzz;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,30)
	priv->resy = abs.resolution;
#endif
    } else
	xf86Msg(X_ERROR, "%s: failed to query axis range (%s)\n", pInfo->name,
		strerror(errno));

    priv->has_pressure = FALSE;
    priv->has_width = FALSE;
    SYSCALL(rc = ioctl(pInfo->fd, EVIOCGBIT(EV_ABS, sizeof(ecpriv->absbits)),
                       ecpriv->absbits));
    if (rc >= 0)
    {
	priv->has_pressure = (TEST_BIT(ABS_PRESSURE, ecpriv->absbits) != 0);
	priv->has_width = (TEST_BIT(ABS_TOOL_WIDTH, ecpriv->absbits) != 0);
    }
    else
	xf86Msg(X_ERROR, "%s: failed to query ABS bits (%s)\n", pInfo->name,
		strerror(errno));

    if (priv->has_pressure)
    {
	SYSCALL(rc = ioctl(pInfo->fd, EVIOCGABS(ABS_PRESSURE), &abs));
	if (rc >= 0)
	{
	    xf86Msg(X_PROBED, "%s: pressure range %d - %d\n", pInfo->name,
		    abs.minimum, abs.maximum);
	    priv->minp = abs.minimum;
	    priv->maxp = abs.maximum;
	}
    } else
	xf86Msg(X_INFO,
		"%s: device does not report pressure, will use touch data.\n",
		pInfo->name);

    if (priv->has_width)
    {
	SYSCALL(rc = ioctl(pInfo->fd, EVIOCGABS(ABS_TOOL_WIDTH), &abs));
	if (rc >= 0)
	{
	    xf86Msg(X_PROBED, "%s: finger width range %d - %d\n", pInfo->name,
		    abs.minimum, abs.maximum);
	    priv->minw = abs.minimum;
	    priv->maxw = abs.maximum;
	}
    }

    SYSCALL(rc = ioctl(pInfo->fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits));
    if (rc >= 0)
    {
	buf[0] = 0;
	if ((priv->has_left = (TEST_BIT(BTN_LEFT, keybits) != 0)))
	   strcat(buf, " left");
	if ((priv->has_right = (TEST_BIT(BTN_RIGHT, keybits) != 0)))
	   strcat(buf, " right");
	if ((priv->has_middle = (TEST_BIT(BTN_MIDDLE, keybits) != 0)))
	   strcat(buf, " middle");
	if ((priv->has_double = (TEST_BIT(BTN_TOOL_DOUBLETAP, keybits) != 0)))
	   strcat(buf, " double");
	if ((priv->has_triple = (TEST_BIT(BTN_TOOL_TRIPLETAP, keybits) != 0)))
	   strcat(buf, " triple");

	if ((TEST_BIT(BTN_0, keybits) != 0) ||
	    (TEST_BIT(BTN_1, keybits) != 0) ||
	    (TEST_BIT(BTN_2, keybits) != 0) ||
	    (TEST_BIT(BTN_3, keybits) != 0))
	{
	    priv->has_scrollbuttons = 1;
	    strcat(buf, " scroll-buttons");
	}

	xf86Msg(X_PROBED, "%s: buttons:%s\n", pInfo->name, buf);
    }

    for (i = ABS_MT_TOUCH_MAJOR; i <= ABS_MT_PRESSURE; i++) {
	if (!BitIsOn(ecpriv->absbits, i))
            continue;

        SYSCALL(rc = ioctl(pInfo->fd, EVIOCGABS(i), &ecpriv->absinfo[i]));
        if (rc < 0) {
            ClearBit(ecpriv->absbits, i);
            continue;
        }

        ecpriv->mt_axis_map[i - ABS_MT_TOUCH_MAJOR] = ecpriv->num_mt_axes++;
        priv->has_touch = TRUE;
    }

    if (priv->has_touch) {
        /* We don't support SemiMultitouch devices yet. */
        SYSCALL(rc = ioctl(pInfo->fd, EVIOCGPROP(sizeof(prop)), &prop));
        if (rc >= 0 && (prop & INPUT_PROP_SEMI_MT))
            ecpriv->semi_mt = TRUE;
        else
            ecpriv->semi_mt = FALSE;
    }

}

static Bool
EventQueryHardware(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;

    if (!event_query_is_touchpad(pInfo->fd, ecpriv->need_grab))
	return FALSE;

    xf86Msg(X_PROBED, "%s: touchpad found\n", pInfo->name);

    return TRUE;
}

static void
ProcessTouch(InputInfoPtr pInfo, SynapticsPrivate *priv)
{
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;
    SynapticsFinger *slotp = &(ecpriv->slot_info[ecpriv->cur_slot]);

    if (!priv->has_touch || ecpriv->cur_slot < 0 || slotp->tracking_id == SLOT_INACTIVE)
    {
        if (ecpriv->touch_mask) {
            valuator_mask_zero(ecpriv->touch_mask);
        }
        return;
    }

    if (ecpriv->close_slot)
    {
        if ( (!ecpriv->semi_mt) && slotp->has_touch_event) {
            xf86PostTouchEvent(pInfo->dev,
                               slotp->tracking_id,
                               XI_TouchEnd, 0, ecpriv->touch_mask);
            slotp->has_touch_event = FALSE;
        }
        slotp->tracking_id = SLOT_INACTIVE;
        ecpriv->close_slot = FALSE;
        ecpriv->active_touches--;
    }
    else
    {
        if (ecpriv->new_touch)
        {
            int x_axis = ecpriv->mt_axis_map[ABS_MT_POSITION_X - ABS_MT_TOUCH_MAJOR];
            int y_axis = ecpriv->mt_axis_map[ABS_MT_POSITION_Y - ABS_MT_TOUCH_MAJOR];
            int x = valuator_mask_get(ecpriv->touch_mask, x_axis);
            int y = valuator_mask_get(ecpriv->touch_mask, y_axis);

            if ((!ecpriv->semi_mt && is_inside_active_area(priv, x, y)) ||
                (ecpriv->semi_mt &&
                 (is_inside_active_area(priv, ecpriv->min_x, ecpriv->min_y) &&
                  (ecpriv->active_touches == 0 ||
                   is_inside_active_area(priv, ecpriv->max_x, ecpriv->max_y)))))
            {
                if (!ecpriv->semi_mt) {
                	if(!ecpriv->depressed) {
                		xf86PostTouchEvent(pInfo->dev,
                				slotp->tracking_id,
                				XI_TouchBegin, 0, ecpriv->touch_mask);
                		slotp->has_touch_event = TRUE;
                	}
                    ecpriv->active_touches++;
                }
                else {
                	slotp->tracking_id = SLOT_INACTIVE;
                }
            }
        }
		else if ( (!ecpriv->semi_mt) && slotp->has_touch_event )
		{
			xf86PostTouchEvent(pInfo->dev,
					slotp->tracking_id,
					XI_TouchUpdate, 0, ecpriv->touch_mask);
		}

        ecpriv->new_touch = FALSE;
    }

    valuator_mask_zero(ecpriv->touch_mask);
}

int GDB_watchpoint_hinter = 0;
static void ProcessPosition(EventcommPrivate *ecpriv,
		const struct input_event *ev,
		struct SynapticsHwState *hw, SynapticsFinger *slotp)
{
	int *posptr, *contact_axis;
	SynapticsMetric m;
	if(ev->code == ABS_MT_POSITION_X) {
		m = SYNMETRIC_X;
		posptr = &(hw->x);
	} else {
		m = SYNMETRIC_Y;
		posptr = &(hw->y);
	}
	contact_axis = &(slotp->metric[m]);
	*contact_axis = ev->value;

	valuator_mask_set(ecpriv->touch_mask,
					  ecpriv->mt_axis_map[ev->code - ABS_MT_TOUCH_MAJOR],
					  ev->value);
	valuator_mask_set(ecpriv->cur_vals,
					  ecpriv->mt_axis_map[ev->code - ABS_MT_TOUCH_MAJOR],
					  ev->value);


	char *strax = (ev->code == ABS_MT_POSITION_X) ? "X" : "Y"; //debug stuff

	if(ecpriv->cur_slot == ecpriv->pressing_slot
			&& ecpriv->depressed
			&& ecpriv->active_touches >= 2)
	{
    	yolog_debug("S=%2d %s=%6d. BLOCKED", ecpriv->cur_slot, strax, ev->value);
    	return;
	}

//	yolog_warn("S=%2d %s=%6d. SENDING", ecpriv->cur_slot, strax, ev->value);
	if(ecpriv->depressed && ecpriv->active_touches >= 2) {
		GDB_watchpoint_hinter = !GDB_watchpoint_hinter;
	}

	if(ecpriv->cur_slot != ecpriv->last_sender) {
//		yolog_warn("New Sender here");
		hw->new_coords = TRUE;
		hw->x = slotp->metric[SYNMETRIC_X];
		hw->y = slotp->metric[SYNMETRIC_Y];

	} else {
	    hw->new_coords = FALSE;
	}
	ecpriv->last_sender = ecpriv->cur_slot;

	*posptr = ev->value;

	if(scroll_2f_active(ecpriv)) {
		/*If we have two finger scrolling on, set the fingers.*/
		if(ecpriv->first_2f_scrollid == SCROLL_INACTIVE) {
			ecpriv->first_2f_scrollid = ecpriv->cur_slot;
			hw->scroll_fingers[0] = slotp;
			hw->scroll_fingers[1] = NULL;
			ecpriv->second_2f_scrollid = SCROLL_INACTIVE;
		} else if (ecpriv->second_2f_scrollid == SCROLL_INACTIVE &&
				ecpriv->first_2f_scrollid != ecpriv->cur_slot) {
			ecpriv->second_2f_scrollid = ecpriv->cur_slot;
			hw->scroll_fingers[1] = slotp;
		}
		int fidx = (ecpriv->cur_slot == ecpriv->first_2f_scrollid) ? 0 : 1;
		hw->scroll_pass[m][fidx] = TRUE;
	} else {
		memset(hw->scroll_pass, 0, sizeof(hw->scroll_pass));

		/*2f-scroll off. Reset*/
		ecpriv->first_2f_scrollid = ecpriv->second_2f_scrollid = SCROLL_INACTIVE;
		hw->scroll_fingers[0] = hw->scroll_fingers[1] = NULL;
	}
}

static Bool
SynapticsReadEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    SynapticsPrivate *priv = (SynapticsPrivate *) (pInfo->private);
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;
    int rc = TRUE;
    ssize_t len;
//    ecpriv->grail = NULL;
    int use_grail = (ecpriv->grail != NULL);
    if (use_grail) {
        len = grail_pull(ecpriv->grail, pInfo->fd);
    }
    else {
        len = read(pInfo->fd, ev, sizeof(*ev));
    }
    if (len <= 0)
    {
        /* We use X_NONE here because it doesn't alloc */
        if (errno != EAGAIN)
            xf86MsgVerb(X_NONE, 0, "%s: Read error %s\n", pInfo->name, strerror(errno));
        rc = FALSE;
    } else if (use_grail)
        rc = FALSE;
    else if (len % sizeof(*ev)) {
        xf86MsgVerb(X_NONE, 0, "%s: Read error, invalid number of bytes.", pInfo->name);
        rc = FALSE;
    }
    return rc;
}


Bool
EventProcessEvent(InputInfoPtr pInfo, struct CommData *comm,
                  struct SynapticsHwState *hwRet, const struct input_event *ev)
{
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;
    SynapticsParameters *para = &priv->synpara;
    struct SynapticsHwState *hw = &(comm->hwState);
    Bool v;
    Bool ret = FALSE;
    SynapticsFinger *slotp;
    SynapticsFinger stack_dummy;

    if(hw->new_eventset) {
    	/*Reset scroll data*/
    	hw->new_eventset = FALSE;
    	memset(hw->scroll_pass, 0, sizeof(hw->scroll_pass));

    	if(ecpriv->first_2f_scrollid == SCROLL_INACTIVE) {
    		hw->scroll_fingers[0] = NULL;
    	}
    	if(ecpriv->second_2f_scrollid == SCROLL_INACTIVE) {
    		hw->scroll_fingers[1] = NULL;
    	}
    }

    if(ecpriv->cur_slot >= 0) {
    	slotp = &(ecpriv->slot_info[ecpriv->cur_slot]);
    } else {
    	slotp = &stack_dummy;
    }


    switch (ev->type) {
    case EV_SYN:
        switch (ev->code) {
        case SYN_REPORT:
            ProcessTouch(pInfo, priv);
            if (priv->has_touch && ecpriv->active_touches < 2)
                hw->numFingers = ecpriv->active_touches;
            else if (comm->oneFinger)
                hw->numFingers = 1;
            else if (comm->twoFingers)
                hw->numFingers = 2;
            else if (comm->threeFingers)
                hw->numFingers = 3;
            else
                hw->numFingers = 0;
            *hwRet = *hw;
            ret = TRUE;
        }
    case EV_KEY:
        v = (ev->value ? TRUE : FALSE);
        switch (ev->code) {
        case BTN_LEFT:
            hw->left = v;
            ecpriv->depressed = v;
            ecpriv->pressing_slot = ecpriv->cur_slot;
            if(v == TRUE) {
            	hw->pressing_finger = slotp;
            } else {
            	hw->pressing_finger = NULL;
            }
            break;
        case BTN_RIGHT:
            hw->right = v;
            break;
        case BTN_MIDDLE:
            hw->middle = v;
            break;
        case BTN_FORWARD:
            hw->up = v;
            break;
        case BTN_BACK:
            hw->down = v;
            break;
        case BTN_0:
            hw->multi[0] = v;
            break;
        case BTN_1:
            hw->multi[1] = v;
            break;
        case BTN_2:
            hw->multi[2] = v;
            break;
        case BTN_3:
            hw->multi[3] = v;
            break;
        case BTN_4:
            hw->multi[4] = v;
            break;
        case BTN_5:
            hw->multi[5] = v;
            break;
        case BTN_6:
            hw->multi[6] = v;
            break;
        case BTN_7:
            hw->multi[7] = v;
            break;
        case BTN_TOOL_FINGER:
            comm->oneFinger = v;
            break;
        case BTN_TOOL_DOUBLETAP:
            comm->twoFingers = v;
            break;
        case BTN_TOOL_TRIPLETAP:
            comm->threeFingers = v;
            break;
        case BTN_TOUCH:
            if (!priv->has_pressure)
                    hw->z = v ? para->finger_high + 1 : 0;
            break;
        }
        break;
    case EV_ABS:
        switch (ev->code) {
        case ABS_PRESSURE:
            hw->z = ev->value;
            break;
        case ABS_TOOL_WIDTH:
            hw->fingerWidth = ev->value;
            break;
        case ABS_MT_SLOT:
//        	DBG(7, "Switching slot to %d\n", ev->value);
            if (priv->has_touch)
            {
                ProcessTouch(pInfo, priv);
                ecpriv->cur_slot = ev->value;
            }
            break;
        case ABS_MT_TRACKING_ID:
            if (ecpriv->cur_slot < 0)
                break;
            if (ev->value >= 0)
            {
                if (slotp->tracking_id != SLOT_INACTIVE)
                {
                    xf86Msg(X_WARNING, "%s: Ignoring new tracking ID for "
                            "existing touch.\n", pInfo->dev->name);
                }
                else
                {
                	slotp->finger_id = ecpriv->cur_slot;
                	slotp->tracking_id = ecpriv->next_tracking_id++;
                    ecpriv->new_touch = TRUE;
                    valuator_mask_copy(ecpriv->touch_mask,
                                       ecpriv->cur_vals);
                }
            }
            else if (slotp->tracking_id != SLOT_INACTIVE) {
            	slotp->finger_id = -1;
                ecpriv->close_slot = TRUE;
                if(ecpriv->last_sender == ecpriv->cur_slot) {
                	ecpriv->last_sender = -1;
                }
            }
            break;
        case ABS_MT_TOUCH_MAJOR:
        case ABS_MT_TOUCH_MINOR:
        case ABS_MT_WIDTH_MAJOR:
        case ABS_MT_WIDTH_MINOR:
        case ABS_MT_ORIENTATION:
        case ABS_MT_TOOL_TYPE:
        case ABS_MT_BLOB_ID:
        case ABS_MT_PRESSURE:
            if (ecpriv->cur_slot >= 0)
            {
                valuator_mask_set(ecpriv->touch_mask,
                                  ecpriv->mt_axis_map[ev->code - ABS_MT_TOUCH_MAJOR],
                                  ev->value);
                valuator_mask_set(ecpriv->cur_vals,
                                  ecpriv->mt_axis_map[ev->code - ABS_MT_TOUCH_MAJOR],
                                  ev->value);
            }
            break;
        case ABS_MT_POSITION_X:
        case ABS_MT_POSITION_Y: {
        	if(ecpriv->cur_slot < 0) {
        		break;
        	}
        	ProcessPosition(ecpriv, ev, hw, slotp);
        }

    } /*switch(ev->code)*/
    } /*switch(ev->type)*/

    if(ret == TRUE) {
    	/*Signal that the next update of the hw struct is part of a new eventset*/
    	hw->new_eventset = TRUE;
    }
    return ret;
}

static Bool
EventReadHwState(InputInfoPtr pInfo,
		 struct SynapticsProtocolOperations *proto_ops,
		 struct CommData *comm, struct SynapticsHwState *hwRet)
{
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;
    struct input_event ev;

    while (SynapticsReadEvent(pInfo, &ev)) {
        if (EventProcessEvent(pInfo, comm, hwRet, &ev) && !ecpriv->grail)
            return TRUE;
    }
    return FALSE;
}

/* filter for the AutoDevProbe scandir on /dev/input */
static int EventDevOnly(const struct dirent *dir) {
	return strncmp(EVENT_DEV_NAME, dir->d_name, 5) == 0;
}

/**
 * Probe the open device for dimensions.
 */
static void
EventReadDevDimensions(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    EventcommPrivate *ecpriv = (EventcommPrivate *)priv->proto_data;

    if (event_query_is_touchpad(pInfo->fd, ecpriv->need_grab))
	event_query_axis_ranges(pInfo);
    event_query_info(pInfo);
}

static Bool
EventAutoDevProbe(InputInfoPtr pInfo)
{
    /* We are trying to find the right eventX device or fall back to
       the psaux protocol and the given device from XF86Config */
    int i;
    Bool touchpad_found = FALSE;
    struct dirent **namelist;

    i = scandir(DEV_INPUT_EVENT, &namelist, EventDevOnly, alphasort);
    if (i < 0) {
		xf86Msg(X_ERROR, "Couldn't open %s\n", DEV_INPUT_EVENT);
		return FALSE;
    }
    else if (i == 0) {
		xf86Msg(X_ERROR, "%s The /dev/input/event* device nodes seem to be missing\n",
				pInfo->name);
		free(namelist);
		return FALSE;
    }

    while (i--) {
		char fname[64];
		int fd = -1;

		if (!touchpad_found) {
			sprintf(fname, "%s/%s", DEV_INPUT_EVENT, namelist[i]->d_name);
			SYSCALL(fd = open(fname, O_RDONLY));
			if (fd < 0)
				continue;

			if (event_query_is_touchpad(fd, TRUE)) {
				touchpad_found = TRUE;
			    xf86Msg(X_PROBED, "%s auto-dev sets device to %s\n",
				    pInfo->name, fname);
			    pInfo->options =
			    	xf86ReplaceStrOption(pInfo->options, "Device", fname);
			}
			SYSCALL(close(fd));
		}
		free(namelist[i]);
    }
	free(namelist);

	if (!touchpad_found) {
		xf86Msg(X_ERROR, "%s no synaptics event device found\n", pInfo->name);
		return FALSE;
	}
    return TRUE;
}

struct SynapticsProtocolOperations event_proto_operations = {
    EventDevicePreInitHook,
    EventDeviceInitHook,
    EventDeviceOnHook,
    EventDeviceOffHook,
    EventDeviceCloseHook,
    EventQueryHardware,
    EventReadHwState,
    EventAutoDevProbe,
    EventReadDevDimensions
};
