#!/usr/bin/env python3
import sys, os
sys.path.append(os.path.abspath(".."))

from dotenv import load_dotenv
import re
import time
import sqlite3
import subprocess
from pathlib import Path
from typing import Optional, Tuple
from openai import OpenAI
from RAG import ExperimentRAG
from PromptGenerator import PolicyPromptGenerator


# ──────────────────────────────────────────────────────────────────────────────
# Configuration
# ──────────────────────────────────────────────────────────────────────────────
DB_PATH = "DB/funsearch.db"
LIB_PATH = "ChampSim_CRC2/lib/config1.a"
INCLUDE_DIR = "ChampSim_CRC2/inc"
EXAMPLE_DIR = Path("ChampSim_CRC2/new_policies")

WARMUP_INST = "1000000"
SIM_INST = "10000000"
MODEL = "o4-mini"
ITERATIONS = 100

EXAMPLE_DIR.mkdir(parents=True, exist_ok=True)

workloads = [
    {"name": "astar", "trace_path": "ChampSim_CRC2/traces/astar_313B.trace.gz"},
    {"name": "lbm", "trace_path": "ChampSim_CRC2/traces/lbm_564B.trace.gz"},
    {"name": "mcf", "trace_path": "ChampSim_CRC2/traces/mcf_250B.trace.gz"},
    {"name": "milc", "trace_path": "ChampSim_CRC2/traces/milc_409B.trace.gz"},
    {"name": "omnetpp", "trace_path": "ChampSim_CRC2/traces/omnetpp_17B.trace.gz"}
]

# ──────────────────────────────────────────────────────────────────────────────
# Docker-based execution helpers
# ──────────────────────────────────────────────────────────────────────────────
def run_in_docker(command: list, workdir: str = "/app") -> subprocess.CompletedProcess:
    """Run a command inside a Docker container"""
    docker_cmd = [
        "docker", "run", "--platform", "linux/amd64", "--rm",
        "-v", f"{os.getcwd()}:/app",
        "-w", workdir,
        "champsim-runner",
        "bash", "-c",
        " ".join(command)
    ]
    return subprocess.run(docker_cmd, check=True, capture_output=True, text=True)

def sanitize(name: str) -> str:
    print("     3. 🔧 [Sanitize] Cleaning policy name")
    return "".join(c if c.isalnum() else "_" for c in name).strip("_").lower()

def parse_policy_content(text: str,) -> Tuple[Optional[str], Optional[str], Optional[str]]:
    def _extract(pattern: str):
        m = re.search(pattern, text, flags=re.DOTALL | re.IGNORECASE)
        return m.group(1).strip() if m else None

    name = _extract(r"##\s*Policy\s*Name\s*\n(.*?)\n")
    desc = _extract(r"##\s*Policy\s*Description\s*\n(.*?)\n")
    code = _extract(r"```cpp\s*(.*?)\s*```")

    # print(f"📦 [Parse] Extracted policy: {name}")
    return name, desc, code

def compile_policy(cc: Path) -> Path:
    print(f"     4. 🔨 [Compile] Compiling: {cc.name} using Docker\n")

    exe = cc.with_suffix(".out")
    
    # Use Docker to compile
    compile_cmd = [
        "g++", "-Wall", "--std=c++11",
        str(cc), LIB_PATH, "-o", str(exe)
    ]
    
    try:
        run_in_docker(compile_cmd)
        return exe
    except subprocess.CalledProcessError as e:
        print(f"Docker compilation failed: {e}")
        raise

def run_policy(exe: Path, trace_path: Path) -> str:
    
    print(f"     5. ⏳ [Simulation] Starting simulation for: {exe.name} and {str(trace_path)}")
    start_time = time.time()

    # Use Docker to run the simulation
    run_cmd = [
        str(exe),
        "-warmup_instructions", WARMUP_INST,
        "-simulation_instructions", SIM_INST,
        "-traces", str(trace_path)
    ]
    
    try:
        result = run_in_docker(run_cmd)
        duration = time.time() - start_time
        print(f"     6. 🏁 [Simulation] Finished in {duration:.2f} seconds for: {exe.name} and {trace_path}")
        return result.stdout
    except subprocess.CalledProcessError as e:
        print(f"Docker simulation failed: {e}")
        raise

def parse_hit_rate(output: str) -> float:
    print("     7. 📊 [Metric] Parsing cache hit rate from output")

    # Try the standard format first
    m = re.search(r"LLC TOTAL\s+ACCESS:\s+(\d+)\s+HIT:\s+(\d+)", output)
    if m:
        access_count = int(m.group(1))
        hit_count = int(m.group(2))
        if access_count > 0:
            hit_rate = hit_count / access_count
            print(f"     📊 [Metric] Found LLC stats: {hit_count}/{access_count} = {hit_rate:.4f}")
            return hit_rate
        else:
            print("     ⚠️  [Warning] Zero LLC accesses found")
            return 0.0
    
    # If not found, show debug info
    print("❌ [Debug] LLC TOTAL not found in output. Here's the last 500 chars:")
    print("=" * 50)
    print(output[-500:])
    print("=" * 50)
    raise RuntimeError("LLC TOTAL not found")

def record(workload, name, desc, cc: Path, rate, workload_desc):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute(
        """
      INSERT INTO experiments
        (workload, policy, policy_description, workload_description,
         cpp_file_path, cache_hit_rate, score)
      VALUES (?, ?, ?, ?, ?, ?, ?)""",
        (workload, name, desc, workload_desc, str(cc), rate, rate),
    )
    conn.commit()
    conn.close()

# ──────────────────────────────────────────────────────────────────────────────
# Main Feedback Loop with Reward/Penalty
# ──────────────────────────────────────────────────────────────────────────────
def main():
    
    WORKLOAD = "all"

    # Check if Docker is available
    try:
        subprocess.run(["docker", "--version"], check=True, capture_output=True)
        print("✅ Docker is available")
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("❌ Docker is not available. Please install Docker to run this script.")
        return

    # 1) Setup RAG and PromptGenerator
    rag = ExperimentRAG(DB_PATH)
    prompt_gen = PolicyPromptGenerator(DB_PATH)
    load_dotenv(dotenv_path=Path(".env"), override=False)

    client = OpenAI(api_key=os.getenv("OPENAI_API_KEY"),)

    top_policies = rag.get_top_policies_by_score(WORKLOAD, top_n=5)
    workload_desc, traces = rag.get_all_workloads_with_description_and_traces()

    best_hit = top_policies[0]["score"]
    policy_summary = "\n".join(
            f"Policy: {p['policy']}\nHit Rate: {float(p['score']):.2%}\nDescription:\n{p['policy_description']}\n"
            for p in top_policies
        )

    print(f"     📈 [Init] Starting best cache hit rate: {best_hit:.2%}")

    prev_name = prev_desc = prev_code = None
    current_hit = best_hit
    i=0
    
    while True:

        if i == 0:
            prompt = (
                f"The following workloads are under consideration:\n"
                f"{workload_desc}\n\n"
                "The top-performing cache replacement policies from past experiments are:\n"
                f"{policy_summary}\n\n"
                "Your task: Propose a new cache replacement policy that aims to **outperform all of the above policies** "
                "across these workloads. Consider workload characteristics like branching, memory access patterns, spatial and temporal locality, and phase behavior.\n\n"
                "Suggested approach:\n"
                "1) Generate 3-4 distinct policy ideas (divergent thinking), briefly explain why each could help with different workloads.\n"
                "2) Choose the most promising policy and provide a complete C++ implementation.\n"
                "3) Include any tunable parameters or knobs, and note what telemetry/statistics should be tracked.\n\n"
                "Use the exact output format below:\n\n"
                "## Policy Name\n<name>\n\n"
                "## Policy Description\n<one paragraph describing the approach and why it helps>\n\n"
                "## C++ Implementation\n"
                f"{prompt_gen._get_code_template()}\n"
            )
            
        else:
            if current_hit > best_hit:
                feedback = (
                    f"Great! Policy improved from {best_hit:.2%} to "
                    f"{current_hit:.2%}. Please refine further."
                )
                best_hit = current_hit
            else:
                feedback = (
                    f"Policy hit rate was {current_hit:.2%}, not better than "
                    f"{best_hit:.2%}. Try a different approach."
                )

            prompt = (
                f"The following workloads are under consideration:\n"
                f"{workload_desc}\n\n"
                f"Your previous design was **{prev_name}**:\n\n"
                f"Description:\n{prev_desc}\n\n"
                f"Implementation:\n```cpp\n{prev_code}\n```\n\n"
                f"Feedback from the last run:\n{feedback}\n\n"
                "Task: Refine or redesign the policy to achieve better performance across all workloads. "
                "Consider workload characteristics such as branching behavior, memory access patterns, spatial and temporal locality, and phase changes. "
                "You may propose modifications, hybrid approaches, or completely new ideas if needed.\n\n"
                "Produce the output in the exact format below:\n\n"
                "## Policy Name\n<name>\n\n"
                "## Policy Description\n<one paragraph explaining the approach and why it improves performance>\n\n"
                "## C++ Implementation\n"
                f"{prompt_gen._get_code_template()}\n"
            )

        
        # 5) Call model
        resp = client.responses.create(
            model=MODEL,
            reasoning={"effort": "high"},
            input=prompt,
        )
        print(f"     1. 📤 [LLM] Iteration {i}: Sending prompt to model")

        text = resp.output_text
        print("     2. 📥 [LLM] Response received from OpenAI")

        # 6) Parse LLM output
        name, desc, code = parse_policy_content(text)
        if not (name and desc and code):
            raise RuntimeError(f"❌ Parse failed")

        # 7) Write, compile, run
        base = sanitize(name)
        cc = EXAMPLE_DIR / f"{i:03}_{base}.cc"
        cc.write_text(code, encoding="utf-8")
       
        try:
            exe = compile_policy(cc)
        except subprocess.CalledProcessError as e:
            print(f"❌ [Compile Error]:\n{e}")
            continue  # ← this restarts the loop at the top

        current_hit_tmp=0
        
        for trace_info in workloads:
            WORKLOAD = trace_info["name"]
            trace_path = trace_info["trace_path"]

            out = run_policy(exe, trace_path)
            tmp = parse_hit_rate(out)
            current_hit_tmp += tmp
            record(WORKLOAD, name, desc, cc, tmp, "")
            print(f"      [+] {name} → workload: {WORKLOAD} → hit rate: {tmp}\n")

        current_hit = current_hit_tmp / len(workloads)
        print(f"✅ [Result] Iteration {i}: {name}  → average hit rate {current_hit:.2%}\n")

        # 8) Record experiment
        record("all",name, desc, cc, current_hit, "")

        i+=1

        if current_hit/best_hit > 1.3:
            break

        # 9) Prepare for next iteration
        prev_name, prev_desc, prev_code = name, desc, code

    prompt_gen.close()
    rag.close()


if __name__ == "__main__":
    main()