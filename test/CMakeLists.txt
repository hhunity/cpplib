add_executable(test_logger   test_logger.cpp)
add_executable(test_tiff_io test_tiff_io.cpp)

target_link_libraries(test_logger   PRIVATE db)
target_link_libraries(test_tiff_io  PRIVATE io)
