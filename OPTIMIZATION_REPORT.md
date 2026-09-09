# chjson 优化与验证报告

本文件保留第一轮的测量记录。后续改动与最新验证见 [第二轮优化报告](OPTIMIZATION_ROUND2_REPORT.md)。

日期：2026-09-08。基线：修改前的 `f98fc8d`。本次保留 C++17、单头文件与 24 字节 `sv_value` 的设计，集中处理解析一致性、生命周期、异常安全、分配开销和并行调度。

## 已完成的修改

### 功能与正确性

- 为各解析模式的字符串和键增加 UTF-8 校验，拒绝过长编码、孤立续字节、代理区码点、超过 U+10FFFF 的编码及截断序列。ASCII 仍使用 SIMD 扫描。
- 修复 legacy 字符串扫描在无终止引号、长度恰好跨 SIMD 边界时的越界读取。
- 修复短输入通过字符串 SSO 存储时，移动 `document` 导致字符串视图失效的问题；移动后的源文档可继续使用。
- 修复 `sv_value("text")` 被重载解析为布尔值的问题。
- 修复数字数组快速路径绕过 `max_depth`，以及多线程分片在 `require_eof=false` 时遗漏分片内部非法尾随内容的问题。
- 修复 owning-view 数字数组丢失原始浮点/大整数 token 的问题，保留尾零、指数拼写以及超出 double 范围的合法 JSON 数字。
- 数字转换默认使用 `from_chars`；回退使用独立 C 数字区域设置，避免进程 locale 改变 JSON 的小数点语义。
- 修复 in-situ 解码插入换行后，错误行列号偏离原始输入的问题。
- 修复深层序列化栈扩容时读取已释放旧栈的问题。

### 内存与异常安全

- arena 初始块从 64 KiB 降为 1 KiB；标量与空容器不再预留容器存储。
- 小容器初始容量减小，空容器延迟分配；长字符串数组采用更合适的元素数量估算。
- 容器末次 arena 分配可在剩余空间允许时原地扩展；使用一般 PMR 资源时，扩容后归还旧分配。
- 容器增长与内部分配增加整数溢出检查；保留 trivially-copyable 布局约束。
- 新增 `document::reset()` 与 `release_thread_caches()`，分别释放当前文档存储和当前线程闲置缓存。
- 默认关闭保留页面直到进程退出的实验性内部 allocator；仍可通过宏启用。
- 序列化预留提示关联当前根节点形状，避免在大输出之后为小结果继续分配巨大缓冲区。
- 八个公开解析入口处理 `std::bad_alloc`；修复长数字存储和复制在分配失败后的异常安全问题。
- `as_double_unchecked()` 不再声明为 `noexcept`，使长数字转换的分配失败可以传播。依赖原 `noexcept` 函数类型的调用方需要调整。

### 性能与并行处理

- 小文档使用较短的解析入口，减少进入大型并行分派函数的开销。
- 多线程解析和 `parse_many_into()` 复用线程池；在线程池工作线程中执行的嵌套解析使用单线程路径。
- 修复多线程解析对未构造 `monotonic_buffer_resource` 调用析构的问题。
- 并行操作增加任务收尾保护：分配、提交或序列化失败时，已提交任务结束后才释放它们引用的数据。
- 自动并行序列化默认要求至少 1024 个元素和 1 MiB 估计输出，减少中等尺寸简单数组的调度开销。新增可调宏，显式 `dump_mt()` 的选项保持独立。

## 编译与测试

| 环境 | 配置 | CTest 结果 |
| --- | --- | --- |
| Windows x64 / Clang 17.0.6 | Release，默认及三种替代配置，OOM 注入 | 5/5 通过 |
| Windows x64 / MSVC 19.38 | Debug，默认及三种替代配置，OOM 注入 | 5/5 通过 |
| WSL Linux x64 / Clang 18.1.3 | Debug，ASan + UBSan + 泄漏检测，相同配置矩阵 | 5/5 通过 |
| WSL Linux x64 / GCC 13.3 | Release，`-Wall -Wextra -Wpedantic` | 2/2 通过 |

合计 **17/17 组 CTest 通过**。补充测试包括所有解析模式的确定性随机回环与输入变异、UTF-8 边界、移动和复用、600 层序列化、空容器、容量扩展、locale、错误位置、多线程成功/失败路径、自动并行门槛，以及逐次分配失败注入。

MSVC 的独立 OOM 测试禁用了迭代器调试代理：其 STL 调试代理可能在 `noexcept` 操作中额外分配。正常 Debug 功能测试仍保留默认调试配置。Windows Clang 17 的 ASan 运行库在本机系统函数拦截阶段启动失败，因此 sanitizer 验证使用上表中的 Linux Clang 18 环境；不将其表述为 Windows ASan 通过。

## 基准方法

- CPU：Intel Core i9-12900K，16 核、24 逻辑处理器。
- 基线和新版本使用同一个 CMake 构建、同一份扩展后的 benchmark 源码、Clang 17、Release `-O3 -DNDEBUG`、C++17，以及相同 MSVC 动态运行库参数。
- 使用 `CHJSON_BASELINE_INCLUDE` 指向保留的旧头文件，避免手工编译造成运行库差异。
- 每个样本取 7 次测量的中位数。逐项运行两个版本，不与编译任务同时运行。
- 冷启动 arena 指新线程解析单个文档后的 `arena().bytes_committed()`。它**不是总内存或进程 RSS**，不包含文档字符串缓冲区、线程栈、并行解析独立 backing 和 allocator 元数据。
- 原始命令、吞吐量、arena 统计及进程退出码保存在 [optimization_results.json](benchmark/optimization_results.json)。性能结果仅代表这些样本与本机环境。

### 吞吐量（MiB/s）

| 样本 | 解析：基线 → 新版 | 解析变化 | 序列化：基线 → 新版 | 序列化变化 |
| --- | ---: | ---: | ---: | ---: |
| 标量 | 90.0 → 111.6 | +24.1% | 31.2 → 33.8 | +8.4% |
| 单个小对象 | 978.4 → 805.1 | -17.7% | 895.9 → 941.0 | +5.0% |
| 2000 个对象 | 1154.5 → 1051.9 | -8.9% | 1345.0 → 1355.5 | +0.8% |
| 20000 个整数 | 578.9 → 621.7 | +7.4% | 678.8 → 665.4 | -2.0% |
| 20000 个浮点数 | 173.7 → 224.1 | +29.0% | 796.7 → 2127.5 | +167.0% |
| 5000 组空容器 | 448.2 → 441.1 | -1.6% | 609.3 → 719.6 | +18.1% |
| 2000 个 256 字节字符串 | 10626.3 → 9208.3 | -13.3% | 5802.5 → 19078.5 | +228.8% |
| 128 个大对象（约 518 KiB） | 基线崩溃 → 4465.2 | 无可比数据 | 基线未完成 → 10703.3 | 无可比数据 |

大对象基线返回 `0xC0000005`（访问违规），新版正常结束。该项证明此样本的崩溃已修复，不作为加速比例。

### 冷启动 arena 容量（字节）

| 样本 | 基线 | 新版 | 减少 |
| --- | ---: | ---: | ---: |
| 标量 | 65,536 | 0 | 100.0% |
| 单个小对象 | 65,536 | 1,024 | 98.4% |
| 2000 个对象 | 1,747,740 | 436,935 | 75.0% |
| 20000 个整数 | 3,266,730 | 1,524,474 | 53.3% |
| 20000 个浮点数 | 1,980,006 | 1,980,006 | 0.0% |
| 5000 组空容器 | 9,920,124 | 1,680,021 | 83.1% |
| 2000 个 256 字节字符串 | 2,072,004 | 259,000 | 87.5% |

小对象解析下降约 18%，对象数组和字符串数组解析也有回退；本次没有实现所有路径的统一加速。可确认的收益包括：标量/整数/浮点解析、多个样本的序列化吞吐、冷启动 arena 占用，以及多线程路径的稳定性。浮点样本的 arena 容量在本轮未下降。

## 复现

先将旧版本 `include/chjson/chjson.hpp` 放到独立目录，保证其路径结构仍为 `旧目录/chjson/chjson.hpp`，然后执行：

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCHJSON_BUILD_TESTS=ON -DCHJSON_BUILD_CONFIG_TESTS=ON -DCHJSON_BUILD_BENCHMARKS=ON -DCHJSON_BASELINE_INCLUDE=/absolute/path/to/old/include
cmake --build build/release --config Release --parallel
ctest --test-dir build/release -C Release --output-on-failure
python benchmark/compare_baseline.py --baseline build/release/chjson_baseline_bench --candidate build/release/chjson_bench --output benchmark-results.json
```

Windows 可执行文件带 `.exe`；使用多配置生成器时还需加入对应配置目录。Linux sanitizer 验证使用独立 Debug 构建并启用 `CHJSON_ENABLE_SANITIZERS=ON`，运行时设置 `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1`。

## 使用边界

严格 UTF-8 校验、较小分配策略和异常保护并不保证每种输入都更快，本报告保存的各项测量包含回退项。需要按真实业务文档选择解析模式和并行门槛。

`release_thread_caches()` 只作用于调用线程，不能回收活文档或其他线程的缓存，也不停止共享线程池。显式启用实验性 allocator 时，其页面仍保留到进程退出。raw token 的首次 lazy double 转换会修改值内缓存，对同一值的并发首次访问仍需要外部同步。验证平台为 x64，未包含 ARM、32 位与 ThreadSanitizer。
