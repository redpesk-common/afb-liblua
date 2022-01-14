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

void GlueTimerClear(AfbHandleT *glue) {

    afb_timer_unref (glue->timer.afb);
    json_object_put(glue->timer.configJ);
    glue->timer.usage--;

    // free timer luaState and handle
    if (glue->timer.usage <= 0) {
       lua_settop(glue->luaState,0);
       json_object_put(glue->timer.configJ);
       free(glue);
    }
}

void GlueTimerCb (afb_timer_x4_t timer, void *context, int decount) {
    const char *errorMsg=NULL;
    int status, isnum;
    AfbHandleT *glue= (AfbHandleT*)context;

    int stack = lua_gettop(glue->luaState);
    lua_getglobal(glue->luaState, glue->timer.callback);
    lua_pushlightuserdata(glue->luaState, glue);
    lua_pushlightuserdata(glue->luaState, glue->timer.userdata);

    status = lua_pcall(glue->luaState, 2, LUA_MULTRET, 0);
    if (status)
    {

        errorMsg=lua_tostring(glue->luaState, -1);
        goto OnErrorExit;
    }

    int count = lua_gettop(glue->luaState) - stack;
    if (!count) status=0;
    else {
        status= (int)lua_tointegerx(glue->luaState, -1, &isnum);
        if (!isnum) {
            errorMsg="should return an integer (OK=0)";
            goto OnErrorExit;
        }
    }

    // check for last timer interation
    if (decount == 1 || status != 0) goto OnUnrefExit;

    return;

OnErrorExit:
    LUA_DBG_ERROR(glue->luaState, glue, errorMsg);
OnUnrefExit:
    GlueTimerClear(glue);
}

static void GluePcallFunc (void *context, int status, unsigned nreplies, afb_data_t const replies[]) {
    LuaAsyncCtxT *cbHandle= (LuaAsyncCtxT*) context;
    const char *errorMsg = NULL;
    int err, count;
    AfbHandleT *glue= cbHandle->handle;

    // subcall was refused
    if (AFB_IS_BINDER_ERRNO(status)) {
        errorMsg= afb_error_text(status);
        goto OnErrorExit;
    }

    // define luafunc callback and add glue as 1st argument
    lua_getglobal(glue->luaState, cbHandle->luafunc);
    lua_pushlightuserdata(glue->luaState, glue);
    lua_pushinteger (glue->luaState, status);
    lua_pushlightuserdata(glue->luaState, cbHandle->context);

    // retreive subcall response and build LUA response
    errorMsg= LuaPushAfbReply (glue->luaState, nreplies, replies, &count);
    if (errorMsg) goto OnErrorExit;

    // effectively exec LUA script code
    err = lua_pcall(glue->luaState, count+3, LUA_MULTRET, 0);
    if (err) {
        errorMsg= cbHandle->luafunc;
        goto OnErrorExit;
    }

    // free afb request and glue
    free (cbHandle->luafunc);
    free (cbHandle);
    return;

OnErrorExit:
    LUA_DBG_ERROR(glue->luaState, glue, errorMsg);
}

void GlueApiSubcallCb (void *context, int status, unsigned nreplies, afb_data_t const replies[], afb_api_t api) {
    GluePcallFunc (context, status, nreplies, replies);
}

void GlueRqtSubcallCb (void *context, int status, unsigned nreplies, afb_data_t const replies[], afb_req_t req) {
    GluePcallFunc (context, status, nreplies, replies);
}

void GlueEvtHandlerCb (void *userdata, const char *evtName, unsigned nparams, afb_data_x4_t const params[], afb_api_t api) {
    const char *errorMsg = NULL;
    int err, count;

    AfbHandleT *glue= (AfbHandleT*) afb_api_get_userdata(api);
    assert (glue);

    // on first call we compile configJ to boost following py api/verb calls
    AfbVcbDataT *vcbData= userdata;
    if (vcbData->magic != (void*)AfbAddVerbs) {
        errorMsg = "(hoops) event invalid vcbData handle";
        goto OnErrorExit;
    }

    // retreive lua interpretor from handler handle
    if (!vcbData->state) vcbData->state= glue->luaState;
    lua_State *luaState= (lua_State*)vcbData->state;

    // on first call we need to retreive original callback object from configJ
    if (!vcbData->callback)
    {
        json_object *funcJ=json_object_object_get(vcbData->configJ, "callback");
        if (!funcJ) {
            errorMsg = "(hoops) not callback defined";
            goto OnErrorExit;
        }
        vcbData->callback= (void*)json_object_get_string(funcJ);
    }

    // define luafunc callback and add luaHandler as 1st argument
    lua_getglobal(luaState, (char*)vcbData->callback);
    lua_pushlightuserdata(luaState, glue);

    // push event name
    lua_pushstring (luaState, evtName);

    if (vcbData->userdata) lua_pushlightuserdata(luaState, vcbData->userdata);
    else lua_pushnil(luaState);

    // retreive subcall response and build LUA response
    errorMsg= LuaPushAfbReply (luaState, nparams, params, &count);
    if (errorMsg) {
        goto OnErrorExit;
    }

    // effectively exec LUA script code
    err = lua_pcall(luaState, count+3, 0, 0);
    if (err) {
        errorMsg= (char*)vcbData->callback;
        goto OnErrorExit;
    }
    return;

OnErrorExit:
    LUA_DBG_ERROR(luaState, glue, errorMsg);
}

void GlueSchedWaitCb (int signum, void *context, struct afb_sched_lock *afbLock) {
    const char *errorMsg = NULL;
    AfbHandleT *luaLock= (AfbHandleT*)context;
    assert (luaLock && luaLock->magic == GLUE_LOCK_MAGIC);

    // define luafunc callback and add glue as 1st argument
    int stack = lua_gettop(luaLock->luaState);
    lua_getglobal(luaLock->luaState, luaLock->lock.luafunc);
    lua_State *luaState= luaLock->luaState;
    // create a fake API for waitCB
    AfbHandleT glue;
    glue.luaState= luaState;
    glue.magic= GLUE_API_MAGIC;
    glue.api.afb=luaLock->lock.apiv4;
    lua_pushlightuserdata(luaLock->luaState, &glue);

    // complement luaLock handle with afblock
    luaLock->lock.afb= afbLock;
    lua_pushlightuserdata(luaState, luaLock);

    int err= LuaPushOneArg (luaState, luaLock->lock.dataJ);
    if (err) {
        errorMsg= "fail to push userdata";
        goto OnErrorExit;
    }
    // effectively exec LUA script code
    err = lua_pcall(luaState, 3, LUA_MULTRET, 0);
    if (err) {
        errorMsg= luaLock->lock.luafunc;
        goto OnErrorExit;
    }

    int count = lua_gettop(luaState) - stack;
    if (count) {
        int isNum;
        luaLock->lock.status= (int) lua_tointegerx (luaLock->luaState, -1, &isNum);
        if (!isNum) {
            errorMsg = "waiton callback should return a status";
            goto OnErrorExit;
        }
    }
    return;

OnErrorExit:
    LUA_DBG_ERROR(luaState, luaLock, errorMsg);
    luaLock->lock.status=-1;
    afb_sched_leave(afbLock);
}

void GlueSchedTimeoutCb (int signum, void *context) {
    const char *errorMsg = NULL;
    GlueHandleCbT *handle= (GlueHandleCbT*)context;
    assert (handle && handle->magic == GLUE_SCHED_MAGIC);
    lua_State *luaState= handle->glue->luaState;

    // timer not cancel
    if (signum != SIGABRT) {
        // define luafunc callback and add glue as 1st argument
        lua_getglobal(luaState, handle->callback);
        lua_pushlightuserdata(luaState, handle->glue);
        lua_pushlightuserdata(luaState, handle->userdata);

        // effectively exec LUA script code
        int err = lua_pcall(luaState, 2, LUA_MULTRET, 0);
        if (err) {
            errorMsg= handle->callback;
            goto OnErrorExit;
        }

    }
    free (handle->callback);
    free (handle);
    return;

OnErrorExit:
    LUA_DBG_ERROR(handle->glue->luaState, handle->glue, errorMsg);
    free (handle->callback);
    free (handle);
}

void GlueVerbCb(afb_req_t afbRqt, unsigned nparams, afb_data_t const params[])
{
    const char *errorMsg = NULL;
    int err, count;
    afb_data_t args[nparams];
    json_object *argsJ[nparams];
    AfbHandleT *glue = GlueRqtNew(afbRqt);
    lua_State *luaState= glue->luaState;

    // on first call we compile configJ to boost following py api/verb calls
    AfbVcbDataT *vcbData= afb_req_get_vcbdata(afbRqt);
    if (vcbData->magic != (void*)AfbAddVerbs) {
        errorMsg = "(hoops) invalid vcbData handle";
        goto OnErrorExit;
    }

    // on first call we need to retreive original callback object from configJ
    if (!vcbData->callback)
    {
        json_object *funcJ=json_object_object_get(vcbData->configJ, "callback");
        if (!funcJ) {
            errorMsg = "(hoops) not callback defined";
            goto OnErrorExit;
        }
        vcbData->callback= (void*)json_object_get_string(funcJ);
    }
    glue->rqt.vcbData = vcbData;

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
    lua_getglobal(luaState, (char*)vcbData->callback);
    lua_pushlightuserdata(luaState, glue);

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
        LUA_DBG_ERROR(luaState, glue, "GlueVerbCb");
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
            GlueReply(glue, status, index, reply);
        }
    }

    return;

OnErrorExit:
    {
        afb_data_t reply;
        json_object *errorJ = LuaJsonDbg(luaState, errorMsg);
        GLUE_AFB_WARNING(glue, "verb=[%s] lua=%s", afb_req_get_called_verb(afbRqt), json_object_get_string(errorJ));
        afb_create_data_raw(&reply, AFB_PREDEFINED_TYPE_JSON_C, errorJ, 0, (void *)json_object_put, errorJ);
        GlueReply(glue, -1, 1, &reply);
    }
}

// automatic generation of api/info introspection verb
void GlueInfoCb(afb_req_t afbRqt, unsigned nparams, afb_data_t const params[])
{
    afb_api_t apiv4 = afb_req_get_api(afbRqt);
    afb_data_t reply;

    // retreive interpreteur from API
    AfbHandleT *glue = afb_api_get_userdata(apiv4);
    assert(glue->magic == GLUE_API_MAGIC);

    // extract uid + info from API config
    json_object *uidJ, *infoJ=NULL, *metaJ;
    wrap_json_unpack (glue->api.configJ, "{so s?o}"
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
        if (afbVerb->vcbdata != glue) {
            AfbVcbDataT *vcbData= afbVerb->vcbdata;
            if (vcbData->magic != AfbAddVerbs) continue;
            json_object_array_add(verbsJ, vcbData->configJ);
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
int GlueStartupCb(void* callback, void *context)
{
    const char *funcname= (char*) callback;
    AfbHandleT *binder = (AfbHandleT *)context;
    int err, count;

    assert(binder && binder->magic == GLUE_BINDER_MAGIC);

    if (callback)
    {
        int stack = lua_gettop(binder->luaState);

        // call lua startup
        lua_getglobal(binder->luaState, funcname);

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

int GlueCtrlCb(afb_api_t apiv4, afb_ctlid_t ctlid, afb_ctlarg_t ctlarg, void *context) {
    AfbHandleT *glue= (AfbHandleT*) context;
    static int orphan=0;
    const char *state;
    int err=0;

    // assert context validity
    assert (glue && glue->magic == GLUE_API_MAGIC);


    switch (ctlid) {
    case afb_ctlid_Root_Entry:
        state="root";
        break;

    case afb_ctlid_Pre_Init:
        state="config";
        glue->api.afb= apiv4;

        // extract lua api control function callback
        glue->api.ctrlCb= json_object_get_string(json_object_object_get(glue->api.configJ, "control"));
        break;

    case afb_ctlid_Init:
        state="ready";
        break;

    case afb_ctlid_Class_Ready:
        state="class";
        break;

    case afb_ctlid_Orphan_Event:
        GLUE_AFB_WARNING (glue, "Orphan event=%s count=%d", ctlarg->orphan_event.name, orphan++);
        state="orphan";
        break;

    case afb_ctlid_Exiting:
        state="exit";
        break;

    default:
        break;
    }

    if (!glue->api.ctrlCb) {
        GLUE_AFB_WARNING(glue,"GlueCtrlCb: No init callback state=[%s]", state);

    } else {
        // define lua api/verb function and argument
        int stack = lua_gettop(glue->luaState);
        lua_getglobal(glue->luaState, glue->api.ctrlCb);
        lua_pushlightuserdata (glue->luaState, glue);
        lua_pushstring(glue->luaState, state);

        // effectively exec LUA script code
        GLUE_AFB_NOTICE(glue,"GlueCtrlCb: func=[%s] state=[%s]", glue->api.ctrlCb, state);
        err = lua_pcall(glue->luaState, 2, LUA_MULTRET, 0);
        if (err) goto OnErrorExit;

        // check number of returned arguments
        int argc= lua_gettop(glue->luaState) - stack;
        if (argc) err= (int)lua_tointeger(glue->luaState, -1);
    }
    return err;

OnErrorExit:
    LUA_DBG_ERROR(glue->luaState, glue, glue->api.ctrlCb);
    return -1;
}
