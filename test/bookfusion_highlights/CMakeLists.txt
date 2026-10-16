add_executable(BookFusionHighlightAnchorTest
  BookFusionHighlightAnchorTest.cpp
  ${REPO_ROOT}/lib/BookFusionSync/BookFusionHighlightAnchor.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(BookFusionHighlightAnchorTest PRIVATE
  ${REPO_ROOT}/lib/BookFusionSync
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(BookFusionHighlightAnchorTest PRIVATE
  XML_CONTEXT_BYTES=1024
  XML_GE=0
)

target_link_libraries(BookFusionHighlightAnchorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookFusionHighlightAnchorTest)
