cmake_minimum_required(VERSION 3.16)
project(TimoxVAudioEngine)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(VASIO_ENABLE_CRASH_DUMP "Write a local crash minidump" OFF)
enable_testing()
include(cmake/PwarClient.cmake)
string(LENGTH "${TIMOX_PWAR_CLIENT_SHA256}" pwar_hash_length)
if(NOT pwar_hash_length EQUAL 64 OR NOT TIMOX_PWAR_CLIENT_SHA256 MATCHES "^[0-9A-F]+$")
    message(FATAL_ERROR "PWAR client pin must be a 64-character uppercase SHA-256")
endif()
set_source_files_properties(src/pwar_client_supervisor.cpp tests/pwar_client_supervisor_protocol_tests.cpp
    PROPERTIES COMPILE_DEFINITIONS TIMOX_PWAR_CLIENT_SHA256="${TIMOX_PWAR_CLIENT_SHA256}")
if(MSVC)
    add_compile_options(/EHsc)
endif()
add_executable(PwarClientSupervisorProtocolTests tests/pwar_client_supervisor_protocol_tests.cpp)
target_include_directories(PwarClientSupervisorProtocolTests PRIVATE include vendor)
target_link_libraries(PwarClientSupervisorProtocolTests PRIVATE bcrypt)
target_compile_options(PwarClientSupervisorProtocolTests PRIVATE /EHsc)
add_test(NAME PwarClientSupervisorProtocolTests COMMAND PwarClientSupervisorProtocolTests)
set(ASIO_SDK_PATH "${CMAKE_SOURCE_DIR}/asiosdk" CACHE PATH "Chemin vers ASIO SDK")
set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")

# Headers ASIO SDK
if(NOT EXISTS "${ASIO_SDK_PATH}/common/asio.h")
    message(WARNING "ASIO SDK non trouve a ${ASIO_SDK_PATH}")
    message(FATAL_ERROR "Telechargez ASIO SDK depuis https://www.steinberg.net/asiosdk")
endif()

include_directories(${INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/include ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src)
include_directories(${ASIO_SDK_PATH}/common)
include_directories(${ASIO_SDK_PATH}/host)
include_directories(${ASIO_SDK_PATH}/host/pc)

# Sources
set(SOURCES
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/control_api_server.cpp
    ${SRC_DIR}/pwar_client_supervisor.cpp
    ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src/audio_transport.cpp
    ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src/audio_transport_status_reader.cpp
    ${SRC_DIR}/physical_asio_host.cpp
    ${SRC_DIR}/vasio_client_manager.cpp
    ${SRC_DIR}/asio_sample_conversion.cpp
    ${SRC_DIR}/audio_routing_runtime.cpp
    ${SRC_DIR}/routing_graph.cpp
    ${SRC_DIR}/audio_controller.cpp
    ${SRC_DIR}/audio_configuration_store.cpp
    ${SRC_DIR}/application_profile_store.cpp
    ${SRC_DIR}/application_profile_store_write.cpp
    ${SRC_DIR}/engine_diagnostics.cpp
    ${SRC_DIR}/engine_version.rc
)

# Windows-specific
if(WIN32)

    set(SOURCES ${SOURCES}
        ${ASIO_SDK_PATH}/host/pc/asiolist.cpp
        ${SRC_DIR}/native_audio_installed_transport_win32.cpp
        ${SRC_DIR}/native_audio_broker_transport.cpp
        ${SRC_DIR}/native_audio_socket_channel_win32.cpp
        ${CMAKE_SOURCE_DIR}/native-windows-audio/broker/unix_socket_stream_win32.cpp
        ${CMAKE_SOURCE_DIR}/native-windows-audio/broker/channel_binding.cpp
        ${CMAKE_SOURCE_DIR}/native-windows-audio/broker/pipe_identity_win32.cpp
        ${CMAKE_SOURCE_DIR}/native-windows-audio/broker/schannel_stream_win32.cpp
        ${CMAKE_SOURCE_DIR}/native-windows-audio/broker/socket_discovery.cpp
        ${CMAKE_SOURCE_DIR}/native-windows-audio/broker/socket_discovery_win32.cpp
        ${CMAKE_SOURCE_DIR}/native-windows-audio/broker/socket_discovery_root_win32.cpp
    )

    add_executable(TimoxVirtualAsioEngine ${SOURCES})
    if(MSVC)
        target_compile_options(TimoxVirtualAsioEngine PRIVATE /EHsc)
    endif()
    add_executable(EngineInstanceGuardTests tests/engine_instance_guard_tests.cpp)
    target_include_directories(EngineInstanceGuardTests PRIVATE ${SRC_DIR})
    add_executable(PwarConfigurationStoreTests tests/pwar_configuration_store_tests.cpp)
    add_executable(AsioErrorTests tests/asio_error_tests.cpp)
    target_include_directories(AsioErrorTests PRIVATE ${SRC_DIR})
    target_include_directories(PwarConfigurationStoreTests PRIVATE ${SRC_DIR})

    add_executable(SoftwareClockHostTests
        tests/software_clock_host_tests.cpp
        src/software_clock_host.cpp)
    target_include_directories(SoftwareClockHostTests PRIVATE ${INCLUDE_DIR})
    target_link_libraries(SoftwareClockHostTests PRIVATE avrt)

    add_executable(WasapiDeviceEnumeratorTests
        tests/wasapi_device_enumerator_tests.cpp
        src/wasapi_device_enumerator.cpp)
    target_include_directories(WasapiDeviceEnumeratorTests PRIVATE ${INCLUDE_DIR})
    target_link_libraries(WasapiDeviceEnumeratorTests PRIVATE ole32 propsys uuid)
    add_executable(WasapiDeviceNotificationsTests tests/wasapi_device_notifications_tests.cpp
        src/wasapi_device_notifications.cpp)
    target_include_directories(WasapiDeviceNotificationsTests PRIVATE ${INCLUDE_DIR})
    target_link_libraries(WasapiDeviceNotificationsTests PRIVATE ole32 uuid)
    if(MSVC)
        target_compile_options(WasapiDeviceNotificationsTests PRIVATE /W4 /WX /EHsc)
    endif()

    add_executable(WasapiBackendTests tests/wasapi_backend_tests.cpp
        src/wasapi_backend.cpp src/wasapi_device_enumerator.cpp)
    target_include_directories(WasapiBackendTests PRIVATE ${INCLUDE_DIR})
    target_compile_definitions(WasapiBackendTests PRIVATE NOMINMAX)
    target_link_libraries(WasapiBackendTests PRIVATE mmdevapi ole32 propsys uuid)

    add_executable(WasapiBridgeTests tests/wasapi_bridge_tests.cpp
        src/wasapi_bridge.cpp src/wasapi_sample_conversion.cpp src/drift_resampler.cpp
        src/wasapi_backend.cpp src/wasapi_device_enumerator.cpp)
    target_include_directories(WasapiBridgeTests PRIVATE ${INCLUDE_DIR} ${SRC_DIR})
    target_compile_definitions(WasapiBridgeTests PRIVATE NOMINMAX)
    target_link_libraries(WasapiBridgeTests PRIVATE avrt mmdevapi ole32 propsys uuid)

    add_executable(WasapiBridgeIoTests tests/wasapi_bridge_io_tests.cpp)
    target_include_directories(WasapiBridgeIoTests PRIVATE ${INCLUDE_DIR} ${SRC_DIR})

    add_executable(WasapiSampleConversionTests tests/wasapi_sample_conversion_tests.cpp src/wasapi_sample_conversion.cpp)
    target_include_directories(WasapiSampleConversionTests PRIVATE ${INCLUDE_DIR})

    add_executable(SpscAudioRingTests tests/spsc_audio_ring_tests.cpp)
    target_include_directories(SpscAudioRingTests PRIVATE ${SRC_DIR})

    add_executable(DriftResamplerTests tests/drift_resampler_tests.cpp src/drift_resampler.cpp)
    target_include_directories(DriftResamplerTests PRIVATE ${SRC_DIR})

    add_executable(SoftwareClockRoutingTests
        tests/software_clock_routing_tests.cpp
        src/software_clock_host.cpp
        src/audio_routing_runtime.cpp
        src/routing_graph.cpp
        vendor/timoxvasio/src/audio_transport.cpp)
    target_include_directories(SoftwareClockRoutingTests PRIVATE ${INCLUDE_DIR} ${SRC_DIR})
    target_link_libraries(SoftwareClockRoutingTests PRIVATE avrt ole32)

    target_include_directories(TimoxVirtualAsioEngine PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib
        ${CMAKE_SOURCE_DIR}/vendor
    )
    target_compile_definitions(TimoxVirtualAsioEngine PRIVATE CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH=1048576)

    if(VASIO_ENABLE_CRASH_DUMP)
        target_compile_definitions(TimoxVirtualAsioEngine PRIVATE VASIO_ENABLE_CRASH_DUMP=1)
    endif()

    target_link_libraries(TimoxVirtualAsioEngine PRIVATE
        ole32
        oleaut32
        mmdevapi
        winmm
        user32
        ws2_32
        secur32
        crypt32
        ncrypt
        bcrypt
        advapi32
        ntdll
        shell32
    )
    target_include_directories(TimoxVirtualAsioEngine PRIVATE
        ${CMAKE_SOURCE_DIR}/native-windows-audio/include
        ${CMAKE_SOURCE_DIR}/native-windows-audio/broker
    )
    target_compile_definitions(TimoxVirtualAsioEngine PRIVATE SECURITY_WIN32)
    if(VASIO_ENABLE_CRASH_DUMP)
        target_link_libraries(TimoxVirtualAsioEngine PRIVATE dbghelp)
    endif()

    add_executable(AsioBufferHealthTests tests/asio_buffer_health_tests.cpp)
    target_include_directories(AsioBufferHealthTests PRIVATE ${INCLUDE_DIR})

    add_executable(AsioSampleConversionTests
        ${CMAKE_SOURCE_DIR}/tests/asio_sample_conversion_tests.cpp
        ${SRC_DIR}/asio_sample_conversion.cpp
    )
    target_include_directories(AsioSampleConversionTests PRIVATE
        ${INCLUDE_DIR}
        ${ASIO_SDK_PATH}/common
    )

    add_executable(AsioSampleConversionBlockTests
        ${CMAKE_SOURCE_DIR}/tests/asio_sample_conversion_block_tests.cpp
        ${SRC_DIR}/asio_sample_conversion.cpp
    )
    target_include_directories(AsioSampleConversionBlockTests PRIVATE
        ${INCLUDE_DIR}
        ${ASIO_SDK_PATH}/common
    )

    add_executable(RoutingGraphTests
        ${CMAKE_SOURCE_DIR}/tests/routing_graph_tests.cpp
        ${SRC_DIR}/routing_graph.cpp
    )
    target_include_directories(RoutingGraphTests PRIVATE ${INCLUDE_DIR})

    add_executable(RoutingGraphBenchmark
        ${CMAKE_SOURCE_DIR}/tests/routing_graph_benchmark.cpp
        ${SRC_DIR}/routing_graph.cpp
    )
    target_include_directories(RoutingGraphBenchmark PRIVATE ${INCLUDE_DIR})

    add_executable(AudioRoutingRuntimeTests
        ${CMAKE_SOURCE_DIR}/tests/audio_routing_runtime_tests.cpp
        ${SRC_DIR}/audio_routing_runtime.cpp
        ${SRC_DIR}/routing_graph.cpp
        ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src/audio_transport.cpp
    )
    target_include_directories(AudioRoutingRuntimeTests PRIVATE ${INCLUDE_DIR})
    target_link_libraries(AudioRoutingRuntimeTests PRIVATE ole32)

    add_executable(WasapiRoutingRuntimeTests tests/wasapi_routing_runtime_tests.cpp
        src/audio_routing_runtime.cpp src/routing_graph.cpp vendor/timoxvasio/src/audio_transport.cpp)
    target_include_directories(WasapiRoutingRuntimeTests PRIVATE ${INCLUDE_DIR} ${SRC_DIR})
    target_link_libraries(WasapiRoutingRuntimeTests PRIVATE ole32)

    add_executable(AudioPathBenchmark
        tests/audio_path_benchmark.cpp
        ${SRC_DIR}/audio_routing_runtime.cpp
        ${SRC_DIR}/routing_graph.cpp
        ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src/audio_transport.cpp
        ${SRC_DIR}/asio_sample_conversion.cpp)
    target_include_directories(AudioPathBenchmark PRIVATE ${INCLUDE_DIR} ${ASIO_SDK_PATH}/common)
    target_link_libraries(AudioPathBenchmark PRIVATE ole32)

    add_executable(PerformanceStageMeasurementsTests
        tests/performance_stage_measurements_tests.cpp)
    target_include_directories(PerformanceStageMeasurementsTests PRIVATE ${INCLUDE_DIR})

    add_executable(RealtimeCallbackSlotTests tests/realtime_callback_slot_tests.cpp)
    target_include_directories(RealtimeCallbackSlotTests PRIVATE ${INCLUDE_DIR})

    add_executable(AudioRoutingHotSwapTests
        tests/audio_routing_hot_swap_tests.cpp
        ${SRC_DIR}/audio_routing_runtime.cpp
        ${SRC_DIR}/routing_graph.cpp
        ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src/audio_transport.cpp)
    target_include_directories(AudioRoutingHotSwapTests PRIVATE ${INCLUDE_DIR})
    target_link_libraries(AudioRoutingHotSwapTests PRIVATE ole32)

    add_executable(AudioConfigurationStoreTests
        ${CMAKE_SOURCE_DIR}/tests/audio_configuration_store_tests.cpp
        ${SRC_DIR}/audio_configuration_store.cpp
    )
    target_include_directories(AudioConfigurationStoreTests PRIVATE
        ${INCLUDE_DIR}
        ${SRC_DIR}
        ${CMAKE_SOURCE_DIR}/vendor
    )

    add_executable(PersistentRoutesTests
        ${CMAKE_SOURCE_DIR}/tests/persistent_routes_tests.cpp
        ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src/audio_transport.cpp
    )
    target_include_directories(PersistentRoutesTests PRIVATE ${INCLUDE_DIR} ${SRC_DIR})
    target_link_libraries(PersistentRoutesTests PRIVATE ole32)

    add_executable(EngineDiagnosticsTests
        ${CMAKE_SOURCE_DIR}/tests/engine_diagnostics_tests.cpp
        ${SRC_DIR}/engine_diagnostics.cpp
    )
    target_include_directories(EngineDiagnosticsTests PRIVATE
        ${INCLUDE_DIR}
        ${SRC_DIR}
        ${CMAKE_SOURCE_DIR}/vendor
    )

    add_executable(ApplicationProfileStoreTests
        ${CMAKE_SOURCE_DIR}/tests/application_profile_store_tests.cpp
        ${SRC_DIR}/application_profile_store.cpp
        ${SRC_DIR}/application_profile_store_write.cpp
    )
    target_include_directories(ApplicationProfileStoreTests PRIVATE
        ${INCLUDE_DIR}
        ${SRC_DIR}
        ${CMAKE_SOURCE_DIR}/vendor
    )

    add_executable(AudioControllerPersistenceTests
        ${CMAKE_SOURCE_DIR}/tests/audio_controller_persistence_tests.cpp
        ${SRC_DIR}/audio_controller.cpp
        ${SRC_DIR}/audio_configuration_store.cpp
        ${SRC_DIR}/application_profile_store.cpp
        ${SRC_DIR}/application_profile_store_write.cpp
        ${SRC_DIR}/physical_asio_host.cpp
        ${SRC_DIR}/asio_sample_conversion.cpp
        ${SRC_DIR}/audio_routing_runtime.cpp
        ${SRC_DIR}/routing_graph.cpp
        ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src/audio_transport.cpp
        ${SRC_DIR}/vasio_client_manager.cpp
        ${ASIO_SDK_PATH}/host/pc/asiolist.cpp
    )
    target_include_directories(AudioControllerPersistenceTests PRIVATE
        ${INCLUDE_DIR}
        ${SRC_DIR}
        ${ASIO_SDK_PATH}/common
        ${ASIO_SDK_PATH}/host
        ${ASIO_SDK_PATH}/host/pc
        ${CMAKE_SOURCE_DIR}/vendor
    )
    target_link_libraries(AudioControllerPersistenceTests PRIVATE
        ole32 oleaut32 winmm user32
    )

    add_executable(AudioControllerClockTests
        ${CMAKE_SOURCE_DIR}/tests/audio_controller_clock_tests.cpp
        ${SRC_DIR}/audio_controller.cpp
        ${SRC_DIR}/audio_configuration_store.cpp
        ${SRC_DIR}/application_profile_store.cpp
        ${SRC_DIR}/application_profile_store_write.cpp
        ${SRC_DIR}/physical_asio_host.cpp
        ${SRC_DIR}/asio_sample_conversion.cpp
        ${SRC_DIR}/audio_routing_runtime.cpp
        ${SRC_DIR}/routing_graph.cpp
        ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src/audio_transport.cpp
        ${SRC_DIR}/vasio_client_manager.cpp
        ${ASIO_SDK_PATH}/host/pc/asiolist.cpp
    )
    target_include_directories(AudioControllerClockTests PRIVATE
        ${INCLUDE_DIR}
        ${SRC_DIR}
        ${ASIO_SDK_PATH}/common
        ${ASIO_SDK_PATH}/host
        ${ASIO_SDK_PATH}/host/pc
        ${CMAKE_SOURCE_DIR}/vendor
    )
    target_link_libraries(AudioControllerClockTests PRIVATE
        ole32 oleaut32 winmm user32
    )

    add_executable(ApplicationProfilesApiTests
        ${CMAKE_SOURCE_DIR}/tests/application_profiles_api_tests.cpp
        ${SRC_DIR}/control_api_server.cpp
        ${SRC_DIR}/engine_diagnostics.cpp
        ${SRC_DIR}/audio_controller.cpp
        ${SRC_DIR}/audio_configuration_store.cpp
        ${SRC_DIR}/application_profile_store.cpp
        ${SRC_DIR}/application_profile_store_write.cpp
        ${SRC_DIR}/physical_asio_host.cpp
        ${SRC_DIR}/asio_sample_conversion.cpp
        ${SRC_DIR}/audio_routing_runtime.cpp
        ${SRC_DIR}/routing_graph.cpp
        ${CMAKE_SOURCE_DIR}/vendor/timoxvasio/src/audio_transport.cpp
        ${SRC_DIR}/vasio_client_manager.cpp
        ${ASIO_SDK_PATH}/host/pc/asiolist.cpp
    )
    target_include_directories(ApplicationProfilesApiTests PRIVATE
        ${INCLUDE_DIR}
        ${SRC_DIR}
        ${ASIO_SDK_PATH}/common
        ${ASIO_SDK_PATH}/host
        ${ASIO_SDK_PATH}/host/pc
        ${CMAKE_SOURCE_DIR}/vendor
        ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib
    )
    target_compile_definitions(ApplicationProfilesApiTests PRIVATE CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH=1048576)
    target_link_libraries(ApplicationProfilesApiTests PRIVATE
        ole32 oleaut32 winmm user32 ws2_32
    )

    get_target_property(controller_test_sources AudioControllerClockTests SOURCES)
    list(REMOVE_ITEM controller_test_sources "${CMAKE_SOURCE_DIR}/tests/audio_controller_clock_tests.cpp")
    add_executable(AudioControllerWasapiTests tests/audio_controller_wasapi_tests.cpp ${controller_test_sources})
    target_include_directories(AudioControllerWasapiTests PRIVATE ${INCLUDE_DIR} ${SRC_DIR} ${CMAKE_SOURCE_DIR}/vendor)
    target_link_libraries(AudioControllerWasapiTests PRIVATE ole32 oleaut32 winmm user32)
    add_executable(AudioControllerNativeTests tests/audio_controller_native_tests.cpp ${controller_test_sources})
    target_include_directories(AudioControllerNativeTests PRIVATE ${INCLUDE_DIR} ${SRC_DIR} ${CMAKE_SOURCE_DIR}/vendor)
    target_link_libraries(AudioControllerNativeTests PRIVATE ole32 oleaut32 winmm user32)

    get_target_property(api_test_sources ApplicationProfilesApiTests SOURCES)
    list(REMOVE_ITEM api_test_sources "${CMAKE_SOURCE_DIR}/tests/application_profiles_api_tests.cpp")
    add_executable(WasapiBridgeApiTests tests/wasapi_bridge_api_tests.cpp ${api_test_sources})
    target_include_directories(WasapiBridgeApiTests PRIVATE ${INCLUDE_DIR} ${SRC_DIR}
        ${CMAKE_SOURCE_DIR}/vendor ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib)
    target_compile_definitions(WasapiBridgeApiTests PRIVATE CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH=1048576)
    target_link_libraries(WasapiBridgeApiTests PRIVATE ole32 oleaut32 winmm user32 ws2_32)


    foreach(clock_consumer TimoxVirtualAsioEngine AudioControllerClockTests
            AudioControllerPersistenceTests ApplicationProfilesApiTests AudioControllerWasapiTests WasapiBridgeApiTests AudioControllerNativeTests)
        target_sources(${clock_consumer} PRIVATE src/software_clock_host.cpp
            src/wasapi_bridge_manager.cpp src/wasapi_bridge.cpp src/wasapi_backend.cpp
            src/wasapi_device_enumerator.cpp src/wasapi_sample_conversion.cpp src/drift_resampler.cpp
            src/wasapi_device_notifications.cpp
            src/native_audio_manager.cpp src/native_audio_backend.cpp)
        target_include_directories(${clock_consumer} PRIVATE ${CMAKE_SOURCE_DIR}/native-windows-audio/include)
        target_compile_definitions(${clock_consumer} PRIVATE NOMINMAX)
        target_link_libraries(${clock_consumer} PRIVATE avrt mmdevapi propsys uuid)
    endforeach()
    foreach(store_consumer TimoxVirtualAsioEngine AudioConfigurationStoreTests
            AudioControllerClockTests AudioControllerPersistenceTests ApplicationProfilesApiTests
            AudioControllerWasapiTests WasapiBridgeApiTests AudioControllerNativeTests)
        target_sources(${store_consumer} PRIVATE src/wasapi_bridge_configuration.cpp)
        target_include_directories(${store_consumer} PRIVATE ${SRC_DIR})
    endforeach()
    foreach(runtime_consumer TimoxVirtualAsioEngine AudioControllerClockTests
            AudioControllerPersistenceTests ApplicationProfilesApiTests
            AudioRoutingRuntimeTests AudioRoutingHotSwapTests AudioPathBenchmark
            SoftwareClockRoutingTests)
        target_include_directories(${runtime_consumer} PRIVATE ${SRC_DIR})
    endforeach()
else()
    message(FATAL_ERROR "Ce projet necessite Windows et Visual Studio")
endif()

add_custom_command(TARGET TimoxVirtualAsioEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/api/openapi-v1.json
    $<TARGET_FILE_DIR:TimoxVirtualAsioEngine>/openapi-v1.json
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/api/schemas
    $<TARGET_FILE_DIR:TimoxVirtualAsioEngine>/schemas
    COMMENT "Copie du contrat OpenAPI et des schémas..."
)
add_custom_command(TARGET ApplicationProfilesApiTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/api/openapi-v1.json
    $<TARGET_FILE_DIR:ApplicationProfilesApiTests>/openapi-v1.json
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/api/schemas
    $<TARGET_FILE_DIR:ApplicationProfilesApiTests>/schemas
    COMMENT "Copie du contrat OpenAPI pour le test API..."
)


target_sources(ApplicationProfilesApiTests PRIVATE src/pwar_client_supervisor.cpp vendor/timoxvasio/src/audio_transport_status_reader.cpp)
target_sources(WasapiBridgeApiTests PRIVATE src/pwar_client_supervisor.cpp vendor/timoxvasio/src/audio_transport_status_reader.cpp)
target_link_libraries(ApplicationProfilesApiTests PRIVATE bcrypt)
target_link_libraries(WasapiBridgeApiTests PRIVATE bcrypt)

foreach(test AudioControllerPersistenceTests AudioControllerClockTests AudioControllerWasapiTests
        AudioControllerNativeTests WasapiBridgeTests SoftwareClockRoutingTests
        ApplicationProfilesApiTests WasapiBridgeApiTests)
    add_test(NAME ${test} COMMAND ${test})
    set_tests_properties(${test} PROPERTIES TIMEOUT 90)
endforeach()

foreach(test EngineInstanceGuardTests PwarConfigurationStoreTests AsioErrorTests SoftwareClockHostTests WasapiBridgeIoTests WasapiSampleConversionTests SpscAudioRingTests DriftResamplerTests AsioBufferHealthTests AsioSampleConversionTests AsioSampleConversionBlockTests RoutingGraphTests AudioRoutingRuntimeTests WasapiRoutingRuntimeTests PerformanceStageMeasurementsTests RealtimeCallbackSlotTests AudioRoutingHotSwapTests AudioConfigurationStoreTests PersistentRoutesTests EngineDiagnosticsTests ApplicationProfileStoreTests)
  add_test(NAME ${test} COMMAND ${test})
  set_tests_properties(${test} PROPERTIES TIMEOUT 60)
endforeach()


