# CharacterAssetStudio

一个面向 AI 角色创作流程的 Windows 桌面资源容器工具。

CharacterAssetStudio 将角色元数据、多个身份设定、标准机位参考图、通用图片、细节图片、音频、视频和 LoRA 统一保存到 `.casc` 文件中。资源写入时计算 SHA-256，读取时自动校验完整性，适合角色资产归档、跨机器迁移和 ComfyUI 工作流使用。

## 功能特色

- 直观的 Qt Widgets 图形界面，面向 Windows 和 MinGW-w64 构建。
- 管理角色名称、作者、版本、许可证、描述和多个身份。
- 支持 11 个固定 `settingImage` 机位：Portrait、Front、Left、Right、Rear、Top、Bottom、LeftFronthalf、RightFronthalf、LeftRearHalf、RightRearHalf。
- 分离保存 `generalImages`、`detailImages`、`refAudio`、`refVideo` 和 `privateLora` 资源集合。
- 每个输入资源保存 SHA-256，打开 `.casc` 时重新计算并校验所有资源。
- 支持无压缩、Qt Deflate、LZ4 和 Zstandard。
- 支持 AES-256-CBC 密码保护。
- 图片资源鼠标悬停超过 0.5 秒后显示预览，移开后自动隐藏。
- CASC v3 写入，并兼容读取旧版 v1/v2 容器。

## 依赖项目

- AES：[SergeyBel/AES](https://github.com/SergeyBel/AES/)
- LZ4：[lz4/lz4](https://github.com/lz4/lz4)
- Zstandard：[facebook/zstd](https://github.com/facebook/zstd)

AES、LZ4、Zstandard 和 Qt SDK 均作为本地构建依赖使用，不会随主仓库提交。默认 qmake 配置要求 AES 源码位于项目目录中的 `AES/`；也可以通过 `AES_ROOT` 指定其他位置。

## 在 ComfyUI 中使用

ComfyUI 节点插件位于独立仓库：

- [murasakii0118/comfyUI_casc](https://github.com/murasakii0118/comfyUI_casc)

安装插件后，将 `.casc` 文件加载到 ComfyUI，即可按身份和资源类型输出图片、音频、视频与 LoRA。插件不属于本仓库，不会随 CharacterAssetStudio 一起提交。

## 项目原理

应用先将角色信息和资源集合序列化为 CASC Payload。每个资源记录类型、槽位、原文件名、SHA-256 和实际二进制数据。Payload 可先经过 None、Deflate、LZ4 或 Zstandard 压缩，再按需使用 AES-256-CBC 加密，最后写入 CASC Header。

```text
输入文件
   │
   ├─ 读取二进制内容并计算 SHA-256
   │
   ├─ 组织 Character / Identity / Asset 数据
   │
   ├─ 序列化为 CASC Payload
   │
   ├─ 压缩：None / Deflate / LZ4 / Zstandard
   │
   ├─ 可选加密：AES-256-CBC
   │
   └─ 写入 .casc
```

容器的核心结构如下：

```text
CASC Header
├── Magic: CASC
├── Format version
├── Compression mode
├── Encryption flag
├── Original payload size
└── Packed payload
    └── Character
        ├── Character metadata
        └── Identity[]
            ├── Identity metadata
            ├── settingImages[]
            ├── generalImages[]
            ├── detailImages[]
            ├── refAudio[]
            ├── refVideo[]
            └── privateLora[]
```

## 环境依赖

- Windows 10/11 x86-64
- Qt 5.15.x，包含 Qt Widgets
- MinGW-w64，支持 C++17
- `qmake` 和 `mingw32-make`
- AES 源码依赖：SergeyBel/AES
- LZ4 1.10.x
- Zstandard 1.5.x

## 构建

将 Qt、LZ4、Zstandard 和 AES 依赖准备到本地后，在 PowerShell 中执行：

```powershell
cd D:\murasakii\Desktop\project1

& .\QT5.15.18-static\mingw810_64\bin\qmake.exe .\CharacterAssetStudio.pro AES_ROOT=D:\path\to\AES
& D:\mingw64\bin\mingw32-make.exe -j2
```

如果 AES 位于项目目录中的 `AES/`，可以省略 `AES_ROOT` 参数：

```powershell
& .\QT5.15.18-static\mingw810_64\bin\qmake.exe .\CharacterAssetStudio.pro
& D:\mingw64\bin\mingw32-make.exe -j2
```

生成的程序位于 `release/CharacterAssetStudio.exe`。运行时需要让 `libzstd.dll` 位于可执行文件旁边或系统 `PATH` 中；LZ4 使用静态链接。

## 项目结构

```text
CharacterAssetStudio/
├── CharacterAssetStudio.pro   # Qt / qmake 工程文件
├── main.cpp                   # GUI、数据模型、CASC 读写与校验
├── LICENSE                   # Apache-2.0
├── README.md
└── .gitignore
```

第三方依赖、构建目录、ComfyUI 插件目录和本地开发文档均不纳入主仓库。

## 许可证

本项目使用 [Apache License 2.0](LICENSE)。第三方依赖遵循各自仓库中的许可证。
