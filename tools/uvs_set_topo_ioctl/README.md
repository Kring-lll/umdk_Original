# uvs_set_topo_ioctl

一个独立 C 工具，用于加载文本拓扑并下发到 `ubcore`。也支持 `dry` 模式，只打印解析结果、不实际调用 `ioctl`；支持 `echo` 模式，在设置后再读取并比较拓扑。

## 输入

输入文件是简单文本格式，逐行解析。

规则：

- 空行忽略
- `#` 之后内容视为注释
- 一个节点从 `node` 开始，到下一个 `node` 或文件结尾自动结束
- 未填写字段默认保留为 `0`
- 所有 `link[*][*]` 都保持默认无效值，其中 `peer_port = UINT32_MAX`

支持的指令：

```text
node <node_id>
dev <dev_idx> <ue_idx>
primary_eid <eid_ipv6>
port_eid <port_idx> <eid_ipv6>
```

其中：

- `dev` 会切换当前设备上下文，后续的 `primary_eid` / `port_eid` 都归属于这个 `dev`
- `socket_id` 不需要输入，程序直接使用 `ue_idx`
- `primary_eid_ipv6` / `eid_ipv6` 必须使用完整 IPv6 冒号分隔格式，不支持 `::` 缩写

示例：

```text
# node 0
node 0
dev 0 0
primary_eid 2001:0db8:0000:0000:0000:0000:0000:0001
port_eid 0 2001:0db8:0000:0000:0000:0000:0000:0011
port_eid 1 2001:0db8:0000:0000:0000:0000:0000:0012
dev 0 1
primary_eid 2001:0db8:0000:0000:0000:0000:0000:0002
port_eid 0 2001:0db8:0000:0000:0000:0000:0000:0021

# node 1
node 1
dev 0 0
```

## 构建

```bash
cd tools/uvs_set_topo_ioctl
make
```

## 使用

```bash
./uvs_set_topo_ioctl topo.txt --current-node 0
```

可选参数：

- `--count <N>`: 指定节点数量。不传时根据文件内容自动推导
- `--current-node <N>`: 指定哪个 `node.id` 作为当前节点，必传
- `--dry`: 不实际调用 `ioctl`，只打印解析后的拓扑内容
- `--echo`: 在实际 `ioctl set` 成功后，再调用一次 `get topo`，打印返回值并与输入拓扑比较

示例：

```bash
./uvs_set_topo_ioctl topo.txt --current-node 0 --count 4
./uvs_set_topo_ioctl topo.txt --current-node 0 --dry
./uvs_set_topo_ioctl topo.txt --current-node 0 --echo
```

## 提示

- `--current-node` 必传
- `--dry` 模式下不会访问设备节点
- `--echo` 需要设备节点和 `get topo` 能力可用
- `primary_eid` 和 `port_eid` 必须写成完整 IPv6 形式，例如 `2001:0db8:0000:0000:0000:0000:0000:0001`
