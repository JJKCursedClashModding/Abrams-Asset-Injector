set(TARGET AbramsAssetRegistrar)

add_library(${TARGET} SHARED
    dllmain.cpp
)

target_include_directories(${TARGET} PRIVATE .)
target_link_libraries(${TARGET} PUBLIC UE4SS)

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dlls"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET}>"
        "${CMAKE_SOURCE_DIR}/dlls/main.dll"
)
