import os
import requests
import json
import threading
from datetime import datetime

# GUI ライブラリ
import flet as ft

# Webサーバー & 音声処理ライブラリ
from flask import Flask, request, send_file
import google.generativeai as genai
import speech_recognition as sr
from pydub import AudioSegment

# --- 設定部分 ---
# 【修正1】APIキーはここには書かず、docker-compose.yml から読み込みます
GOOGLE_API_KEY = os.environ.get("GOOGLE_API_KEY")

if not GOOGLE_API_KEY:
    print("警告: GOOGLE_API_KEYが設定されていません")
else:
    genai.configure(api_key=GOOGLE_API_KEY)

model = genai.GenerativeModel('gemini-1.5-flash')

app = Flask(__name__)
page_reference = None

# --- ヘルパー関数 ---
def update_status(text):
    if page_reference:
        try:
            # 【修正2】sent_all ではなく send_all です
            page_reference.pubsub.send_all({"type": "status", "text": text})
        except Exception as e:
            print(f"GUI update error: {e}")

def add_chat_message(user, text):
    if page_reference:
        try:
            # 【修正3】page.reference ではなく page_reference です
            page_reference.pubsub.send_all({"type": "chat", "user": user, "text": text})
        except Exception as e:
            print(f"GUI update error: {e}")

# --- 音声認識 (STT) ---
def speech_to_text(audio_path):
    recognizer = sr.Recognizer()
    try:
        with sr.AudioFile(audio_path) as source:
            audio_data = recognizer.record(source)
            text = recognizer.recognize_google(audio_data, language="ja-JP")
            return text
    except sr.UnknownValueError:
        return None
    except sr.RequestError:
        return None

# --- VOICEVOX連携 (TTS) ---
def generate_voicevox_audio(text, speaker_id=3):
    # 【修正4】Docker内では localhost ではなく、サービス名(voicevox-engine)を使います
    base_url = "http://voicevox-engine:50021"
    
    try:
        # クエリ生成
        query_payload = {'text': text, 'speaker': speaker_id}
        response = requests.post(f"{base_url}/audio_query", params=query_payload, timeout=10)
        
        if response.status_code != 200:
            print(f"VOICEVOX query failed: {response.text}")
            return None
        
        query_data = response.json()
        
        # 音声合成
        # 【修正5】変数のスペルミス修正 (synth_paload -> synth_payload)
        synth_payload = {'speaker': speaker_id}
        response = requests.post(
            f"{base_url}/synthesis",
            params=synth_payload,
            json=query_data,
            timeout=30
        )
        
        if response.status_code != 200:
            # 【修正6】Non -> None
            return None
        
        return response.content
        
    except requests.exceptions.ConnectionError:
        print("エラー: VOICEVOXエンジンが見つかりません。")
        return None
    except Exception as e:
        print(f"VOICEVOX Connection Error: {e}")
        return None

# --- Flaskルート ---
@app.route('/chat', methods=['POST'])
def chat():
    print("--- ESP32 Request Received ---")
    update_status("音声を受信しました...認識中")

    # 【修正7】ファイル名のスペルミス (inpu -> input)
    input_filename = "input.wav"

    # ファイル受信処理
    if 'audio' in request.files:
        file = request.files['audio']
        file.save(input_filename)
    else:
        with open(input_filename, 'wb') as f:
            f.write(request.data)

    # 音声認識実行
    user_text = speech_to_text(input_filename)

    if not user_text:
        print("音声認識失敗")
        update_status("聞き取れませんでした。")
        return "音声認識失敗", 400

    # GUI表示
    add_chat_message("User", user_text)
    update_status("AIが考え中...")

    # Gemini呼び出し
    bot_text = ""
    try:
        prompt = f"あなたは親切で丁寧なおしゃべりアシスタントです。次のユーザーの発言に、語尾に「〜なのだ」をつけて短く答えてください: {user_text}"
        response = model.generate_content(prompt)
        bot_text = response.text.replace("*", "").replace("_", "").replace("\n", " ")
        print(f"Gemini: {bot_text}")
        
    except Exception as e:
        update_status(f"AIエラー: {e}")
        return "AI Error", 500

    # GUI表示
    add_chat_message("Bot", bot_text)
    update_status("音声合成中(VOICEVOX)...")

    # 音声合成
    wav_audio_data = generate_voicevox_audio(bot_text, speaker_id=3)

    output_filename = "output.wav"

    if wav_audio_data:
        with open(output_filename, 'wb') as f:
            f.write(wav_audio_data)

        update_status("応答を送信中...")
        return send_file(output_filename, mimetype="audio/wav")
    else:
        update_status("音声合成に失敗しました。")
        return "Voice generation failed", 500
    
def run_flask():
    app.run(host='0.0.0.0', port=5000, debug=False, use_reloader=False)

# --- Flet (GUI) ---
def main(page: ft.Page):
    global page_reference
    page_reference = page

    page.title = "ESP32 AI Chatbot (Docker)"
    # 【修正8】theme_made -> theme_mode
    page.theme_mode = ft.ThemeMode.DARK

    # 【修正9】audo_scroll -> auto_scroll
    chat_list = ft.ListView(expand=True, spacing=10, auto_scroll=True)

    status_text = ft.Text("サーバー準備完了: ESP32接続待機中...", color=ft.colors.GREEN)

    def on_message(message):
        if message["type"] == "chat":
            is_user = message["user"] == "User"
            chat_list.controls.append(
                ft.Row(
                    [
                        ft.Container(
                            # 【修正10】括弧の位置修正 message["text", size=16] -> message["text"], size=16
                            content=ft.Text(message["text"], size=16),
                            # 【修正11】カンマ抜け修正
                            padding=10,
                            border_radius=10,
                            bgcolor=ft.colors.BLUE_900 if is_user else ft.colors.GREY_800,
                            width=280
                        )
                    ],
                    alignment=ft.MainAxisAlignment.END if is_user else ft.MainAxisAlignment.START
                )
            )
        elif message["type"] == "status":
            status_text.value = message["text"]
            status_text.update()
        page.update()
    
    page.pubsub.subscribe(on_message)

    page.add(
        ft.Container(
            content=ft.Column([
                ft.Text("Conversation History", size=20, weight="bold"),
                ft.Divider(),
                chat_list
            ]),
            expand=True,
            padding=10
        ),
        ft.Container(content=status_text, padding=10, bgcolor=ft.colors.BLACK26)
    )

# --- プログラム実行 ---
if __name__ == "__main__":
    flask_thread = threading.Thread(target=run_flask, daemon=True)
    flask_thread.start()
    print("Flask Server started on port 5000...")

    print("GUI Server running on http://0.0.0.0:8550")
    ft.app(target=main, view=ft.WEB_BROWSER, port=8550, host="0.0.0.0")