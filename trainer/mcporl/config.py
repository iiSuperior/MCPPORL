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

    require_aim_to_hit: bool = True
    max_turn_deg_per_tick: float = 200.0
    max_clicks_per_tick: int = 1


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
