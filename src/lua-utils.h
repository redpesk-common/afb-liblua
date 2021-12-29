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

#pragma once
#include <json-c/json.h>
#include "lua-afb.h"

void LuaAfbVerbose (LuaHandleT *luaState, int level, const char *file, int line, const char *func, const char *fmt, ...);
#define LUA_AFB_INFO(luaHandle,...)    LuaAfbVerbose (luaHandle,AFB_SYSLOG_LEVEL_INFO,__file__,__LINE__,__func__,__VA_ARGS__)
#define LUA_AFB_NOTICE(luaHandle,...)  LuaAfbVerbose (luaHandle,AFB_SYSLOG_LEVEL_NOTICE,__file__,__LINE__,__func__,__VA_ARGS__)
#define LUA_AFB_WARNING(luaHandle,...) LuaAfbVerbose (luaHandle,AFB_SYSLOG_LEVEL_WARNING,__file__,__LINE__,__func__,__VA_ARGS__)
#define LUA_AFB_ERROR(luaHandle,...)   LuaAfbVerbose (luaHandle,AFB_SYSLOG_LEVEL_ERROR,__file__,__LINE__,__func__,__VA_ARGS__)
#define LUA_DBG_ERROR(luaState,luaHandle,...)   LuaInfoDbg    (luaState, luaHandle, AFB_SYSLOG_LEVEL_ERROR, __func__, __VA_ARGS__);

afb_api_t LuaAfbGetApi(LuaHandleT*luaHandle);

void LuaInfoDbg (lua_State* luaState, LuaHandleT *luaHandle, int level, const char *func, const char *message);
int LuaPrintMsg(lua_State *luaState, int level);
json_object *LuaJsonDbg (lua_State *luaState, const char *message);

LuaHandleT *LuaRqtNew(afb_req_t afbRqt);
void LuaRqtAddref(LuaHandleT *luaRqt);
void LuaRqtUnref(LuaHandleT *luaRqt);
int LuaAfbReply (LuaHandleT *luaRqt, int status, int nbreply, afb_data_t *reply);

LuaHandleT *LuaRqtPop(lua_State *luaState, int index);
LuaHandleT* LuaEventPop (lua_State* luaState, int index);
LuaHandleT* LuaApiPop (lua_State* luaState, int index);
LuaHandleT *LuaTimerPop(lua_State *luaState, int index);
LuaHandleT *LuaLockPop(lua_State *luaState, int index);
LuaHandleT *LuaBinderPop(lua_State *luaState);

json_object *LuaTableToJson(lua_State *luaState, int index);
json_object *LuaPopArgs(lua_State *luaState, int start);
json_object *LuaPopOneArg(lua_State *luaState,  int idx);
int LuaPushOneArg(lua_State *luaState, json_object *argsJ);
const char *LuaPushAfbReply (lua_State *luaState, unsigned replies, const afb_data_t *reply, int *index);
