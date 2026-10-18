include(GoogleTest)
add_executable(aqo_tests
  bptree_test.cpp
  slotted_page_test.cpp
  buffer_pool_test.cpp
  disk_btree_test.cpp
)
target_link_libraries(aqo_tests PRIVATE aqo GTest::gtest GTest::gtest_main)
gtest_discover_tests(aqo_tests DISCOVERY_TIMEOUT 60)
