# S3c: boot-path OS services (IPC level)

Part S3c of the Metroid Prime Remastered recompilation. The recompiled SDK (`sdk`, `subsdk0`, `main`) talks to Horizon through SVCs and IPC sessions. The SVC layer is in `src/kernel`; this work answers the IPC services a Switch application needs at boot and while it runs. Everything lives in `src/services/` and is registered by `nxrt::services::register_all()` (called from `nxrt::init`, switch off with `InitOptions.services = false`).

## Which services

`game/exefs/main.npdm` carries the Service Access Control list of the title (46 services): `acc:u0 aoc:u apm appletOE audin:u audout:u audren:u banana bcat:u bsd:u bsdcfg caps:su caps:u csrng ectx:aw friend:u fsp-srv hid htc htc:tenv htcs hwopus irs ldn:u ldr:ro lm mii:u mm:u nfc:mf:u nfc:user nfp:user ngct:u nifm:u nim:eca notif:a nsd:u ntc nvdrv pcm pctl pl:u prepo:u set sfdnsres ssl time:u vi:u`. `fatal`, `err`, `set:sys`, `psm` are not in it: they are provided anyway because the SDK's client library contains code for them. The `nn::` calls the game's `main` imports from the SDK (422 imports) use: `nn::oe` (am), `nn::time`, `nn::account`, `nn::settings`, `nn::fs` (ROM, save data), `nn::audio` (the audio renderer), `nn::vi`, `nn::hid` (npad, six-axis, vibration, touch), `nn::os`.

control.nacp facts (also the defaults of `TitleProfile`): 13 supported languages, startup user account required, user account save data 4 MiB plus 1 MiB journal, no device or cache save data, no BCAT, add-on content base id 010012101468D000.

## How a service is written

* One `src/services/<name>.cpp` per unit with `register_<name>()` and `<name>_reset()` (declared in `services_internal.h`). `register_<name>()` calls `services::add("<sm name>", factory)` for each service name it provides. A command is `svc->add_cmd(id, "SwitchbrewName", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { ... })`; the return value is the CMIF result (0 = success). Inputs: `req.arg<T>(offset)` reads the raw data after the SFCI header (u64 arguments sit at 8-byte aligned offsets, as Switchbrew's tables say); buffers: `req.in_buf(n)` / `req.out_buf(n)`; handles: `req.in_copy`, `req.in_move`, `req.pid`. Outputs: `rsp.push<T>(v)` in order (mind the alignment: pad with zeros where the layout has padding), `rsp.out_copy.push_back(handle)` (copy handles), `rsp.push_object(std::shared_ptr<Service>)` for a returned interface (a move handle in a plain session, an object id in a domain).
* Process-wide state is a function-local singleton in the .cpp, shared by the per-session service objects; `<name>_reset()` puts it back to its start-up state (the tests call it through `nxsvc::Env`).
* Events: `services::ServiceEvent` (readable event, `new_handle()` per request, `signal()` wakes waiters). Shared memory: `services::ServiceSharedMemory` (write through `host()`).
* Facts about the title and console come from `services::profile()` (language, region, docked, uid, save sizes ...).
* Log with `NXLOG_I/D/W("<tag>", ...)`: one Info line for the first use of each command is useful; per-frame commands log at Trace only.
* Tests: `tests/test_svc_<name>.cpp`, `NXTEST_GROUP(svc_<name>)`, client in `tests/svc_test_util.h`. Request bytes and expected reply bytes are written out from Switchbrew / libnx (the test must not be derived from reading the service's own code).

## Service table

Filled in as the services land: see the sections below.
