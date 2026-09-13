# NEXT STEP — CCCI/基带:执行手册(2026-09-13 晚)

**工作目录**:`/home/furruka/文档/项目/Kernel/6.18/`
**权威细节**:`HANDOFF.md` ← 真源(只看它决定下一步);本文件是精简版。
**私有文档树的标签**:文件名前缀 `[ACTIVE]` 才代表"要做/可依赖",其余 `[DONE]`/`[BLOCKED]`/
`[SKIP]`/`[CANCEL]`/`[FAILED]`/`[DANGER]`/`[DEPRECATED]` **都不是欠账** ——
见 `docs-local/handoff/00-INDEX.md`;细节章节见 `[ACTIVE] 80-42.md`。

---

## 一句话

`ccci_util` 的 LK 信息解析**从不运行**(已修,见 §80.33)与 **CCIF 访问门恒 0**(已修,§80.35)两个真缺陷
已解决;**HS1 的根因由 pearl 指名并已在本树修好**(§80.41:CCIF SRAM 的 "smem info tail" 必须
**同时写 baseA 与 baseB**,否则基带早停 `boot_status 0x5443000C/0x53320000` = "TC"/"S2")。
**修复在 `ccci_ccif.ko` 里,已构建、vermagic 与设备逐字一致 ⇒ 设备回来后第一步就是验它。**

---

## 0. 前置检查(设备)

```sh
# 本机 IP 与密码**不进 git**（docs/ 是公开仓库的一部分）：见本地私有 HANDOFF.md 第 1 节。
IP=<看 HANDOFF.md>; PW=<看 HANDOFF.md>
SSH="sshpass -p $PW ssh -o StrictHostKeyChecking=no root@$IP"
$SSH 'uname -r; uptime; cat /proc/modules | wc -l'
```
期望:`6.18.0-g5d55376a6791-dirty`、`uptime` 接近 0、模块数 **0**。
(上一次会话结束时设备停在没走完的 reboot 里,需用户先重启。)

## 1. 部署(主机)

```sh
cd /home/furruka/文档/项目/Kernel/6.18
SCP="sshpass -p $PW scp -o StrictHostKeyChecking=no"
$SCP drivers/misc/mediatek/eccci/ccci_{auxadc,md_clk,rtc,md_all,fsm_scp,ccif}.ko root@$IP:/root/
$SCP flash/tools/hs1_test.sh root@$IP:/root/
```
**部署前必须** `modinfo -F vermagic <ko>` 复核 = `6.18.0-g5d55376a6791-dirty`
(§80.39:之前那条"LOCALVERSION 钉定"的说法是错的,AUTO=y 时 git 后缀总会追加)。

主机 md5 前 12 位对照:

| 模块 | md5 | 含什么 |
|---|---|---|
| `ccci_ccif.ko` | `e90a12e56d2f` | flag 修复 + **baseB 尾字修复(HS1 关键)** |
| `ccci_md_all.ko` | `f44b2a01155a` | §80.33 布局 WA + RPC 内核回退 |
| `ccci_fsm_scp.ko` | `d41629205ee9` | — |
| `ccci_rtc.ko` | `8b14a1bbe253` | — |
| `ccci_md_clk.ko` | `d4dc7a286e49` | — |
| `ccci_auxadc.ko` | `dd5fe9f0d8d6` | — |

## 2. 执行(一条命令)

```sh
$SSH 'sh /root/hs1_test.sh; echo RC=$?'   # $IP/$PW 同前
```
脚本内:**单脚本完成全部步骤**(加载 → 点火 → 采集),日志用
`systemd-run --unit=ccciloghs1 --collect` 起;`mdinit.py` 后台常驻(它必须**一直持有**
`/dev/ccci_monitor`,关闭会触发 `force_md_stop`)。

## 3. HS1 判据(全中才算过)

- `/proc/interrupts`:`CCIF_AP_DATA0`(hwirq 273)、`CCIF_AP_DATA1`(hwirq 274)**计数非 0**
  (注意本 SoC 的 virq ≠ SPI+32,别用加 32 推 virq);
- `/root/ccci_dump_hs1.txt` 出现 **"MD_QUERY_MSG"** 与 **"send runtime data"**;
- dmesg 不再出现 `MD_BOOT_HS1_FAIL`;`md_boot_stats0/1` 不再是 `5443000C/53320000`。

## 4. 分支

- **到了 HS1** ⇒ 与 pearl 持平。HS2 线的 RPC 内核回退已就位;接着看 `/proc/ccci_dump` 里
  DRDI 请求/回包与 `hs2_got/hs2_done`。MOLY 之后会要 `MD1_SIM1/SIM2_HOT_PLUG_EINT`(节点定义
  见 §80.41 附注,⚠ vendor-compatible 要先确认 eint/irq_domain 接得上)与 DRDI 表
  (写 `SMEM_USER_MD_DRDI`)——这些归刷机包。
- **仍停 TC/S2** ⇒ 读 **MD 侧**窗口 CHDATA 尾字确认 `0x7274626E` 是否写进 baseB;
  确认 `ccci_reset_ccif_hw` 走到且 `devapc_check_flag=1`;若都成立 ⇒ 转 §80.39 源时钟线
  (`MD_SRCCLKENA` 只读、ATF 时钟请求 -15,**只能刷机/用户在场**)。

## 5. 刷机包(等用户点头;全部有据)

1. **`ccci_util` 两处**(已在树里、已编译):LKINFO stash fallback + 删 `ccci_util_probe` 门
   ⇒ 官方 LK 解析跑起来;静态确认无内置代码依赖其符号,可安全内置。
2. (可选)DT 补 `ccci,modem_info_v2`;ccifdriver 补 6 条 `clocks` + `clk-mt6895-bus.c`;
   §80.41 附注的两个 EINT 节点与 `md_attr_node`(**`md_drdi_rf_set_idx` 我们 = 0x80**)。
3. 纪律:**单槽刷**、DTB 内嵌于 Image ⇒ `build dtbs THEN Image.gz`、刷前比对内嵌 DTB 字节。

## 6. 硬约束(高于一切,违反 = 设备死到用户回来)

1. 不碰未证实可读的地址/寄存器。**已证实会卡**:MD bank0 `0xD0000000`、MD 域未上电时的
   CCIF `0x1020xxxx`、DEVAPC 实例 `0x1000e000` 等、单独调 `MD_CLOCK_REQUEST` SMC。
2. **不在"eccci 模块已加载 + MD 域已掉电"的状态下 reboot**(停止路径会碰 CCIF ⇒ 卡关机)。
3. 只允许:读 SMEM(`0x8e000000`)/CCB(`0x89000000`)/tag(`0xbdbf0000`)、infra_ao 读、
   insmod/rmmod 已证实可卸载的模块。
4. 设备端一律单脚本;SSH 中途命令会被会话/复位杀掉。

## 7. 索引

- HANDOFF:§80.33(真缺陷 1)、§80.35(真缺陷 2:CCIF 观测曾是假 0)、§80.36/§80.39(两条"AP 侧
  无安全杠杆"的结论)、**§80.41(HS1 根因 + 已修)**、**§80.42(执行手册)**。
- 记忆:`~/.zcode/cli/memories/projects/6.18-627358bfbb5c1ee5/ccci-roadmap-status.md`。
- 主机测试:`cd tools/testing/ccci-probe && make test`(ringbuf 21 项 + 72 组合矩阵,全绿)。
