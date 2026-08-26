# CMake generated Testfile for 
# Source directory: E:/game
# Build directory: E:/game/build_js
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if("${CTEST_CONFIGURATION_TYPE}" MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(unit "E:/game/build_js/Debug/neon_tests.exe")
  set_tests_properties(unit PROPERTIES  WORKING_DIRECTORY "E:/game" _BACKTRACE_TRIPLES "E:/game/CMakeLists.txt;614;add_test;E:/game/CMakeLists.txt;0;")
elseif("${CTEST_CONFIGURATION_TYPE}" MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(unit "E:/game/build_js/Release/neon_tests.exe")
  set_tests_properties(unit PROPERTIES  WORKING_DIRECTORY "E:/game" _BACKTRACE_TRIPLES "E:/game/CMakeLists.txt;614;add_test;E:/game/CMakeLists.txt;0;")
elseif("${CTEST_CONFIGURATION_TYPE}" MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(unit "E:/game/build_js/MinSizeRel/neon_tests.exe")
  set_tests_properties(unit PROPERTIES  WORKING_DIRECTORY "E:/game" _BACKTRACE_TRIPLES "E:/game/CMakeLists.txt;614;add_test;E:/game/CMakeLists.txt;0;")
elseif("${CTEST_CONFIGURATION_TYPE}" MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(unit "E:/game/build_js/RelWithDebInfo/neon_tests.exe")
  set_tests_properties(unit PROPERTIES  WORKING_DIRECTORY "E:/game" _BACKTRACE_TRIPLES "E:/game/CMakeLists.txt;614;add_test;E:/game/CMakeLists.txt;0;")
else()
  add_test(unit NOT_AVAILABLE)
endif()
