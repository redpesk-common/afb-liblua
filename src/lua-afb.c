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
#include <lua.h>
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

static int GluePrintInfo(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_INFO);
    return err;
}

static int GluePrintError(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_ERROR);
    return err; // no value return
}

static int GluePrintWarning(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_WARNING);
    return err;
}

static int GluePrintNotice(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_NOTICE);
    return err;
}

static int GluePrintDebug(lua_State *luaState)
{
    int err = LuaPrintMsg(luaState, AFB_SYSLOG_LEVEL_DEBUG);
    return err;
}

static int GlueTimerAddref(lua_State* luaState) {
    const char *errorMsg="syntax: timeraddref(handle)";

    GlueHandleT *glue= LuaTimerPop(luaState, 1);
    if (!glue) goto OnErrorExit;

    afb_timer_addref (glue->timer.afb);
    json_object_get(glue->timer.configJ);
    glue->usage++;

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueTimerUnref(lua_State* luaState) {
    const char *errorMsg="syntax: timerunref(handle)";

    GlueHandleT *glue= LuaTimerPop(luaState, 1);
    if (!glue) goto OnErrorExit;

    GlueTimerClear(glue);
    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueTimerNew(lua_State *luaState)
{
    const char *errorMsg = "syntax: timernew(api, {'uid':'xxx','callback':yyy,'period':ms,'count':nn}, userdata)";

    GlueHandleT *glue= (GlueHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    GlueHandleT *handle= (GlueHandleT *)calloc(1, sizeof(GlueHandleT));
    handle->magic = GLUE_TIMER_MAGIC;
    handle->luaState = lua_newthread(luaState); // private interpretor
    lua_pushnil(handle->luaState); // keep thread state until timer die

    handle->timer.configJ = LuaPopOneArg(luaState, LUA_FIRST_ARG+1);
    json_object_get(handle->timer.configJ);
    if (!handle->timer.configJ)  goto OnErrorExit;

    // retreive API from py handle
    handle->timer.apiv4= GlueGetApi(glue);
    if (!handle->timer.apiv4) goto OnErrorExit;

    switch (lua_type(luaState, LUA_FIRST_ARG + 2)) {
        case LUA_TLIGHTUSERDATA:
            handle->timer.async.userdata= lua_touserdata(luaState, LUA_FIRST_ARG+2);
            break;
        case LUA_TNIL:
            handle->timer.async.userdata=NULL;
            break;
        default:
            handle->timer.async.userdata= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 2);
    }

    unsigned period, count=0;
    json_object_get(handle->timer.configJ);
    int err = wrap_json_unpack(handle->timer.configJ, "{ss, ss, si, s?i !}",
        "uid"     , &handle->timer.async.uid,
        "callback", &handle->timer.async.callback,
        "period"  , &period,
        "count"   , &count
    );
    if (err)
    {
        errorMsg = "timerconfig= {uid=xxx', callback=MyCallback, period=timer(ms), count=0-xx}";
        goto OnErrorExit;
    }

    // Fulup TBD check how to implement autounref
    err= afb_timer_create (&handle->timer.afb, 0, 0, 0, count, period, 0, GlueTimerCb, (void*)handle, 0);
    if (err) {
        errorMsg= "(hoops) afb_timer_create fail";
        goto OnErrorExit;
    }

    lua_pushlightuserdata(luaState, handle);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue,errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueRespond(lua_State *luaState)
{
    const char *errorMsg =  "syntax: response(RQT, status, [arg1 ... argn])";
    unsigned argc = lua_gettop(luaState);
    json_object *argsJ[argc];
    afb_data_t reply[argc];

    GlueHandleT *glue = LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    // get restart status 1st
    int isnum;
    int status = (int)lua_tointegerx(luaState, LUA_FIRST_ARG + 1, &isnum);
    if (!isnum) goto OnErrorExit;

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

    GlueReply(glue, status, argc - 2, reply);
    return 0;

OnErrorExit:
{
    afb_data_t reply;
    json_object *errorJ = LuaJsonDbg(luaState, errorMsg);
    afb_create_data_raw(&reply, AFB_PREDEFINED_TYPE_JSON_C, errorJ, 0, (void *)json_object_put, errorJ);
    GlueReply(glue, 0, 1, &reply);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}
}

static int GlueEvtPush(lua_State *luaState)
{
    const char *errorMsg = "syntax: eventpush(evtid, [arg1...argn])";
    unsigned argc = lua_gettop(luaState);
    int index;
    afb_data_t reply[argc];

    // check evt handle
    afb_event_t evtid = (afb_event_t)lua_touserdata(luaState, LUA_FIRST_ARG);;
    if (!evtid || !afb_event_is_valid(evtid)) goto OnErrorExit;

    // get response from LUA and push them as afb-v4 object
    for (index = 0; index < argc - 1; index++)
    {
        json_object *argsJ;
        argsJ = LuaPopOneArg(luaState, LUA_FIRST_ARG + index + 1);
        if (!argsJ) goto OnErrorExit;
        afb_create_data_raw(&reply[index], AFB_PREDEFINED_TYPE_JSON_C, argsJ, 0, (void *)json_object_put, argsJ);
    }

    int status = afb_event_push(evtid, index, reply);
    if (status < 0)
    {
        errorMsg = "afb_event_push fail";
        goto OnErrorExit;
    }
    return 0;

OnErrorExit: {
    GlueHandleT *glue= LuaBinderPop(luaState);
    LUA_DBG_ERROR(luaState, glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
  }
}

static int GlueEvtSubscribe(lua_State *luaState)
{
    const char *errorMsg = "syntax: subscribe(rqt, evtid)";

    // check evt handle
    GlueHandleT *glue= LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    afb_event_t evtid = (afb_event_t)lua_touserdata(luaState, LUA_FIRST_ARG+1);
    if (!evtid || !afb_event_is_valid(evtid)) goto OnErrorExit;

    int err = afb_req_subscribe(glue->rqt.afb, evtid);
    if (err)
    {
        errorMsg = "(hoops) afb_req_subscribe fail";
        goto OnErrorExit;
    }
    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueEvtUnsubscribe(lua_State *luaState)
{
    const char *errorMsg = "syntax: unsubscribe(rqt, evtid)";

    GlueHandleT *glue = LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    afb_event_t evtid = (afb_event_t)lua_touserdata(luaState, LUA_FIRST_ARG+1);
    if (!evtid || !afb_event_is_valid(evtid)) goto OnErrorExit;

    int err = afb_req_unsubscribe(glue->rqt.afb, evtid);
    if (err)
    {
        errorMsg = "(hoops) afb_req_unsubscribe fail";
        goto OnErrorExit;
    }
    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState, glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueEvtNew(lua_State *luaState)
{
    const char *errorMsg= "syntax: evtid= eventnew(api,label)";
    afb_event_t evtid;
    int err;

    GlueHandleT *glue= LuaApiPop(luaState, LUA_FIRST_ARG);
    if (!glue)
    {
        errorMsg = "invalid api handle";
        goto OnErrorExit;
    }

    const char *label= lua_tostring(luaState, LUA_FIRST_ARG+1);
    if (!label) goto OnErrorExit;

    err= afb_api_new_event(GlueGetApi(glue), label, &evtid);
    if (err)
    {
        errorMsg = "(hoops) afb-afb_api_new_event fail";
        goto OnErrorExit;
    }

    // push evtid as a LUA opaque handle
    lua_pushlightuserdata(luaState, evtid);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushnil(luaState);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 2;
}

static int GlueCallAsync(lua_State *luaState)
{
    const char *errorMsg = "syntax: callasync(handle, api, verb, callback, userdata, ...)";
    unsigned argc = lua_gettop(luaState);
    json_object *argsJ[argc];
    afb_data_t params[argc];

    GlueHandleT *glue = (GlueHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!glue || !GlueGetApi(glue)) goto OnErrorExit;

    // get restart status 1st
    void *userdata;
    const char *apiname = lua_tostring(luaState, LUA_FIRST_ARG + 1);
    const char *verbname= lua_tostring(luaState, LUA_FIRST_ARG + 2);
    const char *callback = lua_tostring(luaState, LUA_FIRST_ARG + 3);
    int luaType= lua_type(luaState, LUA_FIRST_ARG + 4);
    switch (luaType) {
        case LUA_TLIGHTUSERDATA:
            userdata= lua_touserdata(luaState, LUA_FIRST_ARG+4);
            break;
        case LUA_TNIL:
            userdata=NULL;
            break;
        default:
            userdata= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 4);
            if (!userdata) verbname=NULL; // force syntax error
    }

    if (!apiname || !verbname || !callback) goto OnErrorExit;

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

    GlueCallHandleT *handle= calloc(1,sizeof(GlueCallHandleT));
    handle->magic= GLUE_CALL_MAGIC;
    handle->glue=glue;
    asprintf (&handle->async.uid, "%s/%s", apiname, verbname);
    handle->async.userdata= userdata;
    handle->async.callback= strdup(callback);

    switch (glue->magic) {
        case GLUE_RQT_MAGIC:
            afb_req_subcall (glue->rqt.afb, apiname, verbname, index, params, afb_req_subcall_catch_events, GlueRqtSubcallCb, (void*)handle);
            break;
        default:
            afb_api_call (GlueGetApi(glue), apiname, verbname, index, params, GlueApiSubcallCb, (void*)handle);
    }

    lua_pushinteger (glue->luaState, 0);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushinteger (glue->luaState, -1);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 2;
}

static int GlueCallSync(lua_State *luaState)
{
    const char *errorMsg = "syntax: callsync(handle, api, verb, ...)";
    unsigned argc = lua_gettop(luaState);
    int err, status, index;
    unsigned nreplies= SUBCALL_MAX_RPLY;
    afb_data_t replies[SUBCALL_MAX_RPLY];

    GlueHandleT *glue = (GlueHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    // get restart status 1st
    const char *apiname = lua_tostring(luaState, LUA_FIRST_ARG + 1);
    const char *verbname= lua_tostring(luaState, LUA_FIRST_ARG + 2);
    if (!apiname || !verbname) goto OnErrorExit;

    //  retreive subcall api argument(s) and call api/verb
    {
        json_object *argsJ;
        afb_data_t params[argc];
        for (index = 0; index < argc-3; index++)
        {
            argsJ = LuaPopOneArg(luaState, LUA_FIRST_ARG + index + 3);
            if (!argsJ)
            {
                errorMsg = "invalid input argument type";
                goto OnErrorExit;
            }
            afb_create_data_raw(&params[index], AFB_PREDEFINED_TYPE_JSON_C, argsJ, 0, (void *)json_object_put, argsJ);
        }

        switch (glue->magic) {
            case GLUE_RQT_MAGIC:
                err= afb_req_subcall_sync (glue->rqt.afb, apiname, verbname, index, params, afb_req_subcall_catch_events, &status, &nreplies, replies);
                break;
            case GLUE_API_MAGIC:
            case GLUE_BINDER_MAGIC:
            case GLUE_JOB_MAGIC:
                err= afb_api_call_sync(GlueGetApi(glue), apiname, verbname, index, params, &status, &nreplies, replies);
                break;

            default:
                errorMsg = "handle should be a req|api";
                goto OnErrorExit;
        }
        if (err) {
            status   = err;
            errorMsg= "(hoops) afb_subcall_sync fail";
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
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueJobStart(lua_State *luaState)
{
    GlueHandleT *handle=NULL;
    const char *errorMsg = "syntax: jobstart(handle, timeout, callback, [userdata])";
    int err;

    GlueHandleT *glue = (GlueHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    int isNum;
    int timeout = (int) lua_tointegerx (luaState, LUA_FIRST_ARG+1, &isNum);
    if (!isNum) goto OnErrorExit;

    const char *funcname= lua_tostring(luaState, LUA_FIRST_ARG+2);
    if (!funcname) goto OnErrorExit;

    handle= calloc (1, sizeof(GlueHandleT));
    handle->magic= GLUE_JOB_MAGIC;
    handle->luaState= luaState;
    handle->job.apiv4= GlueGetApi(glue);
    handle->job.dataJ= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 2);
    handle->job.async.callback= strdup(funcname);

    int luaType= lua_type(luaState, LUA_FIRST_ARG + 3);
    switch (luaType) {
        case LUA_TLIGHTUSERDATA:
            handle->job.async.userdata= lua_touserdata(luaState, LUA_FIRST_ARG+3);
            break;
        case LUA_TNIL:
            handle->job.async.userdata=NULL;
            break;
        default:
            handle->job.async.userdata= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 3);
    }

    err= afb_sched_enter(NULL, timeout, GlueJobStartCb, handle);
    if (err < 0) {
        errorMsg= "afb_sched_enter (timeout?)";
        handle->job.status=-1;
    }

    // return user status
    lua_pushinteger (luaState, handle->job.status);
    return 1;

OnErrorExit:
    if (handle) {
        if (handle->job.async.callback) free (handle->job.async.callback);
        free (handle);
    }
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushinteger (luaState, -1);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 2;
}

static int GlueJobCancel(lua_State *luaState)
{
    const char *errorMsg = "syntax: jobcancel(jobid)";

    GlueHandleT *binder = LuaBinderPop(luaState);
    assert(binder);

    int isnum;
    int jobid= (int)lua_tointegerx(luaState, LUA_FIRST_ARG, &isnum);
    if (!isnum) goto OnErrorExit;

    int err= afb_jobs_abort(jobid);
    if (err) goto OnErrorExit;
    return 0;

OnErrorExit:
    GLUE_AFB_INFO(binder, errorMsg);
    return 0;
}

static int GlueJobPost(lua_State *luaState)
{
    const char *errorMsg = "syntax: jobpost(handle,callback,timeout,[,userdata])";
    GlueCallHandleT *handle=calloc (1, sizeof(GlueCallHandleT));
    handle->magic= GLUE_POST_MAGIC;

    handle->glue = (GlueHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!handle->glue) goto OnErrorExit;

    const char* callback= lua_tostring(luaState, LUA_FIRST_ARG + 1);
    if (!callback) goto OnErrorExit;
    handle->async.callback= strdup(callback);

    int isNum;
    int timeout = (int) lua_tointegerx (luaState, LUA_FIRST_ARG+2, &isNum);
    if (!isNum) goto OnErrorExit;

    int luaType= lua_type(luaState, LUA_FIRST_ARG + 2);
    switch (luaType) {
        case LUA_TLIGHTUSERDATA:
            handle->async.userdata= lua_touserdata(luaState, LUA_FIRST_ARG+3);
            break;
        case LUA_TNIL:
            handle->async.userdata=NULL;
            break;
        default:
            handle->async.userdata= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 3);
    }

    // ms delay for OnTimerCB (timeout is dynamic and depends on CURLOPT_LOW_SPEED_TIME)
    int jobid= afb_sched_post_job (NULL /*group*/, timeout,  0 /*exec-timeout*/,GlueJobPostCb, handle, Afb_Sched_Mode_Start);
	if (jobid <= 0) goto OnErrorExit;

    lua_pushinteger (luaState, jobid);
    return 1;

OnErrorExit:
    if (handle->async.callback) free (handle->async.callback);
    free (handle);
    LUA_DBG_ERROR(luaState,handle->glue, errorMsg);
    lua_pushinteger (luaState, -1);
    return 1;
}

static int GlueJobKill(lua_State *luaState)
{
    const char *errorMsg = NULL;
    int err;

    GlueHandleT *glue = (GlueHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!glue || glue->magic != GLUE_JOB_MAGIC) {
        errorMsg = "invalid lock glue: syntax: jobkill(job, status)";
        goto OnErrorExit;
    }

    int isNum;
    glue->job.status = (int)lua_tointegerx (luaState, LUA_FIRST_ARG +1, &isNum);
    if (!isNum) {
        errorMsg = "status require integer: syntax: jobkill(job, status)";
    }

    err= afb_sched_leave(glue->job.afb);
    if (err) {
        errorMsg= "afb_sched_leave (invalid lock)";
        goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState, glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueEvtHandler(lua_State *luaState)
{
    const char *errorMsg = "syntax: evthandler(handle, {'uid':'xxx','pattern':'yyy','callback':'zzz'}, userdata)";

    GlueHandleT *glue = lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    // retreive API from lua handle
    afb_api_t apiv4= GlueGetApi(glue);
    if (!apiv4) goto OnErrorExit;

    // get restart status 1st
    json_object *configJ = LuaPopOneArg(luaState, LUA_FIRST_ARG + 1);
    if (!configJ) goto OnErrorExit;

    const char *pattern, *uid, *callback;
    int err= wrap_json_unpack (configJ, "{ss ss ss}"
        ,"uid"     , &uid
        ,"callback", &callback
        ,"pattern" , &pattern
    );
    if (err) {
        errorMsg= "config={uid='xxx', pattern='yyy', callback='zzz'}";
        goto OnErrorExit;
    }

    void *userdata;
    switch (lua_type(luaState, LUA_FIRST_ARG + 2)) {
        case LUA_TLIGHTUSERDATA:
            userdata= lua_touserdata(luaState, LUA_FIRST_ARG+2);
            break;
        case LUA_TNIL:
            userdata=NULL;
            break;
        default:
            userdata= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 2);
    }

    GlueHandleT *handle= calloc(1, sizeof(GlueHandleT));
    json_object_get(configJ);
    handle->magic = GLUE_EVT_MAGIC;
    handle->luaState= luaState;
    handle->event.apiv4= apiv4;
    handle->event.async.callback= (char*)callback;
    handle->event.async.userdata= userdata;
    handle->event.async.uid= (char*)uid;
    handle->event.configJ= configJ;

    errorMsg= AfbAddOneEvent (apiv4, uid, pattern, GlueEventCb, handle);
    if (errorMsg) goto OnErrorExit;

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueVerbAdd(lua_State *luaState)
{
    const char *errorMsg = "syntax: addverb(api, config, callback, userdata)";
    GlueHandleT *binder = LuaBinderPop(luaState);

    GlueHandleT *glue = LuaApiPop(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    // get restart status 1st
    json_object *configJ = LuaPopOneArg(luaState, LUA_FIRST_ARG + 1);
    if (!configJ) goto OnErrorExit;

    void *userdata;
    switch (lua_type(luaState, LUA_FIRST_ARG + 2)) {
        case LUA_TLIGHTUSERDATA:
            userdata = lua_touserdata(luaState, LUA_FIRST_ARG+2);
            break;
        case LUA_TNIL:
            userdata=NULL;
            break;
        default:
            userdata= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 2);
    }

    errorMsg= AfbAddOneVerb (binder->binder.afb, glue->api.afb, configJ, GlueApiVerbCb, userdata);
    if (errorMsg) goto OnErrorExit;

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueSetLoa(lua_State *luaState)
{
    const char *errorMsg = "syntax: setloa(rqt, newloa)";
    int isNum, loa, err;

    GlueHandleT *glue = LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    loa = (int) lua_tointegerx (luaState, LUA_FIRST_ARG + 1, &isNum);
    if (!isNum) goto OnErrorExit;

    err= afb_req_session_set_LOA(glue->rqt.afb, loa);
    if (err < 0) {
        errorMsg="Invalid Rqt Session";
        goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueClientInfo(lua_State *luaState)
{
    const char *errorMsg = "syntax: clientinfo(rqt, ['key'])";

    GlueHandleT *glue = LuaRqtPop(luaState, LUA_FIRST_ARG);
    if (!glue) goto OnErrorExit;

    // if optional key is provided return only this value
    const char *key = lua_tostring(luaState, LUA_FIRST_ARG+1);

    json_object *clientJ= afb_req_get_client_info(glue->rqt.afb);
    if (!clientJ) {
        errorMsg= "(hoops) afb_req_get_client_info no session info";
        goto OnErrorExit;
    }

    if (!key) {
        LuaPushOneArg (luaState, clientJ);
    } else {
        json_object *keyJ= json_object_object_get(clientJ, key);
        if (!keyJ) {
            errorMsg= "clientinfo unknown session key";
            goto OnErrorExit;
        }
        LuaPushOneArg (luaState, keyJ);
    }
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueGetConfig(lua_State *luaState)
{
    const char *errorMsg = "syntax: config(handle[,key])";
    json_object *configJ;

    GlueHandleT *binder = LuaBinderPop(luaState);
    assert(binder);

    // check api handle
    GlueHandleT *glue = lua_touserdata(luaState, 1);
    if (!glue) goto OnErrorExit;

    switch (glue->magic)
    {
    case GLUE_API_MAGIC:
        configJ = glue->api.configJ;
        break;
    case GLUE_BINDER_MAGIC:
        configJ = glue->binder.configJ;
        break;
    case GLUE_TIMER_MAGIC:
        configJ = glue->timer.configJ;
        break;
    case GLUE_EVT_MAGIC:
        configJ = glue->event.configJ;
        break;
    default:
        errorMsg = "GlueGetConfig: unsupported lua/afb handle";
        goto OnErrorExit;
    }

    if (!configJ) {
        errorMsg= "glue config missing";
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
        if (!slotJ) lua_pushnil(luaState);
        else LuaPushOneArg(luaState, slotJ);
    }
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,glue, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

static int GlueExtract(lua_State *luaState)
{
    GlueHandleT *binder = LuaBinderPop(luaState);
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
            errorMsg = "GlueGetConfig: unknown config key";
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


static int GlueApiCreate(lua_State *luaState)
{
    const char *errorMsg = "syntax: apiadd (config)";
    json_object *configJ;
    int err;

    GlueHandleT *binder = LuaBinderPop(luaState);
    assert(binder);

    // parse afbApi config
    configJ = LuaPopArgs(luaState, LUA_FIRST_ARG);
    if (!configJ) goto OnErrorExit;

    GlueHandleT *glue = calloc(1, sizeof(GlueHandleT));
    glue->magic = GLUE_API_MAGIC;
    glue->luaState = lua_newthread(luaState);
    lua_pushnil(glue->luaState); // prevent garbage collector from cleaning this thread
    glue->api.configJ = configJ;

    const char *afbApiUri = NULL;
    err = wrap_json_unpack(configJ, "{s?s s?s}", "control", &glue->api.ctrlCb, "uri", &afbApiUri);
    if (err) goto OnErrorExit;

    if (afbApiUri)
    {
        // imported shadow api
        errorMsg = AfbApiImport(binder->binder.afb, configJ);
    }
    else
    {
        // this is a lua api, is control function defined let's add GlueCtrlCb afb main control function
        if (glue->api.ctrlCb)
            errorMsg = AfbApiCreate(binder->binder.afb, configJ, &glue->api.afb, GlueCtrlCb, GlueInfoCb, GlueApiVerbCb, GlueApiEventCb, glue);
        else
            errorMsg = AfbApiCreate(binder->binder.afb, configJ, &glue->api.afb, NULL, GlueInfoCb, GlueApiVerbCb, GlueApiEventCb, glue);
    }
    if (errorMsg)
        goto OnErrorExit;

    lua_pushlightuserdata(luaState, glue);
    return 1;

OnErrorExit:
    LUA_DBG_ERROR(luaState,binder, errorMsg);
    lua_pushliteral(luaState, "GlueApiCreate fail");
    lua_error(luaState);
    return 1;
}

static int GlueBindingLoad(lua_State *luaState)
{
    const char *errorMsg = "syntax: binding(config)";
    json_object *configJ;

    GlueHandleT *binder = LuaBinderPop(luaState);
    if (!binder)
        goto OnErrorExit;

    // parse api config
    configJ = LuaPopArgs(luaState, LUA_FIRST_ARG);
    if (!configJ) goto OnErrorExit;

    errorMsg = AfbBindingLoad(binder->binder.afb, configJ);
    if (errorMsg)
        goto OnErrorExit;

    return 0;

OnErrorExit:
    LUA_DBG_ERROR(luaState,binder, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
}

// this routine execute within loopstart userdata when binder is ready to go
static int GlueLoopStart(lua_State *luaState)
{
    const char *errorMsg="syntax: loopstart(handle,[callback],[userdata])";
    int status = 0;

    // get main binder handle
    GlueHandleT *binder = LuaBinderPop(luaState);

    GlueHandleT *glue= (GlueHandleT *)lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!glue || !GlueGetApi(glue)) goto OnErrorExit;

    GlueAsyncCtxT *async=NULL;
    switch (lua_type(luaState,LUA_FIRST_ARG+1)) {
        case   LUA_TNONE:
        case   LUA_TNIL:
            break;
        case LUA_TSTRING:
            // get callback name
            async= calloc (1, sizeof(GlueAsyncCtxT));
            async->callback= strdup(lua_tostring(luaState, LUA_FIRST_ARG+1));
            break;

        default:
        goto OnErrorExit;
    }


    if (async) switch (lua_type(luaState, LUA_FIRST_ARG + 2)) {
        case LUA_TLIGHTUSERDATA:
            async->userdata= lua_touserdata(luaState, LUA_FIRST_ARG+2);
            break;
        case LUA_TNONE:
        case LUA_TNIL:
            async->userdata=NULL;
            break;

        default:
            async->userdata= (void*) LuaPopOneArg(luaState, LUA_FIRST_ARG + 2);
    }

    // main loop only return when binder startup func return status!=0
    GLUE_AFB_NOTICE(glue, "Entering binder loopstart");
    status = AfbBinderStart(binder->binder.afb, (void*)async, GlueStartupCb, glue);
    lua_pushinteger(luaState, status);
    return 1;

OnErrorExit: {
    LUA_DBG_ERROR(luaState, binder, errorMsg);
    lua_pushstring(luaState, errorMsg);
    lua_error(luaState);
    return 1;
  }
}

// Load Lua glue functions
static int GlueBinderConf(lua_State *luaState)
{
    GlueHandleT *binder = LuaBinderPop(luaState);
    static int luaLoaded = 0;
    const char *errorMsg = "syntax: binder(config)";

    // Lua loads only once
    if (luaLoaded)
    {
        errorMsg = "(hoops) binder(config) already loaded";
        goto OnErrorExit;
    }
    luaLoaded = 1;

    // parse api config
    binder->binder.configJ = LuaPopArgs(luaState, LUA_FIRST_ARG);
    if (!binder->binder.configJ) goto OnErrorExit;

    errorMsg = AfbBinderConfig(binder->binder.configJ, &binder->binder.afb, binder);
    if (errorMsg) goto OnErrorExit;

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

static int GlueExit(lua_State *luaState)
{
    GlueHandleT *binder = LuaBinderPop(luaState);
    GlueHandleT *handle = lua_touserdata(luaState, LUA_FIRST_ARG);

    int isNum;
    long exitCode = lua_tointegerx(luaState, LUA_FIRST_ARG+1, &isNum);
    if (!isNum) goto OnErrorExit;

    // exit binder
    AfbBinderExit(binder->binder.afb, (int)exitCode);

    return 0;
OnErrorExit:
    LUA_DBG_ERROR(luaState, handle, "status should integer");
    lua_error(luaState);
    return 0;
}

static int GluePing(lua_State *luaState)
{
    GlueHandleT *binder = LuaBinderPop(luaState);
    static int count = 0;

    GLUE_AFB_NOTICE(binder, "GluePing count=%d", count);
    lua_pushstring(luaState, "pong");
    lua_pushinteger(luaState, count++);
    return 2;
}

static const luaL_Reg afbFunction[] = {
    {"ping", GluePing},
    {"binder", GlueBinderConf},
    {"apiadd", GlueApiCreate},
    {"verbadd", GlueVerbAdd},
    {"config", GlueGetConfig},
    {"binding", GlueBindingLoad},
    {"loopstart", GlueLoopStart},
    {"notice", GluePrintNotice},
    {"info", GluePrintInfo},
    {"warning", GluePrintWarning},
    {"debug", GluePrintDebug},
    {"error", GluePrintError},
    {"reply", GlueRespond},
    {"exit", GlueExit},
    {"evtsubscribe", GlueEvtSubscribe},
    {"evtunsubscribe", GlueEvtUnsubscribe},
    {"evthandler", GlueEvtHandler},
    {"evtnew", GlueEvtNew},
    {"evtpush", GlueEvtPush},
    {"timerunref", GlueTimerUnref},
    {"timeraddref", GlueTimerAddref},
    {"timernew", GlueTimerNew},
    {"callasync", GlueCallAsync},
    {"callsync", GlueCallSync},
    {"setloa", GlueSetLoa},
    {"clientinfo", GlueClientInfo},
    {"jobpost" , GlueJobPost},
    {"jobcancel" , GlueJobCancel},
    {"jobstart" , GlueJobStart},
    {"jobkill", GlueJobKill},
    {"luastrict", GlueStrict},
    {"extract", GlueExtract},


    {NULL, NULL} /* sentinel */
};

// lua_lock() and lua_unlock()  TBD

// lua main entry point call when executing 'register(afb-luaglue)'
int luaopen_luaglue(lua_State *luaState)
{

    GlueHandleT *handle = calloc(1, sizeof(GlueHandleT));
    handle->magic = GLUE_BINDER_MAGIC;
    handle->luaState = luaState;

    // open default Lua exiting lib
    luaL_openlibs(luaState);

    // add lua glue to interpreter
    luaL_newlibtable(luaState, afbFunction);
    lua_pushlightuserdata(luaState, handle);
    luaL_setfuncs(luaState, afbFunction, 1);

    return 1;
}