# include/mqttmsg/version.h is the single source of truth for the version.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/mqttmsg/version.h" _mqttmsg_version_lines
  REGEX "^#define MQTTMSG_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(_part MAJOR MINOR PATCH)
  string(REGEX MATCH "#define MQTTMSG_VERSION_${_part} ([0-9]+)" _m "${_mqttmsg_version_lines}")
  if(NOT _m)
    message(FATAL_ERROR "Could not read MQTTMSG_VERSION_${_part} from include/mqttmsg/version.h")
  endif()
  set(_mqttmsg_${_part} ${CMAKE_MATCH_1})
endforeach()
set(MQTTMSG_VERSION "${_mqttmsg_MAJOR}.${_mqttmsg_MINOR}.${_mqttmsg_PATCH}")

# Standalone build (host tests, mqttlog). Consumers add this directory with
# add_subdirectory() and have already set both, so the block is skipped for
# them and their build is unaffected.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.13)
  project(mqttmsg VERSION ${MQTTMSG_VERSION} LANGUAGES C)
endif()

set(CMAKE_C_STANDARD 11)

# The SHA-256 implementation lives in a submodule, so a clone without
# --recursive has the directory and none of the files.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/crypto-algorithms/sha256.c")
  message(FATAL_ERROR
    "lib/crypto-algorithms is empty — the submodule has not been checked out.\n"
    "  Run: git submodule update --init --recursive")
endif()

add_library(mqttmsg INTERFACE)

IF (PICO)
  target_compile_definitions(mqttmsg INTERFACE
    MQTTMSG_DEBUG=${LOG_DEBUG}
    MQTTMSG_TRACE=${LOG_TRACE}
    # Selects the newlib heap probe in heap_stats.c.
    MQTTMSG_TARGET_PICO=1
    )

  target_link_libraries(mqttmsg INTERFACE
    pico_cyw43_arch_lwip_threadsafe_background
    pico_stdlib)

  target_sources(mqttmsg INTERFACE
    include/mqttmsg/heap_stats.h
    include/mqttmsg/keymanager.h
    include/mqttmsg/logger.h
    include/mqttmsg/mqtt.h
    include/mqttmsg/mqtt_message.hpp
    include/mqttmsg/mqtt_payload.h
    include/mqttmsg/mqtt_string.h
    include/mqttmsg/mqtt_string.hpp
    include/mqttmsg/mqtt_types.h
    include/mqttmsg/mqttmsg.h
    include/mqttmsg/run_loop_manager.h
    include/mqttmsg/transport_pico.h
    include/mqttmsg/version.h
    lib/crypto-algorithms/sha256.c
    src/mqttmsg/backoff.c
    src/mqttmsg/backoff.h
    src/mqttmsg/client.c
    src/mqttmsg/framing.c
    src/mqttmsg/framing.h
    src/mqttmsg/heap_stats.c
    src/mqttmsg/keymanager.c
    src/mqttmsg/message_auth.c
    src/mqttmsg/mqtt.c
    src/mqttmsg/mqtt_string.c
    src/mqttmsg/payload_writer.h
    src/mqttmsg/router.c
    src/mqttmsg/router.h
    src/mqttmsg/run_loop_manager.c
    src/mqttmsg/session.c
    src/mqttmsg/session.h
    src/mqttmsg/transport_pico.c
    )
ENDIF(PICO)

target_include_directories(mqttmsg INTERFACE include/ lib/)

IF (NOT PICO)

  # Leak-checked build flavor. Off by default so the ordinary build stays
  # fast.
  #
  #   cmake -S . -B build-asan -DMQTTMSG_SANITIZE=ON && ctest --test-dir build-asan
  #
  option(MQTTMSG_SANITIZE "Build host targets with AddressSanitizer and LeakSanitizer" OFF)
  if(MQTTMSG_SANITIZE)
    add_compile_options(-fsanitize=address,leak -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,leak)
  endif()

  # Host warnings, and only when this is the top-level project.
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(MQTTMSG_WERROR "Treat warnings as errors in the standalone build" ON)
    add_compile_options(-Wall -Wextra $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
    if(MQTTMSG_WERROR)
      add_compile_options(-Werror)
    endif()

    # Code we do not own is exempt. -w comes last, so it wins.
    set_source_files_properties(
      "${CMAKE_CURRENT_SOURCE_DIR}/lib/crypto-algorithms/sha256.c"
      PROPERTIES COMPILE_OPTIONS "-w")
  endif()

  target_sources(mqttmsg INTERFACE
    include/mqttmsg/heap_stats.h
    include/mqttmsg/keymanager.h
    include/mqttmsg/logger.h
    include/mqttmsg/mqtt.h
    include/mqttmsg/mqtt_message.hpp
    include/mqttmsg/mqtt_payload.h
    include/mqttmsg/mqtt_string.h
    include/mqttmsg/mqtt_string.hpp
    include/mqttmsg/mqtt_types.h
    include/mqttmsg/mqttmsg.h
    include/mqttmsg/transport_fake.h
    include/mqttmsg/transport_socket.h
    include/mqttmsg/version.h
    lib/crypto-algorithms/sha256.c
    src/mqttmsg/backoff.c
    src/mqttmsg/backoff.h
    src/mqttmsg/client.c
    src/mqttmsg/framing.c
    src/mqttmsg/framing.h
    src/mqttmsg/heap_stats.c
    src/mqttmsg/message_auth.c
    src/mqttmsg/mqtt.c
    src/mqttmsg/mqtt_string.c
    src/mqttmsg/payload_writer.h
    src/mqttmsg/router.c
    src/mqttmsg/router.h
    src/mqttmsg/session.c
    src/mqttmsg/session.h
    src/mqttmsg/transport_fake.c
    src/mqttmsg/transport_socket.c
    )

  add_executable(mqttlog)

  target_sources(mqttlog
  PUBLIC
    apps/mqttlog/mqttlog.c
    apps/mqttlog/prog_args.c
    apps/mqttlog/prog_args.h
  )

  target_include_directories(mqttlog
  PUBLIC include/
  )

  target_link_libraries(mqttlog mqttmsg)

  # Unity test runner
  enable_testing()

  add_library(unity STATIC tests/unity/unity.c)
  target_include_directories(unity PUBLIC tests/unity)
  # Code we do not own is exempt, as above.
  target_compile_options(unity PRIVATE -w)

  add_executable(test_mqtt tests/test_mqtt.c)
  target_include_directories(test_mqtt PRIVATE include/)
  target_link_libraries(test_mqtt mqttmsg unity)
  add_test(NAME mqtt COMMAND test_mqtt)

  # Static analysis gate.
  #
  # Run with: cmake --build <dir> --target lint
  set(MQTTMSG_LINT_SOURCES
    apps/mqttlog/mqttlog.c
    apps/mqttlog/prog_args.c
    include/mqttmsg/mqttmsg.h
    include/mqttmsg/transport_fake.h
    include/mqttmsg/transport_socket.h
    src/mqttmsg/backoff.c
    src/mqttmsg/client.c
    src/mqttmsg/framing.c
    src/mqttmsg/heap_stats.c
    src/mqttmsg/message_auth.c
    src/mqttmsg/mqtt.c
    src/mqttmsg/mqtt_string.c
    src/mqttmsg/router.c
    src/mqttmsg/session.c
    src/mqttmsg/transport_fake.c
    src/mqttmsg/transport_socket.c
    tests/compile_device_api.c)

  set(MQTTMSG_LINT_FLAGS
    -fanalyzer -Wall -Wextra -Wstrict-prototypes -Werror
    -std=gnu11
    -I${CMAKE_CURRENT_SOURCE_DIR}/include
    -I${CMAKE_CURRENT_SOURCE_DIR}/lib
    -I${CMAKE_CURRENT_SOURCE_DIR}/src
    # apps/ reach the internal headers by name, the way the library's own
    # sources do.
    -I${CMAKE_CURRENT_SOURCE_DIR}/src/mqttmsg)

  set(MQTTMSG_LINT_COMMANDS)
  foreach(_src ${MQTTMSG_LINT_SOURCES})
    list(APPEND MQTTMSG_LINT_COMMANDS
      COMMAND ${CMAKE_C_COMPILER} ${MQTTMSG_LINT_FLAGS}
              -c ${CMAKE_CURRENT_SOURCE_DIR}/${_src} -o ${CMAKE_BINARY_DIR}/lint.o)
  endforeach()

  # Again with logging compiled in, which no other build does.
  foreach(_src ${MQTTMSG_LINT_SOURCES})
    list(APPEND MQTTMSG_LINT_COMMANDS
      COMMAND ${CMAKE_C_COMPILER} ${MQTTMSG_LINT_FLAGS}
              -DMQTTMSG_DEBUG=1 -DMQTTMSG_TRACE=1
              -c ${CMAKE_CURRENT_SOURCE_DIR}/${_src} -o ${CMAKE_BINARY_DIR}/lint.o)
  endforeach()

  add_custom_target(lint
    ${MQTTMSG_LINT_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/lint.o
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running static analysis (gcc -fanalyzer, logging off and on)"
    VERBATIM)

  # Formatting target, matching the project convention. The exclusions are
  # the ones in .clang-format-ignore, repeated here because the glob is what
  # actually feeds the command; unity is vendored and reviewed by diffing
  # against upstream.
  find_program(CLANG_FORMAT clang-format)
  if(CLANG_FORMAT)
    file(GLOB_RECURSE MQTTMSG_FORMAT_SOURCES
      ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
      ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
      ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/apps/*.c
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    list(FILTER MQTTMSG_FORMAT_SOURCES EXCLUDE REGEX "/(tests/unity|lib)/")

    add_custom_target(format
      COMMAND ${CLANG_FORMAT} -i ${MQTTMSG_FORMAT_SOURCES}
      COMMENT "Running clang-format")
  endif()

  # Wraps malloc so the out-of-memory paths in the encoders can be reached;
  # without it those branches are only ever taken on a device already in
  # trouble, which is where they matter and where nobody is watching.
  add_executable(test_payload tests/test_payload.c)
  target_include_directories(test_payload PRIVATE include/)
  target_link_libraries(test_payload mqttmsg unity)
  target_link_options(test_payload PRIVATE -Wl,--wrap=malloc)
  add_test(NAME payload COMMAND test_payload)

  # Needs src/ on the include path: payload_writer.h is internal and
  # deliberately not shipped in include/.
  add_executable(test_payload_writer tests/test_payload_writer.c)
  target_include_directories(test_payload_writer PRIVATE include/ src/)
  target_link_libraries(test_payload_writer mqttmsg unity)
  add_test(NAME payload_writer COMMAND test_payload_writer)

  # src/ on the include path for the same reason as test_payload_writer:
  # framing.h is internal and not shipped in include/.
  add_executable(test_framing tests/test_framing.c)
  target_include_directories(test_framing PRIVATE include/ src/mqttmsg/)
  target_link_libraries(test_framing mqttmsg unity)
  add_test(NAME framing COMMAND test_framing)

  # src/ on the include path for the same reason as test_framing: the session
  # layer is internal and not shipped in include/.
  add_executable(test_session tests/test_session.c)
  target_include_directories(test_session PRIVATE include/ src/mqttmsg/)
  target_link_libraries(test_session mqttmsg unity)
  add_test(NAME session COMMAND test_session)

  add_executable(test_client tests/test_client.c)
  target_include_directories(test_client PRIVATE include/)
  target_link_libraries(test_client mqttmsg unity)
  add_test(NAME client COMMAND test_client)

  # src/ on the include path for the same reason as test_framing: router.h is
  # internal and not shipped in include/.
  add_executable(test_router tests/test_router.c)
  target_include_directories(test_router PRIVATE include/ src/mqttmsg/)
  target_link_libraries(test_router mqttmsg unity)
  add_test(NAME router COMMAND test_router)

  # The socket transport, driven against a closed port so the reconnect path
  # runs without a broker.
  add_executable(test_transport_socket tests/test_transport_socket.c)
  target_include_directories(test_transport_socket PRIVATE include/)
  target_link_libraries(test_transport_socket mqttmsg unity)
  add_test(NAME transport_socket COMMAND test_transport_socket)

  add_executable(test_backoff tests/test_backoff.c)
  target_include_directories(test_backoff PRIVATE include/ src/mqttmsg/)
  target_link_libraries(test_backoff mqttmsg unity)
  add_test(NAME backoff COMMAND test_backoff)

  add_executable(test_mqtt_frames tests/test_mqtt_frames.c)
  target_include_directories(test_mqtt_frames PRIVATE include/)
  target_link_libraries(test_mqtt_frames mqttmsg unity)
  add_test(NAME mqtt_frames COMMAND test_mqtt_frames)

  add_executable(test_message_auth tests/test_message_auth.c)
  target_include_directories(test_message_auth PRIVATE include/)
  target_link_libraries(test_message_auth mqttmsg unity)
  add_test(NAME message_auth COMMAND test_message_auth)

  add_executable(test_mqtt_string tests/test_mqtt_string.c)
  target_include_directories(test_mqtt_string PRIVATE include/)
  target_link_libraries(test_mqtt_string mqttmsg unity)
  add_test(NAME mqtt_string COMMAND test_mqtt_string)

  # C++ interop for mqtt_string.hpp. Enabling C++ only here keeps the library
  # itself a C project; the header is opt-in and consumers are C++20.
  enable_language(CXX)
  add_executable(test_mqtt_string_cpp tests/test_mqtt_string_cpp.cpp)
  set_target_properties(test_mqtt_string_cpp PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)
  target_include_directories(test_mqtt_string_cpp PRIVATE include/ tests/)
  target_link_libraries(test_mqtt_string_cpp mqttmsg unity)
  add_test(NAME mqtt_string_cpp COMMAND test_mqtt_string_cpp)

  add_executable(test_heap_stats tests/test_heap_stats.c)
  target_include_directories(test_heap_stats PRIVATE include/)
  target_link_libraries(test_heap_stats mqttmsg unity)
  add_test(NAME heap_stats COMMAND test_heap_stats)

  add_executable(test_version tests/test_version.c)
  target_include_directories(test_version PRIVATE include/)
  target_link_libraries(test_version mqttmsg unity)
  add_test(NAME version COMMAND test_version)

  # The parser under test lives with mqttlog now, so the test compiles it
  # directly rather than getting it from the library.
  add_executable(test_prog_args tests/test_prog_args.c apps/mqttlog/prog_args.c)
  target_include_directories(test_prog_args PRIVATE include/ apps/mqttlog/)
  target_link_libraries(test_prog_args mqttmsg unity)
  add_test(NAME prog_args COMMAND test_prog_args)

ENDIF ()
