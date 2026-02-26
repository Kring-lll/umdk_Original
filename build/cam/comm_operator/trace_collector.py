#!/usr/bin/env python3
"""
增强版追踪数据收集器 - 支持64位组合ID（微秒时间戳，名称带seq，队列匹配，rank排序）
"""

import json
import os
import glob
import torch
from collections import deque
from typing import List, Dict, Any, Tuple, Optional

CLOCK_DIVISOR = 50.0  # 时钟频率 (MHz)，用于将时钟周期转换为微秒

def extract_point_id_parts(combined_id: int) -> Tuple[int, int]:
    base_point_id = combined_id & 0xFFFFFFFF
    extra_id = (combined_id >> 32) & 0xFFFFFFFF
    if base_point_id >= 0x80000000:
        base_point_id = base_point_id - 0x100000000
    if extra_id >= 0x80000000:
        extra_id = extra_id - 0x100000000
    return int(base_point_id), int(extra_id)

def parse_profiling_data(profiling_tensor: torch.Tensor, core_type: int = 0, core_id: int = 0, debug: bool = False) -> List[Dict]:
    prof_size_per_core = profiling_tensor.shape[1]
    raw_count = int(profiling_tensor[core_id][0].item())
    record_count = raw_count - 1
    if debug:
        print(f"  Core {core_id}: raw_count={raw_count}, 实际记录数={record_count}, Profiling大小={prof_size_per_core}")
    if record_count <= 0:
        return []
    initial_timestamp = profiling_tensor[core_id][-1].item()
    if debug:
        print(f"  初始时间戳: {initial_timestamp} (索引-1)")
    records = []
    max_records = min(record_count, (prof_size_per_core - 2) // 2)
    if debug and record_count > max_records:
        print(f"  警告: 记录数量({record_count})超过可用存储空间，仅解析前{max_records}条")
    for i in range(max_records):
        combined_id = profiling_tensor[core_id][1 + i].item()
        raw_timestamp = profiling_tensor[core_id][-2 - i].item()
        # 计算64位无符号差值，直接转换为微秒
        diff = (raw_timestamp - initial_timestamp) & 0xFFFFFFFFFFFFFFFF
        timestamp_us = diff / CLOCK_DIVISOR
        base_point_id, extra_id = extract_point_id_parts(combined_id)
        if debug:
            print(f"  记录{i}: 组合ID={combined_id:#x} (索引{1+i}), 时间戳={raw_timestamp} (索引{-2-i}), 差值={diff}, 时间戳_us={timestamp_us:.2f}")
        records.append({
            'timestamp_us': timestamp_us,
            'raw_timestamp': raw_timestamp,
            'initial_timestamp': initial_timestamp,
            'combined_id': int(combined_id),
            'base_point_id': base_point_id,
            'extra_id': extra_id,
            'core_type': core_type,
            'core_id': core_id,
            'record_idx': i
        })
    return records

def load_all_ranks(data_dir: str = "profiling_data", debug_core: Optional[Tuple[int, int, int]] = None) -> Dict[int, Dict]:
    all_data = {}
    pt_files = sorted(glob.glob(os.path.join(data_dir, "rank*.pt")))
    for pt_file in pt_files:
        basename = os.path.basename(pt_file)
        rank_id = int(basename.split('.')[0][4:])
        try:
            split_tensors = torch.load(pt_file, map_location='cpu')
            rank_records = []
            for type_idx, tensor in enumerate(split_tensors):
                core_num = tensor.shape[0]
                for core_id in range(core_num):
                    debug = (debug_core is not None and
                            debug_core[0] == rank_id and
                            debug_core[1] == type_idx and
                            debug_core[2] == core_id)
                    if debug:
                        print(f"\n调试核心 - Rank {rank_id}, 类型 {type_idx}, 核心 {core_id}:")
                        print(f"Tensor形状: {tensor.shape}")
                    records = parse_profiling_data(tensor, type_idx, core_id, debug)
                    for record in records:
                        record['rank_id'] = rank_id
                    rank_records.extend(records)
            all_data[rank_id] = {
                'records': rank_records,
                'tensor_count': len(split_tensors)
            }
            print(f"Rank {rank_id}: {len(rank_records)} 条记录, {len(split_tensors)} 种核心类型")
        except Exception as e:
            print(f"加载 {pt_file} 失败: {e}")
    return all_data

def load_mapping(mapping_file: str = "trace_mapping.json") -> Dict:
    if os.path.exists(mapping_file):
        with open(mapping_file, 'r') as f:
            data = json.load(f)
        if "points" in data:
            return data
        else:
            return {"points": data}
    return {"points": {}}

def convert_to_chrome_event_type(event_type: str) -> Tuple[str, str]:
    event_upper = event_type.upper()
    display_map = {
        'B': 'Begin',
        'E': 'End',
        'I': 'Instant',
        'C': 'Counter',
        'S': 'Async Start',
        'F': 'Async End',
        'M': 'Marker',
    }
    display_name = display_map.get(event_upper, 'Unknown')
    return event_upper, display_name

def generate_chrome_trace(all_data: Dict[int, Dict], mapping: Dict,
                         output_file: str = "chrome_trace.json"):
    """生成Chrome Tracing格式，微秒时间戳，名称带seq，队列匹配，rank排序"""
    rank_to_pid = {}
    next_pid = 0
    points = mapping.get('points', {})
    skipped_count = 0

    # 收集元数据事件（进程名、线程名）
    metadata_events = []
    # 按rank id排序分配pid
    sorted_rank_ids = sorted(all_data.keys())
    for rank_id in sorted_rank_ids:
        pid = next_pid
        rank_to_pid[rank_id] = pid
        next_pid += 1
        # 进程名称
        metadata_events.append({
            'name': 'process_name',
            'ph': 'M',
            'pid': pid,
            'args': {'name': 'Rank'}
        })
        # 进程排序索引
        metadata_events.append({
            'name': 'process_sort_index',
            'ph': 'M',
            'pid': pid,
            'args': {'sort_index': rank_id}
        })

    # 按tid分组的事件列表
    tid_events = {}  # key: (rank_id, core_type, core_id) -> list of raw event dicts
    tid_metadata_set = set()  # 记录已添加线程元数据的tid

    for rank_id in sorted_rank_ids:
        rank_data = all_data[rank_id]
        pid = rank_to_pid[rank_id]

        for record in rank_data['records']:
            base_point_id = str(record['base_point_id'])
            point_info = points.get(base_point_id, {})

            original_event_type = point_info.get('event_type')
            if not original_event_type:
                print(f"警告: 跳过记录 base_point_id={base_point_id} (rank {rank_id}, core {record['core_id']})，映射表中无 event_type")
                skipped_count += 1
                continue

            chrome_event_type, event_display_name = convert_to_chrome_event_type(original_event_type)

            # 基础名称：label + extra_id（用于分组）
            label = point_info.get('label', f'point_{base_point_id}')
            base_name = f"{label} [extra:{record['extra_id']}]"

            tid_key = (rank_id, record['core_type'], record['core_id'])
            # 格式化tid为固定宽度，确保按core id数值顺序排序
            tid_str = f"type{record['core_type']}_core{record['core_id']:03d}"

            # 添加线程名称元数据（每个tid只一次）—— 仅显示Core ID
            if tid_key not in tid_metadata_set:
                tid_metadata_set.add(tid_key)
                metadata_events.append({
                    'name': 'thread_name',
                    'ph': 'M',
                    'pid': pid,
                    'tid': tid_str,
                    'args': {'name': f'Core {record["core_id"]}'}
                })
                # 线程排序索引
                metadata_events.append({
                    'name': 'thread_sort_index',
                    'ph': 'M',
                    'pid': pid,
                    'tid': tid_str,
                    'args': {'sort_index': record['core_id']}
                })

            # 暂存原始信息（时间戳已为微秒）
            raw_item = {
                'event_base': {
                    'cat': 'trace',
                    'ts': record['timestamp_us'],  # 微秒
                    'pid': pid,
                    'tid': tid_str,
                    'ph': chrome_event_type,
                    'args': {
                        'base_point_id': record['base_point_id'],
                        'extra_id': record['extra_id'],
                        'combined_id': f"0x{record['combined_id']:016x}",
                        'rank_id': rank_id,
                        'core_type': record['core_type'],
                        'core_id': record['core_id'],
                        'raw_timestamp': record['raw_timestamp'],
                        'initial_timestamp': record['initial_timestamp'],
                        'clock_divisor': CLOCK_DIVISOR,
                        'file': point_info.get('file', ''),
                        'line': point_info.get('line', 0)
                    }
                },
                'ph': chrome_event_type,
                'base_name': base_name,
                'ts': record['timestamp_us'],  # 用于内部排序
                'point_info': point_info
            }
            if 'event_id' in point_info:
                raw_item['event_base']['args']['event_id'] = point_info['event_id']

            tid_events.setdefault(tid_key, []).append(raw_item)

    # 配对：对每个tid，按base_name分组，每组内队列匹配，并分配seq
    final_events = []
    b_count = 0
    e_count = 0
    unpaired_count = 0

    for tid_key, raw_items in tid_events.items():
        raw_items.sort(key=lambda x: x['ts'])  # 按微秒时间戳排序

        # 按base_name分组
        groups = {}
        for item in raw_items:
            name = item['base_name']
            groups.setdefault(name, []).append(item)

        for base_name, group in groups.items():
            group.sort(key=lambda x: x['ts'])
            b_queue = deque()
            seq_counter = 0
            for item in group:
                if item['ph'] == 'B':
                    seq = seq_counter
                    seq_counter += 1
                    item['seq'] = seq
                    b_queue.append(item)
                elif item['ph'] == 'E':
                    if b_queue:
                        b_item = b_queue.popleft()
                        seq = b_item['seq']
                        item['seq'] = seq
                        # 配对成功：为两个事件添加 begin/end 信息
                        b_file = b_item['point_info'].get('file', '')
                        b_line = b_item['point_info'].get('line', 0)
                        e_file = item['point_info'].get('file', '')
                        e_line = item['point_info'].get('line', 0)

                        full_name = f"{base_name} #{seq}"

                        b_event = b_item['event_base'].copy()
                        b_event['name'] = full_name
                        b_event['args']['begin_file'] = b_file
                        b_event['args']['begin_line'] = b_line
                        b_event['args']['end_file'] = e_file
                        b_event['args']['end_line'] = e_line

                        e_event = item['event_base'].copy()
                        e_event['name'] = full_name
                        e_event['args']['begin_file'] = b_file
                        e_event['args']['begin_line'] = b_line
                        e_event['args']['end_file'] = e_file
                        e_event['args']['end_line'] = e_line

                        final_events.append(b_event)
                        final_events.append(e_event)
                        b_count += 1
                        e_count += 1
                    else:
                        print(f"警告: 孤立E事件，tid={tid_key}, base_name={base_name}, ts={item['ts']}")
                        item['seq'] = -1
                        e_event = item['event_base'].copy()
                        e_event['name'] = f"{base_name} #-1"
                        final_events.append(e_event)
                        e_count += 1
                        unpaired_count += 1
                else:
                    print(f"警告: 未知事件类型 {item['ph']}，tid={tid_key}")
                    e_event = item['event_base'].copy()
                    e_event['name'] = base_name
                    final_events.append(e_event)
                    if item['ph'] == 'B':
                        b_count += 1
                    elif item['ph'] == 'E':
                        e_count += 1

            for item in b_queue:
                print(f"警告: 孤立B事件，tid={tid_key}, base_name={base_name}, ts={item['ts']}")
                seq = item['seq']
                b_event = item['event_base'].copy()
                b_event['name'] = f"{base_name} #{seq}"
                final_events.append(b_event)
                b_count += 1
                unpaired_count += 1

    # 将元数据事件放在所有普通事件之前
    all_events = metadata_events + final_events

    trace_data = {
        "traceEvents": all_events,
        "displayTimeUnit": "us",  # 明确微秒单位
        "otherData": {
            "version": "1.0",
            "generator": "trace_collector_fifo_seq_sorted_us",
            "total_events": len(final_events),
            "skipped_events": skipped_count,
            "unpaired_events": unpaired_count,
            "b_events": b_count,
            "e_events": e_count,
            "total_ranks": len(all_data),
            "clock_divisor": CLOCK_DIVISOR
        }
    }

    with open(output_file, 'w') as f:
        json.dump(trace_data, f, indent=2)

    print(f"\nChrome trace已生成: {output_file}")
    print(f"有效事件数: {len(final_events)}")
    print(f"跳过事件数: {skipped_count}")
    print(f"未配对事件数: {unpaired_count}")
    print(f"B事件数: {b_count}")
    print(f"E事件数: {e_count}")
    if b_count != e_count:
        print("警告: B和E事件数量不匹配")
    print(f"总rank数: {len(all_data)}")
    print(f"时钟频率: {CLOCK_DIVISOR} MHz")

    return trace_data

def analyze_data(all_data: Dict[int, Dict], mapping: Dict):
    print("\n数据分析:")
    print("=" * 60)
    total_records = 0
    total_ranks = len(all_data)
    extra_id_stats = {}
    event_type_stats = {}
    timestamp_stats = {'min_us': float('inf'), 'max_us': float('-inf')}
    points = mapping.get('points', {})

    for rank_id, rank_data in all_data.items():
        record_count = len(rank_data['records'])
        total_records += record_count
        tensor_count = rank_data.get('tensor_count', 0)
        print(f"Rank {rank_id}: {record_count} 条记录, {tensor_count} 种核心类型")
        for record in rank_data['records']:
            extra_id = record['extra_id']
            base_point_id = str(record['base_point_id'])
            timestamp_us = record['timestamp_us']
            extra_id_stats[extra_id] = extra_id_stats.get(extra_id, 0) + 1
            point_info = points.get(base_point_id, {})
            event_type = point_info.get('event_type', 'unknown')
            event_type_stats[event_type] = event_type_stats.get(event_type, 0) + 1
            if timestamp_us < timestamp_stats['min_us']:
                timestamp_stats['min_us'] = timestamp_us
            if timestamp_us > timestamp_stats['max_us']:
                timestamp_stats['max_us'] = timestamp_us

    print(f"\n总计: {total_ranks} 个rank, {total_records} 条记录")
    if total_records > 0:
        print(f"\n时间戳范围 (微秒): 最小值={timestamp_stats['min_us']:.2f}, 最大值={timestamp_stats['max_us']:.2f}, 范围={timestamp_stats['max_us'] - timestamp_stats['min_us']:.2f}")
    if extra_id_stats:
        print(f"\nextra_id统计 (共{len(extra_id_stats)} 种):")
        sorted_extras = sorted(extra_id_stats.items(), key=lambda x: x[1], reverse=True)
        for extra_id, count in sorted_extras[:10]:
            percentage = count / total_records * 100 if total_records > 0 else 0
            print(f"  extra_id={extra_id}: {count} ({percentage:.1f}%)")
        if len(sorted_extras) > 10:
            print(f"  ... 及其他 {len(sorted_extras)-10} 种")
    if event_type_stats:
        print(f"\n事件类型统计:")
        for event_type, count in sorted(event_type_stats.items()):
            percentage = count / total_records * 100 if total_records > 0 else 0
            print(f"  '{event_type}': {count} ({percentage:.1f}%)")

def main():
    import argparse
    parser = argparse.ArgumentParser(description='追踪数据收集器 - 微秒时间戳，名称带seq，队列匹配，rank排序')
    parser.add_argument('data_dir', help='数据目录')
    parser.add_argument('mapping_file', help='映射表文件')
    parser.add_argument('-o', '--output', default='chrome_trace.json', help='输出文件')
    parser.add_argument('--analyze', action='store_true', help='仅分析')
    parser.add_argument('--clock-divisor', type=float, default=50.0, help='时钟频率 (MHz)')
    parser.add_argument('--debug-core', type=str, default=None, help='调试核心 rank,type,core')
    args = parser.parse_args()

    global CLOCK_DIVISOR
    CLOCK_DIVISOR = args.clock_divisor

    debug_core = None
    if args.debug_core:
        try:
            parts = args.debug_core.split(',')
            if len(parts) == 3:
                debug_core = (int(parts[0]), int(parts[1]), int(parts[2]))
                print(f"调试核心: Rank {debug_core[0]}, Type {debug_core[1]}, Core {debug_core[2]}")
        except ValueError:
            print("无效的调试核心格式")

    print(f"收集数据 (时钟频率: {CLOCK_DIVISOR} MHz)...")
    all_data = load_all_ranks(args.data_dir, debug_core)
    if not all_data:
        print("未找到数据")
        return
    print(f"找到 {len(all_data)} 个rank")
    mapping = load_mapping(args.mapping_file)
    analyze_data(all_data, mapping)
    if not args.analyze:
        print("\n生成Chrome trace...")
        generate_chrome_trace(all_data, mapping, args.output)

if __name__ == "__main__":
    main()