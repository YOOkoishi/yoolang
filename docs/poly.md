# 多面体优化设计

yoolang 在 YIR 层实现了轻量的多面体优化 pipeline. 整个建模思路和实现参考了 [llvm-polly](https://github.com/llvm/llvm-project/tree/main/polly). 我们首先做 canonicalize loop bound, stride 与 induction variable, 并识别控制流和数组访问均可分析的 SCoP; 随后以 statement 为单位构造 iteration domain, affine/quasi-affine access map 和 common SCoP schedule space, 该过程支持 constant division, modulo 及 composite affine expression.

依赖分析部分覆盖数组 RAW, WAR 和 WAW, 并结合 distance vector, GCD 检验和 Presburger relation 判断调度合法性. 并专门标注可结合依赖; 如果遇到别名, 副作用或无法精确证明的关系时就采用保守依赖并拒绝变换, 以此保证优化前后语义一致. Structural transformation 完成后会重新执行建模与依赖分析, 以避免使用失效信息.

在调度合法性约束下, cost model 会选择 loop fusion, loop interchange, tiling 和 wavefront scheduling, 并实现 statement-domain partitioning, reduction privatization, accumulator promotion 及 output-dimension unroll-and-jam 等变换. 其中 Unroll factor 可在 2/4 间选择, 并配套 dynamic remainder loop, cross-lane invariant sharing 和 register-pressure guard; 若收益不足则保持原调度不变.

我们的多面体优化设计力求兼顾访存局部性、循环控制开销与寄存器复用, 致力于实现在比赛合规场景下的多面体变换, 具体优化过程可以通过显式的编译器参数 `--emit-poly` 和 cost-model 报告追踪.
