# Third-party code and acknowledgements | 第三方依赖声明

本项目将下列第三方源码直接包含在仓库内，**其版权及许可证不因本项目 LICENSE 而改变**。

| 组件 | 工程目录 | 版本或来源 | 许可核对 |
|---|---|---|---|
| LVGL | `third_lib/lvgl/` | v8.3.11（由本项目应用注释与源码确定） | MIT，原始声明见 `third_lib/lvgl/LICENCE.txt` |
| FreeRTOS Kernel | `third_lib/freertos/` | V10.4.3 LTS Patch 3 | 文件头 MIT 授权文本及版权声明，需继续保留 |
| ST CMSIS / Standard Peripheral Library | `firmware/` | STM32F4 SPL V1.8.1（部分源文件头） | STMicroelectronics 版权所有。发布前请核验对应 STM32 软件授权及再分发条款 |
| ESP-AT | 外部 ESP32-C3 固件 | 使用 AT 命令，不随本仓库提供固件镜像 | 实际烧写固件的许可由其发布方决定 |
| Open-Meteo | 在线天气 API | 使用公开天气服务，不在仓库中转载天气数据集 | 使用时核对 API 服务条款 |

已从本仓库发布包中删除 LVGL 上游 demo、test、docs、scripts、env_support 等不参与本工程编译的内容，避免把无关大型素材作为本项目原创资源发布。

上传到 GitHub 前请确认你对自行添加的字体、图片、图标、文档、参考资料有发布和再授权权利。如果原项目包含第三方素材，先取得许可或移除，再正式设为 public。
