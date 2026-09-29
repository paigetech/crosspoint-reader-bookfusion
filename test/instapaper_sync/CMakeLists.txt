add_executable(InstapaperSyncTest
  InstapaperSyncTest.cpp
  ${REPO_ROOT}/lib/InstapaperSync/HmacSha1.cpp
  ${REPO_ROOT}/lib/InstapaperSync/OAuth1.cpp
  ${REPO_ROOT}/lib/InstapaperSync/ArticleSanitizer.cpp
  ${REPO_ROOT}/lib/InstapaperSync/EpubPackager.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
)

target_include_directories(InstapaperSyncTest PRIVATE
  ${REPO_ROOT}/lib/InstapaperSync
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(InstapaperSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InstapaperSyncTest)
