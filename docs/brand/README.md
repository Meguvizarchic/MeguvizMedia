# MeguvizMedia 图标

来自 Meguviz 图标包（DETAIL REFINEMENT 07，2026-11-06），由同一套 64×64 母版导出，不要单独手改这里的文件；
需要调整时改图标包的母版后重新导出。

| 文件 | 用途 |
|---|---|
| `MeguvizMedia.svg` | 透明背景双色（深紫 `#4B3F63`、珊瑚橙 `#EF927A`） |
| `MeguvizMedia-mono.svg` | 单色 |
| `MeguvizMedia-inverse.svg` | 深色背景用的反白与珊瑚 |
| `MeguvizMedia-512.png`、`MeguvizMedia-128.png` | 透明 PNG |

Android 启动图标资源名为 `@mipmap/ic_meguviz_media`（自适应图标，API 33 起带主题单色层），只放在示例 app 里；
发布的库模块不带图标资源，避免资源随 AAR 进入消费者应用。
