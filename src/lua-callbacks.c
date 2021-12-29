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

void LuaTimerClearCb(LuaHandleT *luaTimer) {

    afb_timer_unref (luaTimer->lua.timer.afb);
    json_object_put(luaTimer->lua.timer.configJ);
    luaTimer->lua.timer.usage--;

    // free timer luaState and handle
    if (luaTimer->lua.timer.usage <= 0) {
       lua_settop(luaTimer->luaState,0);
       json_object_put(luaTimer->lua.timer.configJ);
       free(luaTimer);
    }
}

void LuaTimerCb (afb_timer_x4_t timer, void *context, int decount) {
    const char *errorMsg=NULL;
    int status, isnum;
    LuaHandleT *luaTimer= (LuaHandleT*)context;

    int stack = lua_gettop(luaTimer->luaState);
    lua_getglobal(luaTimer->luaState, luaTimer->lua.timer.callback);
    lua_pushlightuserdata(luaTimer->luaState, luaTimer);
    lua_pushlightuserdata(luaTimer->luaState, luaTimer->lua.timer.usrdata);

    status = lua_pcall(luaTimer->luaState, 2, LUA_MULTRET, 0);
    if (status)
    {

        errorMsg=lua_tostring(luaTimer->luaState, -1);
        goto OnErrorExit;
    }

    int count = lua_gettop(luaTimer->luaState) - stack;
    if (!count) status=0;
    else {
        status= (int)lua_tointegerx(luaTimer->luaState, -1, &isnum);
        if (!isnum) {
            errorMsg="should return an integer (OK=0)";
            goto OnErrorExit;
        }
    }

    // check for last timer interation
    if (decount == 1 || status != 0) goto OnUnrefExit;

    return;

OnErrorExit:
    LUA_DBG_ERROR(luaTimer->luaState, luaTimer, errorMsg);
OnUnrefExit:
    LuaTimerClearCb(luaTimer);
}

static void LuaAfbPcallFunc (void *context, int status, unsigned nreplies, afb_data_t const replies[]) {
    LuaAsyncCtxT *cbHandle= (LuaAsyncCtxT*) context;
    const char *errorMsg = NULL;
    int err, count;
    LuaHandleT *luaHandle= cbHandle->handle;

    // subcall was refused
    if (AFB_IS_BINDER_ERRNO(status)) {
        errorMsg= afb_error_text(status);
        goto OnErrorExit;
    }

    // define luafunc callback and add luahandle as 1st argument
    lua_getglobal(luaHandle->luaState, cbHandle->luafunc);
    lua_pushlightuserdata(luaHandle->luaState, luaHandle);
    lua_pushlightuserdata(luaHandle->luaState, cbHandle->context);
    lua_pushinteger (luaHandle->luaState, status);

    // retreive subcall response and build LUA response
    errorMsg= LuaPushAfbReply (luaHandle->luaState, nreplies, replies, &count);
    if (errorMsg) {
        goto OnErrorExit;
    }

    // effectively exec LUA script code
    err = lua_pcall(luaHandle->luaState, count+3, LUA_MULTRET, 0);
    if (err) {
        errorMsg= cbHandle->luafunc;
        goto OnErrorExit;
    }

    // free afb request and luahandle
    free (cbHandle->luafunc);
    free (cbHandle);
    return;

OnErrorExit:
    LUA_DBG_ERROR(luaHandle->luaState, luaHandle, errorMsg);
}

void LuaAfbApiSubcallCb (void *context, int status, unsigned nreplies, afb_data_t const replies[], afb_api_t api) {
    LuaAfbPcallFunc (context, status, nreplies, replies);
}

void LuaAfbRqtSubcallCb (void *context, int status, unsigned nreplies, afb_data_t const replies[], afb_req_t req) {
    LuaAfbPcallFunc (context, status, nreplies, replies);
}

void LuaAfbEvtHandlerCb (void *context, const char *evtName, unsigned nparams, afb_data_x4_t const params[], afb_api_t api) {
    LuaHandleT *luaHandler= (LuaHandleT*) context;
    assert (luaHandler && luaHandler->magic==LUA_HANDLER_MAGIC);
    const char *errorMsg = NULL;
    lua_State *luaState= luaHandler->luaState;
    int err, count;

    // update statistic
    luaHandler->lua.handler.count++;

    // define luafunc callback and add luaHandler as 1st argument
    lua_getglobal(luaState, luaHandler->lua.handler.callback);
    lua_pushlightuserdata(luaState, luaHandler);

    // push event name
    lua_pushstring (luaState, evtName);

    if (luaHandler->lua.handler.usrdata) lua_pushlightuserdata(luaState, luaHandler->lua.handler.usrdata);
    else lua_pushnil(luaState);

    // retreive subcall response and build LUA response
    errorMsg= LuaPushAfbReply (luaState, nparams, params, &count);
    if (errorMsg) {
        goto OnErrorExit;
    }

    // effectively exec LUA script code
    err = lua_pcall(luaState, count+3, 0, 0);
    if (err) {
        errorMsg= luaHandler->lua.handler.callback;
        goto OnErrorExit;
    }
    return;

OnErrorExit:
    LUA_DBG_ERROR(luaState, luaHandler, errorMsg);
}

void AfbschedwaitCb (int signum, void *context, struct afb_sched_lock *afbLock) {
    const char *errorMsg = NULL;
    LuaHandleT *luaLock= (LuaHandleT*)context;

    // define luafunc callback and add luahandle as 1st argument
    int stack = lua_gettop(luaLock->luaState);
    lua_getglobal(luaLock->luaState, luaLock->lua.lock.luafunc);
    lua_State *luaState= luaLock->luaState;
    // create a fake API for waitCB
    LuaHandleT luaApi;
    luaApi.luaState= luaState;
    luaApi.magic= LUA_API_MAGIC;
    luaApi.lua.api.afb=luaLock->lua.lock.apiv4;
    lua_pushlightuserdata(luaLock->luaState, &luaApi);

    // complement luaLock handle with afblock
    luaLock->lua.lock.afb= afbLock;
    lua_pushlightuserdata(luaState, luaLock);

    int err= LuaPushOneArg (luaState, luaLock->lua.lock.dataJ);
    if (err) {
        errorMsg= "fail to push userdata";
        goto OnErrorExit;
    }
    // effectively exec LUA script code
    err = lua_pcall(luaState, 3, LUA_MULTRET, 0);
    if (err) {
        errorMsg= luaLock->lua.lock.luafunc;
        goto OnErrorExit;
    }

    int count = lua_gettop(luaState) - stack;
    if (count) {
        int isNum;
        luaLock->lua.lock.status= (int) lua_tointegerx (luaLock->luaState, -1, &isNum);
        if (!isNum) {
            errorMsg = "waiton callback should return a status";
            goto OnErrorExit;
        }
    }
    return;

OnErrorExit:
    LUA_DBG_ERROR(luaState, luaLock, errorMsg);
    luaLock->lua.lock.status=-1;
    afb_sched_leave(afbLock);
}


static void LuaFreeJsonCtx (json_object *configJ, void *userdata) {
    free (userdata);
}

void LuaAfbVerbCb(afb_req_t afbRqt, unsigned nparams, afb_data_t const params[])
{
    const char *errorMsg = NULL;
    int err, count;
    afb_data_t args[nparams];
    json_object *argsJ[nparams];
    LuaHandleT *luaRqt = LuaRqtNew(afbRqt);
    lua_State *luaState= luaRqt->luaState;

    // on first call we compile configJ to boost following lua api/verb calls
    json_object *configJ = afb_req_get_vcbdata(afbRqt);
    if (!configJ)
    {
        errorMsg = "fail get verb config";
        LUA_AFB_ERROR(luaRqt, errorMsg);
        goto OnErrorExit;
    }

    luaVerbDataT *luaVcData = json_object_get_userdata(configJ);
    if (!luaVcData)
    {
        luaVcData = calloc(1, sizeof(luaVerbDataT));
        json_object_set_userdata(configJ, luaVcData, LuaFreeJsonCtx);
        luaVcData->magic = LUA_VCDATA_MAGIC;

        err = wrap_json_unpack(configJ, "{ss ss s?s}", "verb", &luaVcData->verb, "func", &luaVcData->func, "info", &luaVcData->info);
        if (err)
        {
            errorMsg = "invalid verb json config";
            goto OnErrorExit;
        }
    }
    else
    {
        if (luaVcData->magic != LUA_VCDATA_MAGIC)
        {
            errorMsg = "fail to converting json to LUA table";
            goto OnErrorExit;
        }
    }
    luaRqt->lua.rqt.vcData = luaVcData;

    // retreive input arguments and convert them to json
    for (int idx = 0; idx < nparams; idx++)
    {
        err = afb_data_convert(params[idx], &afb_type_predefined_json_c, &args[idx]);
        if (err)
        {
            errorMsg = "fail converting input params to json";
            goto OnErrorExit;
        }
        argsJ[idx] = afb_data_ro_pointer(args[idx]);
    }

    // define lua api/verb function
    int stack = lua_gettop(luaState);
    lua_getglobal(luaState, luaVcData->func);
    lua_pushlightuserdata(luaState, luaRqt);

    // push query list argument to lua func
    for (count = 0; count < nparams; count++)
    {
        err = LuaPushOneArg(luaState, argsJ[count]);
        if (err)
        {
            errorMsg = "Fail to converting json to LUA table";
            goto OnErrorExit;
        }
    }

    // effectively exec LUA script code
    err = lua_pcall(luaState, count + 1, LUA_MULTRET, 0);
    if (err)
    {
        LUA_DBG_ERROR(luaState, luaRqt, "LuaAfbVerbCb");
        goto OnErrorExit;
    }
    else
    {
        // if requested process implicit response
        int index = 0, status;
        count = lua_gettop(luaState) - stack;
        if (count)
        {
            int isnum;
            afb_data_t reply[count];
            status= (int)lua_tointegerx(luaState,  count * -1, &isnum);
            if (!isnum) {
                errorMsg="should return an integer (OK=0)";
                goto OnErrorExit;
            }

            for (int idx = count - 1; idx > 0; idx--)
            {
                json_object *responseJ;
                responseJ = LuaPopOneArg(luaState, -1 * idx);
                if (!responseJ)
                {
                    errorMsg = "(hoops) invalid Lua internal response";
                    goto OnErrorExit;
                }
                afb_create_data_raw(&reply[index++], AFB_PREDEFINED_TYPE_JSON_C, responseJ, 0, (void *)json_object_put, responseJ);
            }
            // afb response should be provided by lua api/verb function
            LuaAfbReply(luaRqt, status, index, reply);
        }
    }

    return;

OnErrorExit:
    {
        afb_data_t reply;
        json_object *errorJ = LuaJsonDbg(luaState, errorMsg);
        LUA_AFB_WARNING(luaRqt, "LuaAfbVerbCb: api=[%s] verb=[%s] error=%s", afb_api_name(afb_req_get_api(afbRqt)), afb_req_get_called_verb(afbRqt), errorMsg);
        afb_create_data_raw(&reply, AFB_PREDEFINED_TYPE_JSON_C, errorJ, 0, (void *)json_object_put, errorJ);
        LuaAfbReply(luaRqt, -1, 1, &reply);
    }
}

// automatic generation of api/info introspection verb
void LuaAfbInfoCb(afb_req_t afbRqt, unsigned nparams, afb_data_t const params[])
{
    afb_api_t apiv4 = afb_req_get_api(afbRqt);
    afb_data_t reply;

    // retreive interpreteur from API
    LuaHandleT *luaApi = afb_api_get_userdata(apiv4);
    assert(luaApi->magic == LUA_API_MAGIC);

    // extract uid + info from API config
    json_object *uidJ, *infoJ=NULL, *metaJ;
    wrap_json_unpack (luaApi->lua.api.configJ, "{so s?o}"
        ,"uid", &uidJ
        ,"info", &infoJ
    );
    wrap_json_pack (&metaJ, "{sO sO*}"
        ,"uid", uidJ
        ,"info", infoJ
    );

    // extract info from each verb
    json_object *verbsJ = json_object_new_array();
    for (int idx = 0; idx < afb_api_v4_verb_count(apiv4); idx++)
    {
        const afb_verb_t *afbVerb = afb_api_v4_verb_at(apiv4, idx);
        if (!afbVerb) break;
        if (afbVerb->vcbdata != luaApi) {
            json_object_array_add(verbsJ, (json_object *)afbVerb->vcbdata);
            json_object_get(verbsJ);
        }
    }
    // info devtool require a group array
    json_object *groupsJ;
    wrap_json_pack(&groupsJ, "[{so}]", "verbs", verbsJ);

    wrap_json_pack(&infoJ, "{so so}", "metadata", metaJ, "groups", groupsJ);
    afb_create_data_raw(&reply, AFB_PREDEFINED_TYPE_JSON_C, infoJ, 0, (void *)json_object_put, infoJ);
    afb_req_reply(afbRqt, 0, 1, &reply);
    return;
}

// this routine execute within mainloop context when binder is ready to go
int LuaAfbStartupCb(json_object *configJ, void *context)
{
    const char *startup;
    LuaHandleT *binder = (LuaHandleT *)context;
    int err, count;

    assert(binder && binder->magic == LUA_BINDER_MAGIC);

    startup = json_object_get_string(configJ);
    if (startup)
    {
        int stack = lua_gettop(binder->luaState);

        // call lua startup
        lua_getglobal(binder->luaState, startup);

        // push lua request handle
        lua_pushlightuserdata(binder->luaState, binder);

        // effectively exec LUA script code
        err = lua_pcall(binder->luaState, 1, LUA_MULTRET, 0);
        if (err)
            goto OnErrorExit;

        // check number of returned arguments
        count = lua_gettop(binder->luaState) - stack;
        if (count)
            err = (int)lua_tointeger(binder->luaState, -1);
    }
    return err;

OnErrorExit:
    LUA_DBG_ERROR(binder->luaState, binder, "binder startup fail");
    return -1;
}

int LuaApiCtrlCb(afb_api_t apiv4, afb_ctlid_t ctlid, afb_ctlarg_t ctlarg, void *context) {
    LuaHandleT *luaApi= (LuaHandleT*) context;
    static int orphan=0;
    const char *state;
    int err=0;

    // assert context validity
    assert (luaApi && luaApi->magic == LUA_API_MAGIC);


    switch (ctlid) {
    case afb_ctlid_Root_Entry:
        state="root";
        break;

    case afb_ctlid_Pre_Init:
        state="config";
        luaApi->lua.api.afb= apiv4;

        // extract lua api control function callback
        luaApi->lua.api.ctrlCb= json_object_get_string(json_object_object_get(luaApi->lua.api.configJ, "control"));
        break;

    case afb_ctlid_Init:
        state="ready";
        break;

    case afb_ctlid_Class_Ready:
        state="class";
        break;

    case afb_ctlid_Orphan_Event:
        LUA_AFB_WARNING (luaApi, "Orphan event=%s count=%d", ctlarg->orphan_event.name, orphan++);
        state="orphan";
        break;

    case afb_ctlid_Exiting:
        state="exit";
        break;

    default:
        break;
    }

    if (!luaApi->lua.api.ctrlCb) {
        LUA_AFB_WARNING(luaApi,"LuaApiCtrlCb: No init callback state=[%s]", state);

    } else {
        // define lua api/verb function and argument
        int stack = lua_gettop(luaApi->luaState);
        lua_getglobal(luaApi->luaState, luaApi->lua.api.ctrlCb);
        lua_pushlightuserdata (luaApi->luaState, luaApi);
        lua_pushstring(luaApi->luaState, state);

        // effectively exec LUA script code
        LUA_AFB_NOTICE(luaApi,"LuaApiCtrlCb: func=[%s] state=[%s]", luaApi->lua.api.ctrlCb, state);
        err = lua_pcall(luaApi->luaState, 2, LUA_MULTRET, 0);
        if (err) goto OnErrorExit;

        // check number of returned arguments
        int argc= lua_gettop(luaApi->luaState) - stack;
        if (argc) err= (int)lua_tointeger(luaApi->luaState, -1);
    }
    return err;

OnErrorExit:
    LUA_DBG_ERROR(luaApi->luaState, luaApi, luaApi->lua.api.ctrlCb);
    return -1;
}
