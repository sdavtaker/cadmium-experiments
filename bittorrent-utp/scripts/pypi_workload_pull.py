#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""PyPI workload characterization for the BitTorrent/uTP swarm experiment.

Inputs (public):
  - hugovk top-pypi-packages (30-day download counts, top 15k, BigQuery-derived)
  - PyPI JSON API per-package artifact sizes (latest release), sampled at
    log-spaced popularity ranks with polite pacing.

Outputs (stdout JSON): Zipf fit over ranks, aggregate install rate,
artifact-size percentiles (unweighted and download-weighted).
"""
import json
import math
import sys
import time
import urllib.request

TOP_URL = "https://hugovk.github.io/top-pypi-packages/top-pypi-packages-30-days.min.json"
SIZE_SAMPLE = 48  # log-spaced ranks sampled for artifact size
PACING_S = 1.0    # politeness delay between PyPI JSON API calls


def die(msg: str):
    sys.exit(f"error: {msg}")


def fetch_json(url: str, timeout: float = 30.0):
    req = urllib.request.Request(url, headers={"User-Agent": "research-workload-pull/0.1"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def zipf_fit(counts):
    """OLS fit of log(count) = a - s*log(rank); returns (s, r_squared).

    Counts must be strictly positive (main filters the dataset); degenerate
    inputs abort with a clear message instead of a math traceback.
    """
    if len(counts) < 2:
        die("zipf_fit needs at least 2 positive download counts")
    xs = [math.log(i + 1) for i in range(len(counts))]
    ys = [math.log(c) for c in counts]
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    slope = sxy / sxx
    a = my - slope * mx
    ss_res = sum((y - (a + slope * x)) ** 2 for x, y in zip(xs, ys))
    ss_tot = sum((y - my) ** 2 for y in ys)
    if ss_tot == 0.0:  # all counts identical: flat fit, R^2 undefined
        return -slope, 1.0
    return -slope, 1.0 - ss_res / ss_tot


def latest_release_size(project: str):
    """Bytes of a deterministic install artifact of the latest release.

    The PyPI JSON API's url order is not documented as stable, so pick the
    lexicographically first wheel filename (deterministic across runs and a
    reasonable proxy for what pip installs), falling back to the
    lexicographically first artifact of any type.
    """
    d = fetch_json(f"https://pypi.org/pypi/{project}/json", timeout=20)
    urls = d.get("urls", [])
    wheels = sorted(
        (u.get("filename", ""), u["size"]) for u in urls if u.get("packagetype") == "bdist_wheel"
    )
    if wheels:
        return wheels[0][1]
    artifacts = sorted((u.get("filename", ""), u["size"]) for u in urls)
    if artifacts:
        return artifacts[0][1]
    return None


def percentile(sorted_vals, q):
    """Nearest-rank percentile (avoids banker's-rounding index surprises)."""
    if not sorted_vals:
        return None
    idx = max(0, math.ceil(q * len(sorted_vals)) - 1)
    return sorted_vals[min(idx, len(sorted_vals) - 1)]


def weighted_percentile(pairs, q):
    """Exact weighted percentile: pairs = [(value, weight)], weights > 0.

    Returns the smallest value whose cumulative weight share reaches q.
    """
    if not pairs:
        return None
    ordered = sorted(pairs)
    total = sum(w for _, w in ordered)
    cum = 0.0
    for value, weight in ordered:
        cum += weight
        if cum >= q * total:
            return value
    return ordered[-1][0]


def main():
    try:
        top = fetch_json(TOP_URL)
    except Exception as exc:
        die(f"failed to fetch top-pypi dataset: {exc}")
    rows = [r for r in top.get("rows", []) if r.get("download_count", 0) > 0]
    if len(rows) < 2:
        die(f"dataset malformed: {len(rows)} rows with positive download counts")
    counts = [r["download_count"] for r in rows]

    s_all, r2_all = zipf_fit(counts)
    s_1k, r2_1k = zipf_fit(counts[:1000])

    total_30d = sum(counts)  # > 0: rows filtered to positive counts above
    result = {
        "dataset_last_update": top.get("last_update"),
        "n_packages": len(rows),
        "zipf_exponent_top15k": round(s_all, 4),
        "zipf_r2_top15k": round(r2_all, 4),
        "zipf_exponent_top1k": round(s_1k, 4),
        "zipf_r2_top1k": round(r2_1k, 4),
        "downloads_30d_top15k": total_30d,
        "installs_per_second_top15k": round(total_30d / (30 * 86400), 1),
        "top10_share": round(sum(counts[:10]) / total_30d, 4),
        "top100_share": round(sum(counts[:100]) / total_30d, 4),
        "top1000_share": round(sum(counts[:1000]) / total_30d, 4),
    }

    # Log-spaced ranks: 1 .. n-1 (n >= 2 guaranteed above; SIZE_SAMPLE >= 2)
    n = len(rows)
    n_points = max(2, SIZE_SAMPLE)
    ranks = sorted({int(round(math.exp(math.log(n - 1) * i / (n_points - 1)))) for i in range(n_points)})
    sizes = []          # (rank, project, bytes)
    for rank in ranks:
        project = rows[rank - 1]["project"]
        try:
            size = latest_release_size(project)
        except Exception as exc:  # survey must survive odd/yanked packages
            print(f"warn: {project}: {exc}", file=sys.stderr)
            size = None
        if size is not None:
            sizes.append((rank, project, size))
        time.sleep(PACING_S)

    plain = sorted(b for _, _, b in sizes)
    # Download-weighted: each sampled size weighted by its rank's exact count
    weighted_pairs = [(b, float(rows[rank - 1]["download_count"])) for rank, _, b in sizes]

    result.update({
        "size_sample_n": len(sizes),
        "size_p10": percentile(plain, 0.10),
        "size_median": percentile(plain, 0.50),
        "size_p90": percentile(plain, 0.90),
        "size_max_sampled": plain[-1] if plain else None,
        "size_weighted_median": weighted_percentile(weighted_pairs, 0.50),
        "size_weighted_p90": weighted_percentile(weighted_pairs, 0.90),
        "size_samples": [
            {"rank": r, "project": p, "bytes": b} for r, p, b in sizes
        ],
    })
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
