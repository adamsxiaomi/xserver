/*
 * Copyright © 2011 Collabra Ltd.
 * Copyright © 2011 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Daniel Stone <daniel@fooishbar.org>
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "inputstr.h"
#include "scrnintstr.h"
#include "dixgrabs.h"

#include "eventstr.h"
#include "exevents.h"

#define TOUCH_HISTORY_SIZE 100


/* If a touch queue resize is needed, the device id's bit is set. */
static unsigned char resize_waiting[(MAXDEVICES + 7)/8];

/**
 * Some documentation about touch points:
 * The driver submits touch events with it's own (unique) touch point ID.
 * The driver may re-use those IDs, the DDX doesn't care. It just passes on
 * the data to the DIX. In the server, the driver's ID is referred to as the
 * DDX id anyway.
 *
 * On a TouchBegin, we create a DDXTouchPointInfo that contains the DDX id
 * and the client ID that this touchpoint will have. The client ID is the
 * one visible on the protocol.
 *
 * TouchUpdate and TouchEnd will only be processed if there is an active
 * touchpoint with the same DDX id.
 *
 * The DDXTouchPointInfo struct is stored dev->last.touches. When the event
 * being processed, it becomes a TouchPointInfo in dev->touch-touches which
 * contains amongst other things the sprite trace and delivery information.
 */

/**
 * Check which devices need a bigger touch event queue and grow their
 * last.touches by half it's current size.
 *
 * @param client Always the serverClient
 * @param closure Always NULL
 *
 * @return Always True. If we fail to grow we probably will topple over soon
 * anyway and re-executing this won't help.
 */
static Bool
TouchResizeQueue(ClientPtr client, pointer closure)
{
    int i;

    OsBlockSignals();

    /* first two ids are reserved */
    for (i = 2; i < MAXDEVICES; i++)
    {
        DeviceIntPtr dev;
        DDXTouchPointInfoPtr tmp;
        size_t size;

        if (!BitIsOn(resize_waiting, i))
            continue;

        ClearBit(resize_waiting, i);

        /* device may have disappeared by now */
        dixLookupDevice(&dev, i, serverClient, DixWriteAccess);
        if (!dev)
            continue;

        /* Need to grow the queue means dropping events. Grow sufficiently so we
         * don't need to do it often */
        size = dev->last.num_touches + dev->last.num_touches/2 + 1;

        tmp = realloc(dev->last.touches, size *  sizeof(*dev->last.touches));
        if (tmp)
        {
            int i;
            dev->last.touches = tmp;
            for (i = dev->last.num_touches; i < size; i++)
                TouchInitDDXTouchPoint(dev, &dev->last.touches[i]);
            dev->last.num_touches = size;
        }

    }
    OsReleaseSignals();

    return TRUE;
}

/**
 * Given the DDX-facing ID (which is _not_ DeviceEvent::detail.touch), find the
 * associated DDXTouchPointInfoRec.
 *
 * @param dev The device to create the touch point for
 * @param ddx_id Touch id assigned by the driver/ddx
 * @param create Create the touchpoint if it cannot be found
 */
DDXTouchPointInfoPtr
TouchFindByDDXID(DeviceIntPtr dev, uint32_t ddx_id, Bool create)
{
    DDXTouchPointInfoPtr ti;
    int i;

    if (!dev->touch)
        return NULL;

    for (i = 0; i < dev->last.num_touches; i++)
    {
        ti = &dev->last.touches[i];
        if (ti->active && ti->ddx_id == ddx_id)
            return ti;
    }

    return create ? TouchBeginDDXTouch(dev, ddx_id) : NULL;
}

/**
 * Given a unique DDX ID for a touchpoint, create a touchpoint record and
 * return it.
 *
 * If no other touch points are active, mark new touchpoint for pointer
 * emulation.
 *
 * Returns NULL on failure (i.e. if another touch with that ID is already active,
 * allocation failure).
 */
DDXTouchPointInfoPtr
TouchBeginDDXTouch(DeviceIntPtr dev, uint32_t ddx_id)
{
    static int next_client_id = 1;
    int i;
    TouchClassPtr t = dev->touch;
    DDXTouchPointInfoPtr ti = NULL;
    Bool emulate_pointer = (t->mode == XIDirectTouch);

    if (!t)
        return NULL;

    /* Look for another active touchpoint with the same DDX ID. DDX
     * touchpoints must be unique. */
    if (TouchFindByDDXID(dev, ddx_id, FALSE))
        return NULL;

    for (i = 0; i < dev->last.num_touches; i++)
    {
        /* Only emulate pointer events on the first touch */
        if (dev->last.touches[i].active)
            emulate_pointer = FALSE;
        else if (!ti) /* ti is now first non-active touch rec */
            ti = &dev->last.touches[i];

        if (!emulate_pointer && ti)
            break;
    }

    if (ti)
    {
        int client_id;
        ti->active = TRUE;
        ti->ddx_id = ddx_id;
        client_id = next_client_id;
        next_client_id++;
        if (next_client_id == 0)
            next_client_id = 1;
        ti->client_id = client_id;
        ti->emulate_pointer = emulate_pointer;
        return ti;
    }

    /* If we get here, then we've run out of touches and we need to drop the
     * event (we're inside the SIGIO handler here) schedule a WorkProc to
     * grow the queue for us for next time. */
    ErrorF("%s: not enough space for touch events (max %d touchpoints). "
           "Dropping this event.\n", dev->name, dev->last.num_touches);
    if (!BitIsOn(resize_waiting, dev->id)) {
        SetBit(resize_waiting, dev->id);
        QueueWorkProc(TouchResizeQueue, serverClient, NULL);
    }

    return NULL;
}

void
TouchEndDDXTouch(DeviceIntPtr dev, DDXTouchPointInfoPtr ti)
{
    TouchClassPtr t = dev->touch;

    if (!t)
        return;

    ti->active = FALSE;
}

void
TouchInitDDXTouchPoint(DeviceIntPtr dev, DDXTouchPointInfoPtr ddxtouch)
{
    memset(ddxtouch, 0, sizeof(*ddxtouch));
    ddxtouch->valuators = valuator_mask_new(dev->valuator->numAxes);
}


Bool
TouchInitTouchPoint(TouchClassPtr t, ValuatorClassPtr v, int index)
{
    TouchPointInfoPtr ti;

    if (index >= t->num_touches)
        return FALSE;
    ti = &t->touches[index];

    memset(ti, 0, sizeof(*ti));

    ti->valuators = valuator_mask_new(v->numAxes);
    if (!ti->valuators)
        return FALSE;

    ti->sprite.spriteTrace = calloc(32, sizeof(*ti->sprite.spriteTrace));
    if (!ti->sprite.spriteTrace)
    {
        valuator_mask_free(&ti->valuators);
        return FALSE;
    }
    ti->sprite.spriteTraceSize = 32;
    ti->sprite.spriteTrace[0] = screenInfo.screens[0]->root;
    ti->sprite.hot.pScreen = screenInfo.screens[0];
    ti->sprite.hotPhys.pScreen = screenInfo.screens[0];

    ti->client_id = -1;

    return TRUE;
}

void
TouchFreeTouchPoint(DeviceIntPtr device, int index)
{
    TouchPointInfoPtr ti;

    if (!device->touch || index >= device->touch->num_touches)
        return;
    ti = &device->touch->touches[index];

    if (ti->active)
        TouchEndTouch(device, ti);

    valuator_mask_free(&ti->valuators);
    free(ti->sprite.spriteTrace);
    ti->sprite.spriteTrace = NULL;
    free(ti->listeners);
    ti->listeners = NULL;
    free(ti->history);
    ti->history = NULL;
    ti->history_size = 0;
    ti->history_elements = 0;
}

/**
 * Given a client-facing ID (e.g. DeviceEvent::detail.touch), find the
 * associated TouchPointInfoRec.
 */
TouchPointInfoPtr
TouchFindByClientID(DeviceIntPtr dev, uint32_t client_id)
{
    TouchClassPtr t = dev->touch;
    TouchPointInfoPtr ti;
    int i;

    if (!t)
        return NULL;

    for (i = 0; i < t->num_touches; i++)
    {
        ti = &t->touches[i];
        if (ti->active && ti->client_id == client_id)
            return ti;
    }

    return NULL;
}


/**
 * Given a unique ID for a touchpoint, create a touchpoint record in the
 * server.
 *
 * Returns NULL on failure (i.e. if another touch with that ID is already active,
 * allocation failure).
 */
TouchPointInfoPtr
TouchBeginTouch(DeviceIntPtr dev, int sourceid, uint32_t touchid,
                Bool emulate_pointer)
{
    int i;
    TouchClassPtr t = dev->touch;
    TouchPointInfoPtr ti;
    void *tmp;

    if (!t)
        return NULL;

    /* Look for another active touchpoint with the same client ID.  It's
     * technically legitimate for a touchpoint to still exist with the same
     * ID but only once the 32 bits wrap over and you've used up 4 billion
     * touch ids without lifting that one finger off once. In which case
     * you deserve a medal or something, but not error handling code. */
    if (TouchFindByClientID(dev, touchid))
        return NULL;

try_find_touch:
    for (i = 0; i < t->num_touches; i++)
    {
        ti = &t->touches[i];
        if (!ti->active) {
            ti->active = TRUE;
            ti->client_id = touchid;
            ti->sourceid = sourceid;
            ti->emulate_pointer = emulate_pointer;
            return ti;
        }
    }

    /* If we get here, then we've run out of touches: enlarge dev->touch and
     * try again. */
    tmp = realloc(t->touches, (t->num_touches + 1) * sizeof(*ti));
    if (tmp)
    {
        t->touches = tmp;
        t->num_touches++;
        if (TouchInitTouchPoint(t, dev->valuator, t->num_touches - 1))
            goto try_find_touch;
    }

    return NULL;
}

/**
 * Releases a touchpoint for use: this must only be called after all events
 * related to that touchpoint have been sent and finalised.  Called from
 * ProcessTouchEvent and friends.  Not by you.
 */
void
TouchEndTouch(DeviceIntPtr dev, TouchPointInfoPtr ti)
{
    if (ti->emulate_pointer)
    {
        GrabPtr grab;
        DeviceEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ET_TouchEnd;
        ev.detail.button = 1;
        ev.touchid = ti->client_id;
        ev.flags = TOUCH_POINTER_EMULATED|TOUCH_END;
        UpdateDeviceState(dev, &ev);

        if ((grab = dev->deviceGrab.grab))
        {
            if (dev->deviceGrab.fromPassiveGrab &&
                !dev->button->buttonsDown &&
                !dev->touch->buttonsDown &&
                GrabIsPointerGrab(grab))
                (*dev->deviceGrab.DeactivateGrab)(dev);
        }
    }

    ti->active = FALSE;
    ti->pending_finish = FALSE;
    ti->sprite.spriteTraceGood = 0;
    free(ti->listeners);
    ti->listeners = NULL;
    ti->num_listeners = 0;
    ti->num_grabs = 0;
    ti->client_id = 0;

    TouchEventHistoryFree(ti);

    valuator_mask_zero(ti->valuators);
}

/**
 * Allocate the event history for this touch pointer. Calling this on a
 * touchpoint that already has an event history does nothing but counts as
 * as success.
 *
 * @return TRUE on success, FALSE on allocation errors
 */
Bool
TouchEventHistoryAllocate(TouchPointInfoPtr ti)
{
    if (ti->history)
        return TRUE;

    ti->history = calloc(TOUCH_HISTORY_SIZE, sizeof(*ti->history));
    ti->history_elements = 0;
    if (ti->history)
        ti->history_size = TOUCH_HISTORY_SIZE;
    return ti->history != NULL;
}

void
TouchEventHistoryFree(TouchPointInfoPtr ti)
{
    free(ti->history);
    ti->history = NULL;
    ti->history_size = 0;
    ti->history_elements = 0;
}

/**
 * Store the given event on the event history (if one exists)
 * A touch event history consists of one TouchBegin and several TouchUpdate
 * events (if applicable) but no TouchEnd event.
 * If more than one TouchBegin is pushed onto the stack, the push is
 * ignored, calling this function multiple times for the TouchBegin is
 * valid.
 */
void
TouchEventHistoryPush(TouchPointInfoPtr ti, const DeviceEvent *ev)
{
    if (!ti->history)
        return;

    switch(ev->type)
    {
        case ET_TouchBegin:
            /* don't store the same touchbegin twice */
            if (ti->history_elements > 0)
                return;
            break;
        case ET_TouchUpdate:
            break;
        case ET_TouchEnd:
            return; /* no TouchEnd events in the history */
        default:
            return;
    }

    /* We only store real events in the history */
    if (ev->flags & (TOUCH_CLIENT_ID|TOUCH_REPLAYING))
        return;

    ti->history[ti->history_elements++] = *ev;
    /* FIXME: proper overflow fixes */
    if (ti->history_elements > ti->history_size - 1)
    {
        ti->history_elements = ti->history_size - 1;
        DebugF("source device %d: history size %d overflowing for touch %u\n",
                ti->sourceid, ti->history_size, ti->client_id);
    }
}

void
TouchEventHistoryReplay(TouchPointInfoPtr ti, DeviceIntPtr dev, XID resource)
{
    InternalEvent *tel = InitEventList(GetMaximumEventsNum());
    ValuatorMask *mask = valuator_mask_new(0);
    int i, nev;
    int flags;

    if (!ti->history)
        return;

    valuator_mask_set_double(mask, 0, ti->history[0].valuators.data[0]);
    valuator_mask_set_double(mask, 1, ti->history[0].valuators.data[1]);

    flags = TOUCH_CLIENT_ID|TOUCH_REPLAYING;
    if (ti->emulate_pointer)
        flags |= TOUCH_POINTER_EMULATED;
    /* send fake begin event to next owner */
    nev = GetTouchEvents(tel, dev, ti->client_id, XI_TouchBegin, flags, mask);
    /* FIXME: deliver the event */

    valuator_mask_free(&mask);
    FreeEventList(tel, GetMaximumEventsNum());

    /* First event was TouchBegin, already replayed that one */
    for (i = 1; i < ti->history_elements; i++)
    {
        DeviceEvent *ev = &ti->history[i];
        ev->flags |= TOUCH_REPLAYING;
        /* FIXME: deliver the event */
    }
}

Bool
TouchBuildDependentSpriteTrace(DeviceIntPtr dev, SpritePtr sprite)
{
    int i;
    TouchClassPtr t = dev->touch;
    WindowPtr *trace;
    SpritePtr srcsprite;

    /* All touches should have the same sprite trace, so find and reuse an
     * existing touch's sprite if possible, else use the device's sprite. */
    for (i = 0; i < t->num_touches; i++)
        if (t->touches[i].sprite.spriteTraceGood > 0)
            break;
    if (i < t->num_touches)
        srcsprite = &t->touches[i].sprite;
    else if (dev->spriteInfo->sprite)
        srcsprite = dev->spriteInfo->sprite;
    else
        return FALSE;

    if (srcsprite->spriteTraceGood > sprite->spriteTraceSize)
    {
        trace = realloc(sprite->spriteTrace,
                srcsprite->spriteTraceSize * sizeof(*trace));
        if (!trace)
        {
            sprite->spriteTraceGood = 0;
            return FALSE;
        }
        sprite->spriteTrace = trace;
        sprite->spriteTraceSize = srcsprite->spriteTraceGood;
    }
    memcpy(sprite->spriteTrace, srcsprite->spriteTrace,
            srcsprite->spriteTraceGood * sizeof(*trace));
    sprite->spriteTraceGood = srcsprite->spriteTraceGood;

    return TRUE;
}

/**
 * Ensure a window trace is present in ti->sprite, constructing one for
 * TouchBegin events.
 */
Bool
TouchEnsureSprite(DeviceIntPtr sourcedev, TouchPointInfoPtr ti,
                  InternalEvent *ev)
{
    TouchClassPtr t = sourcedev->touch;
    SpritePtr sprite = &ti->sprite;

    /* We may not have a sprite if there are no applicable grabs or
     * event selections, or if they've disappeared, or if all the grab
     * owners have rejected the touch.  Don't bother delivering motion
     * events if not, but TouchEnd events still need to be processed so
     * we can call FinishTouchPoint and release it for later use. */
    if (ev->any.type == ET_TouchEnd)
        return TRUE;
    else if (ev->any.type != ET_TouchBegin)
        return (sprite->spriteTraceGood > 0);

    if (t->mode == XIDirectTouch)
    {
        /* Focus immediately under the touchpoint in direct touch mode.
         * XXX: Do we need to handle crossing screens here? */
        sprite->spriteTrace[0] =
            sourcedev->spriteInfo->sprite->hotPhys.pScreen->root;
        XYToWindow(sprite, ev->device_event.root_x, ev->device_event.root_y);
    }
    else if (!TouchBuildDependentSpriteTrace(sourcedev, sprite))
        return FALSE;

    if (sprite->spriteTraceGood <= 0)
        return FALSE;

    /* Mark which grabs/event selections we're delivering to: max one grab per
     * window plus the bottom-most event selection. */
    ti->listeners = calloc(sprite->spriteTraceGood + 1, sizeof(*ti->listeners));
    if (!ti->listeners)
    {
        sprite->spriteTraceGood = 0;
        return FALSE;
    }
    ti->num_listeners = 0;

    return TRUE;
}