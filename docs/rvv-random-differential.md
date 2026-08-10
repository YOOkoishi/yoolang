# RVV 随机差分门禁

`scripts/rvv_random_differential_infra_tests.py` 用同一份输入分别执行 rv64gc
标量控制程序和 rv64gcv 程序，并在 VLEN=128/256/512/1024 下要求输出逐字节一致。
manifest、case id 和每个 case 的数据 seed 都由显式 64-bit master seed 确定；任何
required 维度都不能以 skip、XFAIL 或静默未执行计为通过。

## Tier

- `smoke` 是默认 CI 门。它覆盖所有 e32/m1 的 VLMAX±1、2×VLMAX±1、0/1、
  127/257/1023/4095、unit-stride i32/f32、±1/±2/±4 indexed memory、mask
  guard-page、整数 reduction，以及稳定的 alias/dynamic-stride/strict-FP 拒绝码。
- `extended` 保留完整 smoke manifest，再增加 32 组 i32、12 组 f32、每种 indexed
  stride 6 组、10 组 reduction 和每种 guard mask 4 组独立 seed/length/alignment。
- `nightly` 使用 extended 的四倍附加样本。它是显式长门，不会暗中扩大每次提交的
  smoke 时长。

每次 push 仍由 toolchain infra 执行 smoke。GitHub Actions 每周日 03:00 UTC 显式执行
nightly；手动 workflow dispatch 可在 `rvv_diff_tier` 选择 `smoke`、`extended` 或
`nightly`，且长门失败会直接使 job 失败。

普通 kernel 的地址偏移集合显式包含 1、2、3 byte；driver 使用 `memcpy` 构造和读取
oracle 数据，因此 C 侧不发生未对齐解引用。编译后的 scalar/RVV kernel 则真实接收
该非对齐地址。MASK2 guard-page fixture 自己使用 word-addressed page layout，继续保持
自然对齐；它负责验证 inactive lane 不访问 PROT_NONE，而不是替代非对齐门。

```sh
python3 scripts/rvv_random_differential_infra_tests.py
python3 scripts/rvv_random_differential_infra_tests.py --tier extended
python3 scripts/rvv_random_differential_infra_tests.py --tier nightly --seed 0x1234
```

## Manifest、单例与 replay

`--manifest-only` 不要求交叉工具链。`--case-id` 接受完整 id 或唯一前缀；`--replay`
读取保存的 `case.json`。例如：

```sh
python3 scripts/rvv_random_differential_infra_tests.py \
  --tier extended --manifest-only
python3 scripts/rvv_random_differential_infra_tests.py \
  --tier extended --case-id 0fdb82f9daf12e003241
python3 scripts/rvv_random_differential_infra_tests.py \
  --replay build/test-artifacts/rvv-random-differential/CASE/case.json
```

运行失败时，artifact 保存原始 case、输入、全部命令与临时 workspace。对 runtime
case，runner 还会用同一 scalar/RVV binary 和 VLEN 集合，依次最小化 length、alignment
offset 和 data seed，并要求退出类别或 scalar/RVV mismatch VLEN 与原失败签名一致；若
存在更小复现，会写出 `minimized-case.json`、`shrink.json`，并让
可执行的 `repro.sh` 直接指向最小 case。编译/汇编阶段失败不会伪装成数据相关问题，仍
保留原始 case。
