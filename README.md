# sus-key-audio-cli

`sus-key-audio-cli` 是一个独立的 C++ 命令行工具，用于把本地 SUS 谱面渲染成“只有按键/判定音”的音频文件。

## 功能概览

- 读取本地 `.sus` 谱面并生成 key 音轨
- 支持 `tap / flick / trace / hold tick / hold loop`
- 支持 `offset`（与网页预览一致：外部正数，内部取负）
- 输出 `mp3` 或 `wav`
- 默认 `mp3` 码率为 `128 kbps`
- 仓库内置音效文件，默认不依赖外部项目路径

## 来源与许可

本项目部分逻辑修改自 [crash5band/MikuMikuWorld](https://github.com/crash5band/MikuMikuWorld)，遵循 MIT 协议。  
完整许可证见 [LICENSE](./LICENSE)。

## 环境依赖

- `ffmpeg`
- C++20 编译器（`clang++` 或 `g++`）
- 批量脚本依赖 `jq`（用于解析 `musics.json`）

## 目录结构

```text
sus-key-audio-cli/
  assets/sound/
  src/main.cpp
  scripts/build.sh
  scripts/render_all_musics.sh
  scripts/reencode_mp3_128k.sh
  scripts/gh_trigger_build.sh
  scripts/gh_release_tag.sh
  .github/workflows/build-and-release.yml
```

## 本地构建

```bash
cd /Users/watagashi/Documents/Code/sus-key-audio-cli
./scripts/build.sh
```

构建产物：

```text
./bin/render-key-audio
```

构建脚本说明：

- 优先使用环境变量 `CXX`
- 未指定 `CXX` 时，自动尝试 `clang++`，再尝试 `g++`
- 可通过 `EXTRA_CXXFLAGS` 追加编译参数

## 单文件使用

最小示例：

```bash
./bin/render-key-audio \
  --sus /path/to/chart.sus \
  --out /path/to/output.mp3
```

带 offset 示例：

```bash
./bin/render-key-audio \
  --sus /Users/watagashi/Downloads/0703_master.sus \
  --out /Users/watagashi/Documents/Code/sus-key-audio-cli/0703-key.mp3 \
  --offset 9000
```

参数说明：

- `--sus <path>`：必填，SUS 文件路径
- `--out <path>`：必填，输出路径
- `--offset <ms>`：可选，外部传入偏移（内部会自动取负）
- `--format mp3|wav`：可选，不传则按输出扩展名推断
- `--sound-root <dir>`：可选，SE 资源目录

默认 `sound-root`：

```text
./assets/sound
```

内置音效文件位于：

```text
assets/sound/se_live_*.mp3
```

offset 规则：

- 传 `--offset 9000` 时，内部使用 `-9000ms`
- 不传时，回退读取 SUS 里的 `#WAVEOFFSET`

## 批量生成（全部歌曲）

`render_all_musics.sh` 会读取 `musics.json` 中每首歌的 `fillerSec`，换算成毫秒 offset，然后按路径：

```text
{score-root}/{musicId:04d}_01/master
```

例如 `musicId=719` 对应：

```text
/Users/watagashi/Documents/Code/Sekai/data/assets/sekai/assetbundle/resources/startapp/music/music_score/0719_01/master
```

执行：

```bash
./scripts/render_all_musics.sh \
  --out-dir /Users/watagashi/Documents/Code/sus-key-audio-cli/music-key-audio-master-mp3 \
  --workers 12
```

默认行为：

- 多进程并发（`zsh + jq + xargs -P`）
- 已有同名 `musicId.mp3` 则跳过
- 加 `--force` 才覆盖重跑
- 默认使用仓库内置 `assets/sound`

常用参数：

- `--difficulty master`
- `--masterdata <path>`
- `--score-root <path>`
- `--binary <path>`
- `--sound-root <path>`
- `--workers <n>`
- `--force`
