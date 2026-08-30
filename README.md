# mqttmsg

An MQTT 3.1 client for the Raspberry Pi Pico, written in C against lwIP and
the Pico SDK. The frame encoders, parser and framing loop also build on the
host, which is where they are tested.

This project started as a hand-written MQTT library I used on personal
projects for several years. I have used Claude to help fix some bugs and
improve the usability and performance of the library. So the library is now
a bit of a hybrid: my own code and ideas, along with additions, especially
refactorings, from AI.

## Building a Pico application

Here are the steps to build a consumer project:

**1. Add mqttmsg as a git submodule.** It has a submodule of its own, so
the nested update needs `--recursive`:

```sh
git submodule add <url> libs/mqttmsg
git submodule update --init --recursive
```

**2. Supply an `lwipopts.h`** on your own include path — lwIP requires one
from every project. Copy `lwipopts_examples_common.h` from the Pico SDK's
[pico-examples](https://github.com/raspberrypi/pico-examples) repository
(`pico_w/wifi/`) into your project as `include/lwipopts.h` (see
[lwIP configuration](#lwip-configuration)).

**3. Wire it into CMake.** Set `PICO` before the `add_subdirectory()`; that
is what selects the device half of the library. `LOG_DEBUG` and `LOG_TRACE`
must be set to something, even if it is 0:

```cmake
cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2_w)
include(pico_sdk_import.cmake)
project(my-app C CXX ASM)
pico_sdk_init()

set(PICO ON)
set(LOG_DEBUG 0)
set(LOG_TRACE 0)
add_subdirectory(libs/mqttmsg)

add_executable(my-app src/main.c)
target_include_directories(my-app PRIVATE include/)   # where lwipopts.h lives
target_compile_definitions(my-app PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
    MQTT_HOST=\"${MQTT_HOST}\"
    MQTT_PORT=${MQTT_PORT})
target_link_libraries(my-app mqttmsg pico_stdlib)
pico_add_extra_outputs(my-app)
```

**4. Write the application.** One client, one transport; everything below
the attach line is the same source on a Pico and on Linux:

```c
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include <mqttmsg/mqttmsg.h>
#include <mqttmsg/transport_pico.h>

static void onCommand(MqttClient* mqtt, MqttString topic, MqttPayload body, void* userData) {
    // topic and body are views into the receive buffer; copy what you keep
    mqttPublish(mqtt, "device/status", "ok", false);
}

int main(void) {
    stdio_init_all();
    if (cyw43_arch_init()) {
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    MqttPicoTransport* wire = mqttPicoTransportNew();
    mqttPicoSetWifi(wire, WIFI_SSID, WIFI_PASSWORD);

    MqttClient* mqtt = mqttNew();
    mqttPicoTransportAttach(wire, mqtt);

    mqttSetBroker(mqtt, MQTT_HOST, MQTT_PORT);
    mqttSetKeepAlive(mqtt, 90);
    mqttSetWill(mqtt, "device/status", "offline", true);
    mqttSubscribe(mqtt, "device/+/command", onCommand, NULL);

    mqttRun(mqtt);   // mqttPoll() in a loop; does not return in normal use
}
```

**5. Configure, build and flash.** Credentials arrive as `-D` definitions
or in a `local.cmake`:

```sh
cmake -S . -B build -DWIFI_SSID=HomeNet -DWIFI_PASSWORD=secret \
      -DMQTT_HOST=192.168.1.10 -DMQTT_PORT=1883
cmake --build build
```

## Building and testing on the host

```sh
cmake -S . -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

That builds `mqttlog` and the test suite. Nothing lwIP-facing is compiled;
the host source list in `CMakeLists.txt` is the definition of what is
testable off-target.

Every host target compiles with `-Wall -Wextra -Wstrict-prototypes -Werror`.
Pass `-DMQTTMSG_WERROR=OFF` to keep the warnings but stop them failing the
build — useful on a compiler newer than the one this was last checked
against. The flags apply only when this is the top-level project: a consumer
that pulls the library in with `add_subdirectory()` gets none of them, since
a warning in our code is not something their build should die on.

### The Pico half

`transport_pico.c`, `keymanager.c` and `run_loop_manager.c` are in no host
build, so nothing above compiles them. To check them, build a consumer
against a working copy of the library with warnings turned on:

```sh
CFLAGS="-Wall -Wextra -Wstrict-prototypes" cmake -S <consumer> -B <consumer>/build
cmake --build <consumer>/build
```

Set them through the environment, not `-DCMAKE_C_FLAGS=...` — that overrides
the cache variable the Pico SDK appends its own required flags to, and the
build fails inside `spin_lock.h` long before it reaches any of this code.

### Leak-checked flavor

```sh
cmake -S . -B build-asan -DMQTTMSG_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Compiles and links every host target with AddressSanitizer and
LeakSanitizer. LSan reports anything still held at exit, and ASan turns a
framing mistake into a diagnosed heap overflow instead of a wrong answer
that happens to look plausible.

### Static analysis

```sh
cmake --build build-host --target lint
```

`gcc -fanalyzer -Werror` over the host sources, twice: once as they build,
and once with `MQTTMSG_DEBUG` and `MQTTMSG_TRACE` set to 1, which no other
build compiles. A finding in either pass fails the target, so new ones
surface rather than accumulate. Add new host-compiled sources to
`MQTTMSG_LINT_SOURCES` when you add them.

## API reference

```sh
doxygen              # needs doxygen installed; CMake knows nothing about it
```

Writes `docs/html/`, with this file as the front page. The `Doxyfile` keeps
`EXTRACT_ALL = NO` and `WARN_IF_UNDOCUMENTED = YES` on purpose: an
undocumented symbol is a warning rather than an empty entry nobody notices,
so **the run should be silent**. Anything it prints is a gap to fill.

Coverage is every public header, the internal headers under `src/`, and
the implementation files including their static functions.

## Symbol naming

The client API is mostly prefixed with `mqtt*` for the protocol and the
client, `mqttPico*` and `mqttSocket*` for the two transports, and
`mqttmsg*` for the odd corner like logging.

## Versioning

`include/mqttmsg/version.h` carries `MQTTMSG_VERSION_MAJOR`, `_MINOR` and
`_PATCH`, a `MQTTMSG_VERSION` integer that orders correctly, and a
`MQTTMSG_VERSION_STRING`. Consumers pin this library by submodule SHA, which
says nothing about what is in it, so a feature test can be written as an
ordinary preprocessor comparison:

```c
#include <mqttmsg/version.h>

#if MQTTMSG_VERSION >= MQTTMSG_VERSION_ENCODE(1, 0, 0)
  mqttSubscribe(mqtt, "device/cmd/#", onCommand, NULL);
#else
  addSubscription(mqtt, "device/cmd/#", onCommand);   /* the older client */
#endif
```

The header is the single source of truth. `CMakeLists.txt` parses the three
numbers back out of it.

## Test layout

| File | Covers |
|---|---|
| `test_mqtt.c` | Build/parse round-trips for every message type |
| `test_mqtt_frames.c` | Golden bytes — pins the wire format itself |
| `test_framing.c` | The receive-side framing loop: coalesced, split and malformed arrivals |
| `test_session.c` | The protocol state machine, driven against a fake transport |
| `test_client.c` | Whole sessions over the fake transport: connect, subscribe, dispatch, keep-alive timing, write failure — no broker, no socket, no waiting |
| `test_router.c` | The subscription table and topic-filter matching |
| `test_backoff.c` | Reconnect pacing |
| `test_transport_socket.c` | The socket transport against a closed port, so the reconnect path runs without a broker |
| `test_payload.c` | The `MqttPayload` contract: Ok/None/failure, and every entry point handed a failure |
| `test_payload_writer.c` | The internal frame writer's measure/write agreement |
| `test_mqtt_string.c`, `test_mqtt_string_cpp.cpp` | Zero-copy string views, from C and C++ |
| `test_message_auth.c` | `mqttCheckKey()` against golden vectors generated outside this code |
| `test_prog_args.c` | `mqttlog` argument parsing |
| `test_heap_stats.c` | Free-heap telemetry arithmetic |
| `test_version.c` | The version macros: encoding order, and the stringify indirection |
| `compile_device_api.c` | Compile-only, via the `lint` target: the public client headers must build with no lwIP or Pico SDK on the include path |

The split between the first two is deliberate: round-trip tests stay green
if the encoder and parser drift together, so the golden frames pin the format
independently.

## The SHA-256 submodule

The SHA-256 implementation is Brad Conte's public domain implementation, from
[B-Con/crypto-algorithms](https://github.com/B-Con/crypto-algorithms), pinned
as a submodule under `lib/crypto-algorithms`.

So clone with submodules:

```
git clone --recursive <url>
# or, in an existing clone:
git submodule update --init --recursive
```

## Logging

`logger.h` gates each level on a compile-time constant, so a disabled level
costs nothing and the compiler discards the call and its arguments.

| Macro | Default | Enables |
|---|---|---|
| `MQTTMSG_ERROR` | 1 | `mqttmsgErrorPrint` |
| `MQTTMSG_DEBUG` | 0 | `mqttmsgDebugPrint` |
| `MQTTMSG_TRACE` | 0 | `mqttmsgTracePrint`, `MQTTMSG_DUMP_BYTES`, `MQTTMSG_DUMP_PAYLOAD` |

Set them through the `LOG_DEBUG` and `LOG_TRACE` CMake variables, which this
library turns into the macros above.

## lwIP configuration

Every lwIP project supplies its own `lwipopts.h` — lwIP includes it by that
exact name; yours goes on your own include path.

Start from `lwipopts_examples_common.h` in the Pico SDK's
[pico-examples](https://github.com/raspberrypi/pico-examples) repository
(under `pico_w/wifi/`): copy it into your project as `include/lwipopts.h`
and edit from there. Those are the settings this library is developed and
tested against.

## Ownership conventions

The builders (`buildPublish`, `buildConnect`, …) return a `MqttPayload` the
**caller** releases, with `mqttPayloadFree()`. A payload handed *into* a
builder is copied, and the caller still owns that too:

```c
MqttPayload body = makeStringPayload("21.5");
MqttPayload frame = buildPublish("home/temp", 0, body, false);
mqttPayloadFree(&body);
if (mqttPayloadIsOk(frame)) {
    /* ... send frame ... */
}
mqttPayloadFree(&frame);
```

`parseMessage()` allocates nothing. It returns views into the buffer you
passed it, so it needs no free — but those views die when that buffer is
reused, which for the receive path is as soon as the callback returns.

## The `MqttPayload` contract

A `MqttPayload` is in one of three states, and its two fields are not the
way to tell which: use the predicates in `mqtt_payload.h`.

| | Means | Test with |
|---|---|---|
| **Ok** | Carries bytes; a length of zero still counts | `mqttPayloadIsOk()` |
| **None** | This message type has no body — PUBACK, PINGREQ, DISCONNECT | `mqttPayloadIsNone()` |
| **Failure** | Could not be built; ask `mqttPayloadReason()` why | `mqttPayloadIsFailure()` |

Read the bytes with `mqttPayloadLength()` and `mqttPayloadBytes()`, which
answer 0 and `NULL` for anything that is not Ok. Reading `.buffer` and
`.length` directly is outside the contract: a failure records its reason
where the length sits, so a direct read sees a four-billion-byte payload.

Nothing that takes a `MqttPayload` will build on, send or parse a failure — each
of those refuses and returns a failure of its own, so a build error stops at
the first step rather than turning into a corrupt frame further down.

## License

BSD 3-Clause — see [LICENSE](LICENSE).

One piece of the tree is not covered by it and carries its own terms,
recorded in [NOTICE](NOTICE): the `lib/crypto-algorithms` submodule is Brad
Conte's, public domain.
