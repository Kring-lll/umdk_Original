how to build, test and link with HFA
==================================

Install gtest (assuming gtest is not installed yet)
----------------

download from https://open.codehub.huawei.com/api/codehub/v1/projects/4211/repository/blobs/cdb01d9c9f1d2e1573a84c4f9d8682e150b1b415/raw?file_name=gtest_1.10.0_gcc7.3.0_linux_arm64.tar.gz


Compile hfa
--------------

mkdir -p build

cd build

    to build with tests: cmake -DBUILD_TESTS=ON ..

    or

    to build without tests: cmake -DBUILD_TESTS=OFF ..

make -j


(*) hfa library is located at lib/libhfa.so


Run gtest
---------

cd bin

./gtest


Link with hfa:
-------------

include hfa header file: include/hbm_fast_alloc.h

link your program with libhfa.so

