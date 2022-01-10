#!/usr/bin/lua

--[[
Copyright 2021 Fulup Ar Foll fulup@iot.bzh
Licence: $RP_BEGIN_LICENSE$ SPDX:MIT https://opensource.org/licenses/MIT $RP_END_LICENSE$

object:
    subcall-api.lua
    - 1st load helloworld binding
    - 2nd create a 'demo' api requiring 'helloworld' api
    - 3rd check helloworld/ping is responsing before exposing http service (mainLoopCb)
    - 4rd implement two verbs demo/sync|async those two verb subcall helloworld/testargs in synchronous/asynchronous mode
    demo/synbc|async can be requested from REST|websocket from a browser on http:localhost:1234

usage
    - from dev tree: LD_LIBRARY_PATH=../afb-libglue/build/src/ lua samples/subcall-api.lua
    - point your browser at http://localhost:1234/devtools

config: following should match your installation paths
    - devtools alias should point to right path alias= {'/devtools:/usr/share/afb-ui-devtools/binder'},
    - LUA_CPATH='/my-mod-path-aaa/?.so;/my-mod-path-xxx/?.so;;'
    - LD_LIBRARY_PATH='/my-glulib-path/libafb-glue.so'

--]]

-- load libafb lua glue
package.cpath="./build/package/lib/?.so;;"
local libafb=require ("afb-luaglue")
local count= 0

 -- ping/pong test func
function pingCB(rqt)
    count= count+1
    libafb.notice  (rqt, "pingCB count=%d", count)
    libafb.reply (rqt, 0, {'pong', count})
    --return 0, {"pong", count} --implicit response
end

function asyncRespCB(rqt, status, ctx, response)
    libafb.notice  (rqt, "asyncRespCB status=%d ctx='%s', response='%s'", status, libafb.serialize(ctx), response)
    libafb.reply (rqt, status, 'async helloworld/testargs', response)
end

function syncCB(rqt, query)
    libafb.notice  (rqt, "syncCB calling helloworld/testargs query=%s", query)

    local status,response= libafb.callsync(rqt, "helloworld","testargs", query)

    if (status ~= 0) then
        libafb.reply (rqt, status, 'async helloworld/testargs fail')
    else
        libafb.reply (rqt, status, 'async helloworld/testargs success')
    end
end

function asyncCB(rqt, query)
    libafb.notice  (rqt, "asyncCB calling helloworld/testargs query=%s", query)

    libafb.callasync (rqt,"helloworld","testargs","asyncRespCB", 'user-data', query)
    -- response within 'asyncRespCB' callback
end

-- api control function
function startApiCb(api, action)
    local apiname= libafb.config(api, "api")
    libafb.notice(api, "api=[%s] action=[%s]", apiname, action)

    if (action == 'config') then
        libafb.notice(api, "config=%s", libafb.config(api))
    end
    return 0 -- ok
end

-- executed when binder and all api/interfaces are ready to serv
function mainLoopCb(binder)
    libafb.notice(binder, "mainLoopCb=[%s]", libafb.config(binder, "uid"))

    -- implement here after your startup/testing code
    local status= libafb.callsync(binder, "helloworld", "ping")
    if (status ~= 0) then
        -- force an explicit response
        libafb.notice  (binder, "helloworld/ping fail status=%d", status)
    end
    return status -- negative status force mainloop exit
end

-- api verb list
local demoVerbs = {
    {uid='lua-ping'    , verb='ping' , callback='pingCB' , info='lua ping demo function'},
    {uid='lua-synccall', verb='sync' , callback='syncCB' , info='synchronous subcall of private api' , sample={{cezam='open'}, {cezam='close'}}},
    {uid='lua-asyncall', verb='async', callback='asyncCB', info='asynchronous subcall of private api', sample={{cezam='open'}, {cezam='close'}}},
}

-- define and instanciate API
local demoApi = {
    uid     = 'lua-demo',
    api     = 'demo',
    provide   = 'test',
    info    = 'lua api demo',
    verbose = 9,
    export  = 'public',
    require = 'helloworld',
    control = 'startApiCb',
    verbs   = demoVerbs,
}

-- helloworld binding sample definition
local hellowBinding = {
    uid    = 'helloworld',
    export = 'private',
    path   = 'afb-helloworld-skeleton.so',
    ldpath = {os.getenv("HOME") .. '/opt/helloworld-binding/lib','/usr/local/helloworld-binding/lib'},
    alias  = {'/hello:' .. os.getenv("HOME") .. '/opt/helloworld-binding/htdocs', '/devtools:/usr/share/afb-ui-devtools/binder'},
}

-- define and instanciate libafb-binder
local demoOpts = {
    uid     = 'lua-binder',
    port    = 1234,
    verbose = 9,
    roothttp= './conf.d/project/htdocs',
    rootdir = '.',
}

-- create and start binder
libafb.luastrict(true)
local binder= libafb.binder(demoOpts)
local hello = libafb.binding(hellowBinding)
local glue= libafb.apiadd(demoApi)

-- should never return
local status= libafb.mainloop('mainLoopCb')
if (status < 0) then
    libafb.error (binder, "OnError MainLoop Exit")
else
    libafb.notice(binder, "OnSuccess Mainloop Exit")
end
