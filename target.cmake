target_include_directories(${wxl_ext_name} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/extension-support/environment"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui")
target_compile_definitions(${wxl_ext_name} PRIVATE
    WXL_ENVIRONMENT_NAME="${wxl_ext_name}" "IMGUI_API=__declspec(dllimport)")
target_link_libraries(${wxl_ext_name} PRIVATE WarcraftXL d3d9)
include("${wxl_ext_dir}/vfog.cmake")
target_compile_definitions(${wxl_ext_name} PRIVATE WXL_ENVIRONMENT_SWITCH="WXL_EXPERIMENTAL_WATER" WXL_ENVIRONMENT_WIND_CLIENT)
if(CLIENT_PATH)
    add_custom_command(TARGET ${wxl_ext_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${wxl_ext_dir}/src/wxl-experimental-water/lang/wxl-water-lang.json"
            "${CLIENT_PATH}/wxl-water-lang.json"
        COMMENT "Deploy Water tuner strings")
endif()
