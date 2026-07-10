# CMake generated Testfile for 
# Source directory: /home/zdxfgre/jd/umdk/test/urma/fuzz
# Build directory: /home/zdxfgre/jd/umdk/test/urma/build/fuzz
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(urma_api_fuzz "/home/zdxfgre/jd/umdk/test/urma/build/fuzz/urma_api_fuzz")
set_tests_properties(urma_api_fuzz PROPERTIES  LABELS "phase_fuzz;phase_fuzz_urma" _BACKTRACE_TRIPLES "/home/zdxfgre/jd/umdk/test/urma/fuzz/CMakeLists.txt;20;add_test;/home/zdxfgre/jd/umdk/test/urma/fuzz/CMakeLists.txt;0;")
add_test(uvs_api_fuzz "/home/zdxfgre/jd/umdk/test/urma/build/fuzz/uvs_api_fuzz")
set_tests_properties(uvs_api_fuzz PROPERTIES  LABELS "phase_fuzz;phase_fuzz_uvs" _BACKTRACE_TRIPLES "/home/zdxfgre/jd/umdk/test/urma/fuzz/CMakeLists.txt;23;add_test;/home/zdxfgre/jd/umdk/test/urma/fuzz/CMakeLists.txt;0;")
