"""Batched duels from the C++ simulator (sim/include/mcp/env.hpp) via ctypes.

    env = DuelEnv(n=256, seed=1)
    obs = env.observe()                      # [2n, OBS] float32, slot 2i+k = player k of duel i
    obs, rew, done, stats = env.step(actions)  # actions [2n, ACT] float32

Actions per slot: forward/back (-1, 0, 1), left/right (-1, 0, 1), jump,
sprint, click (0/1), the turn this tick in degrees (yaw, pitch), a hotbar key
(-1 none, else 0-8) and the use key (0/1, held while 1).
done[i]: 0 running, 1 a death ended the episode, 2 truncated (time limit or
out of the arena). stats[i] is filled for duels that ended this step.
"""
from __future__ import annotations

import ctypes
import os
from pathlib import Path

import numpy as np

from .config import FairnessCaps, RewardConfig

REPO = Path(__file__).resolve().parents[2]
SIN_TABLE = REPO / "sim" / "data" / "sin_table.bin"

WEAPONS = ["hand", "wooden_sword", "stone_sword", "copper_sword", "iron_sword", "golden_sword", "diamond_sword",
           "netherite_sword", "wooden_axe", "stone_axe", "copper_axe", "iron_axe", "golden_axe", "diamond_axe",
           "netherite_axe", "shield"]

SLOT_STATS = ["clicks", "attacks", "hits", "damage_dealt", "damage_taken", "aim_error_sum", "aim_ticks",
              "advantage_ticks", "disadvantage_ticks", "swaps", "axe_swaps_raised", "axe_swaps_lowered", "disables",
              "blocked_hits", "sword_hits_lowered", "axe_attacks", "swap_backs", "best_ticks"]

CHEAT_SNAP_AIM, CHEAT_TRUE_SIGHT, CHEAT_RANGE_HIT = 1, 2, 4

_F = ctypes.POINTER(ctypes.c_float)
_U8 = ctypes.POINTER(ctypes.c_uint8)


def _find_library() -> Path:
    env = os.environ.get("MCP_ENV_LIB")
    candidates = [Path(env)] if env else []
    for build in (REPO / "sim" / "build", Path("/tmp/mcpbuild")):
        candidates += [build / "libmcp_env.so", build / "libmcp_env.dylib", build / "Release" / "mcp_env.dll",
                       build / "mcp_env.dll"]
    for c in candidates:
        if c.exists():
            return c
    raise FileNotFoundError("libmcp_env not found: build the sim (cmake --build) or set MCP_ENV_LIB")


def _load() -> ctypes.CDLL:
    lib = ctypes.CDLL(str(_find_library()))
    lib.mcp_env_create.restype = ctypes.c_void_p
    lib.mcp_env_create.argtypes = [ctypes.c_int, ctypes.c_ulonglong, ctypes.c_char_p, ctypes.c_int, ctypes.c_uint,
                                   ctypes.c_int, _F, ctypes.c_float, ctypes.c_double, ctypes.c_double]
    lib.mcp_env_destroy.argtypes = [ctypes.c_void_p]
    lib.mcp_env_set_reward.argtypes = [ctypes.c_void_p, _F]
    lib.mcp_env_observe.argtypes = [ctypes.c_void_p, _F]
    lib.mcp_env_step.argtypes = [ctypes.c_void_p, _F, _F, _F, _U8, _F]
    lib.mcp_env_scripted.argtypes = [ctypes.c_void_p, _U8, ctypes.c_int, ctypes.c_float, ctypes.c_float, _F]
    _I32 = ctypes.POINTER(ctypes.c_int32)
    lib.mcp_env_scripted_each.argtypes = [ctypes.c_void_p, _U8, _I32, _F, _F, _F]
    lib.mcp_env_tactician_each.argtypes = [ctypes.c_void_p, _U8, _F, _F, ctypes.POINTER(ctypes.c_uint32), _U8, _F]
    lib.mcp_env_shielder_each.argtypes = [ctypes.c_void_p, _U8, _F, _F, _F, _F, _F, _F]
    lib.mcp_env_set_loadout.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_int]
    lib.mcp_env_reset_duel.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.mcp_env_record.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.mcp_env_record_state.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.mcp_env_record_nonparity.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.mcp_env_export.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p]
    lib.mcp_env_turns_clamped.restype = ctypes.c_longlong
    lib.mcp_env_turns_clamped.argtypes = [ctypes.c_void_p]
    return lib


_LIB: ctypes.CDLL | None = None


def lib() -> ctypes.CDLL:
    global _LIB
    if _LIB is None:
        _LIB = _load()
    return _LIB


def _p(a: np.ndarray, t):
    return a.ctypes.data_as(t)


class DuelEnv:
    def __init__(self, n: int, seed: int = 1, max_ticks: int = 600, weapons: tuple[str, ...] = ("hand",),
                 same_weapon: bool = True, reward: RewardConfig = RewardConfig(),
                 fairness: FairnessCaps = FairnessCaps(), start_dist: tuple[float, float] = (3.0, 10.0)):
        L = lib()
        self.n = n
        self.obs_size = L.mcp_env_obs_size()
        self.action_size = L.mcp_env_action_size()
        self.stats_size = L.mcp_env_stats_size()
        mask = 0
        for w in weapons:
            mask |= 1 << WEAPONS.index(w)
        r = np.array([reward.damage_dealt, reward.damage_taken, reward.win, reward.loss, reward.reach_shaping,
                      reward.gamma, reward.aim_shaping], dtype=np.float32)
        self._h = L.mcp_env_create(n, seed, str(SIN_TABLE).encode(), max_ticks, mask, int(same_weapon), _p(r, _F),
                                   fairness.max_turn_deg_per_tick, start_dist[0], start_dist[1])
        if not self._h:
            raise RuntimeError("could not create the environment (sin table?)")
        self._obs = np.zeros((2 * n, self.obs_size), np.float32)
        self._rew = np.zeros(2 * n, np.float32)
        self._done = np.zeros(n, np.uint8)
        self._stats = np.zeros((n, self.stats_size), np.float32)

    def set_reward(self, reward: RewardConfig, aim_dense: float = 0.0, reach_dense: float = 0.0) -> None:
        """Change the reward, including the annealed dense training aids."""
        r = np.array([reward.damage_dealt, reward.damage_taken, reward.win, reward.loss, reward.reach_shaping,
                      reward.gamma, reward.aim_shaping, aim_dense, reach_dense, reward.wasted_attack, reward.draw],
                     dtype=np.float32)
        lib().mcp_env_set_reward(self._h, _p(r, _F))

    def close(self) -> None:
        if self._h:
            lib().mcp_env_destroy(self._h)
            self._h = None

    def __del__(self):
        self.close()

    def observe(self) -> np.ndarray:
        lib().mcp_env_observe(self._h, _p(self._obs, _F))
        return self._obs.copy()

    def step(self, actions: np.ndarray):
        a = np.ascontiguousarray(actions, dtype=np.float32)
        assert a.shape == (2 * self.n, self.action_size)
        lib().mcp_env_step(self._h, _p(a, _F), _p(self._obs, _F), _p(self._rew, _F), _p(self._done, _U8),
                           _p(self._stats, _F))
        return self._obs.copy(), self._rew.copy(), self._done.copy(), self._stats.copy()

    def scripted(self, mask: np.ndarray, kind: int, noise_deg: float, turn_deg: float,
                 out: np.ndarray | None = None) -> np.ndarray:
        """Scripted actions for the slots where mask is set (kind 0 stands, 1 aim bot)."""
        m = np.ascontiguousarray(mask, dtype=np.uint8)
        a = np.zeros((2 * self.n, self.action_size), np.float32) if out is None else out
        lib().mcp_env_scripted(self._h, _p(m, _U8), kind, noise_deg, turn_deg, _p(a, _F))
        return a

    def scripted_each(self, mask: np.ndarray, kind: np.ndarray, noise_deg: np.ndarray, turn_deg: np.ndarray,
                      out: np.ndarray) -> np.ndarray:
        """Scripted actions with per-slot kind, noise and turn rate ([2n] arrays)."""
        m = np.ascontiguousarray(mask, dtype=np.uint8)
        k = np.ascontiguousarray(kind, dtype=np.int32)
        nz = np.ascontiguousarray(noise_deg, dtype=np.float32)
        tr = np.ascontiguousarray(turn_deg, dtype=np.float32)
        lib().mcp_env_scripted_each(self._h, _p(m, _U8), k.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)), _p(nz, _F),
                                    _p(tr, _F), _p(out, _F))
        return out

    def tactician_each(self, mask: np.ndarray, noise_deg: np.ndarray, turn_deg: np.ndarray, cheats: np.ndarray,
                       crits: np.ndarray, out: np.ndarray) -> np.ndarray:
        """Tactician opponents (a port of the PvP Bot mod's melee) for the masked slots.

        cheats: per-slot bits CHEAT_SNAP_AIM | CHEAT_TRUE_SIGHT | CHEAT_RANGE_HIT (opponent only)."""
        m = np.ascontiguousarray(mask, dtype=np.uint8)
        nz = np.ascontiguousarray(noise_deg, dtype=np.float32)
        tr = np.ascontiguousarray(turn_deg, dtype=np.float32)
        ch = np.ascontiguousarray(cheats, dtype=np.uint32)
        cr = np.ascontiguousarray(crits, dtype=np.uint8)
        lib().mcp_env_tactician_each(self._h, _p(m, _U8), _p(nz, _F), _p(tr, _F),
                                     ch.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)), _p(cr, _U8), _p(out, _F))
        return out

    def shielder_each(self, mask: np.ndarray, noise_deg: np.ndarray, turn_deg: np.ndarray, lower_rate: np.ndarray,
                      panic_health: np.ndarray, out: np.ndarray, react_ticks: np.ndarray | None = None) -> np.ndarray:
        """Shield users (raise the off-hand shield, lower it at random, swing while it is down; at or
        below panic_health they never lower it; lower_rate 0 never lowers it; after a disable they
        wait react_ticks before swinging)."""
        m = np.ascontiguousarray(mask, dtype=np.uint8)
        nz = np.ascontiguousarray(noise_deg, dtype=np.float32)
        tr = np.ascontiguousarray(turn_deg, dtype=np.float32)
        lo = np.ascontiguousarray(lower_rate, dtype=np.float32)
        ph = np.ascontiguousarray(panic_health, dtype=np.float32)
        rt = np.ascontiguousarray(np.zeros(2 * self.n) if react_ticks is None else react_ticks, dtype=np.float32)
        lib().mcp_env_shielder_each(self._h, _p(m, _U8), _p(nz, _F), _p(tr, _F), _p(lo, _F), _p(ph, _F), _p(rt, _F),
                                    _p(out, _F))
        return out

    def set_loadout(self, slot: int, hotbar: tuple[str, ...] | None, offhand: str = "") -> None:
        """Slot `slot` (2i+k) starts every episode with this hotbar (slot 0 selected) and off hand;
        None: back to the weapon drawn from `weapons`. Applies from the duel's next reset."""
        if hotbar is None:
            lib().mcp_env_set_loadout(self._h, slot, None, 0)
            return
        h = (ctypes.c_int * 9)(*[WEAPONS.index(w) if w and w != "-" else 0 for w in (list(hotbar) + [""] * 9)[:9]])
        lib().mcp_env_set_loadout(self._h, slot, h, WEAPONS.index(offhand) if offhand else 0)

    def reset_duel(self, i: int) -> None:
        lib().mcp_env_reset_duel(self._h, i)

    def record(self, i: int) -> None:
        lib().mcp_env_record(self._h, i)

    def record_state(self, i: int) -> int:
        return lib().mcp_env_record_state(self._h, i)

    def record_nonparity(self, i: int) -> int:
        return lib().mcp_env_record_nonparity(self._h, i)

    def export(self, i: int, path: str | Path, title: str) -> None:
        if lib().mcp_env_export(self._h, i, str(path).encode(), title.encode()) != 0:
            raise OSError(f"could not write {path}")

    def turns_clamped(self) -> int:
        return lib().mcp_env_turns_clamped(self._h)


def slot_stats(stats_row: np.ndarray, slot: int) -> dict[str, float]:
    base = 3 + slot * len(SLOT_STATS)
    return {k: float(stats_row[base + j]) for j, k in enumerate(SLOT_STATS)}
