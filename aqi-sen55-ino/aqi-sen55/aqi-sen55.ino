#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <SensirionI2CSen5x.h>

#define SDA_PIN 21
#define SCL_PIN 22

// WiFi credentials
const char* ssid = "your ssid";
const char* password = "your password";

// MQTT Broker
const char* mqtt_server = "broker.hivemq.com";
const char* topic = "your mqtt topic";

WiFiClient espClient;
PubSubClient client(espClient);
SensirionI2CSen5x sen5x;

void setup_wifi() {
  delay(10);
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected!");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    if (client.connect("ESP32_SEN55")) {
      Serial.println("Connected!");
    } else {
      Serial.print("Failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying...");
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(19200);

  // WiFi
  setup_wifi();
  client.setServer(mqtt_server, 1883);

  // I2C + SEN55
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  sen5x.begin(Wire);
  sen5x.startMeasurement();
  delay(1000);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  float pm1, pm2_5, pm4, pm10;
  float humidity, temperature, voc, nox;

  uint16_t error = sen5x.readMeasuredValues(
    pm1, pm2_5, pm4, pm10,
    humidity, temperature, voc, nox
  );

  if (!error) {

    String payload = "{";
    payload += "\"pm2_5\":" + String(pm2_5) + ",";
    payload += "\"temp\":" + String(temperature) + ",";
    payload += "\"humidity\":" + String(humidity) + ",";
    payload += "\"voc\":" + String(voc);
    payload += "}";

    client.publish(topic, payload.c_str());

    Serial.println("Published:");
    Serial.println(payload);
  } else {
    Serial.println("Sensor read error!");
  }

  delay(5000);  // send every 5 seconds
}
