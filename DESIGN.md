# PokeCapsule Design

## Direction

PokeCapsule 像一个安静的语音收件台：新想法进入 Inbox，处理状态沿时间顺序推进，最终文字成为胶囊的正面内容。界面使用清晰的文本层级、连续的行和少量有意义的容器，避免把每项能力做成同等醒目的按钮。

## Shared Information Architecture

### Capsule list

每条胶囊只展示两层：

1. 最佳可用文字，单行或两行截断；
2. 时间、时长、目录、标签和未完成状态。

标题仅在用户主动命名后承担标题角色。自动生成的“语音 + 日期”不与正文争夺第一层。

### Capsule detail

顺序固定为：

1. 标题、时间、位置、状态；
2. 播放与当前最重要的恢复动作；
3. 最终文字或最佳可用文字；
4. 编辑、复制；
5. 标签、收藏、标题等整理动作；
6. 原始转写与校对版本，按需展开。

空内容不占用大块版面。云端错误转成“发生了什么 + 接下来怎么处理”，原始错误码只进入诊断日志。

## Visual System

### Color screens

- Background: `#F5F6F4`
- Surface: `#FFFFFF`
- Primary ink: `#171A18`
- Secondary ink: `#626762`
- Accent: `#315F52`
- Accent soft: `#E2ECE7`
- Error: `#A23B32`
- Error soft: `#F7E8E5`
- Outline: `#D9DDD9`

页面使用中性色和一个低饱和绿色强调录音、播放和确认动作。红色只表示需要处理的问题或永久删除。

### Poke3

背景、表面和文字收敛为白、浅灰、黑三档。边框承担分组，阴影与透明叠层停用。刷新中不使用连续动画；录音状态以离散音量格、倒计时和文字共同表达。

### Typography

使用系统字体。角色限定为：

- Screen title: 24–28sp / bold
- Section title: 17–20sp / semibold
- Body: 16–17sp / regular
- Metadata: 13–14sp / regular
- Control: 15–16sp / medium

### Shape and spacing

- 彩色屏表面圆角 14dp，主要按钮圆角 12dp，小状态标签可用胶囊形。
- Poke3 表面圆角 6dp，按钮圆角 8dp。
- 页面左右边距手机 18–20dp，Poke3 14–16dp。
- 相关项间距 6–10dp，区块间距 20–28dp。

## Controls and State

- 每屏最多一个高强调主操作。
- 常用次级操作使用轻色按钮；低频整理操作进入“更多”或紧凑操作区。
- 短暂反馈使用 Snackbar 或页内状态，避免连续 Toast。
- 错误卡必须包含恢复动作；没有恢复动作的技术错误不进入主界面。
- 处理中显示静态状态标签；手机和 Mac 可以使用轻量进度动画，Poke3 使用文字刷新。

## Platform Adaptation

### Android phone

遵循顶部应用栏、48dp 触控目标和系统返回。录音胶囊位于主界面右下角。详情页把播放与文字放在首屏，整理动作降低权重。

### Poke3

沿用同一信息顺序，减少装饰、动画和层层弹窗。保留悬浮录音胶囊。列表单条高度稳定，正文最多两行。

### Mac

保留设备 / 筛选、胶囊列表、详情三栏。详情首屏展示最佳文字和播放；原始转写、校对版本与诊断信息放入可展开区。批量操作只在选择存在时启用。
