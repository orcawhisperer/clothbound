# Unit, QML smoke tests, and C++ micro-benchmark targets.
# Kept in tests/ so Qt6::Test sets QT_TESTCASE_SOURCEDIR to tests/ cleanly.

if(CLOTHBOUND_BUILD_TESTS)
    add_executable(clothbound_tests
        clothbound_tests.cpp
        tst_online_ingest.cpp
        tst_opds.cpp
        fakeserver.h
        testbooks.h
        "${CMAKE_SOURCE_DIR}/src/prompts.qrc"
    )

    qt_add_resources(clothbound_tests "clothbound_tests_i18n_fallback"
        PREFIX "/i18n"
        BASE "${CMAKE_SOURCE_DIR}/src/i18n"
        FILES "${CMAKE_SOURCE_DIR}/src/i18n/en.json"
    )

    set_target_properties(clothbound_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(clothbound_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(clothbound_tests PRIVATE
        CLOTHBOUND_UNIFIED_TESTS
        QT_FORCE_ASSERTS
    )
    target_link_libraries(clothbound_tests
        PRIVATE
            clothbound_app
            Qt6::Test
    )

    add_test(NAME clothbound_tests COMMAND clothbound_tests)
    set_tests_properties(clothbound_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=generic"
        TIMEOUT 180
    )

    add_executable(tst_concurrency
        tst_concurrency.cpp
        fakeserver.h
        testbooks.h
    )
    set_target_properties(tst_concurrency PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(tst_concurrency PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(tst_concurrency PRIVATE QT_FORCE_ASSERTS)
    target_link_libraries(tst_concurrency
        PRIVATE
            clothbound_app
            Qt6::Test
    )
    add_test(NAME tst_concurrency COMMAND tst_concurrency)
    set(_tsan_test_env
        "QT_QPA_PLATFORM=offscreen"
        "QT_QPA_PLATFORMTHEME=generic"
    )
    if(CLOTHBOUND_SANITIZER STREQUAL "tsan")
        list(APPEND _tsan_test_env
            "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp:ignore_noninstrumented_modules=1:report_thread_leaks=0:halt_on_error=1"
        )
    endif()
    set_tests_properties(tst_concurrency PROPERTIES
        ENVIRONMENT "${_tsan_test_env}"
        TIMEOUT 180
    )

    add_executable(tst_webengine_e2e
        tst_webengine_e2e.cpp
        fakeserver.h
        testbooks.h
        "${CMAKE_SOURCE_DIR}/src/resources.qrc"
    )
    set_target_properties(tst_webengine_e2e PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(tst_webengine_e2e PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(tst_webengine_e2e PRIVATE QT_FORCE_ASSERTS)
    target_link_libraries(tst_webengine_e2e
        PRIVATE
            clothbound_app
            Qt6::Test
    )
    add_test(NAME tst_webengine_e2e COMMAND tst_webengine_e2e)
    set_tests_properties(tst_webengine_e2e PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=generic;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu --disable-dev-shm-usage"
        TIMEOUT 180
    )
endif()

if(CLOTHBOUND_BUILD_BENCH)
    add_executable(clothbound_bench
        bench.cpp
        testbooks.h
        "${CMAKE_SOURCE_DIR}/src/formats/pdfpages.h"
        "${CMAKE_SOURCE_DIR}/src/formats/pdfpages.cpp"
        "${CMAKE_SOURCE_DIR}/src/ai/gemini.h"
        "${CMAKE_SOURCE_DIR}/src/ai/gemini.cpp"
        "${CMAKE_SOURCE_DIR}/src/audio/speech.h"
        "${CMAKE_SOURCE_DIR}/src/audio/speech.cpp"
        "${CMAKE_SOURCE_DIR}/src/audio/voicepacks.h"
        "${CMAKE_SOURCE_DIR}/src/audio/voicepacks.cpp"
    )

    set_target_properties(clothbound_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(clothbound_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(clothbound_bench PRIVATE
        CLOTHBOUND_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(clothbound_bench
        PRIVATE
            clothbound_headless
            Qt6::Qml
            Qt6::Quick
    )
endif()
