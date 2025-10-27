# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

# UBus

===============================================================================
Notes on running urma sample
===============================================================================

You can run urma sample programs on two host_75 and host_76 with the following steps:

1. Build and install
- You can build and install the ubus rpm packages by:
$ cd UBus/code
$ tar -czf /root/rpmbuild/SOURCES/umdk-1.2.0.tar.gz --exclude=.git `ls -A`
$ rpmbuild -bb umdk.spec
$ rpm -ivh /root/rpmbuild/RPMS/*/*.rpm

- Alternately, you can build your programs manually in the code foler for debug.
$ cd UBus/opensource
$ sh compile_opensource.sh
$ mkdir UBus/code/build
$ cd UBus/code/build
$ cmake ..
$ make install

- load kernel modules
$ depmod
$ modprobe ubcore
$ modprobe uburma

2. [Optional]Config and start ubsc
The ubsc controller should be configured and started on one of the hosts, e.g. host_75.

- The configuation file of ubsc is "/etc/ubsc/ubsc.ini". Currently we support TCP/IP socket or urma jetty communication
  bewtween UBSC and local_ubsc. You can read ubsc.md for configuration.

- Start ubsc sevice by:
$ service ubscd start

- Alternately, you can start ubsc in the build path for debug.
$ management/ubsc/ubsc_daemon

3. Start urma sample program
The sample program is included with the example rpm packages.

- start sample at server side (e.g. host_75)
The test progrom can be found in the build path.

$ urma_sample -d hrn0_0

- start sample for testing at client side (e.g. host_76)
$ urma_sample -d hrn0_0 -i xx.xx.xx.xx

Options:
ub_client_server: invalid option -- 'h'
Usage:
  -m, --mode <mode>            urma mode: 0 for UB
  -d, --dev-name <dev>         device name, e.g. udma for UB
  -i, --server-ip <ip>         server ip address given only by client
  -p, --server-port <port>     listen on/connect to port <port> (default 18515)
  -e, --event-mode             demo jfc event
  -a, --address-mode <va|pa>   use VA or PA in WQE
  -c, --cs-coexist             client and server coexist in a process

- The name of ROCE device can be found by:
$ hiroce gids
$ show_gids
