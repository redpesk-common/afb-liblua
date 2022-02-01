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

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <stdarg.h>
#include <assert.h>
#include <wrap-json.h>

#include <libafb/sys/verbose.h>

#include "glue-afb.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>


struct LuaBinderHandleS {
    AfbBinderHandleT *afb;
    json_object *configJ;
};

struct LuajobsstartS {
    char *luafunc;
    json_object *dataJ;
    struct afb_sched_lock *afb;
    afb_api_t  apiv4;
    int status;
};

struct LuaApiHandleS {
    afb_api_t  afb;
    const char *ctrlCb;
    json_object *configJ;
};

struct LuaRqtHandleS {
    struct LuaApiHandleS *api;
    int replied;
    afb_req_t afb;
};

struct LuaTimerHandleS {
    const char *uid;
    char *callback;
    afb_timer_t afb;
    afb_api_t apiv4;
    json_object *configJ;
    void *userdata;
    int usage;
};

struct LuaEvtHandleS {
    const char *name;
    afb_event_t afb;
    json_object *configJ;
    afb_api_t apiv4;
    int count;
};

struct LuaHandlerHandleS {
    const char *uid;
    const char *callback;
    json_object *configJ;
    void *userdata;
    int count;
    afb_api_t apiv4;
};

typedef struct {
    GlueHandleMagicsE magic;
    lua_State *luaState;
    union {
        struct LuaBinderHandleS binder;
        struct LuaEvtHandleS evt;
        struct LuaApiHandleS api;
        struct LuaRqtHandleS rqt;
        struct LuaTimerHandleS timer;
        struct LuajobsstartS lock;
        struct LuaHandlerHandleS handler;
    };
} GlueHandleT;

typedef struct {
    int magic;
    GlueHandleT *glue;
    char *callback;
    void *userdata;
} GlueHandleCbT;

#define LUA_FIRST_ARG 1 // 1st argument
#define LUA_MSG_MAX_LENGTH 2048