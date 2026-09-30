"""Train the duel policy with PPO. Example:

    python3 scripts/train.py --out runs/sword --updates 300 --weapons diamond_sword
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcporl.ppo import PPOConfig, train  # noqa: E402

p = argparse.ArgumentParser()
p.add_argument("--out", default="runs/default")
p.add_argument("--updates", type=int, default=300)
p.add_argument("--duels", type=int, default=256)
p.add_argument("--rollout", type=int, default=128)
p.add_argument("--weapons", default="diamond_sword")
p.add_argument("--self-play", type=float, default=0.5)
p.add_argument("--eval-every", type=int, default=10)
p.add_argument("--seed", type=int, default=1)
a = p.parse_args()
cfg = PPOConfig(n_duels=a.duels, rollout=a.rollout, updates=a.updates, weapons=tuple(a.weapons.split(",")),
                self_play=a.self_play, eval_every=a.eval_every, seed=a.seed)
train(cfg, Path(a.out), log=lambda s: print(s, flush=True))
