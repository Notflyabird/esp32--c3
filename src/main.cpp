#include <WiFi.h>
#include <ArduinoMqttClient.h>
#include <Adafruit_NeoPixel.h>


#define LED_PIN    8  // 数据引脚
#define LED_COUNT  1   // LED数量

// 配置网络和巴法云参数
const char* ssid = "Xiaomi";   // wifi name
const char* password = "18166334677"; // wifi passwords
const char* mqtt_server = "8.137.149.129"; // MQTT服务器地址
const int mqtt_port = 1883;            // 端口号
const char* uid = "79fa744a49ad461cb25cc83a2ee6c01a";            // 用户密钥
const char* topic = "led002";     // 主题名

WiFiClient espClient;
MqttClient mqttClient(espClient);
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

// 处理接收到的MQTT消息
void onMqttMessage(int messageSize) {
  String msg = "";
  // 读取消息内容
  while (mqttClient.available()) {
    msg += (char)mqttClient.read();
  }

  Serial.print("收到消息: ");
  Serial.println(msg);

  // 移除可能存在的引号
  msg.replace("\"", "");

  if (msg == "on") {         // APP发送开灯指令
    strip.setPixelColor(0, strip.Color(255, 0, 0)); 
    strip.show();
    delay(10);
    Serial.println("LED ON");
    // 发送确认消息
    mqttClient.beginMessage(topic);
    mqttClient.print("{\"status\":\"on\"}");
    mqttClient.endMessage();
  } else if (msg == "off") {  // APP发送关灯指令
    strip.setPixelColor(0, strip.Color(0, 0, 0)); 
    strip.show();
    Serial.println("LED OFF");
    // 发送确认消息
    mqttClient.beginMessage(topic);
    mqttClient.print("{\"status\":\"off\"}");
    mqttClient.endMessage();
  }
}

// 连接MQTT服务器
void connectMqtt() {
  Serial.print("Connecting to MQTT...");
  
  mqttClient.setId(uid);  // 使用UID作为客户端ID
  
  if (!mqttClient.connect(mqtt_server, mqtt_port)) {
    Serial.print("Connection failed! Error code = ");
    Serial.println(mqttClient.connectError());
    delay(5000);
    return;
  }
  
  Serial.println("Connected to MQTT broker!");
  
  // 订阅主题
  mqttClient.subscribe(topic);
  
  // 设置消息回调
  mqttClient.onMessage(onMqttMessage);
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  setup_wifi();
  connectMqtt();
}

void loop() {
  // 检查MQTT连接状态
  if (!mqttClient.connected()) {
    connectMqtt();
  }
  
  // 保持MQTT客户端运行，处理消息
  mqttClient.poll();
}