FROM mcr.microsoft.com/devcontainers/python:3.11-bookworm

# ESPHome pulls the ESP-IDF toolchain on first compile; git and the serial
# libs are what it needs from the image itself.
RUN apt-get update && apt-get install -y --no-install-recommends \
        git \
        python3-venv \
        libffi-dev \
        libssl-dev \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir esphome
