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
set(CD57_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/data/parameters/cd57_nominal.json CACHE FILEPATH "Parameter manifest embedded in the build (use a specimen manifest produced by cd57_manifest merge for a measured-specimen build)")
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
    # MSVC declares M_PI only with _USE_MATH_DEFINES; the DSP headers and the plugin use it (this was the whole Windows build failure on main)
    target_compile_definitions(cd57_core PUBLIC _USE_MATH_DEFINES)
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
    find_package(Threads REQUIRED)
    add_test(NAME dossier_validation COMMAND cd57_validate --quick --report ${CMAKE_CURRENT_BINARY_DIR}/validation_quick.md)
    # --- netlist register tests ---
    find_program(CD57_PYTHON NAMES python3 python)
    if(CD57_PYTHON)
        add_test(NAME netlist_vs_schematic_register
                 COMMAND ${CD57_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_netlist_against_register.py)
        add_test(NAME netlist_register_checker_selftest
                 COMMAND ${CD57_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_netlist_against_register.py --self-test)
    endif()
    # --- independent re-implementation (needs python3 with numpy + scipy) ---
    add_executable(cd57_dump_reference tools/dump_reference.cpp)
    target_link_libraries(cd57_dump_reference PRIVATE cd57_core)
    if(CD57_PYTHON)
        execute_process(COMMAND ${CD57_PYTHON} -I -c "import numpy, scipy" RESULT_VARIABLE CD57_PY_SCI OUTPUT_QUIET ERROR_QUIET)
        if(CD57_PY_SCI EQUAL 0)
            add_test(NAME independent_reimplementation
                     COMMAND ${CD57_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/scripts/independent_check.py --cpp-ref-exe $<TARGET_FILE:cd57_dump_reference>)
        else()
            message(STATUS "independent_reimplementation test skipped: ${CD57_PYTHON} lacks numpy/scipy (pip install numpy scipy)")
        endif()
    endif()
    # --- supply tests ---
    add_executable(validate_supply tools/validate_supply.cpp)
    target_include_directories(validate_supply PRIVATE tools)
    target_link_libraries(validate_supply PRIVATE cd57_core)
    target_compile_definitions(validate_supply PRIVATE CD57_DIGITIZED_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/tubes/digitized")
    add_test(NAME supply_regulation_vs_jj_5y3s COMMAND validate_supply)
    # --- tube device tests ---
    add_executable(validate_tubes tools/validate_tubes.cpp)
    target_include_directories(validate_tubes PRIVATE tools)
    target_link_libraries(validate_tubes PRIVATE cd57_core)
    target_compile_definitions(validate_tubes PRIVATE CD57_DIGITIZED_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/tubes/digitized")
    add_test(NAME tube_device_laws COMMAND validate_tubes)
    # --- realtime / robustness tests ---
    add_executable(validate_realtime tools/validate_realtime.cpp)
    target_link_libraries(validate_realtime PRIVATE cd57_core)
    add_test(NAME realtime_robustness COMMAND validate_realtime)
    # --- acoustic tests ---
    add_executable(validate_acoustic tools/validate_acoustic.cpp)
    target_link_libraries(validate_acoustic PRIVATE cd57_core)
    add_test(NAME acoustic_validation COMMAND validate_acoustic)
    # --- Sec. 26 metric library: self-test against analytic answers ---
    add_library(cd57_analysis INTERFACE)
    target_include_directories(cd57_analysis INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/analysis)
    target_compile_features(cd57_analysis INTERFACE cxx_std_17)
    add_executable(validate_metrics tools/validate_metrics.cpp)
    target_link_libraries(validate_metrics PRIVATE cd57_analysis)
    add_test(NAME metrics_selftest COMMAND validate_metrics)
    # --- capture programme (Sec. 25): render the capture matrix through the model; the same harness scores a hardware dataset ---
    add_executable(cd57_capture_matrix tools/cd57_capture_matrix.cpp)
    target_include_directories(cd57_capture_matrix PRIVATE tools)
    target_link_libraries(cd57_capture_matrix PRIVATE cd57_core cd57_analysis Threads::Threads)
    add_executable(cd57_conduction tools/cd57_conduction.cpp)
    target_include_directories(cd57_conduction PRIVATE tools)
    target_link_libraries(cd57_conduction PRIVATE cd57_core cd57_analysis)
    add_test(NAME valve_conduction_consistency COMMAND cd57_conduction --report ${CMAKE_CURRENT_BINARY_DIR}/conduction.md)
    add_executable(cd57_sensitivity tools/cd57_sensitivity.cpp)
    target_include_directories(cd57_sensitivity PRIVATE tools)
    target_link_libraries(cd57_sensitivity PRIVATE cd57_core cd57_analysis Threads::Threads)
    add_test(NAME sensitivity_quick COMMAND cd57_sensitivity --quick --threads 2 --report ${CMAKE_CURRENT_BINARY_DIR}/sensitivity_quick.md)
    add_executable(cd57_diagnostics tools/cd57_diagnostics.cpp)
    target_include_directories(cd57_diagnostics PRIVATE tools)
    target_link_libraries(cd57_diagnostics PRIVATE cd57_core cd57_analysis)
    add_executable(validate_jacks tools/validate_jacks.cpp)
    target_include_directories(validate_jacks PRIVATE tools)
    target_link_libraries(validate_jacks PRIVATE cd57_core cd57_analysis)
    add_test(NAME jack_states_two_plugs COMMAND validate_jacks)
    add_executable(validate_manifest tools/validate_manifest.cpp)
    target_link_libraries(validate_manifest PRIVATE cd57_core)
    target_compile_definitions(validate_manifest PRIVATE CD57_PARAM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/parameters")
    add_test(NAME manifest_provenance_and_specimen_delta COMMAND validate_manifest)
    add_executable(cd57_manifest tools/cd57_manifest.cpp)
    target_link_libraries(cd57_manifest PRIVATE cd57_core)
    add_executable(cd57_compare tools/cd57_compare.cpp)
    target_include_directories(cd57_compare PRIVATE tools src/dsp)
    target_link_libraries(cd57_compare PRIVATE cd57_core cd57_analysis)
    if(CD57_PYTHON)
        add_test(NAME capture_compare_selfcheck
                 COMMAND ${CD57_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_compare.py --capture $<TARGET_FILE:cd57_capture_matrix>
                         --compare $<TARGET_FILE:cd57_compare> --tmp ${CMAKE_CURRENT_BINARY_DIR}/compare_selfcheck)
        add_test(NAME capture_matrix_quick
                 COMMAND ${CD57_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_capture_matrix.py --exe $<TARGET_FILE:cd57_capture_matrix>
                         --tmp ${CMAKE_CURRENT_BINARY_DIR}/capture_quick)
    endif()
    # --- numerics tests ---
    add_executable(validate_numerics tools/validate_numerics.cpp)
    target_link_libraries(validate_numerics PRIVATE cd57_core Threads::Threads)
    add_test(NAME numerics_validation COMMAND validate_numerics --quick --report ${CMAKE_CURRENT_BINARY_DIR}/numerics_quick.md)
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
    target_compile_definitions(cd57_host_test PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=1
                               $<$<CXX_COMPILER_ID:MSVC>:_USE_MATH_DEFINES>)   # this target does not link cd57_core, which carries the define for MSVC
    target_link_libraries(cd57_host_test PRIVATE juce::juce_audio_processors juce::juce_audio_utils juce::juce_recommended_config_flags)
    add_dependencies(cd57_host_test CustomDeluxe57_VST3)

    # processor test: drives the plugin's AudioProcessor directly (raw saved state, synchronous latency): acoustic modes, IR path,
    # input-channel choice, state continuity across an oversampling change
    juce_add_console_app(cd57_processor_test PRODUCT_NAME "cd57_processor_test")
    target_sources(cd57_processor_test PRIVATE tests/ProcessorTest.cpp)
    target_compile_definitions(cd57_processor_test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(cd57_processor_test PRIVATE CustomDeluxe57 cd57_core juce::juce_audio_utils juce::juce_audio_formats juce::juce_recommended_config_flags)
    add_test(NAME plugin_processor COMMAND cd57_processor_test)
endif()
