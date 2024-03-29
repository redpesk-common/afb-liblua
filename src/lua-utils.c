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
#include <stdarg.h>
#include <assert.h>
#include <wrap-json.h>

#include <libafb/sys/verbose.h>

#include "glue-afb.h"
#include "lua-afb.h"
#include "lua-utils.h"



void GlueVerbose(GlueHandleT *handle, int level, const char *file, int line, const char *func, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    switch (handle->magic)
    {
    case GLUE_API_MAGIC:
    case GLUE_EVT_MAGIC:
    case GLUE_JOB_MAGIC:
        afb_api_vverbose(GlueGetApi(handle), level, file, line, func, fmt, args);
        break;

    case GLUE_RQT_MAGIC:
        afb_req_vverbose(handle->rqt.afb, level, file, line, func, fmt, args);
        break;

    default:
        vverbose(level, file, line, func, fmt, args);
        break;
    }
    return;
}

GlueHandleT *LuaRqtPop(lua_State *luaState, int index)
{
    GlueHandleT *handle = (GlueHandleT *)lua_touserdata(luaState, index);
    if (handle == NULL || handle->magic != GLUE_RQT_MAGIC)
        goto OnErrorExit;
    return handle;

OnErrorExit:
    return NULL;
}

GlueHandleT *LuaApiPop(lua_State *luaState, int index)
{
    GlueHandleT *glue = (GlueHandleT *)lua_touserdata(luaState, index);
    if (!glue || !GlueGetApi(glue))
        goto OnErrorExit;
    return glue;

OnErrorExit:
    return NULL;
}
GlueHandleT *LuaLockPop(lua_State *luaState, int index)
{
    GlueHandleT *luaLock = (GlueHandleT *)lua_touserdata(luaState, index);
    if (!luaLock || luaLock->magic != GLUE_JOB_MAGIC)
        goto OnErrorExit;
    return luaLock;

OnErrorExit:
    return NULL;
}

// retreive API from lua handle
afb_api_t GlueGetApi(GlueHandleT*glue) {
   afb_api_t afbApi;
    switch (glue->magic) {
        case GLUE_API_MAGIC:
            afbApi= glue->api.afb;
            break;
        case GLUE_RQT_MAGIC:
            afbApi= afb_req_get_api(glue->rqt.afb);
            break;
        case GLUE_BINDER_MAGIC:
            afbApi= AfbBinderGetApi(glue->binder.afb);
            break;
        case GLUE_JOB_MAGIC:
            afbApi= glue->job.apiv4;
            break;
        case GLUE_EVT_MAGIC:
            afbApi= glue->event.apiv4;
            break;
        default:
            afbApi=NULL;
    }
    return afbApi;
}

GlueHandleT *LuaTimerPop(lua_State *luaState, int index)
{
    GlueHandleT *glue = (GlueHandleT *)lua_touserdata(luaState, index);
    if (!glue || glue->magic != GLUE_TIMER_MAGIC)
        goto OnErrorExit;
    return glue;

OnErrorExit:
    return NULL;
}


static void GlueRqtFree(void *userdata)
{
    GlueHandleT *glue= (GlueHandleT*)userdata;
    assert (glue && glue->magic == GLUE_RQT_MAGIC);

    // make sure rqt lua stack is empty, then free it
    lua_settop(glue->luaState,0);

    free(glue);
    return;
}

// add a reference on Glue handle
void GlueRqtAddref(GlueHandleT *glue) {
    if (glue->magic == GLUE_RQT_MAGIC) {
        afb_req_unref (glue->rqt.afb);
    }
}

// add a reference on Glue handle
void GlueRqtUnref(GlueHandleT *glue) {
    if (glue->magic == GLUE_RQT_MAGIC) {
        afb_req_unref (glue->rqt.afb);
    }

}

// allocate and push a lua request handle
GlueHandleT *GlueRqtNew(afb_req_t afbRqt)
{
    assert(afbRqt);

    // retreive interpreteur from API
    GlueHandleT *api = afb_api_get_userdata(afb_req_get_api(afbRqt));
    assert(api->magic == GLUE_API_MAGIC);

    GlueHandleT *glue = (GlueHandleT *)calloc(1, sizeof(GlueHandleT));
    glue->magic = GLUE_RQT_MAGIC;
    glue->rqt.afb = afbRqt;
    glue->luaState = lua_newthread(api->luaState);

    // add lua rqt handle to afb request livecycle
    afb_req_v4_set_userdata (afbRqt, (void*)glue, GlueRqtFree);

    return glue;
}

// reply afb request only once and unref lua handle
int GlueReply(GlueHandleT *glue, int status, int nbreply, afb_data_t *reply)
{
    if (glue->rqt.replied) goto OnErrorExit;
    afb_req_reply(glue->rqt.afb, status, nbreply, reply);
    glue->rqt.replied = 1;
    return 0;

OnErrorExit:
    LuaInfoDbg(glue->luaState, glue, 0, "GlueReply", "unique response require");
    return -1;
}

// binder handle is pushed as 1st upvalue during lua/C lib creation
GlueHandleT *LuaBinderPop(lua_State *luaState)
{
    GlueHandleT *handle = lua_touserdata(luaState, lua_upvalueindex(1));
    assert(handle && handle->magic == GLUE_BINDER_MAGIC);
    return handle;
}

// Move a table from Internal Lua representation to Json one
// Numeric table are transformed in json array, string one in object
// Mix numeric/string key are not supported
json_object *LuaTableToJson(lua_State *luaState, int index)
{
#define LUA_KEY_INDEX -2
#define GLUE_VALUE_INDEX -1

    int idx;
    int tableType;
    json_object *tableJ = NULL;

    lua_pushnil(luaState); // 1st key
    if (index < 0)  index--;
    for (idx = 1; lua_next(luaState, index) != 0; idx++)
    {

        // uses 'key' (at index -2) and 'value' (at index -1)
        if (lua_type(luaState, LUA_KEY_INDEX) == LUA_TSTRING)
        {

            if (!tableJ)
            {
                tableJ = json_object_new_object();
                tableType = LUA_TSTRING;
            }
            else if (tableType != LUA_TSTRING)
            {
                ERROR("MIX Lua Table with key string/numeric not supported");
                goto OnErrorExit;
            }

            const char *key = lua_tostring(luaState, LUA_KEY_INDEX);
            json_object *argJ = LuaPopOneArg(luaState, GLUE_VALUE_INDEX);
            if (argJ)
                json_object_object_add(tableJ, key, argJ);
            else
            {
                ERROR("LuaTableToJson: key=[%s] invalid data type value", key);
                goto OnErrorExit;
            }
        }
        else
        {
            if (!tableJ)
            {
                tableJ = json_object_new_array();
                tableType = LUA_TNUMBER;
            }
            else if (tableType != LUA_TNUMBER)
            {
                ERROR("MIX Lua Table with key numeric/string not supported");
                goto OnErrorExit;
            }

            json_object *argJ = LuaPopOneArg(luaState, GLUE_VALUE_INDEX);
            json_object_array_add(tableJ, argJ);
        }
        lua_pop(luaState, 1); // removes 'value'; keeps 'key' for next iteration
    }

    // Query is empty free empty json object
    if (idx == 1)
    {
        json_object_put(tableJ);
        goto OnErrorExit;
    }
    return tableJ;

OnErrorExit:
    return NULL;
}

json_object *LuaPopOneArg(lua_State *luaState, int idx)
{
    json_object *valueJ = NULL;

    int luaType = lua_type(luaState, idx);
    switch (luaType)
    {
    case LUA_TNUMBER:
    {
        lua_Number number = lua_tonumber(luaState, idx);
        int nombre = (int)number; // evil trick to determine wether n fits in an integer. (stolen from ltcl.c)
        if (number == nombre) valueJ = json_object_new_int(nombre);
        else valueJ = json_object_new_double(number);
        break;
    }
    case LUA_TBOOLEAN:
        valueJ = json_object_new_boolean(lua_toboolean(luaState, idx));
        break;
    case LUA_TSTRING:
        valueJ = json_object_new_string(lua_tostring(luaState, idx));
        break;
    case LUA_TTABLE:
        valueJ = LuaTableToJson(luaState, idx);
        break;
    case LUA_TNIL:
        valueJ = json_object_new_string("nil");
        break;
    case LUA_TUSERDATA: { // attach lighdata as userdata to a json empty string
        void *pointer= lua_touserdata(luaState, idx);
        valueJ = json_object_new_string("");
        json_object_set_userdata(valueJ, pointer, NULL);
        break;
    }
    default:
        ERROR("LuaPopOneArg: Unsupported type:%d/%s idx=%d", luaType, lua_typename(luaState, luaType), idx);
        valueJ = NULL;
    }

    return valueJ;
}

json_object *LuaPopArgs(lua_State *luaState, int start)
{
    json_object *responseJ;

    int stop = lua_gettop(luaState);
    if (stop - start < 0)
        return NULL;

    // start at 2 because we are using a function array lib
    if (start == stop)
    {
        responseJ = LuaPopOneArg(luaState, start);
    }
    else
    {
        // loop on remaining return arguments
        responseJ = json_object_new_array();
        for (int idx = start; idx <= stop; idx++)
        {
            json_object *argJ = LuaPopOneArg(luaState, idx);
            if (!argJ)
                return NULL;
            json_object_array_add(responseJ, argJ);
        }
    }
    return responseJ;
}

// Push a json structure on the stack as a LUA table
int LuaPushOneArg(lua_State *luaState,json_object *argsJ)
{

    json_type jtype = json_object_get_type(argsJ);
    switch (jtype)
    {
    case json_type_object:
    {
        lua_newtable(luaState);
        json_object_object_foreach(argsJ, key, val)
        {
            //GLUE_AFB_NOTICE(handle, "LuaPushOneArg key='%s' val='%s'", key, json_object_get_string(val));
            int err = LuaPushOneArg(luaState, val);
            if (!err)
                lua_setfield(luaState, -2, key);
        }
        break;
    }
    case json_type_array:
    {
        int length = (int)json_object_array_length(argsJ);
        lua_newtable(luaState);
        for (int idx = 0; idx < length; idx++)
        {
            json_object *val = json_object_array_get_idx(argsJ, idx);
            LuaPushOneArg(luaState, val);
            lua_seti(luaState, -2, idx);
        }
        break;
    }
    case json_type_int:
        lua_pushinteger(luaState, json_object_get_int64(argsJ));
        break;
    case json_type_string: {
        const char *text=  json_object_get_string(argsJ);
        // null string is used to carry lightdata within json_object
        if (text[0] == '\0') {
            void *pointer;
            pointer= json_object_get_userdata(argsJ);
            if (pointer) {
                lua_pushlightuserdata(luaState, pointer);
                break;
            }
        }
        lua_pushstring(luaState, json_object_get_string(argsJ));
        break;
        }
    case json_type_boolean:
        lua_pushboolean(luaState, json_object_get_boolean(argsJ));
        break;
    case json_type_double:
        lua_pushnumber(luaState, json_object_get_double(argsJ));
        break;
    case json_type_null:
        NOTICE("LuaPushOneArg: NULL object type %s", json_object_to_json_string(argsJ));
        lua_pushnil(luaState);
        break;
    default:
        ERROR("LuaPushOneArg: unsupported Json object type %s", json_object_to_json_string(argsJ));
        goto OnErrorExit;
    }
    return 0;

OnErrorExit:
    return -1;
}

// afb_req_v4_get_common(

void LuaInfoDbg(lua_State *luaState, GlueHandleT *handle, int level, const char *func, const char *message)
{
    lua_Debug luaDebug;
    const char *error;
    int line = -1;
    const char *source = "unk";

    if (!handle) goto OnErrorExit;

    if (lua_getstack(luaState, 1, &luaDebug))
    {
        lua_getinfo(luaState, "Sln", &luaDebug);
        line = luaDebug.currentline;
        source = luaDebug.short_src;
    }

    if (level != AFB_SYSLOG_LEVEL_ERROR)
    {
        GlueVerbose(handle, level, source, line, func, "%s", message);
    }
    else
    {
        error = lua_tostring(luaState, -1);
        GlueVerbose(handle, level, source, line, func, "%s error=[%s]", message, error);
    }
    return;

OnErrorExit:
    ERROR("(hoops) Invalid Lua Handle");
}

json_object *LuaJsonDbg(lua_State *luaState, const char *message)
{
    lua_Debug luaDebug;
    json_object *errorJ;
    int line = 0;
    const char *source = NULL, *name = NULL;
    const char *error = lua_tostring(luaState, -1);

    if (lua_getstack(luaState, 1, &luaDebug))
    {
        lua_getinfo(luaState, "Sln", &luaDebug);
        name = luaDebug.name;
        line = luaDebug.currentline;
        source = luaDebug.short_src;
    }

    // if no lua error remove json field
    if (!error || error[0] == '0') error = NULL;
    wrap_json_pack(&errorJ, "{ss* ss* si* ss* ss*}", "info", message, "source", source, "line", line, "name", name, "error", error);

    return (errorJ);
}

int LuaPrintMsg(lua_State *luaState, int level)
{
    char *message;
    const char *errorMsg = NULL;

    // get binder handle
    GlueHandleT *binder = LuaBinderPop(luaState);
    if (!binder)
        goto OnErrorExit;

    // check api handle
    GlueHandleT *glue = lua_touserdata(luaState, LUA_FIRST_ARG);
    if (!glue)
    {
        errorMsg = "missing require api/rqt handle";
        goto OnErrorExit;
    }

    switch (glue->magic)
    {
        case GLUE_API_MAGIC:
            if (!AFB_SYSLOG_MASK_WANT(afb_api_logmask(glue->api.afb), level))
                goto OnQuietExit;
            break;

        case GLUE_RQT_MAGIC:
            if (!AFB_SYSLOG_MASK_WANT(afb_req_logmask(glue->rqt.afb), level))
                goto OnQuietExit;
            break;

        case GLUE_BINDER_MAGIC:
        default:
            if (!AFB_SYSLOG_MASK_WANT(AfbBinderGetLogMask(glue->binder.afb), level))
                goto OnQuietExit;
    }

    json_object *requestJ = LuaPopArgs(luaState, LUA_FIRST_ARG + 1);
    if (!requestJ)
    {
        errorMsg = "LuaPrintMsg empty message";
        goto OnErrorExit;
    }

    // if we have only on argument just return the value.
    if (json_object_get_type(requestJ) != json_type_array || json_object_array_length(requestJ) < 2)
    {
        message = (char *)json_object_get_string(requestJ);
        goto PrintMessage;
    }

    // extract format and push all other parameter on the stack
    message = alloca(LUA_MSG_MAX_LENGTH);
    const char *format = json_object_get_string(json_object_array_get_idx(requestJ, 0));
    int arrayIdx = 1;
    int uidIdx = 0;

    for (int idx = 0; format[idx] != '\0'; idx++)
    {
        if (format[idx] == '%' && format[idx + 1] != '\0')
        {
            json_object *slotJ = json_object_array_get_idx(requestJ, arrayIdx);
            //if (slotJ) GLUE_AFB_NOTICE("**** idx=%d slotJ=%s", arrayIdx, json_object_get_string(slotJ));

            switch (format[++idx])
            {
            case 'd':
                if (slotJ)
                    uidIdx += snprintf(&message[uidIdx], LUA_MSG_MAX_LENGTH - uidIdx, "%d", json_object_get_int(slotJ));
                else
                    uidIdx += snprintf(&message[uidIdx], LUA_MSG_MAX_LENGTH - uidIdx, "nil");
                arrayIdx++;
                break;
            case 'f':
                if (slotJ)
                    uidIdx += snprintf(&message[uidIdx], LUA_MSG_MAX_LENGTH - uidIdx, "%f", json_object_get_double(slotJ));
                else
                    uidIdx += snprintf(&message[uidIdx], LUA_MSG_MAX_LENGTH - uidIdx, "nil");
                arrayIdx++;
                break;

            case '%':
                message[uidIdx] = '%';
                uidIdx++;
                break;

            case 's':
            default:
                if (slotJ)
                    uidIdx += snprintf(&message[uidIdx], LUA_MSG_MAX_LENGTH - uidIdx, "%s", json_object_get_string(slotJ));
                else
                    uidIdx += snprintf(&message[uidIdx], LUA_MSG_MAX_LENGTH - uidIdx, "nil");
                arrayIdx++;
            }
        }
        else
        {
            if (uidIdx >= LUA_MSG_MAX_LENGTH)
            {
                const char *trunc = "... <truncated> ";
                GLUE_AFB_WARNING(glue, "LuaPrintMsg: message[%s] overflow LUA_MSG_MAX_LENGTH=%d\n", format, LUA_MSG_MAX_LENGTH);
                uidIdx = LUA_MSG_MAX_LENGTH - 1;
                memcpy(&message[uidIdx - strlen(trunc)], trunc, strlen(trunc));
                break;
            }
            else
            {
                message[uidIdx++] = format[idx];
            }
        }
    }
    message[uidIdx] = '\0';

PrintMessage:
    LuaInfoDbg(luaState, glue, level, __func__, message);
    json_object_put(requestJ);
    return 0; // no argument returned to lua

OnErrorExit:
    LuaInfoDbg(luaState, glue, level,__func__, errorMsg);
    lua_pushlstring(luaState, errorMsg, strlen(errorMsg));
    lua_error(luaState);
    return 1;

OnQuietExit:
    return 0;
}

// retreive subcall response and build LUA response
const char *LuaPushAfbReply (lua_State *luaState, unsigned nreplies, const afb_data_t *replies, int *index) {
    const char *errorMsg=NULL;
    int count;

    *index=0;
    for (count = 0; count < nreplies; count++)
    {
        if (replies[count]) {
            switch (afb_typeid(afb_data_type(replies[count])))  {

                case Afb_Typeid_Predefined_Stringz: {
                    const char *value= (char*)afb_data_ro_pointer(replies[count]);
                    if (value && value[0]) {
                        lua_pushstring (luaState, value);
                        (*index)++;
                    }
                    break;
                }
                case Afb_Typeid_Predefined_Bool: {
                    const int *value= (int*)afb_data_ro_pointer(replies[count]);
                    lua_pushboolean(luaState, *value);
                    (*index)++;
                    break;
                }
                case Afb_Typeid_Predefined_I8:
                case Afb_Typeid_Predefined_U8:
                case Afb_Typeid_Predefined_I16:
                case Afb_Typeid_Predefined_U16:
                case Afb_Typeid_Predefined_I64:
                case Afb_Typeid_Predefined_U64:
                case Afb_Typeid_Predefined_I32:
                case Afb_Typeid_Predefined_U32: {
                    const long *value= (long*)afb_data_ro_pointer(replies[count]);
                    lua_pushinteger(luaState, *value);
                    (*index)++;
                    break;
                }
                case Afb_Typeid_Predefined_Double:
                case Afb_Typeid_Predefined_Float: {
                    const double *value= (double*)afb_data_ro_pointer(replies[count]);
                    lua_pushnumber(luaState, *value);
                    (*index)++;
                    break;
                }

                case  Afb_Typeid_Predefined_Json: {
                    afb_data_t data;
                    json_object *valueJ;
                    int err;

                    err = afb_data_convert(replies[count], &afb_type_predefined_json_c, &data);
                    if (err) {
                        errorMsg= "unsupported json string";
                        goto OnErrorExit;
                    }
                    valueJ= (json_object*)afb_data_ro_pointer(data);
                    LuaPushOneArg (luaState, valueJ);
                    afb_data_unref(data);
                    (*index)++;
                    break;
                }
                case  Afb_Typeid_Predefined_Json_C: {
                    json_object *valueJ= (json_object*)afb_data_ro_pointer(replies[count]);
                    if (valueJ) {
                        LuaPushOneArg (luaState, valueJ);
                        (*index)++;
                    }
                    break;
                }
                default:
                    errorMsg= "unsupported return data type";
                    goto OnErrorExit;
            }
        }
    }
    return NULL;

OnErrorExit:
    return errorMsg;
}