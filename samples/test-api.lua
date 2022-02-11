#!/usr/bin/lua

--[[
Copyright 2021 Fulup Ar Foll fulup@iot.bzh
Licence: $RP_BEGIN_LICENSE$ SPDX:MIT https://opensource.org/licenses/MIT $RP_END_LICENSE$

object:
    test-api.lua does not implement new api, but test existing APIs, it:
    - imports helloworld-event binding and api
    - call helloworld-event/startTimer to activate binding timer (1 event per second)
    - call helloworld-event/subscribe to subscribe to event
    - lock mainloop with EventGet5Test and register the eventHandler (EventRec5TestCB) with mainloop lock
    - finally (EventRec5TestCB) count 5 events and release the mainloop lock received from EventGet5Test

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
local evtcount=0
local myTestCase=nul

function EventSubscribe(binder, userdata)
    local status= libafb.callsync(binder, "helloworld-event", "subscribe")
    return status
end

function EventUnsubscribe(binder, userdata)
    local status= libafb.callsync(binder, "helloworld-event", "unsubscribe")
    return status
end

function StartEventTimer(binder, userdata)
    local status= libafb.callsync(binder, "helloworld-event", "startTimer")
    return status
end

-- receive helloworld event and count them
function EventRec5TestCB(evt, name, job, args)
    libafb.notice (evt, "event=%s count=%d args=%s", name, evtcount, args)
    evtcount= evtcount -1
    if (evtcount ==  0)
    then
        libafb.notice (evt, "*** EventRec5TestCB releasing lock ***");
        libafb.jobkill (job, 0) -- jobkill(handle, lock, status)
    end
end

-- start an event handler to receive 5 event
function EventGet5Test(job, userdata)
    libafb.notice (job, "EventGet5Test timer-event handler registration")
    libafb.evthandler(job, {uid='timer-event', pattern='helloworld-event/timerCount',callback='EventRec5TestCB'}, job)
    return 0 -- non zero return would force jobstart to exit
end

-- start a jon and wait timeout second until 'jobkill' action
function EventJob5Test(binder, userdata)
    evtcount=userdata.count
    libafb.notice (binder, "waiting (%ds) for test to finish", userdata.timeout)
    local status= libafb.jobstart(binder, userdata.timeout, 'EventGet5Test', nil)
    return status
end

-- executed when binder and all api/interfaces are ready to serv
function startTestCB(binder, userdata)
    local status=0
    local timeout=7 -- seconds
    libafb.notice(binder, "startTestCB=[%s]", libafb.config(binder, "uid"))

    for idx, test in pairs(myTestCase) do
        libafb.notice (binder, "==> Starting [%s] info=%s", test.uid, test.info)
        status= test.callback (binder, test.userdata)
        if (status >=0) then
            libafb.notice (binder, " -- [%s] Success status=%d", test.uid, status)
        else
            libafb.error (binder, " -- [%s] Fail status=%d", test.uid, status)
        end
    end
    return(1) -- 0 would keep mainloop running
end

-- helloworld binding sample definition
local hellowBinding = {
    uid    = 'helloworld-event',
    export = 'private',
    path   = 'afb-helloworld-subscribe-event.so',
    ldpath = {os.getenv("HOME") .. '/opt/helloworld-binding/lib','/usr/local/helloworld-binding/lib'},
}

-- define and instanciate libafb-binder
local eventOpts = {
    uid     = 'lua-binder',
    port    = 0,
    verbose = 9,
    roothttp= './conf.d/project/htdocs',
    rootdir = '.'
}

-- create and start binder
libafb.luastrict(true)
local binder= libafb.binder(eventOpts)
local hello = libafb.binding(hellowBinding)

-- minimalist test framework
myTestCase = {
    {uid='evt-ticstart'   ,callback=StartEventTimer , userdata=nil, expect=0, info='start helloworld binding timer'},
    {uid='evt-subscribe'  ,callback=EventSubscribe  , userdata=nil, expect=0, info='subscribe to hellworld event'},
    {uid='evt-getcount'   ,callback=EventJob5Test   , userdata={timeout=10, count=5}, expect=0, info='wait for 5 helloworld event'},
    {uid='evt-unsubscribe',callback=EventUnsubscribe, userdata=nil, expect=0, info='subscribe to hellworld event'},
}

-- should never return
local status= libafb.loopstart(binder, 'startTestCB')
if (status < 0)
then
    libafb.error (binder, "OnError MainLoop Exit")
else
    libafb.notice(binder, "OnSuccess Mainloop Exit")
end
