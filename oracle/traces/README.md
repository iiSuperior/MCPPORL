# Golden oracle traces

Recorded from vanilla Minecraft **26.3** by the `oracle-traces` workflow
(`oracle/harness`) replaying the scripts in `oracle/scenarios/`. These are
measurement data, not game code. The simulator must reproduce them bit for
bit: `tools/tracediff.py oracle/traces/<name>.jsonl <sim-trace>.jsonl`.

Sanity checks at the time of recording:

| Check | Value |
|---|---|
| Walk speed | 0.215859 blocks/tick |
| Sprint speed | 0.280617 blocks/tick |
| Sneak speed | 0.064758 blocks/tick |
| Jump peak | 1.252203 blocks |
| Sprint-jump average | 0.355 blocks/tick |
| Diagonal walk | 0.220264 blocks/tick (straight / 0.98) |

To regenerate after changing the harness or scenarios: run the workflow,
then decode the `BEGIN-TRACES` block of its log (gzip + base64 tar) here.
Re-record whenever the target game version changes.
