# Unit, QML smoke tests, and C++ micro-benchmark targets.
# Kept in tests/ so Qt6::Test sets QT_TESTCASE_SOURCEDIR to tests/ cleanly.

if(CLOTHBOUND_BUILD_TESTS)
    add_executable(clothbound_tests
        clothbound_tests.h
        tst_test_main.cpp
        tst_core_formats.cpp
        tst_library_marks.cpp
        tst_reader_ui.cpp
        tst_audio_narration.cpp
        tst_ai_companion.cpp
        tst_mcp_cli_waves.cpp
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
        tsan_shims.h
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

    add_executable(tst_w8_storage_palette
        tst_w8_storage_palette.cpp
        testbooks.h
        tsan_shims.h
    )
    set_target_properties(tst_w8_storage_palette PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(tst_w8_storage_palette PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(tst_w8_storage_palette PRIVATE QT_FORCE_ASSERTS)
    target_link_libraries(tst_w8_storage_palette
        PRIVATE
            clothbound_app
            Qt6::Test
    )
    add_test(NAME tst_w8_storage_palette COMMAND tst_w8_storage_palette)
    set_tests_properties(tst_w8_storage_palette PROPERTIES
        ENVIRONMENT "${_tsan_test_env}"
        TIMEOUT 180
    )

    add_executable(tst_w8_passage_quant
        tst_w8_passage_quant.cpp
        fakeserver.h
    )
    set_target_properties(tst_w8_passage_quant PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(tst_w8_passage_quant PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(tst_w8_passage_quant PRIVATE QT_FORCE_ASSERTS)
    target_link_libraries(tst_w8_passage_quant
        PRIVATE
            clothbound_headless
            Qt6::Test
    )
    add_test(NAME tst_w8_passage_quant COMMAND tst_w8_passage_quant)
    set_tests_properties(tst_w8_passage_quant PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=generic"
        TIMEOUT 180
    )

    add_executable(tst_w8_mcp_cli
        tst_w8_mcp_cli.cpp
        testbooks.h
    )
    set_target_properties(tst_w8_mcp_cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(tst_w8_mcp_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(tst_w8_mcp_cli PRIVATE QT_FORCE_ASSERTS)
    target_link_libraries(tst_w8_mcp_cli
        PRIVATE
            clothbound_mcp
            Qt6::Test
    )
    add_dependencies(tst_w8_mcp_cli clothbound-mcp)
    add_test(NAME tst_w8_mcp_cli COMMAND tst_w8_mcp_cli)
    set_tests_properties(tst_w8_mcp_cli PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=generic;CLOTHBOUND_MCP_BIN=$<TARGET_FILE:clothbound-mcp>"
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

option(CLOTHBOUND_BUILD_FUZZERS "Build standalone libFuzzer parser harnesses" OFF)

if(CLOTHBOUND_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "CLOTHBOUND_BUILD_FUZZERS=ON requires Clang with libFuzzer (pass -DCMAKE_CXX_COMPILER=clang++).")
    endif()

    function(clothbound_add_fuzzer target_name)
        cmake_parse_arguments(FUZZ "" "" "SOURCES;LIBS" ${ARGN})
        add_executable(${target_name} ${FUZZ_SOURCES})
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz"
            AUTOMOC OFF
            AUTORCC OFF
        )
        target_include_directories(${target_name} PRIVATE
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_compile_options(${target_name} PRIVATE
            -U_FORTIFY_SOURCE
            -fsanitize=fuzzer,address,undefined
            -fno-omit-frame-pointer
            -g
            -O1
        )
        target_link_options(${target_name} PRIVATE
            -fsanitize=fuzzer,address,undefined
        )
        target_link_libraries(${target_name} PRIVATE ${FUZZ_LIBS})
    endfunction()

    clothbound_add_fuzzer(fuzz_ziparchive
        SOURCES
            fuzz/fuzz_ziparchive.cpp
            "${CMAKE_SOURCE_DIR}/src/formats/ziparchive.cpp"
        LIBS Qt6::Core ZLIB::ZLIB
    )

    clothbound_add_fuzzer(fuzz_htmltokenizer
        SOURCES
            fuzz/fuzz_htmltokenizer.cpp
            "${CMAKE_SOURCE_DIR}/src/formats/htmltokenizer.cpp"
        LIBS Qt6::Core
    )

    clothbound_add_fuzzer(fuzz_opdsparser
        SOURCES
            fuzz/fuzz_opdsparser.cpp
            "${CMAKE_SOURCE_DIR}/src/online/opdsparser.cpp"
        LIBS Qt6::Core
    )

    clothbound_add_fuzzer(fuzz_articleextractor
        SOURCES
            fuzz/fuzz_articleextractor.cpp
            "${CMAKE_SOURCE_DIR}/src/online/articleextractor.cpp"
            "${CMAKE_SOURCE_DIR}/src/formats/htmltokenizer.cpp"
            "${CMAKE_SOURCE_DIR}/src/online/epubwriter.cpp"
        LIBS Qt6::Core Qt6::Network ZLIB::ZLIB
    )

    clothbound_add_fuzzer(fuzz_mathformatter
        SOURCES
            fuzz/fuzz_mathformatter.cpp
            "${CMAKE_SOURCE_DIR}/src/ai/mathformatter.cpp"
        LIBS Qt6::Core Qt6::Gui
    )

    clothbound_add_fuzzer(fuzz_sseparser
        SOURCES
            fuzz/fuzz_sseparser.cpp
            "${CMAKE_SOURCE_DIR}/src/ai/sseparser.cpp"
        LIBS Qt6::Core
    )

    add_custom_target(clothbound_fuzzers
        DEPENDS
            fuzz_ziparchive
            fuzz_htmltokenizer
            fuzz_opdsparser
            fuzz_articleextractor
            fuzz_mathformatter
            fuzz_sseparser
    )
endif()
