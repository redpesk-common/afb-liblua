/*
 * Copyright (C) 2015-2021 IoT.bzh Company
 * Author: Fulup Ar Foll <fulup@iot.bzh>
 *
 * $RP_BEGIN_LICENSE$
 * Commercial License Usage
 *  Licensees holding valid commercial IoT.bzh licenses may use this file in
 *  accordance with the commercial license agreement provided with the
 *  Software or, alternatively, in accordance with the terms contained in
 *  a written agreement between you and The IoT.bzh Company. For licensing terms
 *  and conditions see https://www.iot.bzh/terms-conditions. For further
 *  information use the contact form at https://www.iot.bzh/contact.
 *
 * GNU General Public License Usage
 *  Alternatively, this file may be used under the terms of the GNU General
 *  Public license version 3. This license is as published by the Free Software
 *  Foundation and appearing in the file LICENSE.GPLv3 included in the packaging
 *  of this file. Please review the following information to ensure the GNU
 *  General Public License requirements will be met
 *  https://www.gnu.org/licenses/gpl-3.0.html.
 * $RP_END_LICENSE$
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <stdarg.h>
#include <assert.h>
#include <wrap-json.h>

#include <glue-afb.h>
#include <glue-utils.h>
#include "lua-afb.h"
#include "lua-utils.h"
#include "lua-callbacks.h"
#include "lua-strict.h"

// TDB Jose should be removed
#include <libafb/sys/verbose.h>

#define SUBCALL_MAX_RPLY 8

static int LuaPrintInfo(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_INFO);
    return err;
}

static int LuaPrintError(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_ERROR);
    return err; // no value return
}

static int LuaPrintWarning(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_WARNING);
    return err;
}

static int LuaPrintNotice(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_NOTICE);
    return err;
}

static int LuaPrintDebug(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_DEBUG);
    return err;
}

static int LuaAfbTimerAddref(lua_State* luaState) {
    const char *errorMsg=NULL;

    LuaHandleT *luaTimer= LuaTimerPop(luaState, 1);
    if (!luaTimer) {
        errorMsg= "Invalid timer handle";
        goto OnErrorExit;
    }

    afb_timer_addref (luaTimer->lua.timer.afb);
    json_object_get(luaTimer->lua.timer.configJ);
    luaTimer->lua.timer.usage++;

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaTimer, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbTimerUnref(lua_State* luaState) {
    const char *errorMsg=NULL;

    LuaHandleT *luaTimer= LuaTimerPop(luaState, 1);
    if (!luaTimer) {
        errorMsg= "Invalid timer handle";
        goto OnErrorExit;
    }

    LuaTimerClearCb(luaTimer);
    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaTimer, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaTimerNew(lua_State *luaState)
{
    const char *errorMsg = NULL;

    LuaHandleT *luaHandle = (LuaHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!luaHandle)
    {
        errorMsg = "invalid request handle";
        goto OnErrorExit;
    }

    LuaHandleT *luaTimer = (LuaHandleT *)calloc(1, sizeof(LuaHandleT));
    luaTimer->magic = LUA_TIMER_MAGIC;
    luaTimer->luaState = lua_newthread(luaState); // private interpretor
    lua_pushnil(luaTimer->luaState); // keep thread state until timer die

    luaTimer->lua.timer.configJ = LuaPopOneArg(luaState, LUA_FIRST_ARG+1);
    json_object_get(luaTimer->lua.timer.configJ);
    if (!luaTimer->lua.timer.configJ)
    {
        errorMsg = "error syntax: timernew(api, config, context)";
        goto OnErrorExit;
    }

    switch (lua_type(luaState, LUA_FIRST_ARG + 2)) {
        case LUA_TLIGHTUSERDATA:
            luaTimer->lua.timer.usrdata= lua_touserdata(luaState, LUA_FIRST_ARG+2);
            break;
        case LUA_TNIL:
            luaTimer->lua.timer.usrdata=NULL;
            break;
        default:
            luaTimer->lua.timer.usrdata= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 2);
    }

    unsigned period, count=0;
    luaTimer->lua.timer.configJ= luaTimer->lua.timer.configJ;
    json_object_get(luaTimer->lua.timer.configJ);
    int err = wrap_json_unpack(luaTimer->lua.timer.configJ, "{ss, ss, si, s?i !}",
        "uid"     , &luaTimer->lua.timer.uid,
        "callback", &luaTimer->lua.timer.callback,
        "period"  , &period,
        "count"   , &count
    );
    if (err)
    {
        errorMsg = "error timer config)";
        goto OnErrorExit;
    }

    // Fulup TBD check how to implement autounref
    err= afb_timer_create (&luaTimer->lua.timer.afb, 0, 0, 0, count, period, 0, LuaTimerCb, (void*)luaTimer, 0);
    if (err) {
        errorMsg= "fail to create timer";
        goto OnErrorExit;
    }

    lua_pushlightuserdata(luaState, luaTimer);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaHandle, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbRespond(lua_State *luaState)
{
    const char *errorMsg = NULL;
    unsigned argc = lua_gettop(luaState);
    json_object *argsJ[argc];
    afb_data_t reply[argc];

    LuaHandleT *luaRqt = LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!luaRqt)
    {
        errorMsg = "invalid request handle";
        goto OnErrorExit;
    }

    // get restart status 1st
    int status = (int)lua_tointeger(luaState, LUA_FIRST_ARG + 1);

    // get response from LUA and push them as afb-v4 object
    for (int idx = 0; idx < argc - 2; idx++)
    {
        argsJ[idx] = LuaPopOneArg(luaState, LUA_FIRST_ARG + idx + 2);
        if (!argsJ[idx])
        {
            errorMsg = "error pushing arguments";
            goto OnErrorExit;
        }
        afb_create_data_raw(&reply[idx], AFB_PREDEFINED_TYPE_JSON_C, argsJ[idx], 0, (void *)json_object_put, argsJ[idx]);
    }

    LuaAfbReply(luaRqt, status, argc - 2, reply);
    return 0;

OnErrorExit:
{
    afb_data_t reply;
    json_object *errorJ = LuaJsonDbg(luaState, errorMsg);
    afb_create_data_raw(&reply, AFB_PREDEFINED_TYPE_JSON_C, errorJ, 0, (void *)json_object_put, errorJ);
    LuaAfbReply(luaRqt, 0, 1, &reply);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}
}

static int LuaAfbEventPush(lua_State *luaState)
{
    unsigned argc = lua_gettop(luaState);
    int index;
    const char *errorMsg = NULL;
    json_object *argsJ[argc];
    afb_data_t reply[argc];

    // check evt handle
    LuaHandleT *luaEvt = LuaEventPop(luaState, LUA_FIRST_ARG);
    if (!luaEvt)
    {
        errorMsg = "LuaAfbEventPush: invalid event handle";
        goto OnErrorExit;
    }

    // get response from LUA and push them as afb-v4 object
    for (index = 0; index < argc - 1; index++)
    {
        argsJ[index] = LuaPopOneArg(luaState, LUA_FIRST_ARG + index + 1);
        if (!argsJ[index])
        {
            errorMsg = "error pushing arguments";
            goto OnErrorExit;
        }
        afb_create_data_raw(&reply[index], AFB_PREDEFINED_TYPE_JSON_C, argsJ[index], 0, (void *)json_object_put, argsJ[index]);
    }

    int status = afb_event_push(luaEvt->lua.evt.afb, index, reply);
    if (status < 0)
    {
        LUA_AFB_NOTICE(luaEvt, "LuaAfbEventPush: Fail name subscriber event=%s count=%d", luaEvt->lua.evt.uid, luaEvt->lua.evt.count);
        errorMsg = "fail sending event";
        goto OnErrorExit;
    }
    luaEvt->lua.evt.count++;
    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaEvt, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbEventSubscribe(lua_State *luaState)
{
    const char *errorMsg = NULL;

    LuaHandleT *luaRqt = LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!luaRqt)
    {
        errorMsg = "invalid request handle";
        goto OnErrorExit;
    }

    // check evt handle
    LuaHandleT *luaEvt = LuaEventPop(luaState, LUA_FIRST_ARG + 1);
    if (!luaEvt)
    {
        errorMsg = "invalid event handle";
        goto OnErrorExit;
    }

    int err = afb_req_subscribe(luaRqt->lua.rqt.afb, luaEvt->lua.evt.afb);
    if (err)
    {
        errorMsg = "fail subscribing afb event";
        goto OnErrorExit;
    }
    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaEvt, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbEventUnsubscribe(lua_State *luaState)
{
    const char *errorMsg = NULL;

    LuaHandleT *luaRqt = LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!luaRqt)
    {
        errorMsg = "invalid request handle";
        goto OnErrorExit;
    }

    // check evt handle
    LuaHandleT *luaEvt = LuaEventPop(luaState, LUA_FIRST_ARG + 1);
    if (!luaEvt)
    {
        errorMsg = "invalid event handle";
        goto OnErrorExit;
    }

    int err = afb_req_unsubscribe(luaRqt->lua.rqt.afb, luaEvt->lua.evt.afb);
    if (err)
    {
        errorMsg = "fail subscribing afb event";
        goto OnErrorExit;
    }
    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaEvt, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbEventNew(lua_State *luaState)
{
    const char *errorMsg = NULL;
    int err;

    LuaHandleT *luaApi = LuaApiPop(luaState, LUA_FIRST_ARG);
    if (!luaApi)
    {
        errorMsg = "invalid request handle";
        goto OnErrorExit;
    }

    // create a new binder event
    LuaHandleT *luaEvt = calloc(1, sizeof(LuaHandleT));
    luaEvt->magic = LUA_EVT_MAGIC;
    luaEvt->luaState = lua_newthread(luaState);
    lua_pushnil(luaEvt->luaState); // keep thread state until timer die

    luaEvt->lua.evt.configJ = LuaPopOneArg(luaState, LUA_FIRST_ARG+1);
    json_object_get(luaEvt->lua.evt.configJ);
    if (!luaEvt->lua.evt.configJ)
    {
        errorMsg = "error syntax: evtnew(api, config)";
        goto OnErrorExit;
    }

    luaEvt->lua.evt.configJ= luaEvt->lua.evt.configJ;
    json_object_get(luaEvt->lua.evt.configJ);
    err = wrap_json_unpack(luaEvt->lua.evt.configJ, "{ss, s?s}"
        ,"uid"     , &luaEvt->lua.evt.uid
        ,"name"    , &luaEvt->lua.evt.name
    );
    if (err)
    {
        errorMsg = json_object_get_string(luaEvt->lua.evt.configJ);
        goto OnErrorExit;
    }
    if (!luaEvt->lua.evt.name) luaEvt->lua.evt.name=luaEvt->lua.evt.uid;

    err= afb_api_new_event(luaApi->lua.api.afb, luaEvt->lua.evt.name, &luaEvt->lua.evt.afb);
    if (err)
    {
        errorMsg = "afb-api event creation fail";
        goto OnErrorExit;
    }

    // push event handler as a LUA opaque handle
    lua_pushlightuserdata(luaState, luaEvt);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaEvt, errorMsg);
    lua_pushlightuserdata(luaState, luaEvt);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 2;
}

static int LuaAfbAsyncCall(lua_State *luaState)
{
    const char *errorMsg = NULL;
    unsigned argc = lua_gettop(luaState);
    json_object *argsJ[argc];
    afb_data_t params[argc];

    LuaHandleT *luaHandle = (LuaHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!luaHandle)
    {
        errorMsg = "invalid lua handle";
        goto OnErrorExit;
    }

    // get restart status 1st
    void *context;
    const char *apiname = lua_tostring(luaState, LUA_FIRST_ARG + 1);
    const char *verbname= lua_tostring(luaState, LUA_FIRST_ARG + 2);
    const char *luafunc = lua_tostring(luaState, LUA_FIRST_ARG + 3);
    int luaType= lua_type(luaState, LUA_FIRST_ARG + 4);
    switch (luaType) {
        case LUA_TLIGHTUSERDATA:
            context= lua_touserdata(luaState, LUA_FIRST_ARG+4);
            break;
        case LUA_TNIL:
            context=NULL;
            break;
        default:
            context= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 4);
            if (!context) verbname=NULL; // force syntax error
    }

    if (!apiname || !verbname || !luafunc) {
        errorMsg = "syntax: callasync(rqt|api,'apiname','verbname','lua-callback',context,arg1... argn";
        goto OnErrorExit;
    }

    // retreive subcall api argument(s)
    int index;
    for (index = 0; index < argc-(LUA_FIRST_ARG+4); index++)
    {
        argsJ[index] = LuaPopOneArg(luaState, LUA_FIRST_ARG + index + 5);
        if (!argsJ[index])
        {
            errorMsg = "invalid input argument type";
            goto OnErrorExit;
        }
        afb_create_data_raw(&params[index], AFB_PREDEFINED_TYPE_JSON_C, argsJ[index], 0, (void *)json_object_put, argsJ[index]);
    }

    LuaAsyncCtxT *cbHandle= calloc(1,sizeof(LuaAsyncCtxT));
    cbHandle->handle= luaHandle;
    cbHandle->luafunc= strdup(luafunc);
    cbHandle->context= context;

    switch (luaHandle->magic) {
        case LUA_RQT_MAGIC:
            afb_req_subcall (luaHandle->lua.rqt.afb, apiname, verbname, index, params, afb_req_subcall_catch_events, LuaAfbRqtSubcallCb, (void*)cbHandle);
            break;
        case LUA_LOCK_MAGIC:
        case LUA_API_MAGIC:
        case LUA_BINDER_MAGIC:
            afb_api_call (LuaAfbGetApi(luaHandle), apiname, verbname, index, params, LuaAfbApiSubcallCb, (void*)cbHandle);
            break;

        default:
            errorMsg = "handle should be a req|api";
            goto OnErrorExit;
    }

    lua_pushinteger (luaHandle->luaState, 0);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaHandle, errorMsg);
    lua_pushinteger (luaHandle->luaState, -1);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 2;
}

static int LuaAfbSyncCall(lua_State *luaState)
{
    const char *errorMsg = NULL;
    unsigned argc = lua_gettop(luaState);
    int err, status, index;
    unsigned nreplies= SUBCALL_MAX_RPLY;
    afb_data_t replies[SUBCALL_MAX_RPLY];

    LuaHandleT *luaHandle = (LuaHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!luaHandle)
    {
        errorMsg = "invalid lua handle";
        goto OnErrorExit;
    }

    // get restart status 1st
    const char *apiname = lua_tostring(luaState, LUA_FIRST_ARG + 1);
    const char *verbname= lua_tostring(luaState, LUA_FIRST_ARG + 2);
    if (!apiname || !verbname) {
        errorMsg = "syntax: callsync(rqt|api,'apiname','verbname', arg1... argn";
        goto OnErrorExit;
    }

    //  retreive subcall api argument(s) and call api/verb
    {
        json_object *argsJ[argc];
        afb_data_t params[argc];
        for (index = 0; index < argc-3; index++)
        {
            argsJ[index] = LuaPopOneArg(luaState, LUA_FIRST_ARG + index + 3);
            if (!argsJ[index])
            {
                errorMsg = "invalid input argument type";
                goto OnErrorExit;
            }
            afb_create_data_raw(&params[index], AFB_PREDEFINED_TYPE_JSON_C, argsJ[index], 0, (void *)json_object_put, argsJ[index]);
        }

        switch (luaHandle->magic) {
            case LUA_RQT_MAGIC:
                err= afb_req_subcall_sync (luaHandle->lua.rqt.afb, apiname, verbname, index, params, afb_req_subcall_catch_events, &status, &nreplies, replies);
                break;
            case LUA_API_MAGIC:
            case LUA_BINDER_MAGIC:
            case LUA_LOCK_MAGIC:
                err= afb_api_call_sync(LuaAfbGetApi(luaHandle), apiname, verbname, index, params, &status, &nreplies, replies);
                break;

            default:
                errorMsg = "handle should be a req|api";
                goto OnErrorExit;
        }
        if (err) {
            status   = err;
            errorMsg= "api subcall fail";
            goto OnErrorExit;
        }
        // subcall was refused
        if (AFB_IS_BINDER_ERRNO(status)) {
            errorMsg= afb_error_text(status);
            goto OnErrorExit;
        }
    }

    // retreive subcall response and build LUA response
    lua_pushinteger (luaState, status);
    errorMsg= LuaPushAfbReply (luaState, nreplies, replies, &index);
    if (errorMsg) goto OnErrorExit;

    return index+1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaHandle, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbschedwait(lua_State *luaState)
{
    LuaHandleT *luaLock=NULL;
    const char *errorMsg = NULL;
    int err;

    LuaHandleT *luaHandle = (LuaHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!luaHandle)
    {
        errorMsg = "invalid lua handle";
        goto OnErrorExit;
    }

    const char *funcname= lua_tostring(luaState, LUA_FIRST_ARG + 1);
    if (!funcname) {
        errorMsg = "syntax: schedunlock(handle, lock, status)";
        goto OnErrorExit;
    }

    int isNum;
    int timeout = (int) lua_tointegerx (luaState, LUA_FIRST_ARG+2, &isNum);
    if (!isNum) {
        errorMsg = "syntax: schedunlock(handle, 'lua-funcname',timeout-seconds, context)";
        goto OnErrorExit;
    }

    luaLock= calloc (1, sizeof(LuaHandleT));
    luaLock->magic= LUA_LOCK_MAGIC;
    luaLock->luaState= luaState;
    luaLock->lua.lock.luafunc= strdup(funcname);
    luaLock->lua.lock.apiv4= LuaAfbGetApi(luaHandle);
    luaLock->lua.lock.dataJ= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 2);

    err= afb_sched_enter(NULL, timeout, AfbschedwaitCb, luaLock);
    if (err) {
        errorMsg= "fail to register afb_sched_enter";
        goto OnErrorExit;
    }

    // return user status
    lua_pushinteger (luaState, luaLock->lua.lock.status);

    // free lock handle
    free (luaLock->lua.lock.luafunc);
    free (luaLock);

    return 1;

OnErrorExit:
    if (luaLock) {
        if (luaLock->lua.lock.luafunc) free (luaLock->lua.lock.luafunc);
        free (luaLock);
    }
    LUA_DBG_ERROR(luaState,luaHandle, errorMsg);
    lua_pushinteger (luaState, -1);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 2;
}

static int LuaAfbSchedUnlock(lua_State *luaState)
{
    const char *errorMsg = NULL;
    int err;

    LuaHandleT *luaHandle = (LuaHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!luaHandle)
    {
        errorMsg = "invalid lua handle: syntax: schedunlock(handle, lock, status)";
        goto OnErrorExit;
    }

    LuaHandleT *luaLock = (LuaHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG+1);
    if (!luaLock || luaLock->magic != LUA_LOCK_MAGIC) {
        errorMsg = "invalid lock handle: syntax: schedunlock(handle, lock, status)";
        goto OnErrorExit;
    }

    int isNum;
    luaLock->lua.lock.status = (int)lua_tointegerx (luaState, LUA_FIRST_ARG +2, &isNum);
    if (!isNum) {
        errorMsg = "status require integer: syntax: schedunlock(handle, lock, status)";
    }

    err= afb_sched_leave(luaLock->lua.lock.afb);
    if (err) {
        errorMsg= "fail to register afb_sched_enter";
        goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaHandle, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbEventHandler(lua_State *luaState)
{
    const char *errorMsg = NULL;

    LuaHandleT *luaHandle = lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!luaHandle)
    {
        errorMsg = "invalid lua handle";
        goto OnErrorExit;
    }

    // retreive API from lua handle
    afb_api_t afbApi= LuaAfbGetApi(luaHandle);
    if (!afbApi) {
        errorMsg = "invalid lua handle type (binder|api|rqt)";
        goto OnErrorExit;
}

    // get restart status 1st
    json_object *configJ = LuaPopOneArg(luaState, LUA_FIRST_ARG + 1);
    if (!configJ) {
        errorMsg= "syntax: addevent(handle, {uid='xxx', pattern='yyy', func='zzz'}, context)";
        goto OnErrorExit;
    }

    LuaHandleT *luaHandler= calloc(1, sizeof(LuaHandleT));
    luaHandler->magic= LUA_HANDLER_MAGIC;
    luaHandler->luaState = lua_newthread(luaState);
    lua_pushnil(luaHandler->luaState); // keep thread state until timer die

    const char *unused;
    int err= wrap_json_unpack (configJ, "{ss ss ss}"
        ,"uid"     , &luaHandler->lua.handler.uid
        ,"func"    , &luaHandler->lua.handler.callback
        ,"pattern" , &unused
    );
    if (err) {
        errorMsg= "syntax: addevent(handle, {uid='xxx', pattern='yyy', func='zzz'}, context)";
        goto OnErrorExit;
    }

    switch (lua_type(luaState, LUA_FIRST_ARG + 2)) {
        case LUA_TLIGHTUSERDATA:
            luaHandler->lua.handler.usrdata= lua_touserdata(luaState, LUA_FIRST_ARG+2);
            break;
        case LUA_TNIL:
            luaHandler->lua.handler.usrdata=NULL;
            break;
        default:
            luaHandler->lua.handler.usrdata= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 2);
    }

    errorMsg= AfbAddOneEvent (afbApi, configJ, LuaAfbEvtHandlerCb, luaHandler);
    if (errorMsg) {
        goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaHandle, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbVerbAdd(lua_State *luaState)
{
    const char *errorMsg = NULL;

    LuaHandleT *binder = LuaBinderPop(luaState);
    assert(binder);

    LuaHandleT *luaApi = LuaApiPop(luaState, LUA_FIRST_ARG);
    if (!luaApi)
    {
        errorMsg = "invalid lua api handle";
        goto OnErrorExit;
    }

    // get restart status 1st
    json_object *configJ = LuaPopOneArg(luaState, LUA_FIRST_ARG + 1);
    if (!configJ) {
        errorMsg= "syntax: addevent(handle, {uid='xxx', pattern='yyy', func='zzz}, context)";
        goto OnErrorExit;
    }

    void *context;
    switch (lua_type(luaState, LUA_FIRST_ARG + 2)) {
        case LUA_TLIGHTUSERDATA:
            context = lua_touserdata(luaState, LUA_FIRST_ARG+2);
            break;
        case LUA_TNIL:
            context=NULL;
            break;
        default:
            context= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 2);
    }

    errorMsg= AfbAddOneVerb (binder->lua.binder.afb, luaApi->lua.api.afb, configJ, LuaAfbVerbCb, context);
    if (errorMsg) {
        goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaApi, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbSetLoa(lua_State *luaState)
{
    const char *errorMsg = NULL;
    int isNum, loa, err;

    LuaHandleT *luaRqt = LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!luaRqt)
    {
        errorMsg = "invalid lua request handle";
        goto OnErrorExit;
    }

    loa = (int) lua_tointegerx (luaState, LUA_FIRST_ARG + 1, &isNum);
    if (!isNum) {
        errorMsg = "syntax: setloa(rqt, loa)";
        goto OnErrorExit;
    }

    err= afb_req_session_set_LOA(luaRqt->lua.rqt.afb, loa);
    if (err < 0) {
        errorMsg="Invalid Rqt Session";
        goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaRqt, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbClientInfo(lua_State *luaState)
{
    const char *errorMsg = NULL;

    LuaHandleT *luaRqt = LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!luaRqt)
    {
        errorMsg = "invalid lua request handle";
        goto OnErrorExit;
    }

    // if optionnal key is provided return only this value
    const char *key = lua_tostring(luaState, LUA_FIRST_ARG+1);

    json_object *clientJ= afb_req_get_client_info(luaRqt->lua.rqt.afb);
    if (!clientJ) {
        errorMsg= "(hoops) no client rqt info";
        goto OnErrorExit;
    }

    if (!key) {
        LuaPushOneArg (luaState, clientJ);
    } else {
        json_object *keyJ= json_object_object_get(clientJ, key);
        if (!keyJ) {
            errorMsg= "unknown client info key";
            goto OnErrorExit;
        }
        LuaPushOneArg (luaState, keyJ);
    }

    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaRqt, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaGetConfig(lua_State *luaState)
{
    const char *errorMsg = NULL;
    json_object *configJ;

    LuaHandleT *binder = LuaBinderPop(luaState);
    assert(binder);

    // check api handle
    LuaHandleT *luaAfb = lua_touserdata(luaState, 1);
    if (!luaAfb)
    {
        errorMsg = "invalid lua/afb handle";
        goto OnErrorExit;
    }

    switch (luaAfb->magic)
    {
    case LUA_API_MAGIC:
        configJ = luaAfb->lua.api.configJ;
        break;
    case LUA_BINDER_MAGIC:
        configJ = luaAfb->lua.binder.configJ;
        break;
    case LUA_TIMER_MAGIC:
        configJ = luaAfb->lua.timer.configJ;
        break;
    case LUA_EVT_MAGIC:
        configJ = luaAfb->lua.evt.configJ;
        break;
    default:
        errorMsg = "LuaApiGetConfig: unsupported lua/afb handle";
        goto OnErrorExit;
    }

    if (!configJ) {
        errorMsg= "LuaHandle config missing";
        goto OnErrorExit;
    }

    // if user only one one key return that one
    const char *key = lua_tostring(luaState, 2);
    if (!key)
    {
        LuaPushOneArg(luaState, configJ);
    }
    else
    {
        json_object *slotJ = json_object_object_get(configJ, key);
        if (!slotJ)
        {
            errorMsg = "LuaApiGetConfig: unknown config key";
            goto OnErrorExit;
        }
        LuaPushOneArg(luaState, slotJ);
    }
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,luaAfb, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaJsonToTable(lua_State *luaState)
{
    LuaHandleT *binder = LuaBinderPop(luaState);
    assert(binder);
    const char *errorMsg = NULL;

    // check api handle
    json_object* configJ= (json_object*)lua_touserdata(luaState, LUA_FIRST_ARG);
     if (!configJ) {
        errorMsg= "not lua light data";
        goto OnErrorExit;
    }

    // if user only one one key return that one
    const char *key = lua_tostring(luaState, LUA_FIRST_ARG+1);
    if (!key)
    {
        LuaPushOneArg(luaState, configJ);
    }
    else
    {
        json_object *slotJ = json_object_object_get(configJ, key);
        if (!slotJ)
        {
            errorMsg = "LuaApiGetConfig: unknown config key";
            goto OnErrorExit;
        }
        LuaPushOneArg(luaState, slotJ);
    }
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState, binder, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}


static int LuaAfbApiCreate(lua_State *luaState)
{
    const char *errorMsg = NULL;
    json_object *configJ;
    int err;

    LuaHandleT *binder = LuaBinderPop(luaState);
    assert(binder);

    // parse afbApi config
    configJ = LuaPopArgs(luaState, LUA_FIRST_ARG);
    if (!configJ)
    {
        errorMsg = "LuaAfbApiCreate: invalid lua table config";
        goto OnErrorExit;
    }

    LuaHandleT *luaApi = calloc(1, sizeof(LuaHandleT));
    luaApi->magic = LUA_API_MAGIC;
    luaApi->luaState = lua_newthread(luaState);
    lua_pushnil(luaApi->luaState); // prevent garbage collector from cleaning this thread
    luaApi->lua.api.configJ = configJ;

    const char *afbApiUri = NULL;
    err = wrap_json_unpack(configJ, "{s?s s?s}", "control", &luaApi->lua.api.ctrlCb, "uri", &afbApiUri);
    if (err)
    {
        errorMsg = "LuaAfbApiCreate: invalid json config";
        goto OnErrorExit;
    }

    if (afbApiUri)
    {
        // imported shadow api
        errorMsg = AfbApiImport(binder->lua.binder.afb, configJ);
    }
    else
    {
        // this is a lua api, is control function defined let's add LuaApiCtrlCb afb main control function
        if (luaApi->lua.api.ctrlCb)
            errorMsg = AfbApiCreate(binder->lua.binder.afb, configJ, &luaApi->lua.api.afb, LuaApiCtrlCb, LuaAfbInfoCb, LuaAfbVerbCb, LuaAfbEvtHandlerCb, luaApi);
        else
            errorMsg = AfbApiCreate(binder->lua.binder.afb, configJ, &luaApi->lua.api.afb, NULL, LuaAfbInfoCb, LuaAfbVerbCb, LuaAfbEvtHandlerCb, luaApi);
    }
    if (errorMsg)
        goto OnErrorExit;

    lua_pushlightuserdata(luaState, luaApi);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,binder, errorMsg);
    lua_pushliteral(luaState, "LuaAfbApiCreate fail");
    lua_error(luaState);
    return 1;
}

static int LuaAfbBindingLoad(lua_State *luaState)
{
    const char *errorMsg = NULL;
    json_object *configJ;

    LuaHandleT *binder = LuaBinderPop(luaState);
    if (!binder)
        goto OnErrorExit;

    // parse api config
    configJ = LuaPopArgs(luaState, LUA_FIRST_ARG);
    if (!configJ)
    {
        errorMsg = "LuaAfbBindingLoad invalid lua table config";
        goto OnErrorExit;
    }

    errorMsg = AfbBindingLoad(binder->lua.binder.afb, configJ);
    if (errorMsg)
        goto OnErrorExit;

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,binder, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

// this routine execute within mainloop context when binder is ready to go
static int LuaAfbMainLoop(lua_State *luaState)
{
    LuaHandleT *binder = LuaBinderPop(luaState);
    const char *errorMsg = NULL;
    json_object *configJ;
    int status = 0;

    // parse api config
    configJ = LuaPopArgs(luaState, LUA_FIRST_ARG);
    if (!configJ)
    {
        errorMsg = "invalid lua table config";
        goto OnErrorExit;
    }

    // main loop only return when binder startup func return status!=0
    LUA_AFB_NOTICE(binder, "Entering binder mainloop");
    status = AfbBinderStart(binder->lua.binder.afb, configJ, LuaAfbStartupCb, binder);
    lua_pushinteger(luaState, status);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,binder, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

// Load Lua glue functions
static int LuaAfbBinder(lua_State *luaState)
{
    LuaHandleT *binder = LuaBinderPop(luaState);
    static int luaLoaded = 0;
    const char *errorMsg = NULL;

    // Lua loads only once
    if (luaLoaded)
    {
        errorMsg = "already loaded";
        goto OnErrorExit;
    }
    luaLoaded = 1;

    // parse api config
    binder->lua.binder.configJ = LuaPopArgs(luaState, LUA_FIRST_ARG);
    if (!binder->lua.binder.configJ)
    {
        errorMsg = "invalid config";
        goto OnErrorExit;
    }

    errorMsg = AfbBinderConfig(binder->lua.binder.configJ, &binder->lua.binder.afb);
    if (errorMsg)
        goto OnErrorExit;

    // load auxiliary libraries
    luaL_openlibs(luaState);

    lua_pushlightuserdata(luaState, binder);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,binder, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int LuaAfbExit(lua_State *luaState)
{
    LuaHandleT *binder = LuaBinderPop(luaState);
    LuaHandleT *handle = lua_touserdata(luaState, LUA_FIRST_ARG);

    int isNum;
    long exitCode = lua_tointegerx(luaState, LUA_FIRST_ARG+1, &isNum);
    if (!isNum) goto OnErrorExit;

    // exit binder
    AfbBinderExit(binder->lua.binder.afb, (int)exitCode);

    return 0;
OnErrorExit:
    LUA_DBG_ERROR(luaState, handle, "status should integer");
    lua_error(luaState);
    return 0;
}

static int LuaAfbPing(lua_State *luaState)
{
    LuaHandleT *binder = LuaBinderPop(luaState);
    static int count = 0;

    LUA_AFB_NOTICE(binder, "LuaAfbPing count=%d", count);
    lua_pushstring(luaState, "pong");
    lua_pushinteger(luaState, count++);
    return 2;
}

static const luaL_Reg afbFunction[] = {
    {"ping", LuaAfbPing},
    {"binder", LuaAfbBinder},
    {"apiadd", LuaAfbApiCreate},
    {"verbadd", LuaAfbVerbAdd},
    {"config", LuaGetConfig},
    {"binding", LuaAfbBindingLoad},
    {"mainloop", LuaAfbMainLoop},
    {"notice", LuaPrintNotice},
    {"info", LuaPrintInfo},
    {"warning", LuaPrintWarning},
    {"debug", LuaPrintDebug},
    {"error", LuaPrintError},
    {"respond", LuaAfbRespond},
    {"exit", LuaAfbExit},
    {"evtsubscribe", LuaAfbEventSubscribe},
    {"evtunsubscribe", LuaAfbEventUnsubscribe},
    {"evthandler", LuaAfbEventHandler},
    {"evtnew", LuaAfbEventNew},
    {"evtpush", LuaAfbEventPush},
    {"timerunref", LuaAfbTimerUnref},
    {"timeraddref", LuaAfbTimerAddref},
    {"timernew", LuaTimerNew},
    {"callasync", LuaAfbAsyncCall},
    {"callsync", LuaAfbSyncCall},
    {"setloa", LuaAfbSetLoa},
    {"clientinfo", LuaAfbClientInfo},
    {"schedwait" , LuaAfbschedwait},
    {"schedunlock", LuaAfbSchedUnlock},
    {"luastrict", LuaAfbStrict},
    {"serialize", LuaJsonToTable},


    {NULL, NULL} /* sentinel */
};

// lua_lock() and lua_unlock()  TBD

// lua main entry point call when executing 'register(afb-luaglue)'
int luaopen_luaglue(lua_State *luaState)
{

    LuaHandleT *handle = calloc(1, sizeof(LuaHandleT));
    handle->magic = LUA_BINDER_MAGIC;
    handle->luaState = luaState;

    // open default Lua exiting lib
    luaL_openlibs(luaState);

    // add lua glue to interpreter
    luaL_newlibtable(luaState, afbFunction);
    lua_pushlightuserdata(luaState, handle);
    luaL_setfuncs(luaState, afbFunction, 1);

    return 1;
}