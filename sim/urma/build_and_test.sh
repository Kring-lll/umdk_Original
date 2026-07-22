#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# 新开容器从头到尾：编 urma_admin(非asan) + 编 liburma_sim.so + 验证设备存在。
# 用法: bash /umdk/sim/urma/build_and_test.sh
set -e
echo "================================================"
echo "步骤 1/3: 编非 asan 的 urma_admin + liburma"
echo "================================================"
cd /umdk/src
if [ ! -d build_noasan ]; then
  mkdir build_noasan && cd build_noasan
  cmake .. -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA=disable \
           -DGTEST=disable -DASAN=disable -DCODE_COVERAGE=disable
  cmake --build . -j
  cd ..
else
  echo "  build_noasan 已存在,跳过(要重建请删 /umdk/src/build_noasan)"
fi

echo
echo "================================================"
echo "步骤 2/3: 编 liburma_sim.so"
echo "================================================"
cd /umdk/sim/urma
rm -rf build && mkdir build && cd build
cmake .. > /dev/null
cmake --build . -j
ls -la liburma_sim.so

echo
echo "================================================"
echo "步骤 3/3: 验证设备存在"
echo "================================================"
export LD_LIBRARY_PATH=/umdk/src/build_noasan/urma/lib/urma/core:/umdk/src/build_noasan/urma/common
export LD_PRELOAD=/umdk/sim/urma/build/liburma_sim.so
export URMA_SIM_CONFIG=/umdk/sim/urma/urma_sim_config.json
UA=/umdk/src/build_noasan/urma/tools/urma_admin/urma_admin

echo "--- 3.1 编独立小测试(不依赖 urma_admin,最干净证据) ---"
cat > /tmp/test_sim.c <<'EOF'
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
int main(){
  DIR *d = opendir("/sys/class/ubcore");
  printf("opendir /sys/class/ubcore = %p\n", (void*)d);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
      if (e->d_name[0] != '.') printf("  device: %s\n", e->d_name);
    closedir(d);
  }
  int fd = open("/dev/uburma/udma_sim0", O_RDWR);
  printf("open /dev/uburma/udma_sim0 = %d\n", fd);
  int sfd = open("/sys/class/ubcore/udma_sim0/driver_name", O_RDONLY);
  printf("open driver_name = %d\n", sfd);
  if (sfd >= 0) { char b[64]; ssize_t n=read(sfd,b,sizeof(b)-1); if(n>0){b[n]=0;printf("  driver_name = %s",b);} close(sfd); }
  int bad = open("/dev/uburma/udma_sim9", O_RDWR);
  printf("open /dev/uburma/udma_sim9 (不存在) = %d\n", bad);
  return 0;
}
EOF
gcc /tmp/test_sim.c -o /tmp/test_sim
/tmp/test_sim

echo
echo "--- 3.2 urma_admin 列出设备 ---"
echo "[show -a] 仿真设备应出现在表里:"
"$UA" show -a 2>&1 | grep -vE "Failed read|read file" | head -5
echo
echo "[show -d udma_sim0] 单设备详情:"
"$UA" show -d udma_sim0 2>&1 | grep -vE "Failed read|read file" | head -5
echo
echo "[show -d udma_sim9] 不存在的设备(应报 invalid):"
"$UA" show -d udma_sim9 2>&1 | head -2
echo
echo "[对照] 去掉 preload 后 show -a (应无设备/报错):"
( unset LD_PRELOAD; "$UA" show -a 2>&1 | head -2 )

echo
echo "================================================"
echo "完成。后续运行 urma_admin 需先 export 这 3 个变量:"
echo "  export LD_LIBRARY_PATH=/umdk/src/build_noasan/urma/lib/urma/core:/umdk/src/build_noasan/urma/common"
echo "  export LD_PRELOAD=/umdk/sim/urma/build/liburma_sim.so"
echo "  export URMA_SIM_CONFIG=/umdk/sim/urma/urma_sim_config.json"
echo "================================================"
