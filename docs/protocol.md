# 网页与机器人协议 v1

传输：同源 `/ws`，WebSocket 文本 JSON，UTF-8。请求最大 4096 字节、嵌套最多 12 层。
网页和电脑模拟器使用同一份 C++ 协议解析器。版本不符、重复键、非有限数、错误字段类型均拒绝。

## 会话、时间和应答

每次连接分配新 `session`，请求 `id` 为该连接内递增的正整数（最大 2^53−1）。新连接不能复用旧控制权。
先发 `sync`，根据响应的 `now_ms` 建立控制器单调时间估计；每 100 ms 重新同步。

```json
{"v":1,"id":1,"op":"sync"}
```

动作命令：

```json
{"v":1,"id":2,"issued_ms":1234,"op":"claim","args":{}}
```

`issued_ms` 是控制器运行时间，单位 ms，不是电脑 UTC 时间。动作到达时超过 250 ms，或比控制器时间超前 100 ms 以上，拒绝为 `expired_command`。
序号重复或倒退拒绝为 `duplicate_or_reordered`；被拒绝的有效序号也视为已消耗。每次操作使用新序号，不自动重发运动请求。
`sync` 不需要时戳。合法新序号的 `stop_all` 不受命令时效和控制权限制，保证观察端也能发软件停止。

```json
{"v":1,"type":"ack","id":2,"accepted":true,"code":"accepted","now_ms":1235}
```

**accepted 只表示请求通过控制器校验。电机执行是否成功，以新鲜的反馈和现场观察为准。**
`clamped` 表示已接受但被限幅；拒绝含具体 `code`。`sync` 返回 `type:"state"`，并包含 `state`。

## 命令

电机名称固定为 `left_drive`、`right_drive`、`cutter`。

| op | args | 行为 |
| --- | --- | --- |
| `sync` | 无 | 获取完整状态、配置和最近事件 |
| `claim` | 无 | 空闲时取得控制权，保留停止锁定 |
| `heartbeat` | 无 | 仅当前控制端续租；每 100 ms 发出，500 ms 失效 |
| `release` | 无 | 全部停止、取消刀片解锁、释放控制权 |
| `reset` | 无 | 明确复位控制器锁定，不使能电机、不清电机错误 |
| `select` | `mode:"drive"或"test"`, `motor` | 所有电机停用且有效反馈显示停转时切换；单机模式仅选中电机可使能 |
| `unlock_cutter` | 无 | 仅解锁刀片，仍须使能和设置速度 |
| `enable` | `motor` | 要求板级与电机已核对、在线、无故障；先清零目标再请求使能 |
| `drive` | `forward`, `turn`, `limit_rps` | 整车模式；forward/turn 在 −1 到 +1 之间，非负速度上限 |
| `speed` | `motor`, `rps` | 单机模式控制选中电机；整车模式仅用于刀片，刀片首版不允许负速度 |
| `stop_motor` | `motor` | 目标清零、请求失能；刀片还取消解锁 |
| `stop_all` | 无 | 全部目标清零、请求失能、停止锁定、取消刀片解锁，保留当前控制权 |
| `configure` | `motor`, `config` | 仅当前控制端、停止锁定且停用时可保存 |
| `inject` | `motor`, `kind` | 仅模拟模式与当前控制端；kind 为 offline / driver / can / clear |

动作不会续租，只有心跳续租。断开持有控制权的连接会立即触发停止；只读连接断开不影响当前控制端。
没有全局“全部使能”；刀片必须独立解锁。缺少 H3510 时，不将其离线当作行走电机运行的失败。

行走目标：`left=(forward-turn)/scale*limit`，`right=(forward+turn)/scale*limit`；`scale=max(1,abs(forward-turn),abs(forward+turn))`。在两轮逻辑正方向均校准为前进后，正 turn 表示左转，负 turn 表示右转。
`limit` 取请求上限与左右轮配置上限的较小值；每台电机按 `accel_rps2` 限制正常加减速。
`drive(0,0,0)` 或 `speed(...,0)` 是目标归零并减速；`stop_motor/stop_all` 跳过目标斜坡、请求零速和失能。机械惯性仍可能造成滑行，不能理解为瞬时制动。

## 配置结构

以下仅为**模拟示例**，不能直接用于实机：

```json
{
  "verified":true,"model":"H6215","mode":"velocity",
  "can_id":1,"feedback_id":17,"direction":1,
  "max_rps":2,"accel_rps2":2,"v_max_rad_s":50,
  "feedback_timeout_ms":300
}
```

`verified` 必须明确为布尔值。实机启动时所有电机默认 false、ID 与映射范围未确认。
第一版只实现 `velocity`；必须先在官方软件读回并确认电机本身工作在这个模式，网页不会帮你切模式。
CAN ID 限制 1–15，以避免反馈低四位设备 ID 歧义；反馈 ID 为 0–2046，必须唯一，且不能与任一已核对电机的控制 ID/特殊命令 ID 冲突。
方向只接受 ±1；最大速度、加速度、映射范围必须有限且正数，`max_rps × 2π ≤ v_max_rad_s`；反馈超时 100–500 ms。
这些是首版软件约束，不代表厂家允许的机构工作范围。修改 CAN ID、模式和映射值必须与电机已读回参数保持一致。
保存只持久化配置；使能、速度、所有权、锁定复位、刀片解锁不持久化。保存失败返回拒绝，恢复原内存配置。

## 状态字段

顶层状态：`now_ms/session/owner/simulation/board_ready/bus_healthy/locked/reason/mode/selected/cutter_unlocked`。
`motors[]` 每项包含 `name/config/online/requested/enabled/target_rps/output_rps/actual_rps/age_ms/fault/mos_c/rotor_c/current_a`。

- `requested` 为控制器使能意图，`enabled` 为电机反馈；不能混为一谈。
- `target_rps` 为限幅后的目标，`output_rps` 为斜坡后的发送值，`actual_rps` 为转换后的有效反馈。
- 离线时 `actual_rps/enabled/fault/temperature` 为 null，`age_ms` 保留最后反馈的年龄；从未收到反馈时年龄也是 null。
- 首版状态反馈没有独立电流值，`current_a` 始终 null；不把扭矩当作电流。
- `events` 为最多 128 条固件内存事件，带单调序号和控制器时间；重启清空。

网页在本地保留最近 30,000 条采样、应答和事件记录；CSV 带连接编号、模拟/真机标记及单位。关闭页面后记录不持久化，应在测试结束时导出。
反馈曲线显示最近 30 秒，离线区间不连线。网页超过 600 ms 没有收到状态会关闭连接并隐藏旧读数；固件心跳检查独立运行。

## 固件锁定与 CAN 失败

上电、取得新控制权、全部停止、控制端断开、心跳超时、运行电机反馈超时/故障、CAN 发送失败都会停止锁定。
停止命令无法送达时记录 `stop_delivery_failed`，不声称实体电机已经停止。真实 CAN 故障在传输层保持锁定，检查后重启，再手动复位。
网页复位不自动重试速度，也不解除电机内部错误；需要时使用官方工具排查。
实际电机通信超时停机和实体断动力必须按 [验收表](acceptance.md) 验证，软件不能替代这两项。
