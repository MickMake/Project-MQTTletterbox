#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <stdio.h>
#include "DHT.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_TSL2561_U.h>


WiFiClient espClient;
const char* ssid = "WiFi SSID";
const char* password = "Your password";

PubSubClient client(espClient);
const char* mqtt_server = "your.mqtt.server";

// Topic base for all comms from this device.
#define TOPICBASE "Home/LetterBox/"

// Variables used for temperature & humidity sensing.
#define TEMP_PIN 0
DHT dhtSensor(TEMP_PIN, DHT22);
unsigned long confTempDelay = 10000;    // Default temperature publish delay.
unsigned long LastTempMillis = 0;       // Stores the last millis() for determining update delay.
float TempValue;
float HumidValue;
float HindexValue;

// Variables used for lux sensing.
Adafruit_TSL2561_Unified LuxSensor = Adafruit_TSL2561_Unified(TSL2561_ADDR_FLOAT, 42);
sensor_t LuxSensorInfo;
unsigned long confLuxDelay = 10000;    // Default temperature publish delay, (0 to disable).
unsigned long LastLuxMillis = 0;       // Stores the last millis() for determining update delay.
float LuxValue;

// Variables used for when someone delivers mail to the mailbox.
#define DELIVER_PIN 14
unsigned long confDeliverDelay = 5000;  // Default time to wait to assume the next mail delivery event, (0 to disable).
unsigned long DeliverLastMillis = 0;    // Stores the last millis() when waiting for the next mail delivery event.
boolean DeliverPubFlag = 0;             // Used to keep client.publish out of int handlers.
int DeliverCount = 0;

// Variables used for when someone checks the mailbox for mail.
#define CHECK_PIN 12
#define LAZY_GND 13                     // Instead of having to use additional wires.
unsigned long confCheckDelay = 1000;    // Default time to wait for mailbox check debounce, (0 to disable).
unsigned long CheckLastMillis = 0;      // Stores the last millis() when waiting for the mailbox check debounce.
boolean CheckPubFlag = 0;               // Used to keep client.publish out of int handlers.
int CheckCount = 0;


void setup()
{
	//pinMode(BUILTIN_LED, OUTPUT);
	//digitalWrite(BUILTIN_LED, LOW);

	pinMode(DELIVER_PIN, INPUT);
	attachInterrupt(DELIVER_PIN, DeliverFunc, FALLING);

  pinMode(LAZY_GND, OUTPUT);
  digitalWrite(LAZY_GND, LOW);
	pinMode(CHECK_PIN, INPUT_PULLUP);
	attachInterrupt(CHECK_PIN, CheckFunc, FALLING);

	Serial.begin(115200);
	WiFiConnect();
	client.setServer(mqtt_server, 1883);
	client.setCallback(callback);

  dhtSensor.begin();

  if (!LuxSensor.begin())
  {
    Publish((char *)"Lux", (char *)"DEAD");
  }
  else
  {
    LuxSensor.enableAutoRange(true);
    LuxSensor.setIntegrationTime(TSL2561_INTEGRATIONTIME_402MS);
  }
}


void loop()
{
	if (!client.connected())
	{
		reconnect();
	}
	client.loop();

  // Publish - someone has put mail in the box.
	if (DeliverPubFlag)
	{
    PublishInt((char *)"Delivery", DeliverCount);
		DeliverPubFlag = 0;
	}

  // Publish - someone has checked the box for mail.
  if (CheckPubFlag)
	{
    PublishInt((char *)"Check", 1);
		CheckPubFlag = 0;
	}

  if (confLuxDelay && (millis() - LastLuxMillis > confLuxDelay))
  {
    LastLuxMillis = millis();

    sensors_event_t event;
    LuxSensor.getEvent(&event);
 
    if (event.light)
    {
      LuxValue = event.light;
      PublishFloat((char *)"Lux", LuxValue);
    }
    else
      Publish((char *)"Lux", (char *)"OL");
  }

	if (confTempDelay && (millis() - LastTempMillis > confTempDelay))
	{
		LastTempMillis = millis();

		TempValue = dhtSensor.readTemperature();
    PublishFloat((char *)"Temperature", TempValue);

		HumidValue = dhtSensor.readHumidity();
    PublishFloat((char *)"Humidity", HumidValue);

		HindexValue = dhtSensor.computeHeatIndex(TempValue, HumidValue, false);
    PublishFloat((char *)"HeatIndex", HindexValue);
	}
}


void Publish(char *Topic, char *Message)
{
  char TopicBase[80] = TOPICBASE;

  strcat(TopicBase, Topic);
  client.publish(TopicBase, Message);
}


void PublishInt(char *Topic, int Value)
{
  char TopicBase[80] = TOPICBASE;
  char Message[10] = "NULL";

  if (!isnan(Value))
    itoa(Value, Message, 10);

  strcat(TopicBase, Topic);
  client.publish(TopicBase, Message);
}


void PublishFloat(char *Topic, float Value)
{
  char TopicBase[80] = TOPICBASE;
  char Message[10] = "NULL";

  if (!isnan(Value))
    dtostrf(Value, 5, 2, Message);

  strcat(TopicBase, Topic);
  client.publish(TopicBase, Message);
}


void reconnect()
{
	// Loop until we're reconnected
	while (!client.connected())
	{
		// Attempt to connect
		if (client.connect("LetterBox", "letterbox", "letterbox", (char *)TOPICBASE "State", 1, 0, "DEAD"))
		{
			// Once connected, publish an announcement...
      Publish((char *)"State", (char *)"BOOTUP");
			// Subscribe to enable bi-directional comms.
			client.subscribe(TOPICBASE "Config/#");  // Allow bootup config fetching using MQTT persist flag!
      client.subscribe(TOPICBASE "Put/#");     // Send commands to this device, use Home/LetterBox/Get/# for responses.
		}
		else
			delay(5000);
	}
}


void WiFiConnect()
{
	delay(10);

  WiFi.begin(ssid, password);
	while (WiFi.status() != WL_CONNECTED)
		delay(500);
}


void DeliverFunc()
{
	noInterrupts();
	
	if ((millis() - DeliverLastMillis) > confDeliverDelay)
	{
		DeliverPubFlag = 1;
		DeliverCount++;
	}
	DeliverLastMillis = millis();
	
	interrupts();
}


void CheckFunc()
{
	noInterrupts();
	
	if ((millis() - CheckLastMillis) > confCheckDelay)
	{
		CheckPubFlag = 1;
		DeliverCount = 0;
		CheckCount = 0;
	}
	// CheckCount++;
	CheckLastMillis = millis();
	
	interrupts();
}


void callback(char* topic, byte* payload, unsigned int length)
{
	payload[length] = 0;    // Hack to be able to use this as a char string.

	if (strstr(topic, TOPICBASE "Config/"))
	{
		if (strstr(topic, "TempDelay"))
		    	confTempDelay = atoi((const char *)payload);
		
		else if (strstr(topic, "DeliverDelay"))
			confDeliverDelay = atoi((const char *)payload);
		
		else if (strstr(topic, "CheckDelay"))
			confCheckDelay = atoi((const char *)payload);

		else if (strstr(topic, "LuxDelay"))
			confLuxDelay = atoi((const char *)payload);
	}
}

