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

#ifndef _SYNPROTO_H_
#define _SYNPROTO_H_

#include <unistd.h>
#include <sys/ioctl.h>
#include <xf86Xinput.h>
#include <xisb.h>

/*A structure to keep complete history of scroll movements for each of
 * the two scroll fingers
 */

typedef enum  {
	MOTION_VERT_UNKNOWN = 0,
	MOTION_VERT_UP,
	MOTION_VERT_DOWN,
} SynapticsVertDirection;

typedef enum  {
	MOTION_HORIZ_UNKNOWN = 0,
	MOTION_HORIZ_LEFT,
	MOTION_HORIZ_RIGHT
} SynapticsHorizDirection;

typedef struct {
	/*Coordinates*/
	int x;
	int y;
	int finger_id; /*Slot ID*/
	uint32_t tracking_id;
	Bool has_touch_event;
} SynapticsFinger;

/*
 * A structure to describe the state of the touchpad hardware (buttons and pad)
 */
struct SynapticsHwState {
    int millis;			/* Timestamp in milliseconds */
    int x;			/* X position of finger */
    int y;			/* Y position of finger */
    int z;			/* Finger pressure */
    int numFingers;
    int fingerWidth;

    Bool left;
    Bool right;
    Bool up;
    Bool down;

    Bool multi[8];
    Bool middle;		/* Some ALPS touchpads have a middle button */

    SynapticsFinger *scroll_fingers[2];
    SynapticsFinger *pressing_finger; /*Which finger is currently holding the mouse*/
    Bool scroll_pass_x[2];	/*Indexed by finger. Whether we updated X in this resultset*/
    Bool scroll_pass_y[2];	/*Same, but for Y*/

    Bool new_coords;	/*If we want to restart mapping here*/
    Bool new_eventset;	/*False if we are in the middle of a partial update*/
};

struct CommData {
    XISBuffer *buffer;
    unsigned char protoBuf[6];		/* Buffer for Packet */
    unsigned char lastByte;		/* Last read byte. Use for reset sequence detection. */
    int outOfSync;			/* How many consecutive incorrect packets we
					   have received */
    int protoBufTail;

    /* Used for keeping track of partial HwState updates. */
    struct SynapticsHwState hwState;
    Bool oneFinger;
    Bool twoFingers;
    Bool threeFingers;
};

enum SynapticsProtocol {
    SYN_PROTO_PSAUX,		/* Raw psaux device */
#ifdef BUILD_EVENTCOMM
    SYN_PROTO_EVENT,		/* Linux kernel event interface */
#endif /* BUILD_EVENTCOMM */
#ifdef BUILD_PSMCOMM
    SYN_PROTO_PSM,		/* FreeBSD psm driver */
#endif /* BUILD_PSMCOMM */
    SYN_PROTO_ALPS		/* ALPS touchpad protocol */
};

struct _SynapticsParameters;
struct SynapticsHwInfo;
struct CommData;
struct _SynapticsPrivateRec;

struct SynapticsProtocolOperations {
    int (*DevicePreInitHook)(InputDriverPtr drv, InputInfoPtr pInfo, int flags);
    Bool (*DeviceInitHook)(DeviceIntPtr dev);
    void (*DeviceOnHook)(InputInfoPtr pInfo, struct _SynapticsParameters *para);
    void (*DeviceOffHook)(InputInfoPtr pInfo);
    void (*DeviceCloseHook)(DeviceIntPtr dev);
    Bool (*QueryHardware)(InputInfoPtr pInfo);
    Bool (*ReadHwState)(InputInfoPtr pInfo,
			struct SynapticsProtocolOperations *proto_ops,
			struct CommData *comm, struct SynapticsHwState *hwRet);
    Bool (*AutoDevProbe)(InputInfoPtr pInfo);
    void (*ReadDevDimensions)(InputInfoPtr pInfo);
};

extern struct SynapticsProtocolOperations psaux_proto_operations;
#ifdef BUILD_EVENTCOMM
extern struct SynapticsProtocolOperations event_proto_operations;
#endif /* BUILD_EVENTCOMM */
#ifdef BUILD_PSMCOMM
extern struct SynapticsProtocolOperations psm_proto_operations;
#endif /* BUILD_PSMCOMM */
extern struct SynapticsProtocolOperations alps_proto_operations;

extern int HandleState(InputInfoPtr, struct SynapticsHwState*);
extern CARD32 timerFunc(OsTimerPtr timer, CARD32 now, pointer arg);
extern Bool is_inside_active_area(struct _SynapticsPrivateRec *priv, int x, int y);

#endif /* _SYNPROTO_H_ */
