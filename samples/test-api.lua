#!/usr/bin/lua

--[[
Copyright 2021 Fulup Ar Foll fulup@iot.bzh
Licence: $RP_BEGIN_LICENSE$ SPDX:MIT https://opensource.org/licenses/MIT $RP_END_LICENSE$

object:
    test-api.lua does not implement new api, but test existing APIs, it:
    - imports helloworld-event binding and api
    - call helloworld-event/startTimer to activate binding timer (1 event per second)
    - call helloworld-event/subscribe to subscribe to event
    - lock mainloop with aSyncEventTest and register the eventHandler (EventReceiveCB) with mainloop lock
    - finally (EventReceiveCB) count 5 events and release the mainloop lock received from aSyncEventTest

usage
    - from dev tree: LD_LIBRARY_PATH=../afb-libglue/build/src/ lua samples/test-api.lua
    - result of the test position mainloop exit status

config: following should match your installation paths
    - devtools alias should point to right path alias= {'/devtools:/usr/share/afb-ui-devtools/binder'},
    - LUA_CPATH='/my-mod-path-aaa/?.so;/my-mod-path-xxx/?.so;;'
    - LD_LIBRARY_PATH='/my-glulib-path/libafb-glue.so'

--]]

-- load libafb lua glue
package.cpath="./build/package/lib/?.so;;"
local libafb=require('afb-luaglue')

-- local are static visible only from current file
local evtCount=0

function EventReceiveCB(evt, name, lock, data)
    libafb.notice (evt, "event=%s data=%d", name, data)
    evtCount= evtCount +1
    if (evtCount == 5) then
        libafb.notice (evt, "*** EventReceiveCB releasing lock ***");
        libafb.schedunlock (evt, lock, evtCount) -- schedunlock(handle, lock, status)
    end
end

function aSyncEventTest(api, lock, context)
    evtCount=0
    libafb.notice (api, "Schedlock timer-event handler register")
    libafb.evthandler(api, {uid='timer-event', pattern='helloworld-event/timerCount',callback='EventReceiveCB'}, lock)
    return 0
end

function syncEventTest(binder)
    libafb.notice (binder, "helloworld-event", "startTimer")
    local status= libafb.callsync(binder, "helloworld-event", "subscribe")
    if (status ~= 0) then
        libafb.notice  (binder, "helloworld subscribe-event fail status=%d", status)
    end
    return status
end

function syncTimerTest(binder)
    libafb.notice (binder, "helloworld-event/startTimer")
    local status= libafb.callsync(binder, "helloworld-event", "startTimer")
    if (status ~= 0) then
        libafb.notice  (binder, "helloworld event-timer fail status=%d", status)
    end
    return status
end

-- executed when binder and all api/interfaces are ready to serv
function startTestCB(binder)
    local status=0
    local timeout=7 -- seconds
    libafb.notice(binder, "startTestCB=[%s]", libafb.config(binder, "uid"))

    -- implement here after your startup/testing code
    status= syncTimerTest(binder)
    if (status ~=0) then goto done end

    status= syncEventTest(binder)
    if (status ~=0) then goto done end

    libafb.notice (binder, "waiting (%ds) for test to finish", timeout)
    status= libafb.schedwait(binder, timeout, 'aSyncEventTest', nil)
    if (status < 0) then goto done end

    ::done::
    libafb.notice (binder, "test done status=%d", status)

    -- libafb.exit(binder, status) force binder exit from anywhere
    return(status) -- negative status force mainloop exit
end

-- helloworld binding sample definition
local hellowBinding = {
    uid    = 'helloworld-event',
    export = 'private',
    path   = 'afb-helloworld-subscribe-event.so',
    ldpath = {os.getenv("HOME") .. '/opt/helloworld-binding/lib','/usr/local/helloworld-binding/lib'},
    alias  = {'/hello:' .. os.getenv("HOME") .. '/opt/helloworld-binding/htdocs', '/devtools:/usr/share/afb-ui-devtools/binder'},
}

-- define and instanciate libafb-binder
local eventOpts = {
    uid     = 'lua-binder',
    port    = 1234,
    verbose = 9,
    roothttp= './conf.d/project/htdocs',
    rootdir = '.'
}

-- create and start binder
libafb.luastrict(true)
local binder= libafb.binder(eventOpts)
local hello = libafb.binding(hellowBinding)

-- should never return
local status= libafb.mainloop('startTestCB')
if (status < 0) then
    libafb.error (binder, "OnError MainLoop Exit")
else
    libafb.notice(binder, "OnSuccess Mainloop Exit")
end
