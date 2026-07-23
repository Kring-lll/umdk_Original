"""
Generate ACL table from topology JSON.

Logic:
  - Read topo JSON, extract all NPU devices and their ports (eid, cna).
  - For every pair of ports belonging to DIFFERENT NPUs, create two
    bidirectional ACL entries keyed as "srcEid|dstEid|RCTP".
  - superNodename = topo's "name" field.
  - transportType is always "RCTP", templateId always 1.

Usage:
  python gen_acl.py <topo_json_path> [output_path]

  If output_path is omitted, writes to acl_data_<topo_name>.json beside
  the input file.
"""

import json
import os
import sys
from itertools import combinations


def extract_npu_ports(topo: dict) -> dict[str, list[dict]]:
    """Return {npu_device_name: [port_dicts]} for every NPU device."""
    npu_ports: dict[str, list[dict]] = {}
    for dev_name, dev in topo["devices"].items():
        if dev["deviceType"] != "NPU":
            continue
        ports: list[dict] = []
        for chip in dev["forwardingChip"]:
            ports.extend(chip["ports"])
        npu_ports[dev_name] = ports
    return npu_ports


def build_acl_entries(npu_ports: dict[str, list[dict]]) -> dict:
    """Build the tpAcls dict from all cross-NPU port pairs."""
    tp_acls: dict = {}
    npu_names = sorted(npu_ports.keys())

    # Every pair of distinct NPUs
    for npu_a, npu_b in combinations(npu_names, 2):
        ports_a = npu_ports[npu_a]
        ports_b = npu_ports[npu_b]
        # Every port on NPU_A paired with every port on NPU_B
        for pa in ports_a:
            for pb in ports_b:
                src_eid_fwd = pa["eid"]
                dst_eid_fwd = pb["eid"]
                key_fwd = f"{src_eid_fwd}|{dst_eid_fwd}|RCTP"
                tp_acls[key_fwd] = {
                    "srcEid": src_eid_fwd,
                    "dstEid": dst_eid_fwd,
                    "transportType": "RCTP",
                    "sourceCna": pa["cna"],
                    "destCna": pb["cna"],
                    "templateId": 1,
                }

                src_eid_rev = pb["eid"]
                dst_eid_rev = pa["eid"]
                key_rev = f"{src_eid_rev}|{dst_eid_rev}|RCTP"
                tp_acls[key_rev] = {
                    "srcEid": src_eid_rev,
                    "dstEid": dst_eid_rev,
                    "transportType": "RCTP",
                    "sourceCna": pb["cna"],
                    "destCna": pa["cna"],
                    "templateId": 1,
                }

    return tp_acls


def generate_acl(topo_path: str, output_path: str | None = None) -> str:
    """Load topo, generate ACL JSON, write to output_path, return path."""
    with open(topo_path, "r", encoding="utf-8") as f:
        topo = json.load(f)

    npu_ports = extract_npu_ports(topo)
    total_ports = sum(len(p) for p in npu_ports.values())
    n_npu = len(npu_ports)

    # C(n,2) NPU pairs, each with ports_a * ports_b port pairs, each ×2 directions
    n_npu_pairs = n_npu * (n_npu - 1) // 2
    ports_per_npu = total_ports // n_npu if n_npu else 0
    estimated_entries = n_npu_pairs * (ports_per_npu ** 2) * 2
    print(f"NPUs: {n_npu}, ports/NPU: {ports_per_npu}, total ports: {total_ports}")
    print(f"Estimated ACL entries: {estimated_entries}")

    tp_acls = build_acl_entries(npu_ports)
    print(f"Generated {len(tp_acls)} ACL entries")

    acl_data = {
        "superNodename": topo["name"],
        "tpAcls": tp_acls,
    }

    if output_path is None:
        base = os.path.splitext(topo_path)[0]
        # Derive a short safe name from topo name
        safe_name = topo["name"].replace(" ", "-")
        output_path = os.path.join(
            os.path.dirname(topo_path),
            f"acl_data_{safe_name}.json",
        )

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(acl_data, f, indent=4, ensure_ascii=False)

    print(f"Written to: {output_path}")
    return output_path


def main():
    if len(sys.argv) < 2:
        print("Usage: python gen_acl.py <topo_json_path> [output_path]")
        sys.exit(1)

    topo_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) >= 3 else None
    generate_acl(topo_path, output_path)


if __name__ == "__main__":
    main()
