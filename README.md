# afb-liblua

Exposes afb-libafb to lua scripting language. This module allows to script in lua to either mock binding api, test client, quick prototyping, ... Afb-liblua runs as a standard lua C module, it provides a full access to afb-libafb functionalities, subcall, event, acls, mainloop control, ...

## Dependency

* afb-libafb (from jan/2022 version)
* afb-libglue
* lua
* afb-cmake-modules

## Building

```bash
    mkdir build
    cd build
    cmake ..
    make
```

## Testing

Make sure that your dependencies are reachable from lua scripting engine, before starting your test.

```bash
    export LD_LIBRARY_PATH=/path/to/afb-libglue.so
    export LUA_CPATH=/path/to/afb-libafb.so

    lua samples/simple-api.lua
    lua samples/...
    lua samples/test-api.lua
```

## Import afb-luaglue

Your lua script should import afb-luaglue. require return a table which contains the c-module api.

```lua
    #!/usr/bin/lua
    package.cpath="./build/package/lib/?.so;;"
    local libafb=require('afb-luaglue')
```

## Configure binder services/options

When running mock binding APIs a very simple configuration as following one should be enough. For full options of libafb.binder check libglue API documentation.


```lua
    -- define and instanciate libafb-binder
    local demoOpts = {
        uid     = 'lua-binder',
        port    = 1234,
        verbose = 9,
        roothttp= './conf.d/project/htdocs',
    }
    libafb.luastrict(true)
    local binder= libafb.binder(demoOpts)
```

For HTTPS cert+key should be added. Optionally a list of aliases and ldpath might also be added

```lua
    -- define and instanciate libafb-binder
    local demoOpts = {
        uid     = 'lua-binder',
        port    = 443,
        https-cert= '/path/to/my/https.cert',
        https-key = '/path/to/my/https.key'
        verbose = 9,
        roothttp= './conf.d/project/htdocs',
        alias  = {'/devtools:/usr/share/afb-ui-devtools/binder'},
        ldpath = '/opt/bindings/lib64'},
    }
    libafb.luastrict(true)
    local binder= libafb.binder(demoOpts)
```

## Exposing api/verbs

afb-liblua allows user to implement api/verb directly in scripting language. When api is export=public corresponding api/verbs are visible from HTTP. When export=private they remain visible only from internal calls. Restricted mode allows to exposer API as unix socket with uri='unix:@api' tag.

Expose a new api with ```libafb.apiadd(demoApi)``` as in following example.

```lua
-- ping/pong test func
function pingCB(rqt)
    count= count+1
    libafb.notice  (rqt, "pingCB count=%d", count)
    libafb.respond (rqt, 0, {'pong', count})
    --return 0, {"pong", count} --implicit response
end

local MyVerbs = {
    {uid='lua-ping', verb='ping',func='pingCB',info='lua ping demo function'},
    {...}
}

-- define and instanciate API
local demoApi = {
    uid     = 'lua-demo',
    api     = 'demo',
    class   = 'test',
    info    = 'lua api demo',
    verbose = 9,
    export  = 'public',
    require = 'helloworld',
    control = 'startApiCb',
    verbs   = MyVerbs,
}

local glue= libafb.apiadd(demoApi)
```


## API/RQT Subcalls

Both synchronous and asynchronous call are supported. The fact the subcall is done from a request or from a api context is abstracted to the user. When doing it from RQT context client security context is not propagated and remove event are claimed by the lua api.

Explicit response to a request is done with ``` libafb.respond(rqt,status,arg1,..,argn)```. When running a synchronous request an implicit response may also be done with ```return(status, arg1,...,arg-n)```. Note that with afb-v4 an application may return zero, one or many data.

```lua
Function asyncRespCB(rqt, ctx, status, response)
    libafb.respond (rqt, status, 'async helloworld/testargs', response)
end

function syncCB(rqt, query)
    local status,response= libafb.callsync(rqt
        , "helloworld","testargs"
        , query
    )
    if (status ~= 0) then
        libafb.respond (rqt
            , status
            , 'async helloworld/testargs fail'
        )
    else
        libafb.respond (rqt
            , status
            , 'async helloworld/testargs success'
        )
    end
end

function asyncCB(rqt, query)
    libafb.callasync(rqt
        ,"helloworld","testargs"
        ,"asyncRespCB", 'user-data'
        , query
    )
    -- response should come from 'asyncRespCB' callback
end

```

## Events

Event should attached to an API. As binder as a building secret API, it is nevertheless possible to start a timer directly from a binder. Under normal circumstances, event should be created from API control callback, when API it's state=='ready'. Note that it is developer responsibility to make luaEvent handle visible from the function that create the event to the function that use the event.

```lua
    local luaEvent

    function apiControlCb(api, state)
        local apiname= libafb.config(api, "api")

        if (state == 'ready') then
            local tictime= libafb.config(api,'tictime')*1000
            luaEvent= libafb.evtnew (api
                ,{uid='lua-event'
                , info='lua testing event sample'}
            )
            if (luaEvent == nil) then goto done end

            ::done::
        end

    if (state == 'orphan') then
        libafb.warning(api,"api=[%s] receive an orphan event", apiname)
    end
    return 0 -- 0=ok -1=fatal
    end

    -- later event can be push with evtpush
    libafb.evtpush(luaEvent, {userdata})

```

Client event subscription is handle with evtsubscribe|unsubcribe api. Subscription API should be call from a request context as in following example, extracted from sample/event-api.lua

```lua
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
```

## Timers

Timer are typically used to push event or to handle timeout. Timer is started with ```libafb.timernew``` Timer configuration includes a callback, a ticktime in ms and a number or run (count). When count==0 timer runs infinitely.
In following example, timer runs forever every 'tictime' and call TimerCallback' function. This callback being used to send an event.

```lua
    -- timer handle callback
    function TimerCallback (timer, evt)
        count= count +1
        libafb.evtpush(evt, {count=count})
    end

    local timer= libafb.timernew (api
        , {uid='lua-timer'
        , callback='TimerCallback'
        , period=tictime
        , count=0}
        , luaEvent)
    if (timer == nil) then goto done end

```

When a one shot timer is enough 'schedpost' is usually a better choice. This is especially true for timeout handling. When use in conjunction with mainloop control.

```lua
    -- timer handle callback
    function TimeoutCB (handle, context)
        -- do something with context
        libafb.notice (handle, "I'm in timer callback")
    end

    -- schedpost callback run once and only once
    jobid= libafb.schedpost (api, 'TimeoutCB', timeoutMS)
    -- do something

    --
    libafb.schedcancel (jobid) -- optionally kill timer before timeout.
```


## Binder MainLoop

Under normal circumstance binder mainloop never returns. Nevertheless during test phase it is very common to wait and asynchronous event(s) before deciding if the test is successfully or not.
Mainloop starts with libafb.mainloop('xxx'), where 'xxx' is an optional startup function that control mainloop execution. They are two ways to control the mainloop:

* when startup function returns ```status!=0``` the binder immediately exit with corresponding status. This case is very typical when running pure synchronous api test.
* set a shedwait lock and control the main loop from asynchronous events. This later case is mandatory when we have to start the mainloop to listen event, but still need to exit it to run a new set of test.

Mainloop schedule wait is done with ```libafb.schedwait(binder,'xxx',timeout,{optional-user-data})```. Where 'xxx' is the name of the control callback that received the lock. Schedwait is released with ``` libafb.schedunlock(rqt/evt,lock,status)```

In following example:
* schedwait callback starts an event handler and passes the lock as evt context
* event handler: count the number of event and after 5 events release the lock.

Note:

* libafb.schedwait does not return before the lock is releases. As for events it is the developer responsibility to either carry the lock in a context or to store it within a share space, on order unlock function to access it.

* it is possible to serialize libafb.schedwait in order to build asynchronous cascade of asynchronous tests.

```lua

    -- receive 5 event and then release the lock
    function EventReceiveCB(evt, name, ctx, data)
        evtCount= evtCount +1
        if (evtCount == 5) then
            -- schedunlock(handle, lock, status)
            libafb.schedunlock (evt
                , ctx
                , evtCount
            )
        end
    end

    -- retreive shedwait lock and attach it to the event handler
    function SchedwaitCB(api, lock, context)
        evtCount=0
        libafb.evthandler(api
            , {uid='timer-event', pattern='helloworld-event/timerCount',func='EventReceiveCB'}
            , lock
        )
        return 0;
        end

    -- set a schedwait on running mainloop
    function startTestCB(binder)
        status= libafb.schedwait(binder
            , 'SchedwaitCB'
            , timeout
            , nil -- optional userdata
        )
    end

    -- start mainloop
    local status= libafb.mainloop('startTestCB')
```

## Miscellaneous APIs/utilities

* libafb.clientinfo(rqt): returns client session info.
* libafb.luastrict(true): prevents LUA from creating global variables.
* libafb.config(handle, "key"): returns binder/rqt/timer/... config
* libafb.notice|warning|error|debug print corresponding hookable syslog trace
* libafb.serialize(data, "key") return key value from a json object
