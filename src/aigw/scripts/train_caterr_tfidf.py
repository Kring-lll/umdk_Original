#!/usr/bin/env python3
"""Train TF-IDF + K-means classifier for aigw CatERR.

Uses sklearn TfidfVectorizer for training; the aigw Go runtime replicates the
computation (sublinear TF, smooth IDF, L2-normalized, nearest-centroid cosine).

Outputs:
  - <output-dir>/tfidf_vocab.json      : {"GVocab": [...], "GIdf": {...}}
  - <output-dir>/tfidf_centroids.json  : {"centroids": [[...]], "labels": [...]}

Usage:
  python3 train_caterr_tfidf.py                       # 用默认路径（相对脚本）
  python3 train_caterr_tfidf.py --help                # 看全部选项
  python3 train_caterr_tfidf.py --interactive        # 交互式逐项提问
  python3 train_caterr_tfidf.py \
      --deepswe-tasks /path/to/tasks \
      --nemotron /path/to/nemotron.jsonl.gz \
      --output-dir /path/to/out --clusters 3
"""
import argparse
import gzip
import json
import logging
import math
import os
import random
from collections import Counter

# Heavy deps (numpy, sklearn) are imported lazily inside main() so that
# --help / --interactive work without them installed (stdlib only).


# Defaults are relative to this script so the script is portable (not hardcoded
# to an absolute machine path). Override via CLI flags or --interactive.
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_AIGW_ROOT = os.path.dirname(_SCRIPT_DIR)                       # .../aigw
_SIM_DATA = os.path.join(os.path.dirname(_AIGW_ROOT),           # .../cann-scheduler-backup
                         "cann-scheduler-backup", "data")

DEFAULT_DEEPSWE_TASKS = os.path.join(_SIM_DATA, "tasks")
DEFAULT_NEMOTRON = os.path.join(_SIM_DATA, "office_traces",
                                "nemotron_interactive_full.jsonl.gz")
DEFAULT_OUTPUT_DIR = os.path.join(_AIGW_ROOT, "data", "caterr")


def parse_args():
    p = argparse.ArgumentParser(
        description="Train TF-IDF + K-means CatERR classifier for aigw.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--deepswe-tasks", default=DEFAULT_DEEPSWE_TASKS,
                   help="目录：每个子目录含 DeepSWE 任务 instruction.md（code 类训练 prompt）")
    p.add_argument("--nemotron", default=DEFAULT_NEMOTRON,
                   help="文件：Nemotron office prompts 的 jsonl.gz（office 类训练 prompt）")
    p.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR,
                   help="输出目录（tfidf_vocab.json / tfidf_centroids.json）")
    p.add_argument("--clusters", type=int, default=2,
                   help="K-means 聚类数")
    p.add_argument("--office-sample", type=int, default=1000,
                   help="从 Nemotron 采样的 office prompt 数")
    p.add_argument("--max-features", type=int, default=300,
                   help="TF-IDF 词表最大维度")
    p.add_argument("--seed", type=int, default=42, help="随机种子")
    p.add_argument("--interactive", action="store_true",
                   help="交互模式：逐项提问路径与参数（回车用默认）")
    return p.parse_args()


def interactive_fill(args):
    """Prompt for each value with the current default; Enter keeps default."""
    def ask(label, cur):
        v = input(f"{label} [{cur}]: ").strip()
        return v if v else cur

    logging.info("== CatERR TF-IDF 训练（交互配置，回车用默认）==")
    args.deepswe_tasks = ask("DeepSWE tasks 目录", args.deepswe_tasks)
    args.nemotron = ask("Nemotron jsonl.gz", args.nemotron)
    args.output_dir = ask("输出目录", args.output_dir)
    args.clusters = int(ask("聚类数", args.clusters))
    args.office_sample = int(ask("office 采样数", args.office_sample))
    args.max_features = int(ask("词表维度", args.max_features))
    args.seed = int(ask("随机种子", args.seed))


def load_code_prompts(tasks_dir):
    prompts = []
    for task_name in os.listdir(tasks_dir):
        path = os.path.join(tasks_dir, task_name, "instruction.md")
        if os.path.exists(path):
            with open(path) as f:
                text = f.read().strip()
            if text:
                prompts.append(text)
    return prompts


def load_office_prompts(nemotron_path, n_sample, seed):
    pool = []
    with gzip.open(nemotron_path, "rt") as f:
        for line in f:
            try:
                r = json.loads(line)
                for m in r.get("messages", []):
                    if m.get("role") == "user":
                        c = m.get("content", "").strip()
                        if c and len(c) > 10:
                            pool.append(c)
                        break
            except json.JSONDecodeError:
                continue
    rng = random.Random(seed)
    rng.shuffle(pool)
    return pool[:n_sample], len(pool)


def main():
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    args = parse_args()
    if args.interactive:
        interactive_fill(args)

    # Heavy deps imported lazily so --help/--interactive work without them.
    import re
    import numpy as np
    from sklearn.cluster import KMeans
    from sklearn.feature_extraction.text import TfidfVectorizer

    def go_style_tokens(text):
        """Replicates aigw's Go tokenizePrompt (tfidf_caterr.go): lowercase,
        ASCII runs of 2+ [a-z0-9_] as words, each Han character as its own
        token, plus a bigram over consecutive Han characters. No stop-word
        filtering here — sklearn's stop_words applies downstream, and the Go
        runtime applies none (the trained vocabulary is the single filter)."""
        text = text.lower()
        tokens = list(re.findall(r"[a-z0-9_]{2,}", text))
        han = [ch for ch in text if "\u4e00" <= ch <= "\u9fff"]
        tokens.extend(han)
        tokens.extend(a + b for a, b in zip(han, han[1:]))
        return tokens

    # Validate inputs (helpful errors instead of crashing later).
    if not os.path.isdir(args.deepswe_tasks):
        raise SystemExit(f"--deepswe-tasks 目录不存在: {args.deepswe_tasks}\n"
                         f"（用 --help 看用法，或 --interactive 交互输入）")
    if not os.path.exists(args.nemotron):
        raise SystemExit(f"--nemotron 文件不存在: {args.nemotron}\n"
                         f"（用 --help 看用法，或 --interactive 交互输入）")
    os.makedirs(args.output_dir, exist_ok=True)

    logging.info(f"配置: clusters={args.clusters} office_sample={args.office_sample} "
          f"max_features={args.max_features} seed={args.seed}")
    logging.info(f"  code tasks dir : {args.deepswe_tasks}")
    logging.info(f"  nemotron       : {args.nemotron}")
    logging.info(f"  output dir     : {args.output_dir}")

    # 1. Load prompts
    code_prompts = load_code_prompts(args.deepswe_tasks)
    logging.info(f"Code prompts: {len(code_prompts)}")
    office_prompts, pool_size = load_office_prompts(args.nemotron, args.office_sample, args.seed)
    logging.info(f"Office prompts: {len(office_prompts)} (from {pool_size} pool)")

    all_prompts = code_prompts + office_prompts
    sources = ["code"] * len(code_prompts) + ["office"] * len(office_prompts)
    logging.info(f"Total: {len(all_prompts)} prompts")
    if len(all_prompts) < args.clusters:
        raise SystemExit(f"训练样本 {len(all_prompts)} 少于聚类数 {args.clusters}，无法训练")

    # 2. sklearn TF-IDF (same config the Go runtime replicates)
    vectorizer = TfidfVectorizer(
        max_features=args.max_features,
        stop_words="english",
        analyzer=go_style_tokens,
        sublinear_tf=True,
    )
    tfidf_matrix = vectorizer.fit_transform(all_prompts)
    feature_names = list(vectorizer.get_feature_names_out())
    idf_weights = vectorizer.idf_
    logging.info(f"Vocab: {len(feature_names)} terms")

    # 3. K-means
    kmeans = KMeans(n_clusters=args.clusters, random_state=args.seed, n_init=10)
    labels = kmeans.fit_predict(tfidf_matrix)

    # 4. Cluster stats
    logging.info("\nCluster statistics:")
    for c in range(args.clusters):
        mask = labels == c
        n_code = sum(1 for i in range(len(sources)) if mask[i] and sources[i] == "code")
        n_office = sum(1 for i in range(len(sources)) if mask[i] and sources[i] == "office")
        logging.info(f"  cluster_{c}: {int(mask.sum())} prompts (code={n_code}, office={n_office})")

    # 5. Export vocab
    vocab_data = {
        "GVocab": feature_names,
        "GIdf": {name: float(idf_weights[i]) for i, name in enumerate(feature_names)},
    }
    vocab_path = os.path.join(args.output_dir, "tfidf_vocab.json")
    with open(vocab_path, "w") as f:
        json.dump(vocab_data, f, ensure_ascii=False)
    logging.info(f"\nExported vocab: {vocab_path} ({os.path.getsize(vocab_path)} bytes)")

    # 6. Export centroids (L2-normalized to match the Go runtime's cosine sim)
    centroids = kmeans.cluster_centers_
    norms = np.linalg.norm(centroids, axis=1, keepdims=True)
    norms[norms == 0] = 1.0
    centroids_normalized = (centroids / norms).tolist()
    centroids_data = {
        "centroids": centroids_normalized,
        "labels": [f"cluster_{c}" for c in range(args.clusters)],
    }
    centroids_path = os.path.join(args.output_dir, "tfidf_centroids.json")
    with open(centroids_path, "w") as f:
        json.dump(centroids_data, f)
    logging.info(f"Exported centroids: {centroids_path} ({os.path.getsize(centroids_path)} bytes)")

    # 7. Sanity check: Go-compatible nearest-centroid classification
    logging.info("\nSanity check (simulating Go runtime):")
    vocab_idx = {name: i for i, name in enumerate(feature_names)}
    idf_map = {name: float(idf_weights[i]) for i, name in enumerate(feature_names)}

    def go_tokenize(text):
        import re
        from sklearn.feature_extraction.text import ENGLISH_STOP_WORDS
        tokens = re.findall(r"[a-z0-9_]{2,}", text.lower())
        return [t for t in tokens if t not in ENGLISH_STOP_WORDS]

    def go_compute_tfidf(text):
        terms = list(t for t in go_tokenize(text))
        terms += [terms[i] + " " + terms[i + 1] for i in range(len(terms) - 1)]
        tf = Counter(terms)
        vec = np.zeros(len(feature_names))
        for term, count in tf.items():
            if term in vocab_idx:
                vec[vocab_idx[term]] = (1.0 + math.log(count)) * idf_map[term]
        n = np.linalg.norm(vec)
        return vec / n if n > 0 else vec

    correct = 0
    for i, prompt in enumerate(all_prompts):
        vec = go_compute_tfidf(prompt)
        sims = [np.dot(vec, np.array(centroids_normalized[c])) for c in range(args.clusters)]
        if int(np.argmax(sims)) == labels[i]:
            correct += 1
    logging.info(f"  Go-compatible accuracy: {correct}/{len(all_prompts)} = "
          f"{correct / len(all_prompts) * 100:.1f}%")


if __name__ == "__main__":
    main()
