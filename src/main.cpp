#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>


#define LED_PIN    8  // 数据引脚
#define LED_COUNT  1   // LED数量

// 配置网络和巴法云参数
const char* ssid = "Xiaomi";
const char* password = "18166334677";
const char* mqtt_server = "bemfa.com"; // 巴法云MQTT地址
const int mqtt_port = 9501;            // 端口号
const char* uid = "79fa744a49ad461cb25cc83a2ee6c01a";            // 用户密钥
const char* topic = "led002";     // 主题名

WiFiClient espClient;
PubSubClient client(espClient);
const int ledPin = 8;  // LED连接的GPIO引脚

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
// 连接WiFi
void setup_wifi() {
  strip.begin();          // 初始化NeoPixel
  strip.show();           // 关闭所有LED
  strip.setBrightness(64); // 设置亮度（0-255）

  delay(10);
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi Connected!");
}

// MQTT消息回调函数
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  if (msg == "on") {         // APP发送开灯指令
    strip.setPixelColor(0, strip.Color(255, 0, 0)); 
    strip.show();
    delay(10);
    Serial.println("LED ON");
  } else if (msg == "off") {  // APP发送关灯指令
    strip.setPixelColor(0, strip.Color(0, 0, 0)); 
    strip.show();
    Serial.println("LED OFF");
  }
}

// 重连MQTT
void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    if (client.connect(uid)) {  // 使用UID作为客户端ID
      Serial.println("Connected");
      client.subscribe(topic);  // 订阅主题
    } else {
      Serial.print("Failed, rc=");
      Serial.print(client.state());
      Serial.println(" Retrying in 5s...");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);  // 设置消息回调
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();
}
