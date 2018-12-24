#include <SPI.h>
#include <EthernetUdp.h>
#include <EthernetServer.h>
#include <EthernetClient.h>
#include <Ethernet.h>
#include <Dns.h>
#include <Dhcp.h>
#include <SoftwareSerial.h>
#include <FS.h>
#include <Bounce2.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> 
#include <ArduinoJson.h>

#include "UserConfig.h"


char mqtt_server[40] =		USER_MQTT_SERVER;
char mqtt_port[6] =			USER_MQTT_PORT;
char mqtt_user[20] =        USER_MQTT_USER;
char mqtt_password[20] =	USER_MQTT_PASSWORD;
char mqtt_token[40] =       USER_MQTT_TOKEN; 
char mqtt_token_OnOff[40]=	USER_MQTT_TOKEN_CMD;                 //On Off command
char mqtt_token_State[40]=  "";                 //State variable
char mqtt_token_TimerEnable[40]="";             //Enable Timer Off    
char mqtt_token_PT[40]=     "" ;                //Preset timer
char mqtt_token_RemoteCommand[40] = "";

uint8_t MAC_array[6];       //MAC Address                  
char MAC_char[18];          //MAC Address in string

bool ReleState = LOW;       //Actual relay state
bool OldReleState = LOW;
bool TimerEnable = 0;       //Timer enable

long lastMsg = 0;
long OnTime = 0;
long OnFilter = 0;
int TimePT = 90;

//default custom static IP
char static_ip[16] = USER_STATIC_IP;
char static_gw[16] = USER_STATIC_GATEWAY;
char static_sn[16] = USER_STATIC_NETMASK;

//flag for saving data
bool shouldSaveConfig = false;

WiFiClient espClient;
PubSubClient client(espClient);

Bounce ExternalCmd = Bounce(); 
Bounce RemoteCommand2 = Bounce();

bool bStartPacket;

//callback notifying us of the need to save config
void saveConfigCallback ()
{
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup()
{
    // put your setup code here, to run once:
    Serial.begin(115200);
    Serial.println();

   
    pinMode(RELE, OUTPUT);
	pinMode(LED, OUTPUT);
    pinMode(REMOTE_COMMAND1, INPUT_PULLUP);
	pinMode(REMOTE_COMMAND2, INPUT_PULLUP);
    
    ExternalCmd.attach(REMOTE_COMMAND1);
    ExternalCmd.interval(500);

	RemoteCommand2.attach(REMOTE_COMMAND2);
	RemoteCommand2.interval(500);
    
    //read configuration from FS json
    Serial.println("mounting FS...");
    WiFi.macAddress(MAC_array);
    for (int i = 0; i < sizeof(MAC_array); ++i)
        sprintf(MAC_char,"%s%02x_",MAC_char,MAC_array[i]);
    
    if (SPIFFS.begin())
    {
        Serial.println("mounted file system");
        if (SPIFFS.exists("/config.json"))
        {
            //file exists, reading and loading
            Serial.println("reading config file");
            File configFile = SPIFFS.open("/config.json", "r");
            if (configFile)
            {
                Serial.println("opened config file");
                size_t size = configFile.size();
                // Allocate a buffer to store contents of the file.
                std::unique_ptr<char[]> buf(new char[size]);
    
                configFile.readBytes(buf.get(), size);
                DynamicJsonBuffer jsonBuffer;
                JsonObject& json = jsonBuffer.parseObject(buf.get());
                json.printTo(Serial);
                if (json.success())
                {
                    Serial.println("\nparsed json");
                    strcpy(mqtt_server, json["mqtt_server"]);
                    strcpy(mqtt_port, json["mqtt_port"]);
                    strcpy(mqtt_user, json["mqtt_user"]);
                    strcpy(mqtt_password, json["mqtt_password"]);
                    strcpy(mqtt_token, json["mqtt_token"]);
    
                    if(json["ip"])
                    {
                        Serial.println("setting custom ip from config");
                        strcpy(static_ip, json["ip"]);
                        strcpy(static_gw, json["gateway"]);
                        strcpy(static_sn, json["subnet"]);
                        Serial.println(static_ip);
                    }
                    else
                    {
                        Serial.println("no custom ip in config");
                    }
                }
                else
                {
                    Serial.println("failed to load json config");
                }
            }
        }
    }
    else
    {
        Serial.println("failed to mount FS");
    }
    //end read
    Serial.println(static_ip);
    Serial.println(mqtt_server);

    //Extra parameters
    WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
    WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 5);
    WiFiManagerParameter custom_mqtt_user("user","mqtt user", mqtt_user, 20);
    WiFiManagerParameter custom_mqtt_password("password","mqtt password", mqtt_password, 20);
    WiFiManagerParameter custom_mqtt_token("token","mqtt token", mqtt_token, 40);
    
    //WiFiManager
    //Local intialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    
    //set static ip
    IPAddress _ip,_gw,_sn;
    _ip.fromString(static_ip);
    _gw.fromString(static_gw);
    _sn.fromString(static_sn);
    
    wifiManager.setSTAStaticIPConfig(_ip, _gw, _sn);
    
    //add all your parameters here
    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_port);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_password);
    wifiManager.addParameter(&custom_mqtt_token);

    wifiManager.setMinimumSignalQuality();
  
    //fetches ssid and pass and tries to connect
    //if it does not connect it starts an access point with the specified name
    //here  "AutoConnectAP"
    //and goes into a blocking loop awaiting configuration
    if (!wifiManager.autoConnect(MAC_char, "password"))
    {
        Serial.println("failed to connect and hit timeout");
        delay(3000);
        //reset and try again, or maybe put it to deep sleep
        ESP.reset();
        delay(5000);
    }
    //if you get here you have connected to the WiFi
    Serial.println("connected...yeey :)");
    //read updated parameters
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_password, custom_mqtt_password.getValue());
    strcpy(mqtt_token, custom_mqtt_token.getValue());

    //save the custom parameters to FS
    if (shouldSaveConfig)
    {
        Serial.println("saving config");
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.createObject();
        json["mqtt_server"] = mqtt_server;
        json["mqtt_port"] = mqtt_port;
        json["mqtt_user"] = mqtt_user;
        json["mqtt_password"] = mqtt_password;
        json["mqtt_token"] = mqtt_token;
        
        json["ip"] = WiFi.localIP().toString();
        json["gateway"] = WiFi.gatewayIP().toString();
        json["subnet"] = WiFi.subnetMask().toString();
        
        File configFile = SPIFFS.open("/config.json", "w");
        if (!configFile)
        {
            Serial.println("failed to open config file for writing");
        }
        json.prettyPrintTo(Serial);
        json.printTo(configFile);
        configFile.close();
    //end save
    }
    
    Serial.println("local ip");
    Serial.println(WiFi.localIP());
    Serial.println(WiFi.gatewayIP());
    Serial.println(WiFi.subnetMask());

    strcpy (mqtt_token_OnOff,mqtt_token);
    strcat (mqtt_token_OnOff,"/OnOff"); 
    strcpy (mqtt_token_State,mqtt_token);
    strcat (mqtt_token_State,"/State"); 
    strcpy (mqtt_token_TimerEnable,mqtt_token);
    strcat (mqtt_token_TimerEnable,"/TimerEnable"); 
    strcpy (mqtt_token_PT,mqtt_token);
    strcat (mqtt_token_PT,"/PT"); 
	strcpy(mqtt_token_RemoteCommand, mqtt_token);
	strcat(mqtt_token_RemoteCommand, "/Remote");
    client.setServer(mqtt_server, atoi(mqtt_port));
    client.setCallback(callback);
}

void callback(char* topic, byte* payload, unsigned int length)
{
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    for (int i = 0; i < length; i++)
        Serial.print((char)payload[i]);
    Serial.println();

    if (strcmp(topic,mqtt_token_OnOff)==0)
    {
        if ((char)payload[0] == '1')
        {
            ReleState = HIGH;
            OnTime = millis();
        }
        else 
            ReleState = LOW;
		digitalWrite(RELE, ReleState);  // Turn the LED off by making the voltage HIGH
		digitalWrite(LED, ReleState);  // Turn the LED off by making the voltage HIGH
    }
    else
    {
        if (strcmp(topic,mqtt_token_TimerEnable)==0)
        {
            if ((char)payload[0] == '1')
                TimerEnable = 1;
            else 
                TimerEnable = 0;
        }
        else
        {
            if (strcmp(topic,mqtt_token_PT)==0)
            {
                TimePT = atoi((char*)payload);
                
            }
        }
    }
}

void reconnect()
{
    // Loop until we're reconnected
    while (!client.connected())
    {
        Serial.print("Attempting MQTT connection...");
        // Attempt to connect
        if (client.connect("ESP8266Client", mqtt_user, mqtt_password))
        {
            Serial.println(strcat("connected",mqtt_server));
            // ... and resubscribe
            Serial.println(mqtt_token);
            client.subscribe(mqtt_token_OnOff);
            client.subscribe(mqtt_token_TimerEnable);
            client.subscribe(mqtt_token_PT);
              
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(5000);
        }
    }
}


void loop()
{
    if (!client.connected())
        reconnect();
    client.loop();
    
    long now = millis();
    if (now - lastMsg > 60000 || ReleState!=OldReleState)
    {
        OldReleState = ReleState;
        lastMsg = now;
        if (ReleState==1)
            client.publish(mqtt_token_State, "1");
        else
            client.publish(mqtt_token_State, "0");

    }
    if (TimerEnable == 1 && ReleState == 1)
    {
       if (now - OnTime > TimePT*1000)
        {
            Serial.print("Time expired");
            ReleState = 0;
            digitalWrite(RELE, ReleState);  // Turn the LED off by making the voltage HIGH
			digitalWrite(LED, ReleState);  // Turn the LED off by making the voltage HIGH
            client.publish(mqtt_token_State, "0");
            client.publish(mqtt_token_OnOff, "0");
        }
    }

    

    if (ExternalCmd.update())
    {
        ReleState = ! ReleState;
        digitalWrite(RELE, ReleState); 
		digitalWrite(LED, ReleState);
        if (ReleState==1)
        {
            OnTime= millis();
            client.publish(mqtt_token_State, "1");
        }
        else
            client.publish(mqtt_token_State, "0");
        
    }

	if (RemoteCommand2.update())
	{
		bool Remote2 = RemoteCommand2.read();
		if (Remote2)
			client.publish(mqtt_token_RemoteCommand, "1");
		else
			client.publish(mqtt_token_RemoteCommand, "0");
	}
	
    
     
    
}
