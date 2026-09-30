"""Record episodes of a trained policy and export them as oracle scenarios.

    python3 scripts/export_replays.py runs/sword/checkpoint.pt --count 6 --out ../oracle/replays

Half the episodes are against the hard aim bot (the learner alternating
between slots), half self-play. Episodes whose outcome depended on a random
draw vanilla makes from unreproducible state (knockback between players on
the same spot) are skipped. Each exported file is replayed locally with
duel_replay as a round-trip check; the oracle-traces workflow then replays it
on the real server and the replay_* tests require the simulator to match.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcporl.env import REPO, SIN_TABLE, DuelEnv, _find_library  # noqa: E402
from mcporl.ppo import Policy, RunningNorm, to_env_actions  # noqa: E402

p = argparse.ArgumentParser()
p.add_argument("checkpoint")
p.add_argument("--count", type=int, default=6)
p.add_argument("--out", default=str(REPO / "oracle" / "replays"))
p.add_argument("--prefix", default="r")
p.add_argument("--seed", type=int, default=7)
a = p.parse_args()

ck = torch.load(a.checkpoint, weights_only=False)
cfg = ck["config"]
env = DuelEnv(a.count, seed=a.seed, max_ticks=cfg["max_ticks"], weapons=tuple(cfg["weapons"]))
policy = Policy(env.obs_size, cfg["hidden"])
policy.load_state_dict(ck["policy"])
policy.eval()
norm = RunningNorm(env.obs_size)
norm.mean, norm.var = np.array(ck["norm"]["mean"]), np.array(ck["norm"]["var"])

n = a.count
vs_bot = np.arange(n) < n // 2
learner = np.ones(2 * n, bool)
for i in np.nonzero(vs_bot)[0]:
    learner[2 * i + 1 - (i % 2)] = False  # the bot's slot
for i in range(n):
    env.record(i)
obs = env.observe()
actions = np.zeros((2 * n, 7), np.float32)
outcome = {}
while any(env.record_state(i) == 1 for i in range(n)):
    with torch.no_grad():
        c, t, _, _ = policy.act(torch.from_numpy(norm(obs)), deterministic=False)
    actions[:] = to_env_actions(c.numpy(), t.numpy())
    env.scripted((~learner).astype(np.uint8), 1, 1.5, 45.0, actions)
    obs, _, done, stats = env.step(actions)
    for i in np.nonzero(done)[0]:
        if i not in outcome:
            outcome[i] = (int(stats[i, 0]), int(stats[i, 1]), int(done[i]))

out = Path(a.out)
out.mkdir(parents=True, exist_ok=True)
duel_replay = _find_library().parent / "duel_replay"
written = []
for i in range(n):
    state, nonparity = env.record_state(i), env.record_nonparity(i)
    if nonparity != 0:
        print(f"duel {i}: skipped ({nonparity} unreproducible random draw(s) or unsupported state)")
        continue
    winner, ticks, how = outcome[i]
    kind = "bot" if vs_bot[i] else "self"
    who = {-1: "draw", 0: "A wins", 1: "B wins"}[winner]
    end = "death" if state == 2 else ("arena exit" if how == 1 else "time limit")
    name = f"{a.prefix}{i:02d}_{kind}"
    title = (f"Recorded episode: {Path(a.checkpoint).parent.name} update {ck['update']}, "
             f"{'learner vs hard aim bot' if vs_bot[i] else 'self-play'}; {who} after {ticks} ticks ({end})")
    path = out / f"{name}.txt"
    env.export(i, path, title)
    # Round trip: the exported text must replay to the same length in the simulator.
    with tempfile.TemporaryDirectory() as tmp:
        trace = Path(tmp) / "t.jsonl"
        subprocess.run([str(duel_replay), str(SIN_TABLE), str(path), str(trace)], check=True)
        lines = trace.read_text().splitlines()[1:]
    if len(lines) != ticks:
        raise SystemExit(f"{path}: replayed {len(lines)} ticks, the episode had {ticks}")
    written.append({"file": path.name, "winner": winner, "ticks": ticks, "end": end})
    print(f"{path.name}: {title}")
print(json.dumps(written, indent=1))
