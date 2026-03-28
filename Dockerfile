FROM pkuflyingpig/pintos

# 安装 ripgrep (Ubuntu 18.04 需要从 GitHub 下载)
RUN apt-get update && \
    apt-get install -y wget && \
    wget -q https://github.com/BurntSushi/ripgrep/releases/download/14.1.0/ripgrep_14.1.0-1_amd64.deb && \
    dpkg -i ripgrep_14.1.0-1_amd64.deb && \
    rm ripgrep_14.1.0-1_amd64.deb && \
    apt-get clean && rm -rf /var/lib/apt/lists/*
