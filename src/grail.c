/*
 * Copyright © 2010 Canonical, Ltd.
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
 *	Chase Douglas (chase.douglas@canonical.com)
 */

/* So we can get at the right data in xorg/windowstr.h */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xorg-server.h>
#include <X11/Xatom.h>
#include <xf86.h>
#include <xf86Xinput.h>
#include <exevents.h>
#include <windowstr.h>
#include <xorg/os.h>
#include <grail.h>

#include <xorg/windowstr.h>
#include "synaptics.h"
#include "synapticsstr.h"
#include "eventcomm.h"

//#include "gestureproto.h"

/*
 * Provided by the Maverick X server, we don't want to pollute the official
 * X.org API.
 */
extern _X_EXPORT void xf86PostGestureEvent(DeviceIntPtr dev, unsigned short x,
                         unsigned short y, unsigned short client_id,
                         unsigned short gesture_id, unsigned short gesture_type,
                         Window root, Window event, Window child,
                         unsigned short status, unsigned short num_props,
                         float *props);

static WindowPtr CommonAncestor(WindowPtr a, WindowPtr b)
{
    WindowPtr c;

    if (a == b)
        return a;

    for (b = b; b; b = b->parent)
        for (c = a; c; c = c->parent)
            if (c == b)
                return b;

    return NullWindow;
}

static WindowPtr GetWindowForGestures(struct grail *grail,
                                      const struct grail_coord *contacts,
                                      int num_contacts)
{
    WindowPtr window = NULL;
    int i;

    for (i = 0; i < num_contacts; i++)
    {
        float screen_x = contacts[i].x;
        float screen_y = contacts[i].y;
        WindowPtr this_window;

        this_window = xf86CoordinatesToWindow(screen_x, screen_y, 0);
        if (!this_window)
            return NullWindow;

        if (!window)
            window = this_window;
        else
            window = CommonAncestor(window, this_window);
    }

    return window;
}

static int GetClients(struct grail *grail,
		      struct grail_client_info *clients, int max_clients,
		      const struct grail_coord *contacts, int num_contacts,
		      const grail_mask_t *types, int type_bytes)
{
    WindowPtr child_window;
    WindowPtr window;
    WindowPtr root_window;
    InputInfoPtr pInfo = grail->priv;
    DeviceIntPtr master = pInfo->dev->u.master;
    struct grail_coord cursor_coord;
    int j;
    int found_match = 0;
    int num_clients = 0;
    int type;

    if (max_clients <= 0)
        return 0;

    /* If this mouse isn't hooked up to a cursor, don't do anything */
    if (!master)
        return 0;

    cursor_coord.x = master->last.valuators[0];
    cursor_coord.y = master->last.valuators[1];

    child_window = GetWindowForGestures(grail, &cursor_coord, 1);

    if (!child_window)
        return 0;

    memset(clients, 0, sizeof(struct grail_client_info) * max_clients);

    /* Find the root window. */
    for (root_window = child_window; root_window->parent;
         root_window = root_window->parent);

    /*
     * Check for a root client with SYSFLAG1 set. SYSFLAG1 is effectively an
     * active grab for system gestures. We assume only one client has SYSFLAG1
     * set.
     */
    window = child_window;
    while (window)
    {
        /* Check if any gestures have been selected on this window. */
        if (wGestureMasks(window))
        {
            GestureClientsPtr client;

            /* For each client */
            for (client = wGestureMasks(window)->clients; client;
                 client = client->next)
            {
                int first = 1;

                /* If the client has set the SYSFLAG1 bit */
                if (BitIsOn(client->gestureMask[0], GRAIL_TYPE_SYSFLAG1))
                {
                    /* For each recognized gesture */
                    grail_mask_foreach(type, types, type_bytes)
                    {
                        if (type == GRAIL_TYPE_SYSFLAG1)
                            continue;

                        /*
                         * Check if this client selected for this gesture.
                         * Request may be for this device or all devices.
                         */
                        if (BitIsOn(client->gestureMask[pInfo->dev->id], type) ||
                            BitIsOn(client->gestureMask[0], type))
                        {
                            if (first) {
                                /* Set up new client in array */
                                clients[0].id.client = CLIENT_ID(client->resource);
                                clients[0].id.root = root_window->drawable.id;
                                clients[0].id.child = child_window->drawable.id;
                                clients[0].id.event = window->drawable.id;
                                grail_mask_clear(clients[0].mask,
                                                 DIM_GRAIL_TYPE_BYTES);
                                first = 0;
                            }

                            /* Set this gesture bit in the client's gesture mask */
                            SetBit(clients[0].mask, type);
                            num_clients = 1;
                        }
                    }

                    /*
                     * Either we found a gesture, or we stop looking for SYSFLAG1
                     * clients.
                     */
                    if (num_clients) {
                        SetBit(clients[0].mask, GRAIL_TYPE_SYSFLAG1);
                        goto next_window;
                    }
                }
            }
        }

next_window:
        window = window->parent;
    }

    /*
     * Traverse the window hierarchy looking for a window with a client
     * selecting for one of the recognized gestures.
     *
     * All clients of the top most window with a match will receive events if
     * they have selected for gestures that have been recognized, even if they
     * have selected for different gestures between them.
     *
     * Once any gesture is matched on a window, propagation through the window
     * hierarchy ends.
     */
    for (window = child_window; window && !found_match; window = window->parent)
    {
        /* No client selected for gestures on this window */
        if (!wGestureMasks(window))
            continue;

        /* For each recognized gesture */
        grail_mask_foreach(type, types, type_bytes)
        {
            if (type == GRAIL_TYPE_SYSFLAG1)
                continue;

            /* Check if any client selected for this gesture on the window */
            if (BitIsOn(wGestureMasks(window)->mask, type))
            {
                GestureClientsPtr client;

                /* For each client that selected for gestures on this window */
                for (client = wGestureMasks(window)->clients; client;
                     client = client->next)
                {
                     /*
                      * Check if this client selected for this gesture. Request
                      * may be for this device or all devices.
                      */
                     if (BitIsOn(client->gestureMask[pInfo->dev->id], type) ||
                         BitIsOn(client->gestureMask[0], type))
                     {
                         /*
                          * Find this client in the clients array passed back to
                          * the caller.
                          */
                         for (j = 0; j < num_clients; j++)
                             if (clients[j].id.client ==
                                 CLIENT_ID(client->resource))
                                 break;

                         /* Check if the client exists in the array yet */
                         if (j >= num_clients)
                         {
                             /* We ran out of room in the array, return error */
                             if (num_clients >= max_clients)
                                 return -1;
                             /* Set up new client in array */
                             clients[j].id.client = CLIENT_ID(client->resource);
                             clients[j].id.root = root_window->drawable.id;
                             clients[j].id.child = child_window->drawable.id;
                             clients[j].id.event = window->drawable.id;
                             num_clients++;
                         }

                         /* Set this gesture bit in the client's gesture mask */
                         SetBit(clients[j].mask, type);
                     }
                }

                /* A match has been found, stop propagating */
                found_match = 1;
            }
        }
    }

    return num_clients;
}

static void GrailEvent(struct grail *grail, const struct input_event *ev)
{
    InputInfoPtr pInfo = (InputInfoPtr)grail->priv;
    SynapticsPrivate *priv = (SynapticsPrivate *)pInfo->private;
    struct SynapticsHwState hw;
    int delay = 0;
    Bool newDelay = FALSE;

    if (EventProcessEvent(pInfo, &priv->comm, &hw, ev)) {
        hw.millis = GetTimeInMillis();
        priv->hwState = hw;
        delay = HandleState(pInfo, &hw);
        newDelay = TRUE;
    }

    if (newDelay)
        priv->timer = TimerSet(priv->timer, 0, delay, timerFunc, pInfo);
}

static void GrailGesture(struct grail *grail, const struct grail_event *ev)
{
    InputInfoPtr pInfo = grail->priv;
    int x;
    int y;

    DeviceIntPtr master = pInfo->dev->u.master;

    /* If this mouse isn't hooked up to a cursor, don't do anything */
    if (!master)
        return;

    /* Note: Master device valuators are in screen coordinates */
    x = master->last.valuators[0];
    y = master->last.valuators[1];

    xf86PostGestureEvent(pInfo->dev, x, y, ev->client_id.client, ev->id,
                         ev->type, ev->client_id.root, ev->client_id.event,
                         ev->client_id.child, ev->status,
                         ev->nprop, (float *)ev->prop);
}

int
GrailOpen(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = pInfo->private;
    EventcommPrivate *ecpriv = priv->proto_data;

    ecpriv->grail = malloc(sizeof(struct grail));
    if (!ecpriv->grail) {
        xf86Msg(X_ERROR, "%s: failed to allocate grail structure\n",
                pInfo->name);
        return -1;
    }

    memset(ecpriv->grail, 0, sizeof(struct grail));
    ecpriv->grail->get_clients = GetClients;
    ecpriv->grail->event = GrailEvent;
    ecpriv->grail->gesture = GrailGesture;
    ecpriv->grail->priv = pInfo;

    if (grail_open(ecpriv->grail, pInfo->fd)) {
        xf86Msg(X_INFO, "%s: failed to open grail, no gesture support\n",
                pInfo->name);
        free(ecpriv->grail);
        ecpriv->grail = NULL;
        return -1;
    }

    {
        struct grail_coord min;
        struct grail_coord max;

        min.x = screenInfo.screens[0]->x;
        min.y = screenInfo.screens[0]->y;
        max.x = min.x + screenInfo.screens[0]->width;
        max.y = min.y + screenInfo.screens[0]->height;

        grail_set_bbox(ecpriv->grail, &min, &max);
    }

    return 0;
}

void
GrailClose(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = pInfo->private;
    EventcommPrivate *ecpriv = priv->proto_data;

    if (ecpriv->grail) {
        grail_close(ecpriv->grail, pInfo->fd);
        free(ecpriv->grail);
        ecpriv->grail = NULL;
    }
}
