# Pinned build image for the DKB Linux release (TotK-style, self-contained).
# Ubuntu 24.04 keeps the glibc 2.34 floor that the release's LLVM binaries need.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget git make pkg-config \
        clang lld ninja-build \
        libvulkan1 libvulkan-dev mesa-vulkan-drivers \
        xvfb zsync \
        libfontconfig1 libxft2 libxrender1 libx11-6 libxext6 libxss1 libxt6 libsm6 libice6 \
        python3 python3-venv python3-pip \
    && rm -rf /var/lib/apt/lists/*

# .NET 10 SDK for the shader translator (NativeAOT).
RUN wget -q https://dot.net/v1/dotnet-install.sh -O /tmp/dotnet-install.sh \
    && chmod +x /tmp/dotnet-install.sh \
    && /tmp/dotnet-install.sh --channel 10.0 --install-dir /usr/share/dotnet \
    && ln -s /usr/share/dotnet/dotnet /usr/local/bin/dotnet \
    && rm /tmp/dotnet-install.sh
ENV DOTNET_ROOT=/usr/share/dotnet
# dotnet's first-time use configurer writes to ~/.dotnet; the container runs as the invoking (non-root) user whose
# home would otherwise be /, so point HOME at a writable directory.
ENV HOME=/tmp

# Pillow for the window icon. Ubuntu 24.04 marks its system Python as externally managed (PEP 668); this container is
# throwaway, so overriding is safe.
RUN python3 -m pip install --no-cache-dir --break-system-packages Pillow

WORKDIR /work
CMD ["bash"]