include(GoogleTest)
add_executable(aqo_tests
  bptree_test.cpp
  slotted_page_test.cpp
  buffer_pool_test.cpp
  disk_btree_test.cpp
  wal_test.cpp
  engine_test.cpp
  table_test.cpp
  range_structs_test.cpp
  crash_kill_test.cpp
)
target_link_libraries(aqo_tests PRIVATE aqo GTest::gtest GTest::gtest_main)

add_executable(crash_worker crash_worker.cpp)
target_link_libraries(crash_worker PRIVATE aqo)
target_compile_definitions(aqo_tests PRIVATE AQO_CRASH_WORKER_PATH="$<TARGET_FILE:crash_worker>")
add_dependencies(aqo_tests crash_worker)

gtest_discover_tests(aqo_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)
