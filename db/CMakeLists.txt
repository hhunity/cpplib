add_library(db SHARED logger.cpp)

target_compile_definitions(db PRIVATE DB_BUILD_DLL)

# Project root as include dir so "#include "db/logger.h"" resolves correctly.
target_include_directories(db PUBLIC ${CMAKE_SOURCE_DIR})

target_link_libraries(db
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        unofficial::sqlite3::sqlite3
)
