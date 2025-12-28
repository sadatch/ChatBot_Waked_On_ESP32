#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>

// ==========================================
// ★ここを自分の環境に合わせて書き換えてください
// ==========================================
const char* ssid = "NSD1K-B010-a";         // Wi-FiのSSID
const char* password = "Rakn926ag7nC6"; // Wi-Fiのパスワード

// ThinkPad(PC)のIPアドレスを指定します (ポートは5000)
// 例: "http://192.168.1.15:5000/chat"
const char* serverUrl = "http://192.168.1.2:5000/chat";

// ==========================================
// ピン設定 (配線ガイドと同じ)
// ==========================================
// マイク (INMP441)
#define I2S_MIC_SCK 32
#define I2S_MIC_WS  33
#define I2S_MIC_SD  35
// スピーカー (MAX98357A)
#define I2S_SPK_BCLK 26
#define I2S_SPK_LRC  25
#define I2S_SPK_DIN  22
// ボタン
#define BUTTON_PIN 4

// ==========================================
// 音声設定
// ==========================================
#define SAMPLE_RATE 16000
#define RECORD_TIME_SECONDS 4  // 録音時間(秒)。メモリ限界があるため長くしすぎないこと
// WAVヘッダ(44byte) + データサイズ(16bitモノラル)
const int waveDataSize = RECORD_TIME_SECONDS * SAMPLE_RATE * 2;
const int headerSize = 44;
const int totalFileSize = headerSize + waveDataSize;

// 録音データを入れるバッファ
uint8_t* audioBuffer;

// I2Sポート番号
#define I2S_MIC_PORT I2S_NUM_0
#define I2S_SPK_PORT I2S_NUM_1

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // 1. メモリ確保
  audioBuffer = (uint8_t*)malloc(totalFileSize);
  if (audioBuffer == NULL) {
    Serial.println("メモリ確保失敗！録音時間を短くしてください");
    while (1);
  }

  // 2. Wi-Fi接続
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected!");

  // 3. I2S初期化 (マイク & スピーカー)
  setupI2SMic();
  setupI2SSpeaker();
  
  Serial.println("Setup done. Press button to record.");
}

void loop() {
  // ボタンが押されたら(LOWになったら)処理開始
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("Button Pressed!");
    
    // 録音 -> 送信 -> 受信 -> 再生
    recordAudio();
    sendAudioAndPlayResponse();
    
    Serial.println("Finished. Waiting for next press...");
    // 連続動作防止のウェイト
    delay(1000); 
  }
}

// --- I2S設定 (マイク用) ---
void setupI2SMic() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // L/RピンをGNDにした場合
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  i2s_driver_install(I2S_MIC_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pin_config);
}

// --- I2S設定 (スピーカー用) ---
void setupI2SSpeaker() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE, // ※VOICEVOXの出力に合わせて24000など変更が必要な場合あり
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = true, // ノイズ低減
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_SPK_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &pin_config);
}

// --- WAVヘッダ作成 ---
void createWavHeader(uint8_t* header, int waveDataSize) {
  int fileSize = waveDataSize + 44 - 8;
  int byteRate = SAMPLE_RATE * 16 * 1 / 8; 
  uint8_t h[44] = {
    'R', 'I', 'F', 'F',
    (uint8_t)(fileSize & 0xFF), (uint8_t)((fileSize >> 8) & 0xFF), (uint8_t)((fileSize >> 16) & 0xFF), (uint8_t)((fileSize >> 24) & 0xFF),
    'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
    16, 0, 0, 0,
    1, 0, 1, 0, 
    (uint8_t)(SAMPLE_RATE & 0xFF), (uint8_t)((SAMPLE_RATE >> 8) & 0xFF), (uint8_t)((SAMPLE_RATE >> 16) & 0xFF), (uint8_t)((SAMPLE_RATE >> 24) & 0xFF),
    (uint8_t)(byteRate & 0xFF), (uint8_t)((byteRate >> 8) & 0xFF), (uint8_t)((byteRate >> 16) & 0xFF), (uint8_t)((byteRate >> 24) & 0xFF),
    2, 0, 16, 0,
    'd', 'a', 't', 'a',
    (uint8_t)(waveDataSize & 0xFF), (uint8_t)((waveDataSize >> 8) & 0xFF), (uint8_t)((waveDataSize >> 16) & 0xFF), (uint8_t)((waveDataSize >> 24) & 0xFF)
  };
  memcpy(header, h, 44);
}

// --- 録音処理 ---
void recordAudio() {
  Serial.println("Recording...");
  
  // バッファに残ったゴミデータを読み捨てる
  size_t bytesRead;
  i2s_read(I2S_MIC_PORT, (void*)audioBuffer, 1024, &bytesRead, portMAX_DELAY); // ダミー読み込み

  // WAVヘッダを先頭に書き込む
  createWavHeader(audioBuffer, waveDataSize);

  // マイクからデータを読み込んでバッファを埋める
  size_t bytes_read_total = 0;
  uint8_t* record_ptr = audioBuffer + headerSize; // ヘッダの次から書き込み開始
  
  while (bytes_read_total < waveDataSize) {
    // もしボタンを途中で離したら録音終了したい場合はここに判定を入れる
    
    size_t bytes_to_read = 1024;
    if (waveDataSize - bytes_read_total < 1024) bytes_to_read = waveDataSize - bytes_read_total;
    
    i2s_read(I2S_MIC_PORT, (void*)(record_ptr + bytes_read_total), bytes_to_read, &bytesRead, portMAX_DELAY);
    bytes_read_total += bytesRead;
  }
  Serial.println("Recording finished.");
}

// --- 送信と再生処理 ---
void sendAudioAndPlayResponse() {
  if(WiFi.status() != WL_CONNECTED){
    Serial.println("WiFi Disconnected");
    return;
  }

  HTTPClient http;
  Serial.print("Sending to: ");
  Serial.println(serverUrl);

  // タイムアウトを長めに設定(生成待ち用)
  http.setTimeout(20000); 

  http.begin(serverUrl);
  http.addHeader("Content-Type", "audio/wav");

  // POST送信 (録音データを丸ごと送る)
  int httpResponseCode = http.POST(audioBuffer, totalFileSize);

  if (httpResponseCode == 200) {
    Serial.println("Response Received! Playing audio...");
    
    // 受信データを取得して再生
    int len = http.getSize();
    WiFiClient *stream = http.getStreamPtr();
    
    uint8_t buff[1024];
    size_t bytes_written;
    
    // WAVヘッダ(44byte)はノイズになるので読み飛ばす
    if(stream->available()) {
      stream->readBytes(buff, 44);
      len -= 44;
    }

    // データを読みながらスピーカーに流す
    while(http.connected() && (len > 0 || len == -1)) {
      size_t size = stream->available();
      if(size) {
        // バッファサイズ分だけ読み込む
        int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
        
        // I2Sアンプに書き込む
        i2s_write(I2S_SPK_PORT, buff, c, &bytes_written, portMAX_DELAY);
        
        if(len > 0) len -= c;
      }
      delay(1);
    }
    Serial.println("Playback finished.");
  } else {
    Serial.print("Error code: ");
    Serial.println(httpResponseCode);
    String payload = http.getString();
    Serial.println(payload);
  }
  http.end();
}