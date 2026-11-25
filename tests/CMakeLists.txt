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
