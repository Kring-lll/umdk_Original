# uvs_set_topo_ioctl

一个零额外依赖的独立 C 工具，直接通过 `ioctl` 调用 `/dev/ubcore/ubcore`，实现 `uvs_set_topo_info()` 里 `ubcore` 部分的拓扑下发。

## 输入

输入文件是简单文本格式，逐行解析，不依赖 JSON 或其他格式化解析库。`agg_eid` 不从配置文件读取，而是由文件里的 `device_code` 自动生成。当前节点通过命令行参数指定。

规则：

- 空行忽略
- `#` 之后内容视为注释
- 一个节点从 `node` 开始，到 `endnode` 结束
- 未填写字段默认保留为 `0`
- 所有 `link[*][*]` 都保持默认无效值，其中 `peer_port = UINT32_MAX`

支持的指令：

```text
node <type> <super_node_id> <node_id> <is_current>
devcode <dev_idx> <device_code>
ue <dev_idx> <ue_idx> <chip_id> <die_id> <entity_id> <primary_eid_ipv6>
port_eid <dev_idx> <ue_idx> <port_idx> <eid_ipv6>
endnode
```

其中：

- `devcode` 只提供设备编码，程序会自动生成对应 `agg_eid`
- `primary_eid_ipv6` / `eid_ipv6` 必须使用完整 IPv6 冒号分隔格式，不支持 `::` 缩写

示例：

```text
# node 0
node 0 0 0 1
devcode 0 1000
ue 0 0 0 0 100 2001:0db8:0000:0000:0000:0000:0000:0001
port_eid 0 0 0 2001:0db8:0000:0000:0000:0000:0000:0011
port_eid 0 0 1 2001:0db8:0000:0000:0000:0000:0000:0012
ue 0 1 0 1 101 2001:0db8:0000:0000:0000:0000:0000:0002
port_eid 0 1 0 2001:0db8:0000:0000:0000:0000:0000:0021
endnode

# node 1
node 0 0 1 0
devcode 0 1001
endnode
```

## 构建

```bash
cd tools/uvs_set_topo_ioctl
make
```

## 使用

```bash
./uvs_set_topo_ioctl --input topo.txt --current-node 0
```

可选参数：

- `--count <N>`: 指定节点数量。不传时根据文件内容自动推导
- `--current-node <N>`: 指定哪个 `node_id` 作为当前节点，必传
- `--print-only`: 不实际调用 `ioctl`，只打印解析后的拓扑内容

示例：

```bash
./uvs_set_topo_ioctl --input topo.txt --current-node 0 --count 4
./uvs_set_topo_ioctl --input topo.txt --current-node 0 --print-only
```

## 说明

- `ubcore` 设备路径固定为 `/dev/ubcore/ubcore`
- 实际下发时会按 `uvs_cmd_tlv` 机制把 `topo_info` 和 `topo_num` 封装成 TLV 数组，再通过 `TPSA_CMD` 调用 `UVS_CMD_SET_TOPO`
- 当前 `agg_eid` 生成规则是固定的 16 字节编码：前 4 字节是 `UAGG`，第 5-8 字节保留为 `0`，再接着 4 字节是 `device_code`，最后 4 字节保留为 `0`
- 这要求同一次下发里 `device_code` 本身是全局唯一的，否则生成出的 `agg_eid` 会冲突
- 工具不会从配置文件读取链路信息，所有 `links` 都按默认 invalid 值下发
- 工具会忽略文本里 `node` 行自带的 `is_current` 值，并用 `--current-node` 统一覆盖
- `primary_eid` 和 `port_eid` 必须写成完整 IPv6 形式，例如 `2001:0db8:0000:0000:0000:0000:0000:0001`
- `--print-only` 模式下不会访问 `/dev/ubcore/ubcore`，只会把最终将要下发的拓扑结构打印出来
- 打印出来的 EID 也固定使用完整 IPv6 形式，不会输出 `::` 缩写
- 程序启动时会打印当前编译出来的 `struct urma_topo_node` 大小，便于排查布局问题
