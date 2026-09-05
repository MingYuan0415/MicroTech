# Emissive 色彩语言(AMOLED)

SH8601A 368×448 为自发光面板:`#000000` 像素完全熄灭。整套 UI 因此以
「光即层级」组织色彩——对比度、功耗与观感优势全部来自深色画布上的稀疏
发光,而不是灰色抬升块。

## 单一来源

原始色值只允许出现在 `layers/app_manager/app_theme/include/app_theme_colors.h`
(`APP_THEME_COLOR_*`)。层内映射:

- `app_core`(任务切换器、返回手势)直接 include 该头文件;`app_core`
  REQUIRES `app_theme`,无额外依赖。
- 应用层 `layers/apps/common/include/app_ui_theme.h` 是薄别名层,
  `APP_UI_COLOR_*` → `APP_THEME_COLOR_*`;各 App 的 `*_COLOR_*` 内部别名
  再指向它。页面代码不得出现色彩字面量。
- 宿主回归:`layers/apps/tests/host/theme_contrast_test.c` 按 WCAG 相对
  亮度断言每对 fg/bg 的对比度下限,并钉住画布真黑、明度阶梯单调等语言
  不变式。调色前跑该测试可预判失配。

## 调色板

| 角色 | Token(`APP_THEME_COLOR_*`) | 值 | 说明 |
| --- | --- | --- | --- |
| 画布 | `VOID` | `#000000` | 全屏背景;像素熄灭,均值亮度即功耗 |
| 卡片 | `PLUME` | `#14191E` | 仅在需要成组时使用的稀疏暗层 |
| 按下/选中底 | `PLUME_HI` | `#1E262D` | 按压**变亮**,不做压暗 |
| 图标凹槽 | `SUNKEN` | `#0A0E11` | 低于 PLUME 的 icon-chip 底 |
| 细线 | `HAIRLINE` | `#232C33` | 1px 分隔线、无意义图形、手势指示底 |
| 主文字 | `INK` | `#EAF0F2` | VOID 上 18.2:1;刻意低于纯白,控制光晕 |
| 次级文字 | `INK_SOFT` | `#A9B6BC` | VOID 上 10.1:1 |
| 强调上文字 | `ON_ACCENT` | `#04141A` | 任何高亮 accent 填充上的字/图标 |
| 主强调(青) | `AZURE` | `#43C6DB` | 动作、激活态、充电;10.3:1 |
| 时间/待命(琥珀) | `AMBER` | `#FFB454` | 太阳、等待、时间语义;11.9:1 |
| 成功 | `MINT` | `#4FDD8B` | 成功、达标;12.0:1 |
| 危险 | `CORAL` | `#FF6B6B` | 破坏性操作、故障;7.6:1 |

`app_ui_theme.h` 保留历史名(`RAIN`=AZURE、`SUN`=AMBER、`WARNING`=CORAL 等)。
旧 `WARNING_BG` 已删除:告警横幅改为 accent tint(见下)。

## 使用规则

1. **两阶深度**:VOID → PLUME → PLUME_HI,加 hairline 描边;不引入第三层
   灰阶背景。
2. **选中 = tint + 描边**:accent 作 `bg_color` 配低 `bg_opa`(≈20%),可加
   1px accent 边框(`LV_OPA_40`);不用实色灰块表示选中(weather 告警横幅
   即此形态)。
3. **按下变亮**:press 态取更亮档(`PLUME`→`PLUME_HI`),发光面版的反馈是
   「被点亮」。
4. **accent 上只用 `ON_ACCENT`**:开关 knob 勾选态、按钮反白文字等一律近黑,
   保证 ≥7:1。
5. **亮度花在信息上**:全视口不出现大面积高亮度常驻色块;文字用 INK 而非
   纯白;静态 chrome(表盘指针、切换器条)只让细笔画发光,降低差分老化
   (烧屏)风险。
6. **色相不单独承载语义**:accent 状态同时具备图标或文案;四色分工固定为
   动作/时间/成功/危险,不做第二含义。

## 验证

调色板级改动必须过:`theme_contrast_test`(宿主)+ `sim/ci/run_ci.sh` +
`sim/tools/review_pages.py`(基线需随调色再生成)。
