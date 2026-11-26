# mtl_probe Integration Harness

`mtl_probe` is a plain C++ integration probe binary with zero GoogleTest
dependency and zero shared global context.

Pytest drives `mtl_probe` as a subprocess. Each invocation accepts a scenario
name, an init profile name, and a result file path. The binary writes one JSON
result object and exits with a defined status code.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Test passed |
| `1` | Assertion failed |
| `2` | Hardware prerequisite or topology missing |
| `3` | Host fault |

## Build

`./build.sh` builds `mtl_probe` under
`build/tests/integration_tests/probe/mtl_probe`. The build does not install the
stub binary.

## Run dry-run contract tests

Run the pytest contract suite without NIC hardware:

```bash
python3.12 -m pytest -q -p no:cacheprovider \
  tests/integration_tests/probe/test_st20p_pacing.py \
  --probe-bin=build/tests/integration_tests/probe/mtl_probe \
  --dry-run
```

## Pytest options

- `--probe-bin PATH`: Path to the `mtl_probe` binary (or set `MTL_PROBE_BIN`).
- `--nic-label LABEL`: NIC identifier written to JSONL records.
- `--jsonl-out PATH`: Append one JSONL record per test item to `PATH`.
- `--strict-hw`: Fail instead of skip when `mtl_probe` returns exit code `2`.
- `--dry-run`: Pass `--dry-run` to `mtl_probe` for offline contract checks.
