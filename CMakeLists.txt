cmake_minimum_required(VERSION 3.22)
project(CustomDeluxe57 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CD57_BUILD_PLUGIN "Build the JUCE VST3 + Standalone plugin" ON)
option(CD57_BUILD_TOOLS  "Build the offline renderer and validation suite" ON)
set(CD57_JUCE_DIR "" CACHE PATH "Use an existing JUCE checkout instead of fetching JUCE 8.0.15")

# ------------------------------------------------------------------ core DSP (JUCE-free, header-only + embedded data)
set(CD57_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${CD57_GEN})
set(CD57_NETLIST ${CMAKE_CURRENT_SOURCE_DIR}/data/netlist/cd57_nominal.net)
set(CD57_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/data/parameters/cd57_nominal.json)
add_custom_command(OUTPUT ${CD57_GEN}/EmbeddedNetlist.cpp
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CD57_NETLIST} -DOUTPUT=${CD57_GEN}/EmbeddedNetlist.cpp -DFUNC=embeddedNetlist
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedText.cmake
    DEPENDS ${CD57_NETLIST} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedText.cmake VERBATIM)
add_custom_command(OUTPUT ${CD57_GEN}/EmbeddedManifest.cpp
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CD57_MANIFEST} -DOUTPUT=${CD57_GEN}/EmbeddedManifest.cpp -DFUNC=embeddedManifest
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedText.cmake
    DEPENDS ${CD57_MANIFEST} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedText.cmake VERBATIM)

add_library(cd57_core STATIC ${CD57_GEN}/EmbeddedNetlist.cpp ${CD57_GEN}/EmbeddedManifest.cpp)
target_include_directories(cd57_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp ${CMAKE_CURRENT_SOURCE_DIR}/generated)
target_compile_features(cd57_core PUBLIC cxx_std_17)
set_target_properties(cd57_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(cd57_core PUBLIC /fp:precise /EHsc)
else()
    target_compile_options(cd57_core PUBLIC -fno-math-errno)
endif()

# ------------------------------------------------------------------ tools
if(CD57_BUILD_TOOLS)
    add_executable(cd57_render tools/cd57_render.cpp)
    target_link_libraries(cd57_render PRIVATE cd57_core)
    add_executable(cd57_validate tools/cd57_validate.cpp)
    target_link_libraries(cd57_validate PRIVATE cd57_core)
    # regenerates the unrolled LU kernel after any netlist-topology change:  cmake --build build --target regen_lu_kernel
    add_executable(cd57_gen_lu tools/gen_lu.cpp)
    target_link_libraries(cd57_gen_lu PRIVATE cd57_core)
    add_custom_target(regen_lu_kernel COMMAND cd57_gen_lu ${CD57_NETLIST} ${CMAKE_CURRENT_SOURCE_DIR}/generated/LuKernel.h
        DEPENDS cd57_gen_lu COMMENT "Regenerating generated/LuKernel.h" VERBATIM)
    enable_testing()
    add_test(NAME dossier_validation COMMAND cd57_validate --quick --report ${CMAKE_CURRENT_BINARY_DIR}/validation_quick.md)
    # --- acoustic tests ---
    add_executable(validate_acoustic tools/validate_acoustic.cpp)
    target_link_libraries(validate_acoustic PRIVATE cd57_core)
    add_test(NAME acoustic_validation COMMAND validate_acoustic)
endif()

# ------------------------------------------------------------------ plugin (VST3 + Standalone)
if(CD57_BUILD_PLUGIN)
    if(CD57_JUCE_DIR)
        add_subdirectory(${CD57_JUCE_DIR} juce EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.15 GIT_SHALLOW ON)
        FetchContent_MakeAvailable(juce)
    endif()

    juce_add_plugin(CustomDeluxe57
        COMPANY_NAME "CD57 Project"
        PLUGIN_MANUFACTURER_CODE Cd57
        PLUGIN_CODE Cd57
        FORMATS Standalone VST3
        PRODUCT_NAME "57 Custom Deluxe"
        BUNDLE_ID com.cd57.customdeluxe57
        IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT FALSE
        VST3_CATEGORIES Fx Distortion
        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(CustomDeluxe57 PRIVATE
        plugin/PluginProcessor.cpp plugin/PluginEditor.cpp)
    target_include_directories(CustomDeluxe57 PRIVATE plugin)
    target_compile_definitions(CustomDeluxe57 PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0)
    target_link_libraries(CustomDeluxe57
        PRIVATE cd57_core juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)

    # host test: loads the built VST3 through JUCE's VST3 host (no external tools needed)
    juce_add_console_app(cd57_host_test PRODUCT_NAME "cd57_host_test")
    target_sources(cd57_host_test PRIVATE tests/PluginHostTest.cpp)
    target_compile_definitions(cd57_host_test PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(cd57_host_test PRIVATE juce::juce_audio_processors juce::juce_audio_utils juce::juce_recommended_config_flags)
    add_dependencies(cd57_host_test CustomDeluxe57_VST3)
endif()
