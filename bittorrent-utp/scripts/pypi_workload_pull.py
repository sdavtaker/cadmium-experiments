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


def fetch_json(url: str, timeout: float = 30.0):
    req = urllib.request.Request(url, headers={"User-Agent": "research-workload-pull/0.1"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def zipf_fit(counts):
    """OLS fit of log(count) = a - s*log(rank); returns (s, r_squared)."""
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
    return -slope, 1.0 - ss_res / ss_tot


def latest_release_size(project: str):
    """Bytes of the preferred install artifact of the latest release.

    Preference: first bdist_wheel (what pip installs on most platforms),
    falling back to sdist.
    """
    d = fetch_json(f"https://pypi.org/pypi/{project}/json", timeout=20)
    urls = d.get("urls", [])
    wheels = [u["size"] for u in urls if u.get("packagetype") == "bdist_wheel"]
    if wheels:
        return wheels[0]
    if urls:
        return urls[0]["size"]
    return None


def percentile(sorted_vals, q):
    if not sorted_vals:
        return None
    idx = min(len(sorted_vals) - 1, max(0, round(q * (len(sorted_vals) - 1))))
    return sorted_vals[idx]


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
    top = fetch_json(TOP_URL)
    rows = top["rows"]
    counts = [r["download_count"] for r in rows]

    s_all, r2_all = zipf_fit(counts)
    s_1k, r2_1k = zipf_fit(counts[:1000])

    total_30d = sum(counts)
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

    # Log-spaced ranks: 1 .. n-1
    n = len(rows)
    ranks = sorted({int(round(math.exp(math.log(n - 1) * i / (SIZE_SAMPLE - 1)))) for i in range(SIZE_SAMPLE)})
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
