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
p.add_argument("--init", default="", help="start from this checkpoint")
p.add_argument("--league", type=float, default=0.0, help="share of non-self-play duels against past snapshots")
p.add_argument("--tactician", type=float, default=0.0, help="share of scripted duels against the tactician")
p.add_argument("--expert", type=float, default=0.0, help="share of scripted duels against the cheating expert panel")
p.add_argument("--dummy", type=float, default=0.3)
p.add_argument("--aids", type=float, default=0.05, help="dense aim and hit-range aids at the start (annealed)")
p.add_argument("--lr", type=float, default=3e-4)
p.add_argument("--hotbar", default="", help="learner hotbar, comma-separated (slot 0 held at the start)")
p.add_argument("--offhand", default="")
p.add_argument("--opp-hotbar", default="", help="opponents' hotbar (default: one weapon from --weapons)")
p.add_argument("--opp-offhand", default="")
p.add_argument("--slot-keys", type=int, default=0, help="hotbar keys the policy can press (slots 0..k-1)")
p.add_argument("--use-key", action="store_true", help="give the policy the use key")
p.add_argument("--shielder", type=float, default=0.0, help="share of scripted duels against the shield user")
p.add_argument("--eval", default="", help="evaluation opponents, comma-separated (default: the standard set)")
a = p.parse_args()
split = lambda v: tuple(x for x in v.split(",") if x)
cfg = PPOConfig(n_duels=a.duels, rollout=a.rollout, updates=a.updates, weapons=tuple(a.weapons.split(",")),
                self_play=a.self_play, eval_every=a.eval_every, seed=a.seed, init_checkpoint=a.init,
                league=a.league, tactician=a.tactician, expert=a.expert, dummy=a.dummy,
                aim_dense=a.aids, reach_dense=a.aids, lr=a.lr, hotbar=split(a.hotbar), offhand=a.offhand,
                opp_hotbar=split(a.opp_hotbar), opp_offhand=a.opp_offhand, slot_keys=a.slot_keys,
                use_key=a.use_key, shielder=a.shielder, eval_opponents=split(a.eval))
train(cfg, Path(a.out), log=lambda s: print(s, flush=True))
