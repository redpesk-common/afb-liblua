#!/usr/bin/lua

--[[
Copyright 2021 Fulup Ar Foll fulup@iot.bzh
Licence: $RP_BEGIN_LICENSE$ SPDX:MIT https://opensource.org/licenses/MIT $RP_END_LICENSE$

object:
    event-api.lua
    - 1st create 'demo' api
    - 2nd when API ready, create an event named 'lua-event'
    - 3rd creates a timer(lua-timer) that tic every 3s and call TimerCB that fire previsouly created event
    - 4rd implements two verbs demo/subscribe and demo/unscribe
    - 5rd attaches two events handler to the API evtTimerCB/evtOtherCB those handlers requiere a subcall to subscribe some event
    demo/subscribe|unsubscribe can be requested from REST|websocket from a browser on http:localhost:1234

usage
    - from dev tree: LD_LIBRARY_PATH=../afb-libglue/build/src/ lua samples/event-api.lua
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
local luaEvent

-- When Api ready (state==init) start event & timer
function apiControlCb(api, state)
    local apiname= libafb.config(api, "api")

    if (state == 'config') then
        libafb.notice(api, "api=[%s] info=[%s]", apiname, libafb.config(api, 'info'))
    end

    if (state == 'ready') then
        local tictime= libafb.config(api,'tictime')*1000 -- move from second to ms
        libafb.notice(api, "api=[%s] start event tictime=%dms", apiname, tictime)

        luaEvent= libafb.evtnew (api,{uid='lua-event', info='lua testing event sample'})
        if (luaEvent == nil) then goto done end

        local timer= libafb.timernew (api, {uid='lua-timer', callback='TimerCB', period=tictime, count=0}, luaEvent)
        if (timer == nil) then goto done end

        ::done::
    end

    if (state == 'orphan') then
        libafb.warning(api, "api=[%s] receive an orphan event", apiname)
    end

    return 0 -- 0=ok -1=fatal
end

-- executed when binder and all api/interfaces are ready to serv
function mainLoopCb(binder)
    libafb.notice(binder, "mainLoopCb=[%s]", libafb.config(binder, "uid"))
    -- implement here after your startup/eventing code
    -- ...
    return 0 -- negative status force mainloop exit
end

-- timer handle callback
function TimerCB (timer, evt)
    count= count +1
    libafb.notice (evt, "timer='%s' event='%s' count=%d", libafb.config(timer, 'uid'), libafb.config(evt, 'uid'), count)
    libafb.evtpush(evt, {count=count})
end

 -- ping/pong event func
function pingCB(rqt)
    count= count+1
    libafb.notice  (rqt, "pingCB count=%d", count)
    libafb.respond (rqt, 0, {'pong', count})
end

function subscribeCB(rqt)
    libafb.notice  (rqt, "subscribing api event")
    libafb.evtsubscribe (rqt, luaEvent)
    return 0 -- implicit respond
end

function unsubscribeCB(rqt)
    libafb.notice  (rqt, "subscribing api event")
    libafb.evtunsubscribe (rqt, luaEvent)
    return 0 -- implicit respond
end

function evtTimerCB (api, name, data)
    libafb.notice  (rqt, "evtTimerCB name=%s data=%s", name, data)
end

function evtOtherCB (api, name, data)
    libafb.notice  (rqt, "evtOtherCB name=%s data=%s", name, data)
end

-- api verb list
local apiVerbs = {
    {uid='lua-ping'       , verb='ping'       , callback='pingCB'       , info='ping event function'},
    {uid='lua-subscribe'  , verb='subscribe'  , callback='subscribeCB'  , info='subscribe to event'},
    {uid='lua-unsubscribe', verb='unsubscribe', callback='unsubscribeCB', info='unsubscribe to event'},
}

local apiEvents = {
    {uid='lua-event' , pattern='lua-event', callback='evtTimerCB' , info='timer event handler'},
    {uid='lua-other' , pattern='*', callback='evtOtherCB' , info='any other event handler'},
}

-- define and instanciate API
local glue = {
    uid     = 'lua-event',
    info    = 'lua api event demonstration',
    api     = 'event',
    provide   = 'lua-test',
    verbose = 9,
    export  = 'public',
    control = 'apiControlCb',
    tictime = 3,
    verbs   = apiVerbs,
    events  = apiEvents,
    alias  = {'/devtools:/usr/share/afb-ui-devtools/binder'},
}

-- define and instanciate libafb-binder
local binderOpts = {
    uid     = 'lua-binder',
    port    = 1234,
    verbose = 9,
    roothttp= './conf.d/project/htdocs',
    rootdir = '.'
}

-- create and start binder
libafb.luastrict(true)
local binder= libafb.binder(binderOpts)
local glue= libafb.apiadd(glue)

-- should never return
local status= libafb.mainloop('mainLoopCb')
if (status < 0) then
    libafb.error (binder, "OnError MainLoop Exit")
else
    libafb.notice(binder, "OnSuccess Mainloop Exit")
end
