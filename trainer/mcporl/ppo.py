"""PPO for the duel (docs/TRAINING.md).

One policy plays every learner slot. Duels are split between self-play (both
slots are the learner) and scripted opponents (an aim bot of random skill, or
a dummy), with the learner alternating between slot 0 and slot 1 so neither
side of the server's packet order is favoured.

The action is factored: categorical heads for the keys and the click, and a
Gaussian for the turn (degrees per tick, the fairness cap still applies in
the simulator).
"""
from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

from .config import RewardConfig
from .env import DuelEnv, slot_stats

# Heads: forward/back, left/right (3 each: -1, 0, 1), jump, sprint, click (2 each).
CAT_SIZES = (3, 3, 2, 2, 2)
TURN_SCALE = 45.0  # the turn mean saturates at +-45 degrees per tick


@dataclass
class PPOConfig:
    n_duels: int = 256
    rollout: int = 128
    updates: int = 300
    epochs: int = 4
    minibatches: int = 8
    lr: float = 3e-4
    clip: float = 0.2
    ent_coef: float = 0.01
    vf_coef: float = 0.5
    max_grad_norm: float = 0.5
    lam: float = 0.95
    hidden: int = 256
    self_play: float = 0.5      # share of duels where both slots are the learner
    dummy: float = 0.3          # share of scripted duels against a standing dummy
    max_ticks: int = 600
    start_dist: tuple[float, float] = (2.5, 6.0)
    weapons: tuple[str, ...] = ("diamond_sword",)
    seed: int = 1
    eval_every: int = 10
    eval_episodes: int = 256
    reward: RewardConfig = field(default_factory=RewardConfig)
    # Dense training aids (reward.hpp): per tick, x the aim potential and while
    # in hit range. They make the first hits discoverable but can be farmed,
    # so they fall linearly to 0 by `anneal` x updates (docs/REWARD.md).
    aim_dense: float = 0.05
    reach_dense: float = 0.05
    anneal: float = 0.6


class RunningNorm:
    def __init__(self, size: int):
        self.mean = np.zeros(size, np.float64)
        self.var = np.ones(size, np.float64)
        self.count = 1e-4

    def update(self, x: np.ndarray) -> None:
        m, v, n = x.mean(0), x.var(0), x.shape[0]
        d = m - self.mean
        tot = self.count + n
        self.mean = self.mean + d * n / tot
        self.var = (self.var * self.count + v * n + d * d * self.count * n / tot) / tot
        self.count = tot

    def __call__(self, x: np.ndarray) -> np.ndarray:
        return ((x - self.mean) / np.sqrt(self.var + 1e-8)).clip(-10, 10).astype(np.float32)

    def state(self) -> dict:
        return {"mean": self.mean.tolist(), "var": self.var.tolist(), "count": self.count}


def mlp(i: int, h: int, o: int) -> nn.Sequential:
    return nn.Sequential(nn.Linear(i, h), nn.Tanh(), nn.Linear(h, h), nn.Tanh(), nn.Linear(h, o))


class Policy(nn.Module):
    def __init__(self, obs_size: int, hidden: int):
        super().__init__()
        self.actor = mlp(obs_size, hidden, sum(CAT_SIZES) + 2)
        self.critic = mlp(obs_size, hidden, 1)
        self.log_std = nn.Parameter(torch.full((2,), float(np.log(8.0 / TURN_SCALE))))

    def dists(self, obs: torch.Tensor):
        out = self.actor(obs)
        logits = torch.split(out[:, :sum(CAT_SIZES)], CAT_SIZES, dim=1)
        cats = [torch.distributions.Categorical(logits=l) for l in logits]
        mean = torch.tanh(out[:, sum(CAT_SIZES):])
        turn = torch.distributions.Normal(mean, self.log_std.exp().expand_as(mean))
        return cats, turn

    def act(self, obs: torch.Tensor, deterministic: bool = False):
        cats, turn = self.dists(obs)
        if deterministic:
            c = torch.stack([d.probs.argmax(1) for d in cats], 1)
            t = turn.mean
        else:
            c = torch.stack([d.sample() for d in cats], 1)
            t = turn.sample()
        logp = sum(d.log_prob(c[:, j]) for j, d in enumerate(cats)) + turn.log_prob(t).sum(1)
        return c, t, logp, self.critic(obs).squeeze(1)

    def evaluate(self, obs, c, t):
        cats, turn = self.dists(obs)
        logp = sum(d.log_prob(c[:, j]) for j, d in enumerate(cats)) + turn.log_prob(t).sum(1)
        ent = sum(d.entropy() for d in cats) + turn.entropy().sum(1)
        return logp, ent, self.critic(obs).squeeze(1)


def to_env_actions(c: np.ndarray, t: np.ndarray) -> np.ndarray:
    a = np.zeros((c.shape[0], 7), np.float32)
    a[:, 0] = c[:, 0] - 1
    a[:, 1] = c[:, 1] - 1
    a[:, 2:5] = c[:, 2:5]
    a[:, 5:7] = np.clip(t, -4.0, 4.0) * TURN_SCALE
    return a


class Opponents:
    """Which slots the learner plays, and what the scripted slots do."""

    def __init__(self, n: int, cfg: PPOConfig, rng: np.random.Generator):
        self.n = n
        self.learner = np.zeros(2 * n, bool)
        # Per slot: 0 dummy, 1 aim bot (used on scripted slots only)
        self.kind = np.zeros(2 * n, np.int32)
        self.noise = np.zeros(2 * n, np.float32)
        self.turn = np.zeros(2 * n, np.float32)
        n_self = int(round(n * cfg.self_play))
        for i in range(n):
            if i < n_self:
                self.learner[2 * i] = self.learner[2 * i + 1] = True
            else:
                self.learner[2 * i + (i % 2)] = True
        self.cfg = cfg
        self.rng = rng
        for i in range(n):
            self.redraw(i)

    def redraw(self, i: int) -> None:
        """A new opponent for duel i's next episode: a random skill level."""
        s = slice(2 * i, 2 * i + 2)
        self.kind[s] = 0 if self.rng.random() < self.cfg.dummy else 1
        self.noise[s] = self.rng.uniform(0.5, 12.0)
        self.turn[s] = self.rng.uniform(6.0, 60.0)

    def fill(self, env: DuelEnv, actions: np.ndarray) -> None:
        scripted = ~self.learner
        if scripted.any():
            env.scripted_each(scripted, self.kind, self.noise, self.turn, actions)


def evaluate(policy: Policy, norm: RunningNorm, cfg: PPOConfig, opponent: str, seed: int,
             frozen: Policy | None = None) -> dict:
    """Deterministic learner vs a fixed opponent, the learner on both sides equally.

    opponent: "dummy", "bot_easy", "bot_medium", "bot_hard", or "frozen" (an earlier policy)."""
    levels = {"dummy": (0, 0.0, 30.0), "bot_easy": (1, 8.0, 12.0), "bot_medium": (1, 4.0, 25.0),
              "bot_hard": (1, 1.5, 45.0)}
    n = cfg.eval_episodes  # one episode per duel: counting the first to finish would favour short ones
    env = DuelEnv(n, seed=seed, max_ticks=cfg.max_ticks, weapons=cfg.weapons, reward=cfg.reward)
    learner = np.zeros(2 * n, bool)
    learner[np.arange(n) * 2 + (np.arange(n) % 2)] = True
    obs = env.observe()
    agg = {"episodes": 0, "wins": 0, "losses": 0, "draws": 0, "clicks": 0.0, "attacks": 0.0, "hits": 0.0,
           "damage_dealt": 0.0, "damage_taken": 0.0, "aim_error_sum": 0.0, "aim_ticks": 0.0,
           "advantage_ticks": 0.0, "disadvantage_ticks": 0.0, "ticks": 0.0}
    actions = np.zeros((2 * n, 7), np.float32)
    finished = np.zeros(n, bool)
    while not finished.all():
        with torch.no_grad():
            o = torch.from_numpy(norm(obs))
            c, t, _, _ = policy.act(o, deterministic=True)
            a_learn = to_env_actions(c.numpy(), t.numpy())
            if opponent == "frozen":
                c2, t2, _, _ = frozen.act(o, deterministic=True)
                a_opp = to_env_actions(c2.numpy(), t2.numpy())
        actions[learner] = a_learn[learner]
        if opponent == "frozen":
            actions[~learner] = a_opp[~learner]
        else:
            kind, noise, turn = levels[opponent]
            env.scripted((~learner).astype(np.uint8), kind, noise, turn, actions)
        obs, _, done, stats = env.step(actions)
        for i in np.nonzero(done)[0]:
            if finished[i]:
                continue
            finished[i] = True
            k = i % 2  # the learner's slot in duel i
            agg["episodes"] += 1
            w = int(stats[i, 0])
            agg["wins" if w == k else ("draws" if w < 0 else "losses")] += 1
            agg["ticks"] += float(stats[i, 1])
            for key, v in slot_stats(stats[i], k).items():
                agg[key] += v
    env.close()
    e = agg["episodes"]
    return {
        "opponent": opponent, "episodes": e,
        "win_rate": agg["wins"] / e, "loss_rate": agg["losses"] / e, "draw_rate": agg["draws"] / e,
        "hits_per_click": agg["hits"] / max(agg["clicks"], 1.0),
        "attacks_per_click": agg["attacks"] / max(agg["clicks"], 1.0),
        "clicks_per_episode": agg["clicks"] / e,
        "damage_dealt": agg["damage_dealt"] / e, "damage_taken": agg["damage_taken"] / e,
        "aim_error_deg": agg["aim_error_sum"] / max(agg["aim_ticks"], 1.0),
        "advantage_share": agg["advantage_ticks"] / max(agg["aim_ticks"], 1.0),
        "disadvantage_share": agg["disadvantage_ticks"] / max(agg["aim_ticks"], 1.0),
        "episode_ticks": agg["ticks"] / e,
    }


def train(cfg: PPOConfig, out_dir: Path, log=print) -> Policy:
    out_dir.mkdir(parents=True, exist_ok=True)
    torch.manual_seed(cfg.seed)
    torch.set_num_threads(max(1, torch.get_num_threads()))
    rng = np.random.default_rng(cfg.seed)
    env = DuelEnv(cfg.n_duels, seed=cfg.seed, max_ticks=cfg.max_ticks, weapons=cfg.weapons, reward=cfg.reward,
                  start_dist=cfg.start_dist)
    opp = Opponents(cfg.n_duels, cfg, rng)
    policy = Policy(env.obs_size, cfg.hidden)
    optim = torch.optim.Adam(policy.parameters(), lr=cfg.lr, eps=1e-5)
    norm = RunningNorm(env.obs_size)
    gamma = cfg.reward.gamma
    (out_dir / "config.json").write_text(json.dumps(asdict(cfg), indent=1, default=str))
    metrics = open(out_dir / "metrics.jsonl", "a")
    snapshots: list[dict] = []

    obs = env.observe()
    norm.update(obs)
    S, T = 2 * cfg.n_duels, cfg.rollout
    actions = np.zeros((S, 7), np.float32)
    samples = 0
    t_start = time.time()
    for update in range(1, cfg.updates + 1):
        aid = max(0.0, 1.0 - (update - 1) / max(cfg.anneal * cfg.updates, 1.0))
        env.set_reward(cfg.reward, cfg.aim_dense * aid, cfg.reach_dense * aid)
        buf_obs = np.zeros((T, S, env.obs_size), np.float32)
        buf_c = np.zeros((T, S, len(CAT_SIZES)), np.int64)
        buf_t = np.zeros((T, S, 2), np.float32)
        buf_logp = np.zeros((T, S), np.float32)
        buf_v = np.zeros((T, S), np.float32)
        buf_r = np.zeros((T, S), np.float32)
        buf_d = np.zeros((T, S), np.float32)
        train_eps = {"n": 0, "win": 0, "dmg": 0.0}
        for step in range(T):
            o = norm(obs)
            with torch.no_grad():
                c, t, logp, v = policy.act(torch.from_numpy(o))
            c, t = c.numpy(), t.numpy()
            actions[:] = to_env_actions(c, t)
            opp.fill(env, actions)
            buf_obs[step], buf_c[step], buf_t[step] = o, c, t
            buf_logp[step], buf_v[step] = logp.numpy(), v.numpy()
            obs, r, done, stats = env.step(actions)
            norm.update(obs)
            buf_r[step] = r
            ended = np.repeat(done > 0, 2)
            buf_d[step] = ended
            for i in np.nonzero(done)[0]:
                opp.redraw(i)
                for k in (0, 1):
                    if opp.learner[2 * i + k]:
                        train_eps["n"] += 1
                        train_eps["win"] += int(stats[i, 0]) == k
                        train_eps["dmg"] += slot_stats(stats[i], k)["damage_dealt"]
        with torch.no_grad():
            last_v = policy.critic(torch.from_numpy(norm(obs))).squeeze(1).numpy()
        adv = np.zeros((T, S), np.float32)
        gae = np.zeros(S, np.float32)
        for step in reversed(range(T)):
            nv = last_v if step == T - 1 else buf_v[step + 1]
            nonterm = 1.0 - buf_d[step]
            delta = buf_r[step] + gamma * nv * nonterm - buf_v[step]
            gae = delta + gamma * cfg.lam * nonterm * gae
            adv[step] = gae
        ret = adv + buf_v
        sel = np.broadcast_to(opp.learner, (T, S)).reshape(-1)
        b_obs = torch.from_numpy(buf_obs.reshape(-1, env.obs_size)[sel])
        b_c = torch.from_numpy(buf_c.reshape(-1, len(CAT_SIZES))[sel])
        b_t = torch.from_numpy(buf_t.reshape(-1, 2)[sel])
        b_logp = torch.from_numpy(buf_logp.reshape(-1)[sel])
        b_adv = torch.from_numpy(adv.reshape(-1)[sel])
        b_ret = torch.from_numpy(ret.reshape(-1)[sel])
        N = b_obs.shape[0]
        samples += N
        mb = N // cfg.minibatches
        for _ in range(cfg.epochs):
            perm = torch.randperm(N)
            for j in range(cfg.minibatches):
                idx = perm[j * mb:(j + 1) * mb]
                logp, ent, v = policy.evaluate(b_obs[idx], b_c[idx], b_t[idx])
                a = b_adv[idx]
                a = (a - a.mean()) / (a.std() + 1e-8)
                ratio = (logp - b_logp[idx]).exp()
                pg = -torch.min(ratio * a, ratio.clamp(1 - cfg.clip, 1 + cfg.clip) * a).mean()
                vf = 0.5 * (v - b_ret[idx]).pow(2).mean()
                loss = pg + cfg.vf_coef * vf - cfg.ent_coef * ent.mean()
                optim.zero_grad()
                loss.backward()
                nn.utils.clip_grad_norm_(policy.parameters(), cfg.max_grad_norm)
                optim.step()
        row = {"update": update, "samples": samples, "seconds": round(time.time() - t_start, 1),
               "train_episodes": train_eps["n"],
               "train_win_rate": train_eps["win"] / max(train_eps["n"], 1),
               "train_damage": train_eps["dmg"] / max(train_eps["n"], 1),
               "policy_loss": float(pg.detach()), "value_loss": float(vf.detach()),
               "entropy": float(ent.mean().detach()),
               "turn_std_deg": (policy.log_std.exp() * TURN_SCALE).tolist(), "aid": aid}
        if update % cfg.eval_every == 0 or update == cfg.updates:
            policy.eval()
            row["eval"] = [evaluate(policy, norm, cfg, o, seed=1000 + update)
                           for o in ("dummy", "bot_easy", "bot_medium", "bot_hard")]
            if snapshots:
                frozen = Policy(env.obs_size, cfg.hidden)
                frozen.load_state_dict(snapshots[0]["state"])
                ev = evaluate(policy, norm, cfg, "frozen", seed=2000 + update, frozen=frozen)
                ev["frozen_update"] = snapshots[0]["update"]
                row["eval"].append(ev)
            policy.train()
            torch.save({"policy": policy.state_dict(), "norm": norm.state(), "config": asdict(cfg),
                        "update": update}, out_dir / "checkpoint.pt")
            snapshots.append({"update": update, "state": {k: v.clone() for k, v in policy.state_dict().items()}})
            snapshots = snapshots[-5:]  # compare against the policy of ~5 evaluations ago
        metrics.write(json.dumps(row) + "\n")
        metrics.flush()
        log(summary(row))
    env.close()
    metrics.close()
    return policy


def summary(row: dict) -> str:
    s = (f"upd {row['update']:4d} samples {row['samples'] / 1e6:6.2f}M {row['seconds']:7.0f}s  "
         f"train win {row['train_win_rate']:.2f} dmg {row['train_damage']:5.1f} ent {row['entropy']:.2f}")
    for e in row.get("eval", []):
        s += (f"\n    vs {e['opponent']:10s} win {e['win_rate']:.2f} draw {e['draw_rate']:.2f} "
              f"hits/click {e['hits_per_click']:.2f} aim {e['aim_error_deg']:5.1f}deg "
              f"dmg {e['damage_dealt']:5.1f}/{e['damage_taken']:5.1f} adv {e['advantage_share']:.2f}")
    return s
