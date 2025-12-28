#!/bin/bash

# --- 設定 ---
PROJECT_DIR=~/DockerChatBot
ENV_DIR=/etc/chatbot
ENV_FILE=$ENV_DIR/.env

echo "============================================"
echo "   ESP32 Chatbot 自動セットアップスクリプト"
echo "============================================"

# 1. Dockerのインストールチェック
if ! command -v docker &> /dev/null; then
    echo "⚠️ Dockerが見つかりません。インストールを開始します..."
    curl -fsSL https://get.docker.com -o get-docker.sh
    sudo sh get-docker.sh
    sudo usermod -aG docker $USER
    rm get-docker.sh
    echo "✅ Dockerをインストールしました。"
    echo "⚠️ 権限を反映させるため、一度ログアウトして再ログインしてから、もう一度このスクリプトを実行してください。"
    exit 1
else
    echo "✅ Dockerはインストール済みです。"
fi

# 2. プロジェクトディレクトリの作成
echo "📂 プロジェクトフォルダを作成中: $PROJECT_DIR"
mkdir -p $PROJECT_DIR
cd $PROJECT_DIR

# 3. ファイルの生成 (Heredocを使ってファイルを書き出す)

# --- requirements.txt ---
cat << 'EOF' > requirements.txt
flask
google-generativeai
SpeechRecognition
pydub
flet
requests
gunicorn
EOF

# --- Dockerfile ---
cat << 'EOF' > Dockerfile
FROM python:3.11-slim

RUN apt-get update && apt-get install -y \
    ffmpeg \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

ENV PYTHONUNBUFFERED=1
EXPOSE 5000 8550

CMD ["python", "gui_app.py"]
EOF

# --- docker-compose.yml (env_file対応版) ---
cat << 'EOF' > docker-compose.yml
services:
  chatbot-app:
    build: .
    container_name: chatbot-app
    ports:
      - "5000:5000"
      - "8550:8550"
    volumes:
      - .:/app
    # 外部の環境変数ファイルを読み込む安全な設定
    env_file:
      - /etc/chatbot/.env
    depends_on:
      - voicevox-engine
    restart: always

  voicevox-engine:
    image: voicevox/voicevox_engine:cpu-ubuntu20.04-latest
    container_name: voicevox-engine
    ports:
      - "50021:50021"
    command: ["--host", "0.0.0.0"]
    restart: always
EOF

# --- .gitignore ---
cat << 'EOF' > .gitignore
# Python cache
__pycache__/
*.pyc

# System files
.DS_Store
Thumbs.db

# Virtual environments
.venv/
venv/
env/

# Docker mount data
data/

# Audio output files
input.wav
output.wav
EOF

# --- gui_app.py (修正済みの完全版) ---
cat << 'EOF' > gui_app.py
import os
import requests
import json
import threading
from datetime import datetime
import warnings

# 警告メッセージを非表示にする設定
warnings.filterwarnings("ignore")

import flet as ft
from flask import Flask, request, send_file
import google.generativeai as genai
import speech_recognition as sr
from pydub import AudioSegment

# APIキー読み込み
GOOGLE_API_KEY = os.environ.get("GOOGLE_API_KEY")
if not GOOGLE_API_KEY:
    print("警告: GOOGLE_API_KEYが設定されていません")
else:
    genai.configure(api_key=GOOGLE_API_KEY)

model = genai.GenerativeModel('gemini-1.5-flash')
app = Flask(__name__)
page_reference = None

def update_status(text):
    if page_reference:
        try:
            page_reference.pubsub.send_all({"type": "status", "text": text})
        except: pass

def add_chat_message(user, text):
    if page_reference:
        try:
            page_reference.pubsub.send_all({"type": "chat", "user": user, "text": text})
        except: pass

def speech_to_text(audio_path):
    recognizer = sr.Recognizer()
    try:
        with sr.AudioFile(audio_path) as source:
            audio_data = recognizer.record(source)
            return recognizer.recognize_google(audio_data, language="ja-JP")
    except: return None

def generate_voicevox_audio(text, speaker_id=3):
    base_url = "http://voicevox-engine:50021"
    try:
        q_res = requests.post(f"{base_url}/audio_query", params={'text': text, 'speaker': speaker_id}, timeout=10)
        if q_res.status_code != 200: return None
        s_res = requests.post(f"{base_url}/synthesis", params={'speaker': speaker_id}, json=q_res.json(), timeout=30)
        return s_res.content if s_res.status_code == 200 else None
    except: return None

@app.route('/chat', methods=['POST'])
def chat():
    print("--- ESP32 Request ---")
    update_status("音声を受信中...")
    input_file = "input.wav"
    if 'audio' in request.files: request.files['audio'].save(input_file)
    else: 
        with open(input_file, 'wb') as f: f.write(request.data)

    user_text = speech_to_text(input_file)
    if not user_text:
        update_status("認識できませんでした")
        return "Failed", 400
    
    add_chat_message("User", user_text)
    update_status("AI思考中...")
    
    try:
        res = model.generate_content(f"あなたは親切なロボットです。短く答えて: {user_text}")
        bot_text = res.text.replace("*", "").replace("\n", " ")
    except: return "AI Error", 500

    add_chat_message("Bot", bot_text)
    update_status("音声合成中...")
    
    wav_data = generate_voicevox_audio(bot_text)
    if wav_data:
        with open("output.wav", "wb") as f: f.write(wav_data)
        update_status("送信完了")
        return send_file("output.wav", mimetype="audio/wav")
    return "TTS Failed", 500

def run_flask():
    app.run(host='0.0.0.0', port=5000, debug=False, use_reloader=False)

def main(page: ft.Page):
    global page_reference
    page_reference = page
    page.title = "ESP32 Chatbot"
    page.theme_mode = ft.ThemeMode.DARK
    chat_list = ft.ListView(expand=True, spacing=10, auto_scroll=True)
    status_text = ft.Text("待機中...", color=ft.colors.GREEN)

    def on_message(msg):
        if msg["type"] == "chat":
            is_u = msg["user"] == "User"
            chat_list.controls.append(ft.Row([ft.Container(content=ft.Text(msg["text"]), padding=10, border_radius=10, bgcolor=ft.colors.BLUE_900 if is_u else ft.colors.GREY_800)], alignment=ft.MainAxisAlignment.END if is_u else ft.MainAxisAlignment.START))
        elif msg["type"] == "status":
            status_text.value = msg["text"]
            status_text.update()
        page.update()

    page.pubsub.subscribe(on_message)
    page.add(ft.Container(content=chat_list, expand=True), ft.Container(content=status_text, padding=10))

if __name__ == "__main__":
    flask_thread = threading.Thread(target=run_flask, daemon=True)
    flask_thread.start()
    # 【修正】最新のFletに対応するため ft.AppView.WEB_BROWSER に変更
    ft.app(target=main, view=ft.AppView.WEB_BROWSER, port=8550, host="0.0.0.0")
EOF

echo "✅ ファイルの生成が完了しました。"

# 4. APIキーのセットアップ
if [ ! -f "$ENV_FILE" ]; then
    echo "🔑 APIキーの設定を行います。"
    echo "Google Gemini APIキーを入力してください (入力内容は表示されません):"
    read -s API_KEY
    
    if [ -z "$API_KEY" ]; then
        echo "❌ APIキーが入力されませんでした。処理を中断します。"
        exit 1
    fi

    echo "🔒 /etc/chatbot フォルダを作成し、キーを安全に保存します..."
    sudo mkdir -p $ENV_DIR
    sudo chown $USER:$USER $ENV_DIR
    echo "GOOGLE_API_KEY=$API_KEY" > $ENV_FILE
    sudo chmod 600 $ENV_FILE # 自分だけが読めるように権限設定
    echo "✅ APIキーを保存しました: $ENV_FILE"
else
    echo "✅ APIキーは既に設定済みです ($ENV_FILE)"
fi

# 5. アプリ起動
echo "🚀 アプリケーションをビルドして起動します..."
echo "起動後は http://localhost:8550 にアクセスしてください。"
echo "停止するには Ctrl+C を押してください。"
echo "--------------------------------------------"

docker compose up --build