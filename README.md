# yoolang

## 首次拉取（Git LFS）

本仓库的部分测试数据由 Git LFS 管理。首次拉取请执行：

```bash
# 1) 安装并初始化 Git LFS（Ubuntu/Debian）
sudo apt update && sudo apt install -y git-lfs
git lfs install

# 2) 克隆仓库
git clone <你的仓库地址>
cd yoolang

# 3) 确保拉取到 LFS 大文件
git lfs pull
```

如果看到的是指针文本而不是真实文件，通常是本机未安装或未初始化 Git LFS。

## 使用方法

使用 xmake 作为构建工具.

输入

```bash
xmake
```

即可构建
