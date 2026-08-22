# iSH-AOK

> **翻译说明：** 本文是 [README.md](README.md) 较早修订版（2026-08-14）的翻译，缺少 549
> 版新增的原生程序（SmallCLUE、bash、zsh）相关内容。最新内容请参阅 README.md。
>
> 特别是许可证方面：`git submodule update --init --recursive` 会包含 `deps/bash`，
> 因此默认构建会产生 GPLv3 二进制文件。若打算分发，请先阅读 README.md 中的
> "Native bash and licensing" 一节。

iSH-AOK 是 [ish-app/ish](https://github.com/ish-app/ish) 的一个分支（fork），在此基础上添加了用于日常开发的产品、工具链和平台相关改动。

Testflight: https://testflight.apple.com/join/X1flyiqE

这个分支不只是改个名字。它包含了分支专属的行为、内置的根文件系统、诊断相关工作、File Provider 集成，以及对四种客户机架构的支持。如果你想要上游的 iSH，请使用 `ish-app/ish`。如果你正在这个仓库中进行开发，这份 README 才是你需要参考的文档。

## 本分支新增的内容

- 分支专属的应用标识:
  - 产品名 `iSH-AOK`
  - Bundle root `app.ish.iSH-AOK`
- **四种客户机架构**，全部基于 JIT：`i386`、`amd64`（x86_64）、`arm64`（aarch64）和 `riscv64`。
- 内置在应用中的根文件系统（Alpine 3.23.3 与 Devuan 6，各自提供 i386、x86_64 和 aarch64 版本），以及包含 riscv64 在内的可下载镜像。
- 通过 iOS 系统 API 暴露客户机文件的 File Provider 支持。
- 可选加速器：用原生代码替换热点 libc 例程，以及加密与 pixman 卸载。
- 该分支专属的额外诊断与运维相关改动。

## 客户机架构

四种客户机都受支持，且都通过 gadget JIT 运行。没有任何一种是原生执行的：在 ARM 宿主机
上，一条 `arm64` 客户机指令同样是一次 gadget 分派，和 `riscv64` 完全一样。与宿主机同属
一个 ISA 家族只会让每个 gadget 的主体更便宜，而不会让它免费。

| 客户机 | 状态 |
|---|---|
| `i386` | 最初的客户机，仅 JIT |
| `amd64` | 已支持，JIT |
| `arm64` | 已支持，JIT |
| `riscv64` | 已支持，JIT |

各客户机的回归测试套件在四种架构上都能通过。解释器属于遗留实现且即将移除，新的工作
应当以 JIT 为目标。

相关文件：

- [jit/gen.c](jit/gen.c) 所有客户机的指令翻译
- [jit/jit.c](jit/jit.c) 代码块缓存与分派
- [kernel/calls.c](kernel/calls.c) 各 ABI 的系统调用表
- [docs/amd64_port_plan.md](docs/amd64_port_plan.md)
- [docs/aarch64_guest_plan.md](docs/aarch64_guest_plan.md)

## 性能

引擎受限于分派开销，约为每次 gadget 分派 6.8 ns，因此开销与客户机指令数成正比。测量
方法与数据见 [docs/perf_benchmarks_2026_08.md](docs/perf_benchmarks_2026_08.md)。

指令融合与返回缓存可以在运行时按客户机切换，便于做 A/B 测量：

```sh
cat /proc/ish/arm64_jit_fuse          # 每个系列一行 "名称 on|off"
echo retcache=0 > /proc/ish/arm64_jit_fuse
echo all=1 > /proc/ish/riscv64_jit_fuse
```

`i386`、`amd64`、`arm64` 和 `riscv64` 都有对应的节点。这些位在翻译时被读取，因此改动
只影响新编译的代码块；每次计时测量都应作为独立进程运行。
[tests/manual/jit_fuse_ab.sh](tests/manual/jit_fuse_ab.sh) 会自动执行交替的 A/B 测量，
并在退出时恢复原有掩码。

## 可选加速器

三者**默认均为关闭**，需要显式启用：

| 功能 | CLI | 作用 |
|---|---|---|
| HLE | `ISH_HLE=1` | 用原生代码替换热点 libc 例程（`memcpy`、`strlen`、`memcmp` 等） |
| 加密 | `ISH_CRYPTO_ACCEL=1` | AES-GCM 与 ChaCha20-Poly1305 卸载 |
| Pixman | `ISH_PIX_ACCEL=1` | pixman 合成卸载 |

其中 HLE 影响最大。在以被替换例程为主的循环中，客户机可以从比原生慢约 250 倍改善到
约 1.4 倍，因为工作发生在一次原生调用内部，而不是每条客户机指令一次分派。它是纯粹的
快速路径：无法识别的 libc 不会匹配，直接回退到普通翻译。`ISH_HLE_STATS=1` 会输出每个
函数的调用次数。

## 仓库结构

- `app/`: iOS 应用、UI、根文件系统选择、诊断、File Provider 集成。
- `emu/`: 客户机 CPU 状态、内存、TLB、FPU/向量支持。
- `kernel/`: 系统调用翻译、进程模型、exec、信号、内存管理。
- `fs/`: 文件系统层、fakefs、procfs、tmpfs、挂载。
- `jit/`: gadget JIT 及各客户机的翻译器。
- `tests/`: 端到端测试与客户机侧回归套件。
- `tools/`: 开发者工具与宿主机侧辅助脚本。

## 克隆

本仓库使用子模块。

```bash
git clone --recurse-submodules git@github.com:emkey1/ish-AOK.git
cd ish-AOK
```

如果你已经在没有子模块的情况下克隆：

```bash
git submodule update --init --recursive
```

## 构建依赖

本地开发通常需要：

- Xcode
- Python 3
- Meson
- Ninja
- Clang/LLVM 工具链
- sqlite3
- libarchive

macOS 上常见的准备方式：

```bash
brew install meson ninja llvm libarchive
```

`sqlite3` 通常已经存在。

在 Apple Silicon 上请注意，构建会优先在 `/opt/homebrew` 而不是 `/usr/local` 下查找
`llvm`、`libarchive` 和 `unicorn`。即使旧的 Intel 版 Homebrew 仍然存在，其中的 x86_64
版本也不会被使用。

## 构建 iOS 应用

用 Xcode 打开 [iSH-AOK.xcodeproj](iSH-AOK.xcodeproj) 并构建 `iSH` scheme。

分支专属的重要设置：

- Bundle ID 由 [app/iSH.xcconfig](app/iSH.xcconfig) 决定。
- `ROOT_BUNDLE_IDENTIFIER` 默认为 `app.ish.iSH-AOK`。
- 项目使用分支专属的调试配置 `Debug-ApplePleaseFixFB19282108`。

面向真机的命令行构建：

```bash
xcodebuild \
  -project iSH-AOK.xcodeproj \
  -scheme iSH \
  -configuration Debug-ApplePleaseFixFB19282108 \
  -destination 'generic/platform=iOS' \
  -allowProvisioningUpdates build
```

iOS 构建脚本会把仓库根目录下的根文件系统压缩包复制进应用包。缺少其中任何一个，对应的
内置根文件系统就无法使用。

## 构建原生 CLI / 模拟器

做模拟器一侧的工作时，Meson 构建比完整的 Xcode 构建快得多。

```bash
meson setup build --buildtype=debugoptimized
ninja -C build
```

请使用 `--buildtype=debugoptimized`。Meson 的默认值是 `debug`（`-O0`），而 `-O0` 的
模拟器不只是更慢，它会让在其上得到的测量结果失去意义。在这种构建上，客户机的
`uname -v` 会包含 `" unoptimized"`。

运行客户机：

```bash
./build/ish -f build/alpine /bin/login -f root
```

从根文件系统压缩包创建文件系统：

```bash
./build/tools/fakefsify alpine-minirootfs-*.tar.gz alpine
```

## 原生程序

原生程序是编译进应用内部的宿主代码。对 `/AOK/native` 下的路径执行 `execve`，不会去
加载一个客户机镜像，而是直接分发到 iSH-AOK 内部的一个函数，调用方察觉不到区别。
`/AOK/native` 中每个注册表（`kernel/native.c`）里的程序各占一项 —— `smallclue`、
`motepad`、`hx`、`rust-probe`、`bash`、`zsh`、`zsh-multio` —— 其余全是指向它们的符号
链接，和 busybox 一样由链接名选择 applet：

| 程序 | 说明 |
|---|---|
| `/AOK/native/smallclue` | busybox 风格的多合一工具箱，由 `argv[0]` 选择 applet |
| `ssh`、`scp`、`sftp`、`ssh-keygen`、`ssh-copy-id` | OpenSSH，作为 SmallCLUE 的 applet（构建时不含 OpenSSL） |
| `vi` | Nextvi 编辑器，SmallCLUE 的一个 applet |
| `/AOK/native/motepad` | 无模式的终端文本编辑器，对应 Workspace 的 MotePad applet |
| `/AOK/native/hx` | [helix](https://helix-editor.com)，带语法高亮的模式化编辑器。采用 MPL-2.0，因此和 bash 一样有构建开关（`-Dnative_helix`）；其语法文件位于 `/AOK/native/libs` |
| `/AOK/native/rust-probe` | 用于验证 `hx` 所依赖的 Rust-on-shim 路径的探针，日常用不到 |
| `/AOK/native/bash` | 见[原生 bash 与许可证](#原生-bash-与许可证) |
| `/AOK/native/zsh` | 见[原生 zsh](#原生-zsh) |

`/AOK/tools/native-links.sh` 会建立把这些 applet 放进 `PATH` 的符号链接集合，
`--shell bash|zsh|/path` 用于切换登录 shell，`--remove` 撤销这两者。应用内文档在
`/AOK/docs/native-programs.md`（它们是什么）和 `/AOK/docs/native-setup.md`（如何配置），
源文件在 [opt/AOK/docs/](opt/AOK/docs) 下。

难点不在速度，而在于原生程序必须回答关于*客户机*的问题，而不是关于它实际运行其上的
那台 iPhone：环境变量、身份、文件系统、`/etc/hosts` 与 `/etc/resolv.conf`、terminfo、
locale 以及 rc 文件位置，全都由一个先于系统头文件编译进来的 shim
（`kernel/native_libc.c`）路由到根文件系统。真正的判据不是「这个函数是纯的吗？」，
而是「这个函数的答案在宿主和客户机上会不会不同？」。`tools/check-native-libc.py`
就是这道关卡：它扫描构建产物，报告原生程序引用的、不在显式白名单上的每一个宿主
libc 符号。它是特意手动运行的，没有接进构建流程。

## 原生 bash 与许可证

> 关于许可证，以英文版 README 的
> [Native bash and licensing](README.md#native-bash-and-licensing) 为准，
> 下文为便于理解的译文。

bash 作为原生程序编译进应用。收益在于解释执行而非 fork：算术循环比模拟执行的 shell
快约 16 倍，而子 shell 和命令替换则接近持平，因为原生程序无法 `fork`，只能重新启动
自身。数据与测量方法见 [docs/bash_native_plan.md](docs/bash_native_plan.md)。这同时
也意味着二进制里带上了 GPLv3 代码：bash 本身、随附的 readline，以及 GNU termcap。

这一点对 App Store 分发很重要。iSH-AOK 本身同样是 GPLv3，但
[LICENSE.IOS](LICENSE.IOS) 是*本项目*版权持有者作出的承诺，即不就 GPL 与 Apple 条款
之间的冲突进行追究。它无法约束持有 bash 版权的 FSF，而 FSF 已经两次让 GPL 软件从
App Store 下架 —— 2010 年的 [GNU
Go](https://www.theregister.com/2010/05/27/gnu_go_fsf_apple_itunes/) 和 2011 年的
[VLC](https://www.fsf.org/blogs/licensing/vlc-enforcement)，理由是商店的使用条款
构成了 [GPL 第 6 条](https://www.fsf.org/blogs/licensing/more-about-the-app-store-gpl-enforcement)
所禁止的「进一步限制」。FSF 表示该分析适用于所有 GPL 版本，而不只是 v3。

因此它是一个构建选项：

```bash
meson setup build .                          # auto：存在 deps/bash 时开启
meson setup build . -Dnative_bash=disabled   # 二进制中不含第三方 GPL
meson setup build . -Dnative_bash=enabled    # 缺少 deps/bash 时构建失败
```

configure 会在 `Licensing` 标题下打印实际生效的是哪一种。请查看，不要假定：

```
Licensing
  native bash: no -- no third-party GPL in the binary
```

`disabled` 会把 bash、readline 和 termcap 完全排除在归档之外 —— 0 个目标文件，已用
`ar t` 核实。用户仍然可以用 bash：来自客户机根文件系统、由模拟执行的 `/bin/bash`，
这与 Devuan 或 Alpine 中其他所有 GPL 工具处于相同的「单纯聚合」地位。

**仅仅删掉 `kernel/native.c` 里的 applet 表项是不够的。** `meson.build` 用
`link_whole` 把这些归档整体链入，因此无论有没有东西引用，目标文件都会进入二进制 ——
实测在删除注册表项之后，仍有 144 个 bash 目标文件和 35 个 readline 目标文件留在里面。
只有构建选项能真正移除它们。

二进制中的其余部分不含第三方 GPL：SmallCLUE 是 MIT，OpenSSH 和 libarchive 是 BSD，
liblzma 属于公有领域，而 `deps/linux` 不会被编译进这个目标。

## 原生 zsh

zsh 作为第三个原生程序编译进来，通过 `/AOK/native/zsh` 访问，并且**默认开启** ——
`-Dnative_zsh=disabled` 可以将其排除。与 bash 不同，这里没有许可证问题：zsh 的许可证
是宽松型的，其参与编译的 C 代码没有 GPL 部分。

它是一个可用的 shell。行编辑器 ZLE 可以工作：提示符、回显、编辑、历史按键、自动折行、
完整的终端协商都正常。此前无法工作的 `fork` 现在也可以了。原生程序是客户机任务线程上
的一个 C 函数而非一个进程，因此 `fork` 无法复制地址空间；zsh 转而把自身状态序列化成
一个脚本并重新启动自己，这一设计先在 bash 上得到验证
（`deps/zsh/Src/aok_fork.c`、`deps/bash/aok_fork.c`）。命令替换、管道、子 shell 和
后台作业都走这条路径：

```
% echo $(echo A); echo B | tr B C; (echo D); sleep 0.1 & wait; echo E
A
C
D
E
```

MULTIOS 重定向使用配套的原生程序 `zsh-multio`，因为那些描述符必须由 shell 以外的
东西持有。

`/AOK/tools/native-links.sh --shell zsh` 可以把它设为登录 shell。

客户机中随附了 119 个差分用例，位于 `/AOK/tests/native_zsh_fork_state.sh`，每一条
期望值都取自真实 zsh 的输出，而不是「看起来合理」的结果；其中 116 条通过。失败的两条
是**进程替换** —— `<(...)` 和 `>(...)` —— 而这属于根文件系统而非 shell 的性质：它需要
`/dev/fd`，Alpine 镜像没有提供，因此在那里用模拟执行的 `/bin/bash` 也同样失败；而在
`/dev/fd` 是指向 `/proc/self/fd` 的符号链接的 Devuan 上，两个 shell 都正常。两个确实
属于 shell 自身的已知缺陷记录在
[docs/release-notes-since-iSH-AOK_549.md](docs/release-notes-since-iSH-AOK_549.md)
的 *Known gaps* 中：模式在首次使用时被编译并缓存进语法树，而当时生效的选项没有被任何
地方记录下来，因此重新启动的子进程可能在与父进程不同的选项下编译它；以及在 multio 下
`pipestatus` 报告 `1 0`，而 zsh 报告 `0 0`。

`deps/zsh` 这棵树是 [emkey1/zsh](https://github.com/emkey1/zsh) 仓库 `ish-aok` 分支
的子模块。它携带了 zsh *生成的*源文件 —— `config.h`、`Src/signames.c`、各模块的
`.mdh`/`.epro`/`.pro` —— 这些是逆着上游的 `.gitignore` 提交进去的，因为本构建用 meson
编译 zsh，从不运行 zsh 自己的 `make`。因此检出之后无需 configure 步骤即可构建：

```bash
git submodule update --init deps/zsh
```

它被配置为仅使用 termcap，并且所有模块静态链接。两者都是强制的：iOS SDK 提供了
curses 的 `.tbd` 存根却没有 `curses.h`/`term.h`，而原生程序无法 `dlopen` —— 单用
`--disable-dynamic` 会让 `zsh/regex` 被悄悄映射为 `link=no`，于是 `[[ =~ ]]` 在运行时
失败。

## 回归测试

宿主机侧测试：

```bash
meson test -C build
```

在 `long double` 不是 x87 80 位格式的宿主机上，`float80` 会被跳过；Apple Silicon 就属
于这种情况，因为那里根本没有可供比较的参考值。在 x86_64 宿主机上它会完整运行。

客户机侧套件是主要的回归关卡。它位于 [tests/manual/](tests/manual)，在客户机内以只读
方式提供于 `/AOK/tests`，包含约 120 个专项程序，覆盖信号、futex、进程生命周期、文件
系统层、JIT 以及各架构的指令行为。每个程序在失败时以非零值退出，并支持 `-v`。

在客户机内：

```sh
sh /AOK/tests/setup-regressions.sh --install-deps --run   # 全部构建并运行
sh /AOK/tests/setup-regressions.sh --only fs_conformance,futex_core --run
```

新增测试时，把源码放进 `tests/manual/`，并登记到
[fs/aok-tests.manifest](fs/aok-tests.manifest)，正是这个清单把它发布到 `/AOK/tests`；
同时加入 [tests/manual/setup-regressions.sh](tests/manual/setup-regressions.sh) 以便被
构建和运行。清单里遗漏的测试会在设备上悄无声息地消失。

## 使用根文件系统

应用内置：Alpine 3.23.3 与 Devuan 6（excalibur），各自提供 `i386`、`x86_64` 和
`aarch64` 版本。包括 `riscv64` 和 Arch 在内的更多镜像可在应用内下载，目录见
[deps/rootfs-manifest](deps/rootfs-manifest)。

根文件系统选择界面与元数据处理位于：

- [app/Roots.m](app/Roots.m)
- [app/RootsTableViewController.m](app/RootsTableViewController.m)

说明：

- 应用会为每个导入的根文件系统记录客户机 ABI。
- 所有已安装的根文件系统还会在已启动的客户机中以读写方式暴露于 `/AOK/roots/<名称>`，
  因此你可以 chroot 进入另一种架构的用户空间。
- 受管理的根文件系统会同步 File Provider 域。

## 日志与诊断

日志由 [app/iSH.xcconfig](app/iSH.xcconfig) 中的 `ISH_LOG` 控制；CLI 构建则使用
`meson configure -Dlog=...`。

```xcconfig
ISH_LOG = verbose strace
```

常用通道：`strace`（系统调用参数与返回值，最有用）、`verbose`，以及 `instr`（每条指令，
非常慢）。

日志器默认值在 iPhone 与模拟器上为 `nslog`，在 macOS 上为 `dprintf`。

## File Provider

本分支包含一个 iOS File Provider 扩展，使客户机文件可以通过系统文件 API 呈现。

- [app/FileProvider/FileProviderExtension.m](app/FileProvider/FileProviderExtension.m)
- [app/FileProvider/FileProviderEnumerator.m](app/FileProvider/FileProviderEnumerator.m)
- [app/FileProvider/FileProviderItem.m](app/FileProvider/FileProviderItem.m)

这是分支专属功能，属于这里维护的产品范围。

## 发布自动化

[tools/release-aok.sh](tools/release-aok.sh) 封装了归档与导出流程：

```bash
./tools/release-aok.sh preflight
./tools/release-aok.sh archive
./tools/release-aok.sh export latest /tmp/iSH-AOK-export
./tools/release-aok.sh upload-fastlane      # 完整的 TestFlight 自动化
```

`upload-fastlane` 使用既有的 `fastlane upload_build` lane，需要 Ruby/Bundler/Fastlane
环境以及签名和认证密钥。

发布本身的做法是：提升 `CURRENT_PROJECT_VERSION`，添加
`docs/release-notes-since-iSH-AOK_<N>.md` 和 `docs/release-summary-iSH-AOK_<N>.md`，
然后在该提交上打上 `builds/iSH-AOK_<N>` 标签。标签名本身是有作用的：
`.github/workflows/build-release-ipa.yml` 由 `builds/iSH-AOK_*` 触发，因此命名不同的
标签不会产生发布构建。

## 分支

- `working` 是默认分支，也是活跃的集成分支。缺陷修复、功能开发和发布候选都先进入这里。
- `amd64`、`aarch64` 和 `riscv` 原本是各客户机的初期开发分支。这些工作已经合并进
  `working`，而 `working` 会构建全部四种客户机。

## 与上游的关系

iSH-AOK 基于上游 iSH，但有意与之分化。

这意味着：

- 上游 README 的说明对本分支可能不完整或不正确
- 分支名称与构建配置并不相同
- 内置根文件系统与运行行为是本分支特有的
- 不应假定这里的 amd64、arm64 和 riscv64 客户机在上游同样存在

在带有 `upstream` 远端的克隆中使用 `gh` CLI 时，请加上 `--repo emkey1/ish-AOK`。否则
`gh` 会解析到 `ish-app/ish`，回答的是上游的工作流、发布和标签，而不是本分支的。

## 致谢

ARM64 客户机的工作受到 [OpenMinis/ish-arm64](https://github.com/OpenMinis/ish-arm64)
的启发，部分内容也改编自该项目。它是 `ish-app/ish` 的一个 GPLv3 分支，独立实现了同样的
能力。文件级别的署名见 [docs/CREDITS-aarch64.md](docs/CREDITS-aarch64.md)。

## 许可证

请参阅：

- [LICENSE.md](LICENSE.md)
- [LICENSE.IOS](LICENSE.IOS)
