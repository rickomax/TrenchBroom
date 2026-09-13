add_executable(TbRenderLibTest)

target_sources(TbRenderLibTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_AllocationTracker.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LightPreviewBvh.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LightPreviewLights.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LightPreviewTracer.cpp
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

target_link_libraries(TbRenderLibTest
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
    Catch2::Catch2WithMain
    TbBaseTestUtilsLib
    TbMdlTestUtilsLib
    TbRenderLib
  )

# The light preview reads a map's entities, so its tests need a map, and a map needs the
# game configurations the fixture loads.
set(TEST_FIXTURE_DEST_DIR $<TARGET_FILE_DIR:TbRenderLibTest>/fixture)

add_custom_command(TARGET TbRenderLibTest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${TEST_FIXTURE_DEST_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games" "${TEST_FIXTURE_DEST_DIR}/games"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games-testing" "${TEST_FIXTURE_DEST_DIR}/games"
)

include(Catch)
catch_discover_tests(TbRenderLibTest)
