import dataclasses
import unittest

from mcporl.config import (ContractConfig, FairnessCaps, Range, RewardConfig, RuntimeConfig,
                           TrainingRanges, check_deployable)


class ConfigTest(unittest.TestCase):
    def test_defaults_are_deployable(self):
        self.assertEqual(check_deployable(ContractConfig(), RuntimeConfig()), [])

    def test_fairness_caps_the_simulator_cannot_enforce_are_rejected(self):
        with self.assertRaises(ValueError):
            FairnessCaps(require_aim_to_hit=False)
        with self.assertRaises(ValueError):
            FairnessCaps(max_clicks_per_tick=2)
        with self.assertRaises(ValueError):
            FairnessCaps(max_turn_deg_per_tick=0.0)

    def test_reward_is_contract_and_validated(self):
        base = ContractConfig()
        reshaped = dataclasses.replace(base, reward=RewardConfig(reach_shaping=0.5))
        self.assertNotEqual(base.fingerprint(), reshaped.fingerprint())
        self.assertEqual(ContractConfig.from_json(reshaped.to_json()), reshaped)
        with self.assertRaises(ValueError):
            RewardConfig(gamma=0.0)
        with self.assertRaises(ValueError):
            RewardConfig(win=-1.0)

    def test_runtime_change_within_trained_range_is_fine(self):
        self.assertEqual(check_deployable(ContractConfig(), RuntimeConfig(latency_base_ms=240)), [])

    def test_runtime_change_outside_trained_range_is_rejected(self):
        problems = check_deployable(ContractConfig(), RuntimeConfig(latency_base_ms=400))
        self.assertEqual(len(problems), 1)
        self.assertIn("latency_base_ms", problems[0])

    def test_untrained_difficulty_knob_is_rejected(self):
        problems = check_deployable(ContractConfig(), RuntimeConfig(extra_reaction_delay_ticks=3))
        self.assertIn("extra_reaction_delay_ticks", problems[0])

    def test_untrained_arena_is_rejected(self):
        problems = check_deployable(ContractConfig(), RuntimeConfig(arena="sky_islands"))
        self.assertIn("sky_islands", problems[0])

    def test_contract_change_changes_fingerprint(self):
        base = ContractConfig()
        tighter = dataclasses.replace(base, fairness=FairnessCaps(max_turn_deg_per_tick=90.0))
        wider = dataclasses.replace(base, ranges=TrainingRanges(latency_base_ms=Range(0, 400)))
        self.assertNotEqual(base.fingerprint(), tighter.fingerprint())
        self.assertNotEqual(base.fingerprint(), wider.fingerprint())
        self.assertEqual(base.fingerprint(), ContractConfig().fingerprint())

    def test_contract_mismatch_is_reported_as_retrain(self):
        checkpoint = ContractConfig()
        plugin = dataclasses.replace(checkpoint, fairness=FairnessCaps(max_turn_deg_per_tick=90.0))
        problems = check_deployable(checkpoint, RuntimeConfig(), expected=plugin)
        self.assertEqual(len(problems), 1)
        self.assertIn("fairness", problems[0])
        self.assertIn("retrain", problems[0])

    def test_json_round_trip_preserves_fingerprint(self):
        c = dataclasses.replace(ContractConfig(), domains=("movement", "melee"))
        self.assertEqual(ContractConfig.from_json(c.to_json()), c)
        self.assertEqual(ContractConfig.from_json(c.to_json()).fingerprint(), c.fingerprint())

    def test_empty_range_rejected(self):
        with self.assertRaises(ValueError):
            Range(5, 1)


if __name__ == "__main__":
    unittest.main()
