# CacheForge — Homework 1 (Fall 2025)
LLM-guided discovery of **LLC cache replacement policies** on **ChampSim CRC2**. The workflow generates C++ policies with an LLM, compiles them, runs traces in a Dockerized toolchain, and records results in a **single** SQLite database (`funsearch.db`). Utilities then export CSVs/plots for the report.

- **Author:** Fahim Sharif (`msharif3`)
- **Repo:** https://github.com/Mudhathir/CacheForge
- **One DB only:** `funsearch.db` (no multiple databases)

---

## Repository Map

- `run_loop.py`, `run_loop_docker.py` — main experiment loops (LLM → compile → simulate).
- `tools_export_and_plot.py` — exports CSVs/plots (top-k, per-workload, best-so-far).
- `ChampSim_CRC2/new_policies/` — generated C++ policies and built binaries.
- `results/` — CSV exports (e.g., `pivot_by_policy.csv`, `top5_by_average.csv`, …).
- `plots/` — figures (e.g., `top_policy_per_workload.png`, `best_so_far_vs_iteration.png`).
- `runs/` — raw simulator logs when you do quick probes.
- `paper/` — report sources and figures (e.g., `HW1_IEEE_Report.pdf`).

> Heads-up: **don’t** commit `.env` (contains API keys). It’s ignored via `.gitignore`.

---

## Best Policy (from exports)

- **Name:** `TriSHiP` (best-by-average in my exports)
- **Source file:** `ChampSim_CRC2/new_policies/051_triship.cc`  
  *(If your export names differ, check `results/pivot_by_policy.csv` for the row whose `policy` equals the best average and copy that file instead.)*
- **Where the numbers come from:** `tools_export_and_plot.py` writes
  - `results/pivot_by_policy.csv` (per-workload + average),
  - `results/top5_by_average.csv` (ranking),
  - plus diagnostic plots in `plots/`.

---

## Setup

```bash
# Python env
conda env create -f environment.yml
conda activate cacheforge

# Build the ChampSim runner image once
docker build --platform linux/amd64 -f Dockerfile.champsim -t champsim-runner .



Traces expected at:


ChampSim_CRC2/traces/
  astar_313B.trace.gz  lbm_564B.trace.gz  mcf_250B.trace.gz
  milc_409B.trace.gz   omnetpp_17B.trace.gz


Run the Loop (optional)
python run_loop_docker.py
