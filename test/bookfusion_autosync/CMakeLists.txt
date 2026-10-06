add_executable(BookFusionAutoSyncPolicyTest
  BookFusionAutoSyncPolicyTest.cpp
)

target_include_directories(BookFusionAutoSyncPolicyTest PRIVATE
  ${REPO_ROOT}/lib/BookFusionSync
)

target_link_libraries(BookFusionAutoSyncPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookFusionAutoSyncPolicyTest)
