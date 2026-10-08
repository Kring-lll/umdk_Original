#!/usr/bin/env python3
"""One-shot v2 → v2.1 catalog migration. YAML in place; then run build_catalog.py."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "sources" / "scans" / "catlass" / "candidate_catalog.yaml"

DTYPES = {
    "catlass-43-ascend950-basic-matmul": {
        "input": ["fp16", "bf16", "fp32"],
        "output": ["fp16", "bf16", "fp32"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-49-ascend950-flash-attention-infer": {
        "input": ["fp16", "bf16"],
        "output": ["fp16", "bf16"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-71-ascend950-mx-gmm-finalize-routing": {
        "input": ["fp8_e4m3", "fp8_e5m2"],
        "output": ["fp32"],
        "accumulator": ["fp32"],
        "scale": ["e8m0"],
    },
    "catlass-64-ascend950-matmul-evg": {
        "input": ["fp16", "fp32"],
        "output": ["fp16", "fp32"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-66-ascend950-streamk-matmul": {
        "input": ["fp16"],
        "output": ["fp16"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-60-ascend950-grouped-matmul-slice-m": {
        "input": ["fp16"],
        "output": ["fp16"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-46-ascend950-matmul-fixpipe-opti": {
        "input": ["fp16"],
        "output": ["fp32"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-53-ascend950-fp8-mx-matmul": {
        "input": ["fp8_e4m3", "fp8_e5m2"],
        "output": ["fp32"],
        "accumulator": ["fp32"],
        "scale": ["e8m0"],
    },
    "catlass-65-ascend950-mx-gmm-swiglu-quant": {
        "input": ["fp8_e4m3"],
        "output": ["fp8_e4m3"],
        "accumulator": ["fp32"],
        "scale": ["e8m0"],
    },
    "catlass-73-ascend950-matmul-full-loadA": {
        "input": ["fp16"],
        "output": ["fp16"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-70-ascend950-fa-chunk-prefill": {
        "input": ["fp16", "bf16"],
        "output": ["fp16", "bf16"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-68-ascend950-multi-core-splitk": {
        "input": ["fp16"],
        "output": ["fp16"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-56-ascend950-basic-conv2d-tla": {
        "input": ["fp16"],
        "output": ["fp16"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-17-gemv-aiv": {
        "input": ["fp16"],
        "output": ["fp16"],
        "accumulator": ["fp32"],
        "scale": [],
    },
    "catlass-19-mla": {
        "input": ["fp16", "bf16"],
        "output": ["fp16", "bf16"],
        "accumulator": ["fp32"],
        "scale": [],
    },
}

TEST_EVIDENCE = {
    "catlass-43-ascend950-basic-matmul": "tests/optest/tests/test_43_ascend950_basic_matmul.py",
    "catlass-49-ascend950-flash-attention-infer": "tests/optest/tests/test_49_ascend950_flash_attention_infer.py",
    "catlass-71-ascend950-mx-gmm-finalize-routing": "tests/optest/tests/test_71_ascend950_fp8_mx_grouped_matmul_finalize_routing.py",
    "catlass-64-ascend950-matmul-evg": "tests/optest/tests/test_64_ascend950_matmul_evg.py",
    "catlass-66-ascend950-streamk-matmul": "tests/optest/tests/test_66_ascend950_streamk_matmul.py",
    "catlass-60-ascend950-grouped-matmul-slice-m": "tests/optest/tests/test_60_ascend950_grouped_matmul_slice_m.py",
    "catlass-46-ascend950-matmul-fixpipe-opti": "tests/optest/tests/test_46_ascend950_matmul_fixpipe_opti.py",
    "catlass-53-ascend950-fp8-mx-matmul": "tests/optest/tests/test_53_ascend950_fp8_mx_matmul_aswt.py",
    "catlass-65-ascend950-mx-gmm-swiglu-quant": "tests/optest/tests/test_65_ascend950_fp8_mx_grouped_matmul_slice_m_swiglu_mx_quant.py",
    "catlass-73-ascend950-matmul-full-loadA": "tests/optest/tests/test_73_ascend950_matmul_full_loadA.py",
    "catlass-70-ascend950-fa-chunk-prefill": "tests/optest/tests/test_70_ascend950_flash_attention_chunk_prefill.py",
    "catlass-68-ascend950-multi-core-splitk": "tests/optest/tests/test_68_ascend950_multi_core_splitk_matmul.py",
    "catlass-56-ascend950-basic-conv2d-tla": "tests/optest/tests/test_56_ascend950_basic_conv2d_tla.py",
    "catlass-17-gemv-aiv": "tests/optest/tests/test_17_gemv_aiv.py",
    "catlass-19-mla": "tests/optest/tests/test_19_mla.py",
}

CLAIM_REWRITES = {
    "enableShuffleK should stay false on 950.": (
        "This 950 example recommends leaving enableShuffleK=false. "
        "That is sample guidance, not a measured 950-wide requirement."
    ),
    "On 950 CANN 9.0.0, N-unaligned shapes speed up vs Fixpipe-to-GM; aligned MN is ~0.99× (no gain).": (
        "This optimization targets Fixpipe-bound cases such as N-unaligned writes. "
        "In the example's aligned-MN benchmark, no speedup was observed (~0.99×)."
    ),
}


def map_competes(row: dict) -> dict:
    old = row.get("status")
    why = (row.get("why") or "").lower()
    out = {
        "source": row["source"],
        "relation": "alternative",
        "review_state": "pending",
        "why": row.get("why"),
    }
    if row.get("ref"):
        out["ref"] = row["ref"]
    if old == "rejected-duplicate":
        out["relation"] = "duplicate"
        out["review_state"] = "rejected"
    elif old == "sibling-candidate":
        out["relation"] = "sibling"
        out["review_state"] = "pending"
    elif old == "pending-scan":
        out["relation"] = "alternative"
        out["review_state"] = "pending"
    if "baseline" in why:
        out["relation"] = "baseline"
        if old != "rejected-duplicate":
            out["review_state"] = "pending"
    if "without routing" in why or "not kernel sot" in why or "serving integration" in why:
        out["relation"] = "complement"
    if "skill may already" in why:
        out["relation"] = "complement"
    return out


def migrate_candidate(cand: dict) -> dict:
    cid = cand["id"]
    cand = deepcopy(cand)
    cand["status"] = "candidate"
    cand.pop("source", None)
    cand.pop("dtype", None)
    cand["dtypes"] = deepcopy(DTYPES[cid])
    for tech in cand.get("key_techniques") or []:
        if tech.get("claim") in CLAIM_REWRITES:
            tech["claim"] = CLAIM_REWRITES[tech["claim"]]
        for ev in tech.get("evidence") or []:
            if "kind" in ev and "path" not in ev:
                continue
    if cid in TEST_EVIDENCE:
        (cand.setdefault("key_techniques", [])).append(
            {
                "claim": "Upstream optest covers this example.",
                "evidence": [
                    {
                        "kind": "test",
                        "path": TEST_EVIDENCE[cid],
                    }
                ],
            }
        )
    cand["competes_with"] = [map_competes(row) for row in cand.get("competes_with") or []]
    # stable key order
    ordered = {
        "id": cand["id"],
        "name": cand["name"],
        "status": cand["status"],
        "bucket": cand["bucket"],
        "source_path": cand.get("source_path"),
        "source_commit": cand.get("source_commit"),
        "category": cand["category"],
        "algorithm": cand["algorithm"],
        "hardware": cand["hardware"],
        "compute_engine": cand["compute_engine"],
        "memory_pattern": cand["memory_pattern"],
        "pipeline_pattern": cand["pipeline_pattern"],
        "communication_pattern": cand["communication_pattern"],
        "dtypes": cand["dtypes"],
        "shape_characteristics": cand.get("shape_characteristics"),
        "scores": cand["scores"],
        "key_techniques": cand.get("key_techniques"),
        "learn": cand.get("learn"),
        "competes_with": cand.get("competes_with"),
    }
    if cand.get("notes"):
        ordered["notes"] = cand["notes"]
    return {k: v for k, v in ordered.items() if v is not None}


def main() -> None:
    raw = yaml.safe_load(CATALOG.read_text(encoding="utf-8"))
    if str(raw.get("schema_version")) == "2.1":
        print("already schema 2.1; refusing to migrate twice")
        return
    out = {
        "schema_version": "2.1",
        "source": {
            "registry_ref": "catlass",
            "commit": "a8b4fffbd9cabc021dfd47253e907a1cde59de25",
            "branch": "master",
            "scan_path": "/home/wangyibo/catlass",
        },
        "scoring": {
            "intrinsic_max": 80,
            "project_priority_scale": "1-5",
            "note": "uniqueness_score is not used. Sort is defined in sources/catalog_schema.yaml.",
        },
        "candidates": [migrate_candidate(c) for c in raw["candidates"]],
        "rejected": raw.get("rejected") or [],
    }
    header = (
        "# Generated catalog schema v2.1. YAML is the source of truth.\n"
        "# Do not hand-edit candidate_catalog.md — run: python3 tools/build_catalog.py\n"
        "# Schema: sources/catalog_schema.yaml\n"
        "# Frozen: enums, two-track scores, typed dtypes, lifecycle status,\n"
        "# evidence kinds, registry_ref, structured competes_with.\n"
        "# Does NOT modify cases/. Same 15 candidate IDs as v1.\n\n"
    )
    dumped = yaml.dump(
        out,
        sort_keys=False,
        allow_unicode=True,
        width=100,
        default_flow_style=False,
    )
    CATALOG.write_text(header + dumped, encoding="utf-8")
    print(f"wrote {CATALOG} ({len(out['candidates'])} candidates)")


if __name__ == "__main__":
    main()
