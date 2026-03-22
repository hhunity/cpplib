add_library(io SHARED tiff_io.cpp)

target_compile_definitions(io PRIVATE IO_BUILD_DLL)

target_include_directories(io PUBLIC ${CMAKE_SOURCE_DIR})

target_link_libraries(io
    PRIVATE
        TIFF::TIFF
        ZLIB::ZLIB
)
