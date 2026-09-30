"""Settings, split by what changing them costs.

ContractConfig: anything the policy was trained against. Changing any of it
means retraining (or fine-tuning) the bot. It is stored inside every
checkpoint and identified by a fingerprint.

RuntimeConfig: knobs that may change at deploy time without retraining, but
only within the ranges recorded in the checkpoint's contract. A trained range
is itself part of the contract: widening it is a training change, picking a
value inside it is not.

    contract = ContractConfig()                  # what we train with
    fingerprint = contract.fingerprint()         # saved with the checkpoint
    problems = check_deployable(contract, RuntimeConfig(latency_base_ms=180))
"""
from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass, field


@dataclass(frozen=True)
class Range:
    """Inclusive range of values a runtime knob was randomised over in training."""

    lo: float
    hi: float

    def __post_init__(self) -> None:
        if self.lo > self.hi:
            raise ValueError(f"empty range [{self.lo}, {self.hi}]")

    def contains(self, value: float) -> bool:
        return self.lo <= value <= self.hi


@dataclass(frozen=True)
class FairnessCaps:
    """Input plausibility policy (docs/ARCHITECTURE.md). Contract: the policy
    learns to act within these caps, so changing them needs retraining."""

    require_aim_to_hit: bool = True     # structural in the simulator: clicks go through the client's pick
    max_turn_deg_per_tick: float = 200.0
    max_clicks_per_tick: int = 1        # the simulator's action carries at most one click

    def __post_init__(self) -> None:
        # sim/include/mcp/fairness.hpp implements exactly these; anything else
        # would train a policy under rules the simulator does not enforce.
        if not self.require_aim_to_hit:
            raise ValueError("require_aim_to_hit=False is not supported: the simulator resolves every click "
                             "with the vanilla client's crosshair pick")
        if self.max_clicks_per_tick != 1:
            raise ValueError("max_clicks_per_tick must be 1 (one click per tick is built into the action)")
        if not 0.0 < self.max_turn_deg_per_tick <= 360.0:
            raise ValueError(f"max_turn_deg_per_tick={self.max_turn_deg_per_tick} must be in (0, 360]")


@dataclass(frozen=True)
class RewardConfig:
    """Reward weights (sim/include/mcp/reward.hpp, docs/REWARD.md). Contract:
    the policy optimises exactly this, so changing it needs retraining."""

    damage_dealt: float = 1.0
    damage_taken: float = 1.0
    win: float = 10.0
    loss: float = 10.0
    # Potential-based reach shaping: pays for getting into (and keeping the
    # opponent out of) hit range, never for hovering there.
    reach_shaping: float = 0.1
    # Must equal the learner's discount, or the shaping can change which
    # policy is optimal.
    gamma: float = 0.99
    # Potential-based aim shaping: pays for turning the crosshair onto the
    # opponent as the client sees it (1 on target, 0 at 180 degrees off).
    aim_shaping: float = 0.2

    def __post_init__(self) -> None:
        if not 0.0 < self.gamma <= 1.0:
            raise ValueError(f"gamma={self.gamma} must be in (0, 1]")
        for name in ("damage_dealt", "damage_taken", "win", "loss", "reach_shaping", "aim_shaping"):
            if getattr(self, name) < 0.0:
                raise ValueError(f"{name} must be non-negative (signs are fixed by the reward's definition)")


@dataclass(frozen=True)
class TrainingRanges:
    """What each runtime knob was randomised over during training."""

    latency_base_ms: Range = Range(0, 250)
    latency_jitter_ms: Range = Range(0, 60)
    # Artificial extra reaction delay (a difficulty knob). Only 0 is valid
    # until a policy is trained with it randomised.
    extra_reaction_delay_ticks: Range = Range(0, 0)
    arenas: tuple[str, ...] = ("flat",)


@dataclass(frozen=True)
class ContractConfig:
    mc_version: str = "26.3"
    domains: tuple[str, ...] = ("movement",)
    obs_schema_version: int = 1
    action_schema_version: int = 1
    tick_rate: int = 20
    fairness: FairnessCaps = field(default_factory=FairnessCaps)
    reward: RewardConfig = field(default_factory=RewardConfig)
    ranges: TrainingRanges = field(default_factory=TrainingRanges)

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))

    def fingerprint(self) -> str:
        return hashlib.sha256(self.to_json().encode()).hexdigest()[:16]

    @staticmethod
    def from_json(text: str) -> ContractConfig:
        d = json.loads(text)
        r = d["ranges"]
        ranges = TrainingRanges(
            latency_base_ms=Range(**r["latency_base_ms"]),
            latency_jitter_ms=Range(**r["latency_jitter_ms"]),
            extra_reaction_delay_ticks=Range(**r["extra_reaction_delay_ticks"]),
            arenas=tuple(r["arenas"]),
        )
        return ContractConfig(
            mc_version=d["mc_version"],
            domains=tuple(d["domains"]),
            obs_schema_version=d["obs_schema_version"],
            action_schema_version=d["action_schema_version"],
            tick_rate=d["tick_rate"],
            fairness=FairnessCaps(**d["fairness"]),
            reward=RewardConfig(**d["reward"]),
            ranges=ranges,
        )


@dataclass(frozen=True)
class RuntimeConfig:
    """Deploy-time knobs. Safe to change as long as check_deployable passes."""

    latency_base_ms: float = 50
    latency_jitter_ms: float = 10
    extra_reaction_delay_ticks: int = 0
    arena: str = "flat"


def check_deployable(contract: ContractConfig, runtime: RuntimeConfig,
                     expected: ContractConfig | None = None) -> list[str]:
    """Problems that make `runtime` unsafe for a policy trained under `contract`.

    If `expected` is given (e.g. what the server plugin was built for), any
    contract difference is also reported: that policy was trained for different
    rules and must be retrained, not reconfigured.
    """
    problems: list[str] = []
    r = contract.ranges
    for name, rng in (("latency_base_ms", r.latency_base_ms),
                      ("latency_jitter_ms", r.latency_jitter_ms),
                      ("extra_reaction_delay_ticks", r.extra_reaction_delay_ticks)):
        value = getattr(runtime, name)
        if not rng.contains(value):
            problems.append(f"{name}={value} is outside the trained range [{rng.lo}, {rng.hi}]")
    if runtime.arena not in r.arenas:
        problems.append(f"arena {runtime.arena!r} was not trained on (trained: {', '.join(r.arenas)})")
    if expected is not None and expected.fingerprint() != contract.fingerprint():
        a, b = asdict(expected), asdict(contract)
        for key in sorted(a):
            if a[key] != b[key]:
                problems.append(f"contract mismatch in {key!r}: checkpoint has {b[key]!r}, "
                                f"deployment expects {a[key]!r} (retrain, do not reconfigure)")
    return problems
