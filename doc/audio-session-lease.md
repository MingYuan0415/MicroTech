# audio_service 独占音频契约

## 范围与所有权

`audio_service` 在 BSP 音频设备之上提供单例全双工 PCM、独占租约和系统级音量、静音、PA 请求策略。BSP 持有设备资源；服务没有 worker、解码器、混音或抢占机制。公共接口见 `layers/middleware/components/audio_service/include/audio_service.h`。

- `audio_service_acquire(kind, &lease)` 成功才建立 owner 并启动 DMA。句柄保持 `audio_service_lease_t { uint32_t token; uint32_t generation; }`；两字段都必须匹配。
- RECORD 与 MAINTENANCE 可读写，PLAYBACK 只写。busy 时返回 `ESP_ERR_INVALID_STATE`，不排队、不停止已有消费者。
- `write_leased/read_leased` 在同一互斥区检查初始化、双字段身份、RUNNING、准入和权限，并登记在途 I/O；锁外调用 BSP，完成后归还引用。因此旧句柄不能跨 release/reacquire 进入新流。
- 被服务拒绝的 I/O 将非空 transferred 输出置零。已准入的 BSP 调用可能在超时或其他错误时返回部分进度，消费者应从未传输尾部继续，不能重放已发送数据。
- 只有当前 owner 可 release。成功才清 owner；陈旧句柄不触碰 DMA。release 失败（包括 reservation 冲突）不表示已释放，消费者必须保存同代句柄重试，不得以新 acquire 覆盖清理义务。
- 音量、静音和 PA 是系统请求策略，不属于租约；release 不重置它们。PA 请求值不等于停止 DMA 后的物理电平。

## 初始化、状态与并发

启动 owner 必须串行完成首次 `audio_service_init()`，包括同步对象创建；板级音频须先初始化。初始化后，服务以内部互斥锁和 SUSPENDING reservation 隔离控制事务，不要求消费者另建生命周期锁。

| 状态 | 当前含义 |
| --- | --- |
| UNINITIALIZED | 不能 acquire 或 I/O；partial-init 清理义务仍可能需要 deinit |
| READY | DMA 已停；既可能 idle 无 owner，也可能是 suspend 后保留 owner |
| RUNNING | 有 owner 且允许该代次的 I/O |
| SUSPENDING | 排空/stop/commit 的独占 reservation；冲突控制返回 INVALID_STATE |
| ERROR | 准入关闭；保留 owner 或尚未完成的清理义务 |

`configure` 只接受 READY 且无 owner。`deinit` 拒绝活动租约，owner 应先 release。deinit 保留同步对象及跨 deinit/init 的 generation，旧句柄不会因重初始化复活。

release、suspend、deinit 在同一 entry 临界区关闭准入并预留 SUSPENDING。无在途 I/O 时持锁完成 stop 和状态提交；有在途 I/O 才解锁等待 drained，再持锁 stop/commit。冲突 acquire/configure/resume/release/suspend/deinit 不能插入该事务、清 owner 或停止新流。排空超时恢复进入前的状态和准入，保留句柄；stop 失败转 ERROR 并保留清理义务。READY 有 owner 的 release 直接清 owner，不再次 stop；READY suspend 是空操作，`resume_required=false`。

## ERROR 与重试

- initialized、无 owner 的 READY/ERROR 才能 acquire。ERROR 先在控制锁内 stop 清理；失败保持 ERROR 并返回该错误，不能启动新流。
- acquire 的 start 失败立即在同一临界区 stop 补偿。补偿成功回 READY，失败保留 ERROR；返回最初 start 错误，输出 lease 为零，不分配 generation。
- partial-init ERROR 不能 acquire，由 deinit/init 收尾恢复；deinit 失败保留重试所需信息。
- 有 owner 的 ERROR 可由同代 release 重试 stop，或 resume 重试 start。不能把任意 `INVALID_STATE` 归一化为 release 成功。
- resume 在 READY 无 owner 时返回 OK 且不启动 DMA；RUNNING 有 owner 时幂等成功；READY/ERROR 有 owner 时以原句柄重启。未初始化、SUSPENDING 和 ERROR 无 owner 返回 INVALID_STATE。

## 生产消费端与 PM

- `main/app_runtime.c` 提供启动 PCM/音量/静音/PA 策略并管理服务初始化，不持有播放租约。
- `recorder_service`：录音使用 RECORD、WAV 播放使用 PLAYBACK。页面离开不抢停后台录音/播放。清理失败保留旧句柄，后续 start/play/deinit 先完成旧义务；不得覆盖 owner。
- 飞鸟 `flybirds_audio_session.c` / `flybirds_audio.c`：使用 PLAYBACK；拿不到租约时游戏静默，按页面既有节奏重试，不打断后台消费者。writer 完成信号表示不再访问应用音频状态，reap 成功后才能复用 synth/PCM；release 失败保留 session 供 open/close 重试。双静音仍持有租约并供给 20 ms PCM。
- `main/display_benchmark.c`：音频维护负载使用 MAINTENANCE，不能复用其他 owner 的流。完整保存设置后才修改；恢复设置失败保留 snapshot 和 lease，release 失败保留 owner，由 stop 重试。清理未完成不允许新 benchmark 覆盖上下文。
- `main/app_runtime_pm.c` 是实际 suspend/resume 调用者。系统休眠前排空音频，租约跨 suspend 保留，唤醒或休眠回滚由 PM 按恢复标记调用 resume；不是只由消费者恢复。owner 若在 suspended READY 时 release，后续 PM resume 是 idle OK，不启动无主 DMA；若已有新 owner RUNNING，resume 不重启它。

## 验证边界

服务 host 测试覆盖租约身份、生命周期、I/O 准入、排空及错误恢复；各消费者 host、集成与 simulator fake 应遵守同代校验、独占和失败保留 owner 的契约。宿主和模拟器不证明真机 I2S/DMA 时序、音质、编解码/功放电气行为或功耗。
