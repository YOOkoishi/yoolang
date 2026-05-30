
## Apr. 5

新建文件夹

## Apr. 8

我重构了一下上一版本代码原本的 AST 定义，将其中冗杂不合理的地方去掉了，AST 文件基本没有问题了。

然后将原本的IR 重构成 machine IR 层了，里面有些东西还有问题，等lc自行重构一下。

## Apr. 16

完成了中层 ssa 形式 oir 的创建，参考llvm 本体的代码，相比 之前我自己写的 IR 要好很多.

## Apr. 18

~~完成了 ast -> ir 的 lowering， 前端还没有完成，不过直接手写 ast 倒是能验证。~~

## Apr. 22

~~完成了 ir -> Mir 的 lowering， 添加了前端， 去年初赛的结果都可以通过了？~~

# Apr. 24

通过个鬼，不能相信ai.

今天重构了所有的ir，完成了passmanager ，我重新设计了 高层IR yir,现在可以正常使用了，另外IR的设计后面再做吧。

# Apr. 25

添加了符号表进行语义分析，目前集成在ASTToYIRPASS 中，后续可以拆分出来。目前yir生成没什么问题了。

# Apr. 26

添加了test 的基础设施。现在可以自动的跑所有的汇编测试，以及ir 的 verified。

# Apr. 29

添加了毕昇杯2026的测例，func和perf全部通过。基础设施我也比较满意了。

# May 3

添加了 loop count 的 pass.

在添加这个 pass 之前,

```bash
summary: 1380 passed, 6 failed, 1 skipped, 0 xfailed, 0 xpassed
```

添加之后也仍然保持一样的 summary.

# May 6

添加了 RegAlloc 的 pass , 以及 MIR 的 Peephole pass,现在的速度比之前栈式分配内存要快不少（50%）。

# May 8

优化了oir的一些功能，添加了几个中端的常量优化pass，常量折叠，代数化简，SCCP 等。

# May 9

重构了main函数中的pipeline,添加了 OIR 层面的 AA DCE GVN LICM 等优化。 但是目前效果不是很好。

# May 10

添加了保守的inline,效果还行，但是在板子上不行。

# May 11

增强了后端的窥孔和图着色，修改了一些问题，加速了蛮多。

# May 12

添加了 yir 的 Analysis 。

# May 14

添加了 oir 上的 SCEV ， loop unrolling ， SCCP 。

# May 16

添加了 oir 上的常态化 use_list 现在可以更好的被 DCE LICM GVN 等优化利用

# May 17

添加了对 x/2^k 等类型的优化，现在可以被识别，最后形成位运算的形式加速。

# May 18

添加了 MIR 上的指令选择 PASS

# May 19

增强了 inline 目前进度 0.94x Clang o3

# May 31

重构了几次代码，主要规范化了pass pipeline中的结构。目前时间加速比达到1.1x clang o3 ，但几何加速比还是差一些。
