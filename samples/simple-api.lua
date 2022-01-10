#!/usr/bin/lua

--[[
Copyright 2021 Fulup Ar Foll fulup@iot.bzh
Licence: $RP_BEGIN_LICENSE$ SPDX:MIT https://opensource.org/licenses/MIT $RP_END_LICENSE$

object:
    simple-api.lua create a single api(demo) with two verbs 'ping' + 'args'
    this api can be requested from REST|websocket from a browser on http:localhost:1234

usage
    - from dev tree: LD_LIBRARY_PATH=../afb-libglue/build/src/ lua samples/simple-api.lua
    - point your browser at http://localhost:1234/devtools

config: following should match your installation paths
    - devtools alias should point to right path alias= {'/devtools:/usr/share/afb-ui-devtools/binder'},
    - LUA_CPATH='/my-mod-path-aaa/?.so;/my-mod-path-xxx/?.so;;'
    - LD_LIBRARY_PATH='/my-glulib-path/libafb-glue.so'

--]]

-- load libafb lua glue
package.cpath="./build/package/lib/?.so;;"
local libafb=require('afb-luaglue')

-- static variables
local count= 0

 -- ping/pong test func
function pingCB(rqt)
    count= count+1
    libafb.notice  (rqt, "pingCB count=%d", count)
    libafb.reply (rqt, 0, {'pong', count})
    --return 0, {"pong", count} --implicit response
end

function argsCB(rqt, query)
    libafb.notice  (rqt, "actionCB query=%s", query)
    libafb.reply (rqt, 0, {'query', query})
end

-- api verb list
local demoVerbs = {
    {uid='lua-ping', verb='ping', callback='pingCB'  , info='lua ping demo function'},
    {uid='lua-args', verb='args', callback='argsCB', info='lua check input query', sample={{arg1='arg-one', arg2='arg-two'}, {argA=1, argB=2}}},
}

-- define and instanciate API
local demoApi = {
    uid     = 'lua-demo',
    api     = 'demo',
    class   = 'test',
    info    = 'lua api demo',
    verbose = 9,
    export  = 'public',
    verbs   = demoVerbs,
    alias  = {'/devtools:/usr/share/afb-ui-devtools/binder'},
}

-- define and instanciate libafb-binder
local demoOpts = {
    uid     = 'lua-binder',
    port    = 1234,
    verbose = 9,
    roothttp= './conf.d/project/htdocs',
    rootdir = '.'
}

-- executed when binder and all api/interfaces are ready to serv
function loopBinderCb(binder)
    libafb.notice(binder, "loopBinderCb=%s", libafb.config(binder, "uid"))
    return 0 -- keep running for ever
end

-- create and start binder
libafb.luastrict(true)
local binder= libafb.binder(demoOpts)
local glue= libafb.apiadd(demoApi)

-- should never return
local status= libafb.mainloop('loopBinderCb')
if (status < 0) then
    libafb.error (binder, "OnError MainLoop Exit")
else
    libafb.notice(binder, "OnSuccess Mainloop Exit")
end
