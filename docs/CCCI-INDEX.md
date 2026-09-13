# CCCI / 基带（modem）工作索引

**最后更新**：2026-09-13 晚。

> 这份文件是**公开仓库里**的入口，只写"东西在哪、怎么编译、别踩什么坑"。
> 真正的现状、下一步与全部历史放在**本地私有的**两处（都进 gitignore，因为含台架 IP 与凭据）：
>
> - **`HANDOFF.md`（仓库根，私有主文件）** —— 只看它决定下一步：现状 / 命令与判据 / 硬约束。
> - **`docs-local/handoff/`** —— 从旧 HANDOFF（3217 行）拆出的大全：
>   - `00-INDEX.md` 索引（**先看这个**）：[Active] 可依赖 / [Completed] 历史但成立 /
>     [Deprecated] 已被更正、别引用；
>   - `sections/*.md` 每节一个小文件，首行带标签与"被谁更正"；
>   - `archive/HANDOFF-full-20260913-v80.42.md` 逐字旧档。

## 这份工作的当前状态（一句话版）

`ccci_util` 的 LK 信息解析**从不运行**、CCIF 访问门 `devapc_check_flag` **恒 0**（⇒ 以往所有 CCIF
观测都是软件伪造的 0）——两个真缺陷已修。**HS1 的根因已定位并修好**：CCIF SRAM 的
"smem info tail"（magic `0x7274626E` + MDSS 的 MD 视图地址 + size）必须**同时写 AP 侧与 MD 侧**，
否则基带早停（`boot_status` = "TC"/"S2"）永不发 HS1。修复在可加载模块里，等待设备验证。

## 代码入口

| 路径 | 作用 |
|---|---|
| `drivers/misc/mediatek/eccci/` | 官方 5.10 `_ace_race` CCCI 驱动整体移植（`=m`，CCIF=y / CLDMA=n / C2K=n） |
| `drivers/misc/mediatek/eccci/hif/ccci_hif_ccif.c` | CCIF HIF：门控/复位、SRAM 尾字发布、**devapc 访问门** |
| `drivers/misc/mediatek/eccci/ccci_modem.c` | `ccci_md_config()`：MD 内存/共享内存布局 |
| `drivers/misc/mediatek/eccci/port/port_rpc.c` | RPC 端口（`AMMS_DRDI_CONTROL` 已改为内核处理） |
| `drivers/misc/mediatek/ccci_util/` | 共享库（tag 解析、LK 信息解析、SMEM/CCB 记账）；`ccci_tag_parse.c` 是抽出来的共用解析器 |
| `drivers/misc/mediatek/ccci_smem_dump/` | 只读取证模块（SMEM/CCB/tag/infra_ao 读；MD bank0 与 DEVAPC 读**默认关闭**，见下） |
| `arch/arm64/kernel/setup.c` | `CCCI-LKINFO`：把 LK FDT 里的 `ccci,modem_info_v2` 存进全局（内嵌 DTB 会丢掉它） |
| `tools/testing/ccci-probe/` | 主机测试：`make test`（tag 解析 / ringbuf / smem 布局 + Kconfig 矩阵，全绿） |

## 编译与部署要点

- 模块必须**逐字匹配运行内核的 vermagic**：本机配方 = Kconfig 里
  `CONFIG_LOCALVERSION_AUTO=n` + `CONFIG_LOCALVERSION="-g<设备内核的 sha>-dirty"`，
  并强制重生成 `include/config/kernel.release` 与 `include/generated/utsrelease.h`；
  **每次部署前用 `modinfo -F vermagic` 复核**。`LOCALVERSION=` 环境变量**不足以**顶掉 git
  后缀（`CONFIG_LOCALVERSION_AUTO=y` 时总会追加，这是本项目踩过的坑）。
- 构建：`make ARCH=arm64 LLVM=1 M=drivers/misc/mediatek/eccci modules`（**只允许 Clang/LLVM**，
  用 GCC 整树重编会导致 boot-loop）。
- 设备端加载顺序（保证 CCIF 被编程时 MD 电源域是开的）：
  `ccci_auxadc` → `ccci_md_clk` → `ccci_rtc` → `ccci_md_all` → `ccci_fsm_scp` → `ccci_ccif`。

## 安全红线（无凭据但必须遵守）

0. **不要把台架 IP、密码、任何凭据写进 `docs/` 下的文件**——`docs/` 是公开仓库的一部分，
   而这个仓库会推送。凭据只属于 gitignore 的 `HANDOFF.md` 与 `docs-local/`。
   （2026-09-13 曾把 IP/密码写进 `docs/NEXT_STEP.md` 并提交，发现后已重写该提交、清理对象，
   未推送；此后凡涉及设备命令，公开文档里一律用 `$IP`/`$PW` 占位。）
1. **不要读未经证实可读的物理地址/寄存器**：读 MD 私有 DRAM `0xD0000000` 会 Oops 并打死
   workqueue；MD 电源域未上电时读 CCIF `0x1020xxxx` 会挂总线；DEVAPC 实例块不可读。
2. **不要在"eccci 模块已加载 + MD 域已掉电"的状态下 reboot**：停止路径会碰 CCIF，关机流程会卡住。
3. 禁止 `/dev/mem`（在本机任何访问都可能挂总线）。
4. 设备端操作一律写成**一个脚本**再跑（SSH 中途命令会被会话/复位杀掉），日志用
   `systemd-run --unit=... --collect` 起。
