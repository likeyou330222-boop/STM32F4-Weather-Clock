# 发布到 GitHub：一步步操作（Windows）

## 上传前检查

1. **不要直接上传最初的 59 MB 原始压缩包**：其中包含构建缓存和硬编码 Wi-Fi 凭据。
2. 本公开副本的 `app/network_task.c` 已改为引入 `app/wifi_credentials.h`，而该头文件已在 `.gitignore` 中排除。
3. 启动 Keil 前先执行 `copy app\wifi_credentials.example.h app\wifi_credentials.h` 并填写 Wi-Fi 信息。
4. 仓库没有自动授予用户对你原创代码的再分发权。选 MIT 或 GPL 等后，在根目录添加正式 `LICENSE`；第三方代码遵守各自许可。
5. 发布前再检查图片资源、DOCX 元数据、Git 提交内容中有无不适合公开的个人信息。

## Windows 图形界面（推荐）

- 安装并登录 https://desktop.github.com/ 。
- 解压本压缩包，确保 README.md 与 mdk、app 在同一级根目录。
- GitHub Desktop → File → Add Local Repository → Choose 选择解压出的 `STM32F4-Weather-Clock_OpenSource` 文件夹。
- 如果提示这不是 Git 仓库，选择 Create a Repository Here 并初始化 Git。
- 检查 Changes 列表中看不到 `app/wifi_credentials.h`、`mdk/Objects/`、`mdk/Listings/`。
- 填写提交摘要 `Initial open-source release`，点 Commit to main。
- 点击 Publish repository，仓库名填 `STM32F4-Weather-Clock`。
- 如果希望公开，取消勾选 `Keep this code private`；正式提交前应完成许可证和敏感信息核对。

## Git Bash / PowerShell 命令方式

```bash
git init
git branch -M main
git add .
git status
git commit -m "Initial open-source release"
# 先在 GitHub 网页创建同名空仓库，不要预置 README / .gitignore / LICENSE
git remote add origin https://github.com/YOUR_USERNAME/STM32F4-Weather-Clock.git
git push -u origin main
```

首次使用 Git：请先设置 `git config --global user.name "Your Name"` 和 `git config --global user.email "your-public-or-private-github-noreply-email"`。连接 GitHub 需要完成网页登录、Git Credential Manager 或其他受支持的认证方式，不要把 GitHub Token 写进源码或提交历史。

注意 `.gitignore` 只阻止以后跟踪匹配文件，不能抹去已经提交过的敏感凭据；如果原始密码曾公开提交过，请立即更换 Wi-Fi 密码，并清理 Git 历史。
