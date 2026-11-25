cmake_minimum_required(VERSION 3.24)

# Match Arch makepkg & qmake release optimization (-O2) by default while still
# honoring caller-supplied -DCMAKE_CXX_FLAGS_RELEASE=... or CXXFLAGS.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Flags used by the CXX compiler during RELEASE builds.")

project(clothbound VERSION 0.5.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CheckPIESupported)
check_pie_supported(OUTPUT_VARIABLE _pie_output LANGUAGES CXX)
include(GNUInstallDirs)

option(CLOTHBOUND_BUILD_TESTS "Build the Clothbound unit and QML smoke test suite" OFF)
option(CLOTHBOUND_BUILD_BENCH "Build the Clothbound C++ micro-benchmark suite" OFF)
option(CLOTHBOUND_FORCE_ASSERTS "Force Q_ASSERT checks on across Clothbound targets" OFF)
set(CLOTHBOUND_SANITIZER "" CACHE STRING "Optional sanitizer mode: empty, asan, or tsan")
set_property(CACHE CLOTHBOUND_SANITIZER PROPERTY STRINGS "" asan tsan)

# ---------------------------------------------------------------------------
# Compiler & linker hardening flags
# ---------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Werror=return-type)

if(UNIX AND NOT APPLE)
    add_compile_options(-fstack-protector-strong)

    set(_clothbound_existing_flags
        "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE} ${CMAKE_CXX_FLAGS_RELWITHDEBINFO} $ENV{CXXFLAGS} $ENV{CPPFLAGS}")
    if(NOT CLOTHBOUND_SANITIZER AND NOT _clothbound_existing_flags MATCHES "_FORTIFY_SOURCE")
        add_compile_options(
            $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
            $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
        )
    endif()

    add_link_options(-Wl,-z,relro,-z,now -Wl,--as-needed)
endif()

if(CLOTHBOUND_SANITIZER STREQUAL "asan")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
elseif(CLOTHBOUND_SANITIZER STREQUAL "tsan")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_compile_definitions(CLOTHBOUND_TSAN_ACTIVE=1)
    add_link_options(-fsanitize=thread)
elseif(NOT CLOTHBOUND_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unsupported CLOTHBOUND_SANITIZER='${CLOTHBOUND_SANITIZER}'. Expected empty, 'asan', or 'tsan'.")
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

set(_qt6_components
    Core
    Gui
    DBus
    Sql
    Concurrent
    Network
    Pdf
    Qml
    Quick
    QuickControls2
    Multimedia
    WebEngineCore
    WebEngineQuick
    WebChannel
)
if(CLOTHBOUND_BUILD_TESTS)
    list(APPEND _qt6_components Test)
endif()
find_package(Qt6 REQUIRED COMPONENTS ${_qt6_components})

if(COMMAND qt_policy)
    if(Qt6_VERSION VERSION_GREATER_EQUAL "6.5.0")
        qt_policy(SET QTP0001 NEW)
    endif()
    if(Qt6_VERSION VERSION_GREATER_EQUAL "6.8.0")
        qt_policy(SET QTP0004 NEW)
        qt_policy(SET QTP0005 NEW)
    endif()
endif()

# Common compile definitions applied across Clothbound libraries & binaries.
add_library(clothbound_common_config INTERFACE)
target_include_directories(clothbound_common_config INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(clothbound_common_config INTERFACE
    CLOTHBOUND_VERSION="${PROJECT_VERSION}"
    CLOTHBOUND_SOURCE_I18N_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/i18n"
    $<$<BOOL:${CLOTHBOUND_FORCE_ASSERTS}>:QT_FORCE_ASSERTS>
)

# ---------------------------------------------------------------------------
# Layer 1: clothbound_headless (strictly ZERO WebEngine / Quick / Qml / Multimedia)
# ---------------------------------------------------------------------------
add_library(clothbound_headless STATIC
    # Formats (headless)
    src/formats/audiopcm.h
    src/formats/audiopcm.cpp
    src/formats/bookinfo.h
    src/formats/bookinfo.cpp
    src/formats/bookname.h
    src/formats/bookname.cpp
    src/formats/bookrecord.h
    src/formats/epubbook.h
    src/formats/epubbook.cpp
    src/formats/htmltokenizer.h
    src/formats/htmltokenizer.cpp
    src/formats/pdfbook.h
    src/formats/pdfbook.cpp
    src/formats/tsanhandoff.h
    src/formats/ziparchive.h
    src/formats/ziparchive.cpp

    # Library (headless)
    src/library/library.h
    src/library/library.cpp
    src/library/libraryfinder.h
    src/library/libraryfinder.cpp
    src/library/libraryindex.h
    src/library/libraryindex.cpp
    src/library/librarymodel.h
    src/library/librarymodel.cpp
    src/library/libraryschema.h
    src/library/libraryschema.cpp
    src/library/marks.h
    src/library/marks.cpp
    src/library/readingtelemetry.h
    src/library/readingtelemetry.cpp
    src/library/scanner.h
    src/library/scanner.cpp

    # AI (headless)
    src/ai/ai.h
    src/ai/aiservices.h
    src/ai/aiservices.cpp
    src/ai/aiusage.h
    src/ai/aiusage.cpp
    src/ai/keystore.h
    src/ai/keystore.cpp
    src/ai/localmodels.h
    src/ai/localmodels.cpp
    src/ai/passageindex.h
    src/ai/passageindex.cpp
    src/ai/sseparser.h
    src/ai/sseparser.cpp
)

target_link_libraries(clothbound_headless
    PUBLIC
        clothbound_common_config
        Qt6::Core
        Qt6::Gui
        Qt6::DBus
        Qt6::Sql
        Qt6::Concurrent
        Qt6::Network
        Qt6::Pdf
        ZLIB::ZLIB
        PkgConfig::LIBSECRET
)

# Isolated headless MCP library layered strictly on top of clothbound_headless
# so clothbound_bench links clothbound_headless cleanly without MCP's CLI hook.
add_library(clothbound_mcp STATIC
    src/mcp/libraryqueryservice.h
    src/mcp/libraryqueryservice.cpp
    src/mcp/mcpprotocol.h
    src/mcp/mcpprotocol.cpp
    src/mcp/mcpserver.h
    src/mcp/mcpserver.cpp
)
target_link_libraries(clothbound_mcp PUBLIC clothbound_headless)

# ---------------------------------------------------------------------------
# Thin WebEngine-free MCP server executable: clothbound-mcp
# ---------------------------------------------------------------------------
add_executable(clothbound-mcp
    src/mcp/mcp_main.cpp
)
target_link_libraries(clothbound-mcp PRIVATE clothbound_mcp)

# ---------------------------------------------------------------------------
# Layer 2: clothbound_app (full GUI / Reader / Audio / AI / Online core)
# ---------------------------------------------------------------------------
add_library(clothbound_app STATIC
    # App
    src/app/cli.h
    src/app/cli.cpp
    src/app/filepicker.h
    src/app/instance.h
    src/app/migration.h
    src/app/migration.cpp
    src/app/notifier.h
    src/app/pointerdriver.h
    src/app/portalfilepicker.h
    src/app/portalfilepicker.cpp
    src/app/surfacewatcher.h
    src/app/surfacewatcher.cpp
    src/app/systemcheck.h
    src/app/systemcheck.cpp
    src/app/theme.h
    src/app/theme.cpp
    src/app/translator.h
    src/app/translator.cpp
    src/app/trayicon.h
    src/app/trayicon.cpp

    # Formats (WebEngine / Quick image providers)
    src/formats/bookscheme.h
    src/formats/bookscheme.cpp
    src/formats/coverprovider.h
    src/formats/coverprovider.cpp
    src/formats/pdfpages.h
    src/formats/pdfpages.cpp

    # Reader
    src/reader/reader.h
    src/reader/reader.cpp
    src/reader/readerimageprovider.h
    src/reader/readerimageprovider.cpp

    # Audio
    src/audio/audiobook.h
    src/audio/audiobook.cpp
    src/audio/audiocache.h
    src/audio/audiocache.cpp
    src/audio/cast.h
    src/audio/cast.cpp
    src/audio/director.h
    src/audio/director.cpp
    src/audio/download.h
    src/audio/download.cpp
    src/audio/localspeech.h
    src/audio/localspeech.cpp
    src/audio/mpris.h
    src/audio/mpris.cpp
    src/audio/narration.h
    src/audio/narration.cpp
    src/audio/responsespeech.h
    src/audio/responsespeech.cpp
    src/audio/sherpaonnx.h
    src/audio/sleepcontroller.h
    src/audio/sleepcontroller.cpp
    src/audio/speech.h
    src/audio/speech.cpp
    src/audio/speechcleaner.h
    src/audio/speechcleaner.cpp
    src/audio/speechplayer.h
    src/audio/speechplayer.cpp
    src/audio/voicecatalog.h
    src/audio/voicecatalog.cpp
    src/audio/voicepacks.h
    src/audio/voicepacks.cpp

    # AI (reader-coupled / companion / online)
    src/ai/bookcontextreader.h
    src/ai/bookcontextreader.cpp
    src/ai/booksearch.h
    src/ai/booksearch.cpp
    src/ai/companion.h
    src/ai/companion.cpp
    src/ai/companionstore.h
    src/ai/companionstore.cpp
    src/ai/gemini.h
    src/ai/gemini.cpp
    src/ai/illustrator.h
    src/ai/illustrator.cpp
    src/ai/localdictionary.h
    src/ai/localdictionary.cpp
    src/ai/localsetup.h
    src/ai/localsetup.cpp
    src/ai/mathformatter.h
    src/ai/mathformatter.cpp

    # Online
    src/online/articleextractor.h
    src/online/articleextractor.cpp
    src/online/epubwriter.h
    src/online/epubwriter.cpp
    src/online/opdsclient.h
    src/online/opdsclient.cpp
    src/online/opdsparser.h
    src/online/opdsparser.cpp
    src/online/urlguard.h
    src/online/urlguard.cpp
    src/online/webimporter.h
    src/online/webimporter.cpp
)

target_link_libraries(clothbound_app
    PUBLIC
        clothbound_mcp
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Multimedia
        Qt6::WebEngineCore
        Qt6::WebEngineQuick
        Qt6::WebChannel
)

# ---------------------------------------------------------------------------
# AOT-compiled Qt Quick Controls style module: qrc:/ui/style/Clothbound/*
# ---------------------------------------------------------------------------
set(CLOTHBOUND_STYLE_QML_FILES
    src/ui/style/Clothbound/BusyIndicator.qml
    src/ui/style/Clothbound/Button.qml
    src/ui/style/Clothbound/CheckBox.qml
    src/ui/style/Clothbound/ComboBox.qml
    src/ui/style/Clothbound/ItemDelegate.qml
    src/ui/style/Clothbound/Label.qml
    src/ui/style/Clothbound/Menu.qml
    src/ui/style/Clothbound/MenuItem.qml
    src/ui/style/Clothbound/MenuSeparator.qml
    src/ui/style/Clothbound/Popup.qml
    src/ui/style/Clothbound/ProgressBar.qml
    src/ui/style/Clothbound/RadioButton.qml
    src/ui/style/Clothbound/ScrollBar.qml
    src/ui/style/Clothbound/ScrollIndicator.qml
    src/ui/style/Clothbound/Slider.qml
    src/ui/style/Clothbound/Switch.qml
    src/ui/style/Clothbound/TextArea.qml
    src/ui/style/Clothbound/TextField.qml
    src/ui/style/Clothbound/ToolTip.qml
)
foreach(_style_qml IN LISTS CLOTHBOUND_STYLE_QML_FILES)
    get_filename_component(_style_alias "${_style_qml}" NAME)
    set_source_files_properties("${_style_qml}" PROPERTIES QT_RESOURCE_ALIAS "${_style_alias}")
endforeach()

qt_add_qml_module(clothbound_style
    STATIC
    NO_PLUGIN
    NO_GENERATE_AOT_VALIDATION
    URI Clothbound
    VERSION 6.0
    RESOURCE_PREFIX /ui/style
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml/Clothbound"
    IMPORTS QtQuick.Controls.Basic/auto
    QML_FILES ${CLOTHBOUND_STYLE_QML_FILES}
)
set_target_properties(clothbound_style PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
target_link_libraries(clothbound_style PRIVATE Qt6::Quick Qt6::QuickControls2)

# ---------------------------------------------------------------------------
# Full GUI executable: clothbound (with qmlcachegen AOT at qrc:/ui/*)
# ---------------------------------------------------------------------------
set(CLOTHBOUND_UI_QML_FILES
    src/ui/Main.qml
    src/ui/Welcome.qml
    src/ui/CommandPalette.qml
    src/ui/HelpOverlay.qml
    src/ui/ClothboundMark.qml
    src/ui/components/Badge.qml
    src/ui/components/Glyph.qml
    src/ui/components/IconButton.qml
    src/ui/components/KeyHint.qml
    src/ui/components/MenuRow.qml
    src/ui/components/OptionCard.qml
    src/ui/components/ProviderPill.qml
    src/ui/components/Segmented.qml
    src/ui/components/Sheet.qml
    src/ui/components/SidePanel.qml
    src/ui/components/Skeleton.qml
    src/ui/components/SmoothWheel.qml
    src/ui/components/TextButton.qml
    src/ui/components/Toast.qml
    src/ui/components/Format.js
    src/ui/components/Icons.js
    src/ui/components/Shortcuts.js
    src/ui/components/Themes.js
    src/ui/components/Ui.js
    src/ui/library/LibraryView.qml
    src/ui/library/LibrarySidebar.qml
    src/ui/library/ContinueShelf.qml
    src/ui/library/BookCard.qml
    src/ui/library/BookDetails.qml
    src/ui/library/CoverImage.qml
    src/ui/library/GeneratedCover.qml
    src/ui/library/EmptyLibrary.qml
    src/ui/library/SearchBox.qml
    src/ui/library/AddUrlSheet.qml
    src/ui/library/CatalogsSheet.qml
    src/ui/reader/EpubReader.qml
    src/ui/reader/PdfSurface.qml
    src/ui/reader/ReaderBookmarkFold.qml
    src/ui/reader/ReaderChrome.qml
    src/ui/reader/ReaderLightbox.qml
    src/ui/reader/ReaderMenus.qml
    src/ui/reader/ReaderPanels.qml
    src/ui/reader/AppearancePanel.qml
    src/ui/reader/ContentsPanel.qml
    src/ui/reader/MarksPanel.qml
    src/ui/reader/NoteEditor.qml
    src/ui/reader/GoToBox.qml
    src/ui/reader/ProgressScrubber.qml
    src/ui/reader/SearchBar.qml
    src/ui/reader/SelectionBar.qml
    src/ui/audio/ListenBar.qml
    src/ui/audio/ListenOffer.qml
    src/ui/audio/VoicePanel.qml
    src/ui/companion/CompanionPanel.qml
    src/ui/companion/CompanionChooser.qml
    src/ui/settings/SettingsView.qml
    src/ui/settings/SettingsCard.qml
    src/ui/settings/SettingsRow.qml
    src/ui/settings/SettingsGeneral.qml
    src/ui/settings/SettingsReading.qml
    src/ui/settings/SettingsListening.qml
    src/ui/settings/SettingsIntelligence.qml
    src/ui/settings/SettingsShortcuts.qml
    src/ui/settings/SettingsAbout.qml
    src/ui/settings/WhereItRuns.qml
)
foreach(_ui_qml IN LISTS CLOTHBOUND_UI_QML_FILES)
    string(REGEX REPLACE "^src/" "" _ui_alias "${_ui_qml}")
    set_source_files_properties("${_ui_qml}" PROPERTIES QT_RESOURCE_ALIAS "${_ui_alias}")
endforeach()

add_executable(clothbound
    src/app/main.cpp
    src/resources.qrc
    src/prompts.qrc
)

qt_add_qml_module(clothbound
    URI ClothboundApp
    VERSION 1.0
    RESOURCE_PREFIX /
    NO_RESOURCE_TARGET_PATH
    NO_GENERATE_AOT_VALIDATION
    QML_FILES ${CLOTHBOUND_UI_QML_FILES}
)
set_target_properties(clothbound PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")

target_link_libraries(clothbound
    PRIVATE
        clothbound_app
        $<LINK_LIBRARY:WHOLE_ARCHIVE,clothbound_style>
)

# ---------------------------------------------------------------------------
# Unit & QML smoke test suite + C++ micro-benchmarks (tests/CMakeLists.txt)
# ---------------------------------------------------------------------------
if(CLOTHBOUND_BUILD_TESTS)
    enable_testing()
endif()
if(CLOTHBOUND_BUILD_TESTS OR CLOTHBOUND_BUILD_BENCH)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
install(TARGETS clothbound clothbound-mcp
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/i18n/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/clothbound/i18n"
    FILES_MATCHING PATTERN "*.json"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/pkgbuild/clothbound.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/pkgbuild/clothbound.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/clothbound"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/fonts/OFL.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/clothbound"
    RENAME "OFL-Literata.txt"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/fonts/Inter-OFL.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/clothbound"
    RENAME "OFL-Inter.txt"
)
