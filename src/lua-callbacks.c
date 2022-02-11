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

#include <lua.h>
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

void GlueTimerClear(GlueHandleT *glue) {

    afb_timer_unref (glue->timer.afb);
    json_object_put(glue->timer.configJ);
    glue->usage--;

    // free timer luaState and handle
    if (glue->usage <= 0) {
       lua_settop(glue->luaState,0);
       json_object_put(glue->timer.configJ);
       free(glue);
    }
}


static void GluePcallFunc (GlueHandleT *glue, GlueAsyncCtxT *async, const char *label, int status, unsigned nreplies, afb_data_t const replies[]) {
//static void GluePcallFunc (void *userdata, int status, unsigned nreplies, afb_data_t const replies[]) {
    const char *errorMsg = "internal-error";
    int err, count;

    // subcall was refused
    if (AFB_IS_BINDER_ERRNO(status)) {
        errorMsg= afb_error_text(status);
        goto OnErrorExit;
    }

    // define luafunc callback and add glue as 1st argument
    lua_getglobal(glue->luaState, async->callback);
    lua_pushlightuserdata(glue->luaState, glue);

    // depending on case 2nd argument is an integer or string
    if (label) lua_pushstring(glue->luaState, label);
    else lua_pushinteger (glue->luaState, status);

    if (async->userdata) lua_pushlightuserdata(glue->luaState, async->userdata);
    else lua_pushnil(glue->luaState);

    // retreive subcall response and build LUA response
    errorMsg= LuaPushAfbReply (glue->luaState, nreplies, replies, &count);
    if (errorMsg) goto OnErrorExit;

    // effectively exec LUA script code
    err = lua_pcall(glue->luaState, count+3, LUA_MULTRET, 0);
    if (err) {
        errorMsg= async->callback;
        goto OnErrorExit;
    }

    return;

OnErrorExit:
    LUA_DBG_ERROR(glue->luaState, glue, errorMsg);
}


void GlueJobStartCb (int signum, void *userdata, struct afb_sched_lock *afbLock) {

    GlueHandleT *glue= (GlueHandleT*)userdata;
    assert (glue->magic == GLUE_JOB_MAGIC);

    glue->job.afb= afbLock;
    GluePcallFunc (glue, &glue->job.async, NULL, signum, 0, NULL);
}

// used when declaring event with the api
void GlueApiEventCb (void *userdata, const char *label, unsigned nparams, afb_data_x4_t const params[], afb_api_t api) {
    const char *errorMsg;
    GlueHandleT *glue= (GlueHandleT*) afb_api_get_userdata(api);

    // on first call we compile configJ to boost following py api/verb calls
    AfbVcbDataT *vcbData= userdata;
    assert (vcbData->magic == (void*)AfbAddVerbs);

    // retreive lua interpretor from handler handle
    if (!vcbData->state) vcbData->state= glue->luaState;
    lua_State *luaState= (lua_State*)vcbData->state;

   // on first call we need to retreive original callback object from configJ
    if (!vcbData->callback)
    {
        json_object *callbackJ=json_object_object_get(vcbData->configJ, "callback");
        if (!callbackJ) {
            errorMsg = "(hoops) not callback defined";
            goto OnErrorExit;
        }

        // create an async structure to use gluePcallFunc and extract callbackR from json userdata
        GlueAsyncCtxT *async= calloc (1, sizeof(GlueAsyncCtxT));
        async->callback=  (char*)json_object_get_string (callbackJ);
        vcbData->callback = (void*)async;
    }

    //GluePcallFunc (glue, (GlueAsyncCtxT*)vcbData->callback, label, 0, nparams, params);
    GluePcallFunc (glue, (GlueAsyncCtxT*)vcbData->callback, label, 0, 0, NULL);
    return;

OnErrorExit:
    LUA_DBG_ERROR(luaState, glue, errorMsg);
}

// user when declaring event with libafb.evthandler
void GlueEventCb (void *userdata, const char *label, unsigned nparams, afb_data_x4_t const params[], afb_api_t api) {
    GlueHandleT *glue= (GlueHandleT*) userdata;
    assert (glue->magic == GLUE_EVT_MAGIC);
    GluePcallFunc (glue, &glue->event.async, label, 0, nparams, params);
}

void GlueTimerCb (afb_timer_x4_t timer, void *userdata, int decount) {
   GlueHandleT *glue= (GlueHandleT*) userdata;
   assert (glue->magic == GLUE_TIMER_MAGIC);
   GluePcallFunc (glue, &glue->timer.async, NULL, decount, 0, NULL);
}

void GlueJobPostCb (int signum, void *userdata) {
    GlueCallHandleT *handle= (GlueCallHandleT*) userdata;
    assert (handle->magic == GLUE_POST_MAGIC);
    if (!signum) GluePcallFunc (handle->glue, &handle->async, NULL, signum, 0, NULL);
    free (handle->async.uid);
    free (handle);
}

void GlueApiSubcallCb (void *userdata, int status, unsigned nreplies, afb_data_t const replies[], afb_api_t api) {
    GlueCallHandleT *handle= (GlueCallHandleT*) userdata;
    assert (handle->magic == GLUE_CALL_MAGIC);
    GluePcallFunc (handle->glue, &handle->async, NULL, status, nreplies, replies);
    free (handle->async.uid);
    free (handle->async.callback);
    free (handle);
}

void GlueRqtSubcallCb (void *userdata, int status, unsigned nreplies, afb_data_t const replies[], afb_req_t req) {
    GlueCallHandleT *handle= (GlueCallHandleT*) userdata;
    assert (handle->magic == GLUE_CALL_MAGIC);
    GluePcallFunc (handle->glue, &handle->async, NULL, status, nreplies, replies);
    free (handle->async.uid);
    free (handle->async.callback);
    free (handle);
}

void GlueApiVerbCb(afb_req_t afbRqt, unsigned nparams, afb_data_t const params[])
{
    const char *errorMsg = NULL;
    int err, count;
    GlueHandleT *glue = GlueRqtNew(afbRqt);
    lua_State *luaState= glue->luaState;
    json_object *argsJ[nparams];

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

    // retreive input arguments and convert them to json
    for (int idx = 0; idx < nparams; idx++)
    {
        afb_data_t arg;
        err = afb_data_convert(params[idx], &afb_type_predefined_json_c, &arg);
        if (err)
        {
            errorMsg = "fail converting input params to json";
            goto OnErrorExit;
        }
        argsJ[idx] = afb_data_ro_pointer(arg);
        afb_data_unref(arg);
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
        LUA_DBG_ERROR(luaState, glue, "GlueApiVerbCb");
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
    GlueHandleT *glue = afb_api_get_userdata(apiv4);
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

// this routine execute within mainloop userdata when binder is ready to go
int GlueStartupCb(void* callback, void *userdata)
{
    GlueAsyncCtxT *async  = (GlueAsyncCtxT*)callback;
    GlueHandleT   *handle = (GlueHandleT  *)userdata;
    int err, count;

    if (callback)
    {
        int stack = lua_gettop(handle->luaState);

        // call lua startup
        lua_getglobal(handle->luaState, async->callback);

        // push lua request handle
        lua_pushlightuserdata(handle->luaState, handle);

        if (async->userdata)  lua_pushlightuserdata(handle->luaState, async->userdata);
        else lua_pushnil(handle->luaState);

        // effectively exec LUA script code
        err = lua_pcall(handle->luaState, 2, LUA_MULTRET, 0);
        if (err)
            goto OnErrorExit;

        // check number of returned arguments
        count = lua_gettop(handle->luaState) - stack;
        if (count)
            err = (int)lua_tointeger(handle->luaState, -1);
    }
    return err;

OnErrorExit:
    LUA_DBG_ERROR(handle->luaState, handle, "handle startup fail");
    return -1;
}

int GlueCtrlCb(afb_api_t apiv4, afb_ctlid_t ctlid, afb_ctlarg_t ctlarg, void *userdata) {
    GlueHandleT *glue= (GlueHandleT*) userdata;
    static int orphan=0;
    const char *state;
    int err=0;

    // assert userdata validity
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
