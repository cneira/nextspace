/*
 *  Workspace window manager
 *  Copyright (c) 2015-2021 Sergii Stoian
 *
 *  WINGs library (Window Maker)
 *  Copyright (c) 1998 scottc
 *  Copyright (c) 1999-2004 Dan Pascu
 *  Copyright (c) 1999-2000 Alfredo K. Kojima
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>

#include <CoreFoundation/CFNotificationCenter.h>

#include "WMcore.h"
#include "util.h"
#include "string_utils.h"
#include "log_utils.h"

#include "wscreen.h"
#include "wevent.h"
#include "dragcommon.h"
#include "selection.h"
#include "drawing.h"

#define XDND_SOURCE_RESPONSE_MAX_DELAY 3000

#define XDND_PROPERTY_FORMAT 32
#define XDND_ACTION_DESCRIPTION_FORMAT 8

#define XDND_SOURCE_VERSION(dragInfo) dragInfo->protocolVersion
#define XDND_DEST_INFO(dragInfo) dragInfo->destInfo
#define XDND_AWARE_VIEW(dragInfo) dragInfo->destInfo->xdndAwareView
#define XDND_SOURCE_WIN(dragInfo) dragInfo->destInfo->sourceWindow
#define XDND_DEST_VIEW(dragInfo) dragInfo->destInfo->destView
#define XDND_DEST_STATE(dragInfo) dragInfo->destInfo->state
#define XDND_SOURCE_ACTION_CHANGED(dragInfo) dragInfo->destInfo->sourceActionChanged
#define XDND_SOURCE_TYPES(dragInfo) dragInfo->destInfo->sourceTypes
#define XDND_TYPE_LIST_AVAILABLE(dragInfo) dragInfo->destInfo->typeListAvailable
#define XDND_REQUIRED_TYPES(dragInfo) dragInfo->destInfo->requiredTypes
#define XDND_SOURCE_ACTION(dragInfo) dragInfo->sourceAction
#define XDND_DEST_ACTION(dragInfo) dragInfo->destinationAction
#define XDND_DROP_DATAS(dragInfo) dragInfo->destInfo->dropDatas
#define XDND_DEST_VIEW_IS_REGISTERED(dragInfo) ((dragInfo->destInfo) != NULL) \
  && ((dragInfo->destInfo->destView->dragDestinationProcs) != NULL)

static const unsigned char XDNDversion = XDND_VERSION;
static CFRunLoopTimerRef dndDestinationTimer = NULL;

static void *idleState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info);
static void *waitEnterState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info);
static void *inspectDropDataState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info);
static void *dropAllowedState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info);
static void *dropNotAllowedState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info);
static void *waitForDropDataState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info);

/* ----- Types & datas list ----- */
static void freeSourceTypeArrayItem(CFAllocatorRef allocator, const void *type)
{
  XFree((void *)type);
}

static CFMutableArrayRef createSourceTypeArray(int initialSize)
{
  CFArrayCallBacks cbs = {0, NULL, freeSourceTypeArrayItem, NULL, NULL};
  return CFArrayCreateMutable(kCFAllocatorDefault, initialSize, &cbs);
}

static void freeDropDataArrayItem(CFAllocatorRef allocator, const void *data)
{
  if (data != NULL)
    WMReleaseData((WMData *) data);
}

static CFMutableArrayRef createDropDataArray(CFMutableArrayRef requiredTypes)
{
  if (requiredTypes != NULL) {
    CFArrayCallBacks cbs = {0, NULL, freeDropDataArrayItem, NULL, NULL};
    return CFArrayCreateMutable(kCFAllocatorDefault, CFArrayGetCount(requiredTypes), &cbs);
  }
  else {
    return CFArrayCreateMutable(kCFAllocatorDefault, 0, NULL);
  }
}

static CFMutableArrayRef getTypesFromTypeList(WMScreen * scr, Window sourceWin)
{
  Atom dataType;
  Atom *typeAtomList;
  CFMutableArrayRef typeList;
  int i, format;
  unsigned long count, remaining;
  unsigned char *data = NULL;

  XGetWindowProperty(scr->display, sourceWin, scr->xdndTypeListAtom,
                     0, 0x8000000L, False, XA_ATOM, &dataType, &format, &count, &remaining, &data);

  if (dataType != XA_ATOM || format != XDND_PROPERTY_FORMAT || count == 0 || !data) {
    if (data) {
      XFree(data);
    }
    return createSourceTypeArray(0);
  }

  typeList = createSourceTypeArray(count);
  typeAtomList = (Atom *) data;
  for (i = 0; i < count; i++) {
    CFArrayAppendValue(typeList, XGetAtomName(scr->display, typeAtomList[i]));
  }

  XFree(data);

  return typeList;
}

static CFMutableArrayRef getTypesFromThreeTypes(WMScreen * scr, XClientMessageEvent * event)
{
  CFMutableArrayRef typeList;
  Atom atom;
  int i;

  typeList = createSourceTypeArray(3);
  for (i = 2; i < 5; i++) {
    if (event->data.l[i] != None) {
      atom = (Atom) event->data.l[i];
      CFArrayAppendValue(typeList, XGetAtomName(scr->display, atom));
    }
  }

  return typeList;
}

static void storeRequiredTypeList(WMDraggingInfo * info)
{
  WMView *destView = XDND_DEST_VIEW(info);
  WMScreen *scr = WMViewScreen(destView);
  CFMutableArrayRef requiredTypes;

  /* First, see if the stored source types are enough for dest requirements */
  requiredTypes =
    destView->dragDestinationProcs->requiredDataTypes(destView,
                                                      WMActionToOperation(scr, XDND_SOURCE_ACTION(info)),
                                                      XDND_SOURCE_TYPES(info));

  if (requiredTypes == NULL && XDND_TYPE_LIST_AVAILABLE(info)) {
    /* None of the stored source types fits, but the whole type list
       hasn't been retrieved yet. */
    CFRelease(XDND_SOURCE_TYPES(info));
    XDND_SOURCE_TYPES(info) = getTypesFromTypeList(scr, XDND_SOURCE_WIN(info));
    /* Don't retrieve the type list again */
    XDND_TYPE_LIST_AVAILABLE(info) = False;

    requiredTypes =
      destView->dragDestinationProcs->requiredDataTypes(destView,
                                                        WMActionToOperation(scr, XDND_SOURCE_ACTION(info)),
                                                        XDND_SOURCE_TYPES(info));
  }

  XDND_REQUIRED_TYPES(info) = requiredTypes;
}

static char *getNextRequestedDataType(WMDraggingInfo * info)
{
  /* get the type of the first data not yet retrieved from selection */
  int nextTypeIndex;

  if (XDND_REQUIRED_TYPES(info) != NULL) {
    nextTypeIndex = CFArrayGetCount(XDND_DROP_DATAS(info));
    return (char *)CFArrayGetValueAtIndex(XDND_REQUIRED_TYPES(info), nextTypeIndex);
    /* NULL if no more type */
  } else {
    return NULL;
  }
}

/* ----- Action list ----- */

static CFMutableArrayRef sourceOperationList(WMScreen * scr, Window sourceWin)
{
  Atom dataType, *actionList;
  int i, size;
  unsigned long count, remaining;
  unsigned char *actionDatas = NULL;
  unsigned char *descriptionList = NULL;
  CFMutableArrayRef operationArray;
  WMDragOperationItem *operationItem;
  char *description;

  remaining = 0;
  XGetWindowProperty(scr->display, sourceWin, scr->xdndActionListAtom,
                     0, 0x8000000L, False, XA_ATOM, &dataType, &size, &count, &remaining, &actionDatas);

  if (dataType != XA_ATOM || size != XDND_PROPERTY_FORMAT || count == 0 || !actionDatas) {
    WMLogWarning("Cannot read action list");
    if (actionDatas) {
      XFree(actionDatas);
    }
    return NULL;
  }

  actionList = (Atom *) actionDatas;

  XGetWindowProperty(scr->display, sourceWin, scr->xdndActionDescriptionAtom,
                     0, 0x8000000L, False, XA_STRING, &dataType, &size,
                     &count, &remaining, &descriptionList);

  if (dataType != XA_STRING || size != XDND_ACTION_DESCRIPTION_FORMAT || count == 0 || !descriptionList) {
    WMLogWarning("Cannot read action description list");
    if (actionList) {
      XFree(actionList);
    }
    if (descriptionList) {
      XFree(descriptionList);
    }
    return NULL;
  }

  operationArray = WMCreateDragOperationArray(count);
  description = (char *)descriptionList;

  for (i = 0; count > 0; i++) {
    size = strlen(description);
    operationItem = WMCreateDragOperationItem(WMActionToOperation(scr, actionList[i]),
                                              wstrdup(description));

    CFArrayAppendValue(operationArray, operationItem);
    count -= (size + 1);	/* -1 : -NULL char */

    /* next description */
    description = &(description[size + 1]);
  }

  XFree(actionList);
  XFree(descriptionList);

  return operationArray;
}

/* ----- Dragging Info ----- */
static void updateSourceWindow(WMDraggingInfo * info, XClientMessageEvent * event)
{
  XDND_SOURCE_WIN(info) = (Window) event->data.l[0];
}

static WMView *findChildInView(WMView * parent, int x, int y)
{
  if (parent->childrenList == NULL)
    return parent;
  else {
    WMView *child = parent->childrenList;

    while (child != NULL
           && (!child->flags.mapped
               || x < WMGetViewPosition(child).x
               || x > WMGetViewPosition(child).x + WMGetViewSize(child).width
               || y < WMGetViewPosition(child).y
               || y > WMGetViewPosition(child).y + WMGetViewSize(child).height))

      child = child->nextSister;

    if (child == NULL)
      return parent;
    else
      return findChildInView(child,
                             x - WMGetViewPosition(child).x, y - WMGetViewPosition(child).y);
  }
}

static WMView *findDestinationViewInToplevel(WMView *toplevel, int x, int y)
{
  WMScreen *scr = WMViewScreen(toplevel);
  Window toplevelWin = WMViewXID(toplevel);
  int xInToplevel, yInToplevel;
  Window foo;

  XTranslateCoordinates(scr->display, scr->rootWin, toplevelWin, x, y, &xInToplevel, &yInToplevel, &foo);
  return findChildInView(toplevel, xInToplevel, yInToplevel);
}

/* Clear datas only used by current destination view */
static void freeDestinationViewInfos(WMDraggingInfo *info)
{
  if (XDND_SOURCE_TYPES(info) != NULL) {
    CFRelease(XDND_SOURCE_TYPES(info));
    XDND_SOURCE_TYPES(info) = NULL;
  }

  if (XDND_DROP_DATAS(info) != NULL) {
    CFRelease(XDND_DROP_DATAS(info));
    XDND_DROP_DATAS(info) = NULL;
  }

  XDND_REQUIRED_TYPES(info) = NULL;
}

void WMDragDestinationInfoClear(WMDraggingInfo * info)
{
  WMDragDestinationStopTimer();

  if (XDND_DEST_INFO(info) != NULL) {
    freeDestinationViewInfos(info);

    wfree(XDND_DEST_INFO(info));
    XDND_DEST_INFO(info) = NULL;
  }
}

static void initDestinationDragInfo(WMDraggingInfo * info, WMView * destView)
{
  wassertr(destView != NULL);

  XDND_DEST_INFO(info) = (WMDragDestinationInfo *) wmalloc(sizeof(WMDragDestinationInfo));

  XDND_DEST_STATE(info) = idleState;
  XDND_DEST_VIEW(info) = destView;

  XDND_SOURCE_ACTION_CHANGED(info) = False;
  XDND_SOURCE_TYPES(info) = NULL;
  XDND_REQUIRED_TYPES(info) = NULL;
  XDND_DROP_DATAS(info) = NULL;
}

void WMDragDestinationStoreEnterMsgInfo(WMDraggingInfo * info, WMView * toplevel, XClientMessageEvent * event)
{
  WMScreen *scr = WMViewScreen(toplevel);

  if (XDND_DEST_INFO(info) == NULL)
    initDestinationDragInfo(info, toplevel);

  XDND_SOURCE_VERSION(info) = (event->data.l[1] >> 24);
  XDND_AWARE_VIEW(info) = toplevel;
  updateSourceWindow(info, event);

#if 0
  if (event->data.l[1] & 1)
    /* XdndTypeList property is available */
    XDND_SOURCE_TYPES(info) = getTypesFromTypeList(scr, XDND_SOURCE_WIN(info));
  else
    XDND_SOURCE_TYPES(info) = getTypesFromThreeTypes(scr, event);
#endif
  XDND_SOURCE_TYPES(info) = getTypesFromThreeTypes(scr, event);

  /* to use if the 3 types are not enough */
  XDND_TYPE_LIST_AVAILABLE(info) = (event->data.l[1] & 1);
}

void WMDragDestinationStorePositionMsgInfo(WMDraggingInfo * info, WMView * toplevel, XClientMessageEvent * event)
{
  int x = event->data.l[2] >> 16;
  int y = event->data.l[2] & 0xffff;
  WMView *newDestView;

  newDestView = findDestinationViewInToplevel(toplevel, x, y);

  if (XDND_DEST_INFO(info) == NULL) {
    initDestinationDragInfo(info, newDestView);
    XDND_AWARE_VIEW(info) = toplevel;
    updateSourceWindow(info, event);
  } else {
    if (newDestView != XDND_DEST_VIEW(info)) {
      updateSourceWindow(info, event);
      XDND_DEST_VIEW(info) = newDestView;
      XDND_SOURCE_ACTION_CHANGED(info) = False;

      if (XDND_DEST_STATE(info) != waitEnterState)
        XDND_DEST_STATE(info) = idleState;
    } else {
      XDND_SOURCE_ACTION_CHANGED(info) = (XDND_SOURCE_ACTION(info) != event->data.l[4]);
    }
  }

  XDND_SOURCE_ACTION(info) = event->data.l[4];

  /* note: source position is not stored */
}

/* ----- End of Dragging Info ----- */

/* ----- Messages ----- */

/* send a DnD message to the source window */
static void
sendDnDClientMessage(WMDraggingInfo * info, Atom message,
		     unsigned long data1, unsigned long data2, unsigned long data3, unsigned long data4)
{
  if (!WMSendDnDClientMessage(WMViewScreen(XDND_AWARE_VIEW(info))->display,
                              XDND_SOURCE_WIN(info),
                              message, WMViewXID(XDND_AWARE_VIEW(info)), data1, data2, data3, data4)) {
    /* drop failed */
    WMDragDestinationInfoClear(info);
  }
}

/* send a xdndStatus message to the source, with position and size
   of the destination if it has no subwidget (requesting a position message
   on every move otherwise) */
static void sendStatusMessage(WMView * destView, WMDraggingInfo * info, Atom action)
{
  unsigned long data1;

  data1 = (action == None) ? 0 : 1;

  if (destView->childrenList == NULL) {
    WMScreen *scr = WMViewScreen(destView);
    int destX, destY;
    WMSize destSize = WMGetViewSize(destView);
    Window foo;

    XTranslateCoordinates(scr->display, WMViewXID(destView), scr->rootWin, 0, 0, &destX, &destY, &foo);

    sendDnDClientMessage(info,
                         WMViewScreen(destView)->xdndStatusAtom,
                         data1,
                         (destX << 16) | destY, (destSize.width << 16) | destSize.height, action);
  } else {
    /* set bit 1 to request explicitly position message on every move */
    data1 = data1 | 2;

    sendDnDClientMessage(info, WMViewScreen(destView)->xdndStatusAtom, data1, 0, 0, action);
  }
}

static void
storeDropData(WMView *destView, Atom selection, Atom target, Time timestamp, void *cdata, WMData *data)
{
  WMScreen *scr = WMViewScreen(destView);
  WMDraggingInfo *info = scr->dragInfo;
  WMData *dataToStore = NULL;

  /* Parameter not used, but tell the compiler that it is ok */
  (void) selection;
  (void) target;
  (void) timestamp;
  (void) cdata;

  if (data != NULL)
    dataToStore = WMRetainData(data);

  if (XDND_DEST_INFO(info) != NULL && XDND_DROP_DATAS(info) != NULL) {
    CFArrayAppendValue(XDND_DROP_DATAS(info), dataToStore);
    WMSendDnDClientMessage(scr->display, WMViewXID(destView),
                           scr->xdndSelectionAtom, WMViewXID(destView), 0, 0, 0, 0);
  }
}

static Bool requestDropDataInSelection(WMView * destView, const char *type)
{
  WMScreen *scr = WMViewScreen(destView);

  if (type != NULL) {
    if (!WMRequestSelection(destView,
                            scr->xdndSelectionAtom,
                            XInternAtom(scr->display, type, False),
                            CurrentTime, storeDropData, NULL)) {
      WMLogWarning("could not request data for dropped data");
      return False;
    }

    return True;
  }

  return False;
}

static Bool requestDropData(WMDraggingInfo * info)
{
  WMView *destView = XDND_DEST_VIEW(info);
  char *nextType = getNextRequestedDataType(info);

  while ((nextType != NULL)
         && (!requestDropDataInSelection(destView, nextType))) {
    /* store NULL if request failed, and try with next type */
    CFArrayAppendValue(XDND_DROP_DATAS(info), NULL);
    nextType = getNextRequestedDataType(info);
  }

  /* remains types to retrieve ? */
  return (nextType != NULL);
}

static void concludeDrop(WMView * destView)
{
  destView->dragDestinationProcs->concludeDragOperation(destView);
}

/* send cancel message to the source */
static void cancelDrop(WMView * destView, WMDraggingInfo * info)
{
  sendStatusMessage(destView, info, None);
  concludeDrop(destView);
  freeDestinationViewInfos(info);
}

/* suspend drop, when dragged icon enter an unregistered view
   or a register view that doesn't accept the drop */
static void suspendDropAuthorization(WMView * destView, WMDraggingInfo * info)
{
  sendStatusMessage(destView, info, None);

  /* Free datas that depend on destination behaviour */
  if (XDND_DROP_DATAS(info) != NULL) {
    CFRelease(XDND_DROP_DATAS(info));
    XDND_DROP_DATAS(info) = NULL;
  }

  XDND_REQUIRED_TYPES(info) = NULL;
}

/* cancel drop on Enter message, if protocol version is nok */
void WMDragDestinationCancelDropOnEnter(WMView * toplevel, WMDraggingInfo * info)
{
  if (XDND_DEST_VIEW_IS_REGISTERED(info))
    cancelDrop(XDND_DEST_VIEW(info), info);
  else
    sendStatusMessage(toplevel, info, None);

  WMDragDestinationInfoClear(info);
}

static void finishDrop(WMView * destView, WMDraggingInfo * info)
{
  sendDnDClientMessage(info, WMViewScreen(destView)->xdndFinishedAtom, 0, 0, 0, 0);
  concludeDrop(destView);
  WMDragDestinationInfoClear(info);
}

static Atom getAllowedAction(WMView * destView, WMDraggingInfo * info)
{
  WMScreen *scr = WMViewScreen(destView);

  return WMOperationToAction(scr,
                             destView->dragDestinationProcs->allowedOperation(destView,
                                                                              WMActionToOperation(scr,
                                                                                                  XDND_SOURCE_ACTION
                                                                                                  (info)),
                                                                              XDND_SOURCE_TYPES(info)));
}

static void *checkActionAllowed(WMView * destView, WMDraggingInfo * info)
{
  XDND_DEST_ACTION(info) = getAllowedAction(destView, info);

  if (XDND_DEST_ACTION(info) == None) {
    suspendDropAuthorization(destView, info);
    return dropNotAllowedState;
  }

  sendStatusMessage(destView, info, XDND_DEST_ACTION(info));
  return dropAllowedState;
}

static void *checkDropAllowed(WMView *destView, WMDraggingInfo *info)
{
  storeRequiredTypeList(info);

  if (destView->dragDestinationProcs->inspectDropData != NULL) {
    XDND_DROP_DATAS(info) = createDropDataArray(XDND_REQUIRED_TYPES(info));

    /* store first available data */
    if (requestDropData(info))
      return inspectDropDataState;

    /* no data retrieved, but inspect can allow it */
    if (destView->dragDestinationProcs->inspectDropData(destView, XDND_DROP_DATAS(info)))
      return checkActionAllowed(destView, info);

    suspendDropAuthorization(destView, info);
    return dropNotAllowedState;
  }

  return checkActionAllowed(destView, info);
}

static WMPoint *getDropLocationInView(WMView * view)
{
  Window rootWin, childWin;
  int rootX, rootY;
  unsigned int mask;
  WMPoint *location;

  location = (WMPoint *) wmalloc(sizeof(WMPoint));

  XQueryPointer(WMViewScreen(view)->display,
                WMViewXID(view), &rootWin, &childWin, &rootX, &rootY, &(location->x), &(location->y), &mask);

  return location;
}

static void callPerformDragOperation(WMView * destView, WMDraggingInfo * info)
{
  CFMutableArrayRef operationList = NULL;
  WMScreen *scr = WMViewScreen(destView);
  WMPoint *dropLocation;

  if (XDND_SOURCE_ACTION(info) == scr->xdndActionAsk)
    operationList = sourceOperationList(scr, XDND_SOURCE_WIN(info));

  dropLocation = getDropLocationInView(destView);
  destView->dragDestinationProcs->performDragOperation(destView, XDND_DROP_DATAS(info),
                                                       operationList, dropLocation);
  wfree(dropLocation);
  if (operationList != NULL)
    CFRelease(operationList);
}

/* ----- Destination timer ----- */
static void dragSourceResponseTimeOut(CFRunLoopTimerRef timer, void *destView)
{
  WMView *view = (WMView *) destView;
  WMDraggingInfo *info;

  WMLogWarning("delay for drag source response expired");
  info = WMViewScreen(view)->dragInfo;
  if (XDND_DEST_VIEW_IS_REGISTERED(info))
    cancelDrop(view, info);
  else {
    sendStatusMessage(view, info, None);
  }

  WMDragDestinationInfoClear(info);
}

void WMDragDestinationStopTimer()
{
  if (dndDestinationTimer != NULL) {
    WMDeleteTimerHandler(dndDestinationTimer);
    dndDestinationTimer = NULL;
  }
}

void WMDragDestinationStartTimer(WMDraggingInfo * info)
{
  WMDragDestinationStopTimer();

  if (XDND_DEST_STATE(info) != idleState)
    dndDestinationTimer = WMAddTimerHandler(XDND_SOURCE_RESPONSE_MAX_DELAY, 0,
                                            dragSourceResponseTimeOut,
                                            XDND_DEST_VIEW(info));
}

/* ----- End of Destination timer ----- */

/* ----- Destination states ----- */

#ifdef XDND_DEBUG
static const char *stateName(WMDndState * state)
{
  if (state == NULL)
    return "no state defined";

  if (state == idleState)
    return "idleState";

  if (state == waitEnterState)
    return "waitEnterState";

  if (state == inspectDropDataState)
    return "inspectDropDataState";

  if (state == dropAllowedState)
    return "dropAllowedState";

  if (state == dropNotAllowedState)
    return "dropNotAllowedState";

  if (state == waitForDropDataState)
    return "waitForDropDataState";

  return "unknown state";
}
#endif

static void *idleState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info)
{
  WMScreen *scr;
  Atom sourceMsg;

  if (destView->dragDestinationProcs != NULL) {
    scr = WMViewScreen(destView);
    sourceMsg = event->message_type;

    if (sourceMsg == scr->xdndPositionAtom) {
      destView->dragDestinationProcs->prepareForDragOperation(destView);

      if (XDND_SOURCE_TYPES(info) != NULL) {
        /* enter message infos are available */
        return checkDropAllowed(destView, info);
      }

      /* waiting for enter message */
      return waitEnterState;
    }
  }

  suspendDropAuthorization(destView, info);
  return idleState;
}

/*  Source position and action are stored,
    waiting for xdnd protocol version and source type */
static void *waitEnterState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info)
{
  WMScreen *scr = WMViewScreen(destView);
  Atom sourceMsg = event->message_type;

  if (sourceMsg == scr->xdndEnterAtom) {
    WMDragDestinationStoreEnterMsgInfo(info, destView, event);
    return checkDropAllowed(destView, info);
  }

  return waitEnterState;
}

/* We have requested a data, and have received it */
static void *inspectDropDataState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info)
{
  WMScreen *scr;
  Atom sourceMsg;

  scr = WMViewScreen(destView);
  sourceMsg = event->message_type;

  if (sourceMsg == scr->xdndSelectionAtom) {
    /* a data has been retrieved, store next available */
    if (requestDropData(info))
      return inspectDropDataState;

    /* all required (and available) datas are stored */
    if (destView->dragDestinationProcs->inspectDropData(destView, XDND_DROP_DATAS(info)))
      return checkActionAllowed(destView, info);

    suspendDropAuthorization(destView, info);
    return dropNotAllowedState;
  }

  return inspectDropDataState;
}

static void *dropNotAllowedState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info)
{
  WMScreen *scr = WMViewScreen(destView);
  Atom sourceMsg = event->message_type;

  if (sourceMsg == scr->xdndDropAtom) {
    finishDrop(destView, info);
    return idleState;
  }

  if (sourceMsg == scr->xdndPositionAtom) {
    if (XDND_SOURCE_ACTION_CHANGED(info)) {
      return checkDropAllowed(destView, info);
    } else {
      sendStatusMessage(destView, info, None);
      return dropNotAllowedState;
    }
  }

  return dropNotAllowedState;
}

static void *dropAllowedState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info)
{
  WMScreen *scr = WMViewScreen(destView);
  Atom sourceMsg = event->message_type;

  if (sourceMsg == scr->xdndDropAtom) {
    if (XDND_DROP_DATAS(info) != NULL) {
      /* drop datas were cached with inspectDropData call */
      callPerformDragOperation(destView, info);
    } else {
      XDND_DROP_DATAS(info) = createDropDataArray(XDND_REQUIRED_TYPES(info));

      /* store first available data */
      if (requestDropData(info))
        return waitForDropDataState;

      /* no data retrieved */
      callPerformDragOperation(destView, info);
    }

    finishDrop(destView, info);
    return idleState;
  }

  if (sourceMsg == scr->xdndPositionAtom) {
    if (XDND_SOURCE_ACTION_CHANGED(info)) {
      return checkDropAllowed(destView, info);
    } else {
      sendStatusMessage(destView, info, XDND_DEST_ACTION(info));
      return dropAllowedState;
    }
  }

  return dropAllowedState;
}

static void *waitForDropDataState(WMView * destView, XClientMessageEvent * event, WMDraggingInfo * info)
{
  WMScreen *scr = WMViewScreen(destView);
  Atom sourceMsg = event->message_type;

  if (sourceMsg == scr->xdndSelectionAtom) {
    /* store next data */
    if (requestDropData(info))
      return waitForDropDataState;

    /* all required (and available) datas are stored */
    callPerformDragOperation(destView, info);

    finishDrop(destView, info);
    return idleState;
  }

  return waitForDropDataState;
}

/* ----- End of Destination states ----- */

void WMDragDestinationStateHandler(WMDraggingInfo * info, XClientMessageEvent * event)
{
  WMView *destView;
  WMDndState *newState;

  wassertr(XDND_DEST_INFO(info) != NULL);
  wassertr(XDND_DEST_VIEW(info) != NULL);

  destView = XDND_DEST_VIEW(info);
  if (XDND_DEST_STATE(info) == NULL)
    XDND_DEST_STATE(info) = idleState;

#ifdef XDND_DEBUG

  printf("current dest state: %s\n", stateName(XDND_DEST_STATE(info)));
#endif

  newState = (WMDndState *) XDND_DEST_STATE(info) (destView, event, info);

#ifdef XDND_DEBUG

  printf("new dest state: %s\n", stateName(newState));
#endif

  if (XDND_DEST_INFO(info) != NULL) {
    XDND_DEST_STATE(info) = newState;
    if (XDND_DEST_STATE(info) != idleState)
      WMDragDestinationStartTimer(info);
  }
}

static void realizedObserver(CFNotificationCenterRef center,
                             void *self,
                             CFNotificationName name,
                             const void *viewObject,
                             CFDictionaryRef userInfo)
{
  WMView *view = (WMView *)viewObject;
  WMScreen *scr = WMViewScreen(view);

  XChangeProperty(scr->display, WMViewDrawable(view),
                  scr->xdndAwareAtom, XA_ATOM, XDND_PROPERTY_FORMAT, PropModeReplace, &XDNDversion, 1);

  CFNotificationCenterRemoveEveryObserver(CFNotificationCenterGetLocalCenter(), self);
}

static void WMSetXdndAwareProperty(WMScreen *scr, WMView *view)
{
  WMView *toplevel = WMTopLevelOfView(view);

  if (!toplevel->flags.xdndHintSet) {
    toplevel->flags.xdndHintSet = 1;

    if (toplevel->flags.realized) {
      XChangeProperty(scr->display, WMViewDrawable(toplevel),
                      scr->xdndAwareAtom, XA_ATOM, XDND_PROPERTY_FORMAT,
                      PropModeReplace, &XDNDversion, 1);
    } else {
      CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(),
                                      /* just use as an id */
                                      &view->dragDestinationProcs,
                                      realizedObserver,
                                      WMViewDidRealizeNotification, toplevel,
                                      CFNotificationSuspensionBehaviorDeliverImmediately);
    }
  }
}

void WMRegisterViewForDraggedTypes(WMView *view, CFMutableArrayRef acceptedTypes)
{
  Atom *types;
  int typeCount;
  int i;

  typeCount = CFArrayGetCount(acceptedTypes);
  types = wmalloc(sizeof(Atom) * (typeCount + 1));

  for (i = 0; i < typeCount; i++) {
    types[i] = XInternAtom(WMViewScreen(view)->display, CFArrayGetValueAtIndex(acceptedTypes, i), False);
  }
  types[i] = 0;

  view->droppableTypes = types;
  /* WMFreeArray(acceptedTypes); */

  WMSetXdndAwareProperty(WMViewScreen(view), view);
}

void WMUnregisterViewDraggedTypes(WMView * view)
{
  if (view->droppableTypes != NULL)
    wfree(view->droppableTypes);
  view->droppableTypes = NULL;
}

/*
  requestedOperation: operation requested by the source
  sourceDataTypes:  data types (mime-types strings) supported by the source
  (never NULL, destroyed when drop ends)
  return operation allowed by destination (self)
*/
static WMDragOperationType
defAllowedOperation(WMView *self, WMDragOperationType requestedOperation, CFMutableArrayRef sourceDataTypes)
{
  /* Parameter not used, but tell the compiler that it is ok */
  (void) self;
  (void) requestedOperation;
  (void) sourceDataTypes;

  /* no operation allowed */
  return WDOperationNone;
}

/*
  requestedOperation: operation requested by the source
  sourceDataTypes:  data types (mime-types strings) supported by the source
  (never NULL, destroyed when drop ends)
  return data types (mime-types strings) required by destination (self)
  or NULL if no suitable data type is available (force
  to 2nd pass with full source type list).
*/
static CFMutableArrayRef defRequiredDataTypes(WMView *self,
                                              WMDragOperationType requestedOperation,
                                              CFMutableArrayRef sourceDataTypes)
{
  /* Parameter not used, but tell the compiler that it is ok */
  (void) self;
  (void) requestedOperation;
  (void) sourceDataTypes;

  /* no data type allowed (NULL even at 2nd pass) */
  return NULL;
}

/*
  Executed when the drag enters destination (self)
*/
static void defPrepareForDragOperation(WMView * self)
{
  /* Parameter not used, but tell the compiler that it is ok */
  (void) self;
}

/*
  Checks datas to be dropped (optional).
  dropDatas: datas (WMData*) required by destination (self)
  (given in same order as returned by requiredDataTypes).
  A NULL data means it couldn't be retreived.
  Destroyed when drop ends.
  return true if data array is ok
*/
/* Bool (*inspectDropData)(WMView *self, WMArray *dropDatas); */

/*
  Process drop
  dropDatas: datas (WMData*) required by destination (self)
  (given in same order as returned by requiredDataTypes).
  A NULL data means it couldn't be retrieved.
  Destroyed when drop ends.
  operationList: if source operation is WDOperationAsk, contains
  operations (and associated texts) that can be asked
  to source. (destroyed after performDragOperation call)
  Otherwise this parameter is NULL.
*/
static void defPerformDragOperation(WMView *self,
                                    CFMutableArrayRef dropDatas,
                                    CFMutableArrayRef operationList,
                                    WMPoint *dropLocation)
{
  /* Parameter not used, but tell the compiler that it is ok */
  (void) self;
  (void) dropDatas;
  (void) operationList;
  (void) dropLocation;
}

/* Executed after drop */
static void defConcludeDragOperation(WMView *self)
{
  /* Parameter not used, but tell the compiler that it is ok */
  (void) self;
}

void WMSetViewDragDestinationProcs(WMView *view, WMDragDestinationProcs *procs)
{
  if (view->dragDestinationProcs == NULL) {
    view->dragDestinationProcs = wmalloc(sizeof(WMDragDestinationProcs));
  }

  *view->dragDestinationProcs = *procs;

  /*XXX fill in non-implemented stuffs */
  if (procs->allowedOperation == NULL) {
    view->dragDestinationProcs->allowedOperation = defAllowedOperation;
  }
  if (procs->allowedOperation == NULL) {
    view->dragDestinationProcs->requiredDataTypes = defRequiredDataTypes;
  }

  /* note: inspectDropData can be NULL, if data consultation is not needed
     to give drop permission */

  if (procs->prepareForDragOperation == NULL) {
    view->dragDestinationProcs->prepareForDragOperation = defPrepareForDragOperation;
  }
  if (procs->performDragOperation == NULL) {
    view->dragDestinationProcs->performDragOperation = defPerformDragOperation;
  }
  if (procs->concludeDragOperation == NULL) {
    view->dragDestinationProcs->concludeDragOperation = defConcludeDragOperation;
  }
}
