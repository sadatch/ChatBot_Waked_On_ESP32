FROM python:3.11-slim

# 音声処理(pydub)に必要なffmpeg等をインストール
RUN apt-get update && apt-get install -y \
    ffmpeg \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# ライブラリのインストール
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# コードをコピー
COPY . .

# ポート公開 (Flask:5000, Flet:8550)
EXPOSE 5000 8550

CMD ["python", "gui_app.py"]