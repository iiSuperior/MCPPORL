"""PPO for the duel (docs/TRAINING.md).

One policy plays every learner slot. Duels are split between self-play (both
slots are the learner) and scripted opponents (an aim bot of random skill, or
a dummy), with the learner alternating between slot 0 and slot 1 so neither
side of the server's packet order is favoured.

The action is factored: categorical heads for the keys and the click
(optionally a hotbar-key head and a use-key head), and a Gaussian for the
turn (degrees per tick, the fairness cap still applies in the simulator).
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
from .env import CHEAT_RANGE_HIT, CHEAT_SNAP_AIM, CHEAT_TRUE_SIGHT, SLOT_STATS, DuelEnv, slot_stats

# Heads: forward/back, left/right (3 each: -1, 0, 1), jump, sprint, click (2 each);
# then, if enabled, a hotbar key (none, slot 0, ..., slot k-1) and the use key (2).
CAT_SIZES = (3, 3, 2, 2, 2)


def cat_sizes(slot_keys: int = 0, use_key: bool = False) -> tuple[int, ...]:
    return CAT_SIZES + ((1 + slot_keys,) if slot_keys else ()) + ((2,) if use_key else ())
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
    # Start from a trained checkpoint (its policy and observation statistics).
    init_checkpoint: str = ""
    # Opponent mix of the duels that are not self-play: a share plays a past
    # snapshot of the learner (a league: the initial checkpoint and a snapshot
    # every `league_every` updates); the rest are scripted, split between the
    # tactician (a port of the PvP Bot mod's melee), the expert panel (the
    # tactician with opponent-only cheats: snap aim, true sight, range hit),
    # the aim bot, and the dummy.
    league: float = 0.0
    league_every: int = 20
    tactician: float = 0.0
    expert: float = 0.0
    # Items (weapons.hpp names): the learner's hotbar (slot 0 selected) and off
    # hand, and the opponents'; empty: one weapon drawn from `weapons`.
    hotbar: tuple[str, ...] = ()
    offhand: str = ""
    opp_hotbar: tuple[str, ...] = ()
    opp_offhand: str = ""
    # Extra action heads: hotbar keys for slots 0..slot_keys-1, and the use key.
    slot_keys: int = 0
    use_key: bool = False
    # Share of scripted duels against the shield user, its range of per-tick
    # chances to lower the shield voluntarily, the share of shield users that
    # never lower it, and the range of health at or below which they panic
    # (never lower it again; 0: never panic).
    shielder: float = 0.0
    shield_lower: tuple[float, float] = (0.01, 0.06)
    shield_never: float = 0.0
    shield_panic: tuple[float, float] = (0.0, 0.0)
    # Logit bias of "no key" in heads added to an init checkpoint (4: ~96% at first).
    head_bias: float = 4.0
    # Evaluation opponents (keys of EVAL_OPPONENTS); empty: the standard set.
    eval_opponents: tuple[str, ...] = ()


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
    def __init__(self, obs_size: int, hidden: int, slot_keys: int = 0, use_key: bool = False):
        super().__init__()
        self.slot_keys, self.use_key = slot_keys, use_key
        self.cat_sizes = cat_sizes(slot_keys, use_key)
        self.actor = mlp(obs_size, hidden, sum(self.cat_sizes) + 2)
        self.critic = mlp(obs_size, hidden, 1)
        self.log_std = nn.Parameter(torch.full((2,), float(np.log(8.0 / TURN_SCALE))))

    def env_actions(self, c: np.ndarray, t: np.ndarray) -> np.ndarray:
        return to_env_actions(c, t, self.slot_keys, self.use_key)

    def dists(self, obs: torch.Tensor):
        out = self.actor(obs)
        n = sum(self.cat_sizes)
        logits = torch.split(out[:, :n], self.cat_sizes, dim=1)
        cats = [torch.distributions.Categorical(logits=l) for l in logits]
        mean = torch.tanh(out[:, n:])
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


ACTION_SIZE = 9


def to_env_actions(c: np.ndarray, t: np.ndarray, slot_keys: int = 0, use_key: bool = False) -> np.ndarray:
    a = np.zeros((c.shape[0], ACTION_SIZE), np.float32)
    a[:, 0] = c[:, 0] - 1
    a[:, 1] = c[:, 1] - 1
    a[:, 2:5] = c[:, 2:5]
    a[:, 5:7] = np.clip(t, -4.0, 4.0) * TURN_SCALE
    a[:, 7] = c[:, 5] - 1 if slot_keys else -1  # head value 0: no key
    if use_key:
        a[:, 8] = c[:, -1]
    return a


def load_policy(ck: dict, obs_size: int, slot_keys: int | None = None, use_key: bool | None = None,
                none_bias: float = 4.0) -> tuple["Policy", "RunningNorm"]:
    """A checkpoint's policy and observation statistics, grown to `obs_size`
    observations and the requested heads if it has fewer: new inputs get zero
    weights (mean 0, variance 1), new heads zero weights with a bias towards
    pressing nothing, so the grown policy acts exactly as before at first."""
    cfg = ck["config"]
    old_slots, old_use = int(cfg.get("slot_keys", 0)), bool(cfg.get("use_key", False))
    slot_keys = old_slots if slot_keys is None else slot_keys
    use_key = old_use if use_key is None else use_key
    policy = Policy(obs_size, cfg["hidden"], slot_keys, use_key)
    state = {k: v.clone() for k, v in ck["policy"].items()}
    old_obs = state["actor.0.weight"].shape[1]
    for net in ("actor", "critic"):
        w = state[f"{net}.0.weight"]
        state[f"{net}.0.weight"] = torch.cat([w, torch.zeros(w.shape[0], obs_size - old_obs)], 1)
    old_cats, new_cats = cat_sizes(old_slots, old_use), policy.cat_sizes
    if new_cats != old_cats:
        if old_cats != CAT_SIZES:
            raise ValueError(f"can only add heads to a policy with the base heads, not {old_cats}")
        w, b = state["actor.4.weight"], state["actor.4.bias"]
        base = sum(CAT_SIZES)
        extra_w, extra_b = [], []
        for size in new_cats[len(CAT_SIZES):]:
            extra_w.append(torch.zeros(size, w.shape[1]))
            nb = torch.zeros(size)
            nb[0] = none_bias  # no hotbar key / use key up
            extra_b.append(nb)
        state["actor.4.weight"] = torch.cat([w[:base], *extra_w, w[base:]])
        state["actor.4.bias"] = torch.cat([b[:base], *extra_b, b[base:]])
    policy.load_state_dict(state)
    norm = RunningNorm(obs_size)
    m, v = np.array(ck["norm"]["mean"]), np.array(ck["norm"]["var"])
    norm.mean[:len(m)], norm.var[:len(v)] = m, v
    norm.count = float(ck["norm"]["count"])
    return policy, norm


def set_loadouts(env: DuelEnv, learner: np.ndarray, cfg: "PPOConfig") -> None:
    """Give the learner's and the opponents' slots their configured items and restart the duels."""
    for s in range(2 * env.n):
        hotbar, off = (cfg.hotbar, cfg.offhand) if learner[s] else (cfg.opp_hotbar, cfg.opp_offhand)
        env.set_loadout(s, hotbar if (hotbar or off) else None, off)
    for i in range(env.n):
        env.reset_duel(i)


class Opponents:
    """Which slots the learner plays, and who plays the others.

    Duel i is self-play for i < self_play * n (both slots learn); otherwise the
    learner is slot i % 2 and, per episode, the other slot is drawn: a league
    snapshot, or a scripted opponent (tactician, expert panel, aim bot,
    dummy) with random skill."""

    LEAGUE, DUMMY, AIM, TACT, EXPERT, SHIELD = 0, 1, 2, 3, 4, 5

    def __init__(self, n: int, cfg: PPOConfig, rng: np.random.Generator):
        self.n = n
        self.learner = np.zeros(2 * n, bool)
        self.who = np.full(n, self.AIM, np.int32)
        self.noise = np.zeros(2 * n, np.float32)
        self.turn = np.zeros(2 * n, np.float32)
        self.cheats = np.zeros(2 * n, np.uint32)
        self.crits = np.zeros(2 * n, np.uint8)
        self.lower = np.zeros(2 * n, np.float32)
        self.panic = np.zeros(2 * n, np.float32)
        n_self = int(round(n * cfg.self_play))
        self.self_play = np.arange(n) < n_self
        for i in range(n):
            if self.self_play[i]:
                self.learner[2 * i] = self.learner[2 * i + 1] = True
            else:
                self.learner[2 * i + (i % 2)] = True
        self.cfg = cfg
        self.rng = rng
        for i in range(n):
            self.redraw(i)

    def redraw(self, i: int) -> None:
        """A new opponent for duel i's next episode."""
        if self.self_play[i]:
            return
        c = self.cfg
        if self.rng.random() < c.league:
            self.who[i] = self.LEAGUE
        else:
            r = self.rng.random()
            if r < c.tactician:
                self.who[i] = self.TACT
            elif r < c.tactician + c.expert:
                self.who[i] = self.EXPERT
            elif r < c.tactician + c.expert + c.dummy:
                self.who[i] = self.DUMMY
            elif r < c.tactician + c.expert + c.dummy + c.shielder:
                self.who[i] = self.SHIELD
            else:
                self.who[i] = self.AIM
        s = slice(2 * i, 2 * i + 2)
        self.noise[s] = self.rng.uniform(0.5, 12.0) if self.who[i] != self.EXPERT else 0.0
        self.turn[s] = self.rng.uniform(6.0, 60.0)
        self.crits[s] = self.rng.random() < 0.5
        self.lower[s] = 0.0 if self.rng.random() < c.shield_never else self.rng.uniform(*c.shield_lower)
        self.panic[s] = self.rng.uniform(*c.shield_panic)
        cheats = 0
        if self.who[i] == self.EXPERT:
            while cheats == 0:  # at least one cheat, any combination
                cheats = int(self.rng.integers(0, 8))
        self.cheats[s] = cheats

    def fill(self, env: DuelEnv, actions: np.ndarray, obs_norm: np.ndarray, league: "Policy | None") -> None:
        other = ~self.learner
        who = np.repeat(self.who, 2)
        aim = other & ((who == self.AIM) | (who == self.DUMMY))
        if aim.any():
            kind = np.where(who == self.DUMMY, 0, 1).astype(np.int32)
            env.scripted_each(aim, kind, self.noise, self.turn, actions)
        tact = other & ((who == self.TACT) | (who == self.EXPERT))
        if tact.any():
            env.tactician_each(tact, self.noise, self.turn, self.cheats, self.crits, actions)
        shield = other & (who == self.SHIELD)
        if shield.any():
            env.shielder_each(shield, self.noise, self.turn, self.lower, self.panic, actions)
        past = other & (who == self.LEAGUE)
        if past.any():
            if league is None:
                raise RuntimeError("league opponents need a league policy")
            with torch.no_grad():
                c, t, _, _ = league.act(torch.from_numpy(obs_norm[past]))
            actions[past] = league.env_actions(c.numpy(), t.numpy())


# Evaluation opponents: (kind, noise, turn, cheats, crits); kind "aim", "dummy" or "tact";
# kind "shield": (kind, noise, turn, chance per tick to lower the shield, panic health).
# "expert" cheats (opponent only): report it apart from the fair ones.
EVAL_OPPONENTS = {
    "dummy": ("dummy", 0.0, 30.0, 0, 0),
    "bot_easy": ("aim", 8.0, 12.0, 0, 0),
    "bot_medium": ("aim", 4.0, 25.0, 0, 0),
    "bot_hard": ("aim", 1.5, 45.0, 0, 0),
    "tactician": ("tact", 1.5, 45.0, 0, 0),
    "tactician_crits": ("tact", 1.5, 45.0, 0, 1),
    "expert": ("tact", 0.0, 45.0, CHEAT_SNAP_AIM | CHEAT_TRUE_SIGHT | CHEAT_RANGE_HIT, 0),
    "shielder": ("shield", 1.5, 45.0, 0.03, 0),
    "shielder_stubborn": ("shield", 1.5, 45.0, 0.005, 0),
    "shielder_open": ("shield", 1.5, 45.0, 0.2, 0),
    "shielder_never": ("shield", 1.5, 45.0, 0.0, 0),   # never lowers it voluntarily
    "shielder_panic": ("shield", 1.5, 45.0, 0.03, 8),  # stops lowering it at 8 health or less
}
STANDARD_EVAL = ("dummy", "bot_easy", "bot_medium", "bot_hard", "tactician", "tactician_crits", "expert")


def evaluate(policy: Policy, norm: RunningNorm, cfg: PPOConfig, opponent: str, seed: int,
             frozen: Policy | None = None, deterministic: bool = True) -> dict:
    """Deterministic learner vs a fixed opponent, the learner on both sides equally.

    opponent: a key of EVAL_OPPONENTS, or "frozen" (an earlier policy)."""
    n = cfg.eval_episodes  # one episode per duel: counting the first to finish would favour short ones
    env = DuelEnv(n, seed=seed, max_ticks=cfg.max_ticks, weapons=cfg.weapons, reward=cfg.reward)
    learner = np.zeros(2 * n, bool)
    learner[np.arange(n) * 2 + (np.arange(n) % 2)] = True
    set_loadouts(env, learner, cfg)
    obs = env.observe()
    agg = {"episodes": 0, "wins": 0, "losses": 0, "draws": 0, "ticks": 0.0}
    agg.update({k: 0.0 for k in SLOT_STATS})
    actions = np.zeros((2 * n, env.action_size), np.float32)
    finished = np.zeros(n, bool)
    while not finished.all():
        with torch.no_grad():
            o = torch.from_numpy(norm(obs))
            c, t, _, _ = policy.act(o, deterministic=deterministic)
            a_learn = policy.env_actions(c.numpy(), t.numpy())
            if opponent == "frozen":
                c2, t2, _, _ = frozen.act(o, deterministic=True)
                a_opp = frozen.env_actions(c2.numpy(), t2.numpy())
        actions[learner] = a_learn[learner]
        if opponent == "frozen":
            actions[~learner] = a_opp[~learner]
        else:
            kind, noise, turn, cheats, crits = EVAL_OPPONENTS[opponent]
            mask = (~learner).astype(np.uint8)
            if kind == "tact":
                env.tactician_each(mask, np.full(2 * n, noise, np.float32), np.full(2 * n, turn, np.float32),
                                   np.full(2 * n, cheats, np.uint32), np.full(2 * n, crits, np.uint8), actions)
            elif kind == "shield":
                env.shielder_each(mask, np.full(2 * n, noise, np.float32), np.full(2 * n, turn, np.float32),
                                  np.full(2 * n, cheats, np.float32), np.full(2 * n, crits, np.float32), actions)
            else:
                env.scripted(mask, 0 if kind == "dummy" else 1, noise, turn, actions)
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
        # Hotbar and shield play, per episode (best_share: of ticks, holding the
        # hotbar's highest-damage item).
        "swaps": agg["swaps"] / e, "axe_swaps_raised": agg["axe_swaps_raised"] / e,
        "axe_swaps_lowered": agg["axe_swaps_lowered"] / e, "disables": agg["disables"] / e,
        "blocked_hits": agg["blocked_hits"] / e, "sword_hits_lowered": agg["sword_hits_lowered"] / e,
        "axe_attacks": agg["axe_attacks"] / e, "swap_backs": agg["swap_backs"] / e,
        "best_share": agg["best_ticks"] / max(agg["ticks"], 1.0),
    }


def train(cfg: PPOConfig, out_dir: Path, log=print) -> Policy:
    out_dir.mkdir(parents=True, exist_ok=True)
    torch.manual_seed(cfg.seed)
    torch.set_num_threads(max(1, torch.get_num_threads()))
    rng = np.random.default_rng(cfg.seed)
    env = DuelEnv(cfg.n_duels, seed=cfg.seed, max_ticks=cfg.max_ticks, weapons=cfg.weapons, reward=cfg.reward,
                  start_dist=cfg.start_dist)
    opp = Opponents(cfg.n_duels, cfg, rng)
    set_loadouts(env, opp.learner, cfg)
    policy = Policy(env.obs_size, cfg.hidden, cfg.slot_keys, cfg.use_key)
    norm = RunningNorm(env.obs_size)
    league_pool: list[dict] = []
    if cfg.init_checkpoint:
        ck = torch.load(cfg.init_checkpoint, weights_only=False)
        policy, norm = load_policy(ck, env.obs_size, cfg.slot_keys, cfg.use_key, none_bias=cfg.head_bias)
        league_pool.append({k: v.clone() for k, v in policy.state_dict().items()})
    optim = torch.optim.Adam(policy.parameters(), lr=cfg.lr, eps=1e-5)
    league_net = Policy(env.obs_size, cfg.hidden, cfg.slot_keys, cfg.use_key)
    league_net.eval()
    gamma = cfg.reward.gamma
    (out_dir / "config.json").write_text(json.dumps(asdict(cfg), indent=1, default=str))
    metrics = open(out_dir / "metrics.jsonl", "a")
    snapshots: list[dict] = []

    obs = env.observe()
    norm.update(obs)
    S, T = 2 * cfg.n_duels, cfg.rollout
    actions = np.zeros((S, env.action_size), np.float32)
    n_cat = len(policy.cat_sizes)
    samples = 0
    t_start = time.time()
    for update in range(1, cfg.updates + 1):
        aid = max(0.0, 1.0 - (update - 1) / max(cfg.anneal * cfg.updates, 1.0))
        env.set_reward(cfg.reward, cfg.aim_dense * aid, cfg.reach_dense * aid)
        if cfg.league > 0.0:
            if update % cfg.league_every == 1 or not league_pool:
                league_pool.append({k: v.clone() for k, v in policy.state_dict().items()})
                league_pool = league_pool[-10:]
            # This rollout's league opponent: a random snapshot, recent ones favoured.
            w = np.arange(1, len(league_pool) + 1, dtype=np.float64)
            league_net.load_state_dict(league_pool[int(rng.choice(len(league_pool), p=w / w.sum()))])
        buf_obs = np.zeros((T, S, env.obs_size), np.float32)
        buf_c = np.zeros((T, S, n_cat), np.int64)
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
            actions[:] = policy.env_actions(c, t)
            opp.fill(env, actions, o, league_net if cfg.league > 0.0 else None)
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
        b_c = torch.from_numpy(buf_c.reshape(-1, n_cat)[sel])
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
                           for o in (cfg.eval_opponents or STANDARD_EVAL)]
            if snapshots:
                frozen = Policy(env.obs_size, cfg.hidden, cfg.slot_keys, cfg.use_key)
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
        s += (f"\n    vs {e['opponent']:15s} win {e['win_rate']:.2f} draw {e['draw_rate']:.2f} "
              f"hits/click {e['hits_per_click']:.2f} aim {e['aim_error_deg']:5.1f}deg "
              f"dmg {e['damage_dealt']:5.1f}/{e['damage_taken']:5.1f} adv {e['advantage_share']:.2f}")
        if e.get("swaps", 0) or e.get("disables", 0) or e.get("blocked_hits", 0):
            s += (f"\n      swaps {e['swaps']:.2f} best {e['best_share']:.2f} axe-swaps up/down "
                  f"{e['axe_swaps_raised']:.2f}/{e['axe_swaps_lowered']:.2f} disables {e['disables']:.2f} "
                  f"blocked {e['blocked_hits']:.2f} sword-hits-down {e['sword_hits_lowered']:.2f} "
                  f"axe-attacks {e['axe_attacks']:.2f} swap-backs {e['swap_backs']:.2f}")
    return s
