#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>        /*Include the mDNS library*/
#include <Ticker.h>             /*Ticker Library*/
#include <LittleFS.h>           /*Reference https://microcontrollerslab.com/littlefs-introduction-install-esp8266-nodemcu-filesystem-uploader-arduino/ */
#include <NTPClient.h>          /*Reference https://www.instructables.com/Getting-Time-From-Internet-Using-ESP8266-NTP-Clock/ */
#include <ESP32Time.h>          /*Reference https://github.com/fbiego/ESP32Time, setting and retrieving internal RTC time */

/* GPIO Assignment:
 * GPIO0: Boot mode selection, held low on reset to enter bootloader
 *        ESP01 relay module
 * GPIO2: BLUE LED on ESP-01S, active low
 */
#define BUILD_DEBUG   /*Comment this line for release*/
#ifdef BUILD_DEBUG   /*Debug*/
  #define RELAY 2   /*DEBUG: connected to GPIO2 (BLUE LED on ESP-01S, active low)*/
  #define FACTORY_RESET 0   /*Press and hold for 5 seconds to factory reset*/
  #define RELAY_ON   LOW
  #define RELAY_OFF  HIGH
#else   /*Release*/
  #define RELAY 0   /*ESP01 relay module is connected to GPIO0, active high*/
  #define FACTORY_RESET 2   /*Press and hold for 5 seconds to factory reset*/
  #define RELAY_ON   HIGH
  #define RELAY_OFF  LOW
#endif   /*#ifdef DEBUG*/
#define SSID_LEN_MAX 32
#define PWRD_LEN_MAX 64
#define NTPSRV_LEN_MAX 100
#define NTPSRV_INTERVAL 1200000   /*Sync with NTP every 20 minutes*/

#define TICK_INTERVAL_STATUS  60   /*in second*/
#define TICK_INTERVAL_BLINK   250  /*in ms*/
#define TICK_INTERVAL_SCHED   2    /*in second*/
#define DEFAULT_CLK_NTPSRV    "pool.ntp.org"
#define PIN_LONG_PRESS_MS     5000

/* Function Prototypes */
String urldecode(String str);
unsigned char h2int(char c);

/* Configurations */
char ssid[SSID_LEN_MAX] = "SSID"; // fill in here your router or wifi SSID
char password[PWRD_LEN_MAX] = "password"; // fill in here your router or wifi password
char ap_ssid[SSID_LEN_MAX] = "";
char ap_password[PWRD_LEN_MAX] = "";
char sched[24];
char clk_ntpsrv[NTPSRV_LEN_MAX] = DEFAULT_CLK_NTPSRV;
int clk_zone = 480;   /*UTC offset in minutes*/

String newHostname = "esp8266";
WiFiServer server(80);   /*The plain TCP Server*/

/*NTP Client*/
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, clk_ntpsrv, clk_zone*60l /*utcOffsetInSeconds*/);
ESP32Time timeRTC(0);   /*Offline RTC*/

/*Ticker*/
Ticker TickStatusCheck, TickSchedule, TickBlink;
void vTickCheckStaStatus();
void vTickSchedule();
void vTickBlink();

/*Pin change handler*/
unsigned int pinLastTrigger = 0;
bool pinStatus = true;   /*Configure to default pin status (HIGH), otherwise, first press will be ignored*/
bool longPress = false;
bool shortPress = false;

enum enEvent{
  nenEvNoEvent,
  nenEvWiFiConnected,
  nenEvWiFiDisconnected,
} enEvent = nenEvNoEvent;

typedef enum enCtrlState{
  nenCtrlStateOff,
  nenCtrlStateOn,
  nenCtrlStateBlink,
  nenCtrlStateSched,
} tenCtrlState;
tenCtrlState enCtrlState = nenCtrlStateOn;

void setup() 
{
  Serial.begin(115200); /*must be same baudrate with the Serial Monitor*/

  pinMode(RELAY,OUTPUT);
  digitalWrite(RELAY, RELAY_ON);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS Error!");
    return;  /*Abort*/
  }

  /*Get configuration from config files*/
  /*FILE 1: CONFIG.TXT*/
  File file;
  file = LittleFS.open("/config.txt", "r");
  if (file) {
    String StrConfig = file.readString();
    Serial.println("\n" + StrConfig);

    urldecode(StrConfig.substring(StrConfig.indexOf("ssid=")+5, StrConfig.indexOf("&pwrd="))).toCharArray(ssid, SSID_LEN_MAX);
    urldecode(StrConfig.substring(StrConfig.indexOf("&pwrd=")+6, StrConfig.indexOf("&ap_ssid="))).toCharArray(password, PWRD_LEN_MAX);
    urldecode(StrConfig.substring(StrConfig.indexOf("&ap_ssid=")+9, StrConfig.indexOf("&ap_pwrd="))).toCharArray(ap_ssid, SSID_LEN_MAX);
    urldecode(StrConfig.substring(StrConfig.indexOf("&ap_pwrd=")+9, StrConfig.indexOf("&clk_ntp="))).toCharArray(ap_password, PWRD_LEN_MAX);
    urldecode(StrConfig.substring(StrConfig.indexOf("&clk_ntp=")+9, StrConfig.indexOf("&clk_zone="))).toCharArray(clk_ntpsrv, NTPSRV_LEN_MAX);
    clk_zone = urldecode(StrConfig.substring(StrConfig.indexOf("&clk_zone=")+10)).toInt();
  }
  file.close();
  /*Construct default SSID*/
  if (ap_ssid[0] == '\0') {
    char StrChipId[8];
    sprintf(StrChipId, "%06X", ESP.getChipId());
    strcpy(ap_ssid, "ESP-");
    strcat(ap_ssid, StrChipId);
  }
  /*Default NTP Server*/
  if (clk_ntpsrv[0] == '\0') {
    strcpy(clk_ntpsrv, DEFAULT_CLK_NTPSRV);
  }

  /*FILE 2: SCHED.TXT*/
  file = LittleFS.open("/sched.txt", "r");
  if (file) {
    String StrConfig = file.readString();
    Serial.println(StrConfig);

    char match_pattern[10] = "&hr00=on";
    for (int hr=0; hr<24; hr++) {
      match_pattern[4] = hr % 10 + '0';
      match_pattern[3] = hr / 10 + '0';
      sched[hr] = (StrConfig.indexOf(match_pattern) != -1) ? 1 : 0;
    }
  }
  file.close();

  /*FILE 3: STATE.TXT*/
  file = LittleFS.open("/state.txt", "r");
  if (file) {
    int i32State = file.parseInt();
    Serial.println(i32State);
    enCtrlState = (tenCtrlState)i32State;
  }
  file.close();
  switch (enCtrlState)
  {
    case nenCtrlStateOn:
      digitalWrite(RELAY, RELAY_ON);
      break;
    case nenCtrlStateOff:
      digitalWrite(RELAY, RELAY_OFF);
      break;
    case nenCtrlStateBlink:
      TickBlink.attach_ms(TICK_INTERVAL_BLINK, vTickBlink);
      break;
    case nenCtrlStateSched:
      TickSchedule.attach(TICK_INTERVAL_SCHED, vTickSchedule);
      break;
    default:
      break;
  }

  /*Set new hostname*/
  wifi_station_set_hostname(newHostname.c_str());
  WiFi.hostname(newHostname.c_str());
  Serial.printf("Hostname: %s\n", WiFi.hostname().c_str());

  /*Turn on Access Point Mode as fallback mechanism*/
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("Enable AP ");
  Serial.println(WiFi.softAPSSID());

  /*Print the IP address*/
  Serial.print("Use this URL to connect: ");
  Serial.print("http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/");

  /*Start the mDNS responder for esp8266.local*/
  if (!MDNS.begin(newHostname.c_str())) {
    Serial.println("Error setting up MDNS responder!");
  } else {
    Serial.println("mDNS responder started");
  }

  /*Start the server*/
  server.begin();
  Serial.println("Server started");
 
  /*Station Mode: Init Wi-Fi*/
  /*Connect to WiFi network*/
  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  /*Initialize Ticker every 60s*/
  TickStatusCheck.attach(TICK_INTERVAL_STATUS, vTickCheckStaStatus); //Use attach_ms if you need time in ms

  /*Initialize NTP Client*/
  timeClient.setPoolServerName(clk_ntpsrv);
  timeClient.setTimeOffset(clk_zone*60l);
  timeClient.setUpdateInterval(NTPSRV_INTERVAL);
  timeClient.begin();

  /*Pin change interrupt*/
  pinMode(FACTORY_RESET, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FACTORY_RESET), pinInterruptHandler, CHANGE);
}


void loop() 
{
  /*Variables*/
  File file;
  enum enPage {
    nenPageSwitch,
    nenPageSetting,
    nenPageStatus,
    nenPageSchedule,
    nenPageSubmitted
  } enPage = nenPageSwitch;

  switch (enEvent)
  {
    case nenEvWiFiConnected:
      /*Connected to WiFi, no longer need the fallback Access Point*/
      WiFi.softAPdisconnect (true);   /*Disable AP but keep WiFi on for STA mode*/
      Serial.print("Disable AP ");
      Serial.println(ap_ssid);
      delay(500);
      /*Print the IP address*/
      Serial.println("");
      Serial.println("WiFi connected");
      Serial.print("Use this URL to connect: ");
      Serial.print("http://");
      Serial.print(WiFi.localIP());
      Serial.println("/");
      enEvent = nenEvNoEvent;
      break;
    case nenEvWiFiDisconnected:
      /*Turn on fallback AP if lost WiFi connection*/
      WiFi.softAP(ap_ssid, ap_password);
      Serial.print("Enable AP ");
      Serial.println(ap_ssid);
      delay(500);
      /*Print the IP address*/
      Serial.print("Use this URL to connect: ");
      Serial.print("http://");
      Serial.print(WiFi.softAPIP());
      Serial.println("/");
      enEvent = nenEvNoEvent;
      break;
    default:
      break;
  }

  MDNS.update();
  /*Check if a client has connected*/
  WiFiClient client = server.available();

  if(timeClient.update())
  {
    timeRTC.setTime(timeClient.getEpochTime());   /*Sync offline RTC*/
    Serial.println("NTP " + timeClient.getFormattedTime());
  }

  /*Pin change handler*/
  if(pinLastTrigger != 0)   /*Visual indication before factory reset*/
  {
    if(millis() > pinLastTrigger + PIN_LONG_PRESS_MS)
    {
      if (TickSchedule.active()) TickSchedule.detach();
      TickBlink.attach_ms(TICK_INTERVAL_BLINK, vTickBlink);
    }
  }
  if(shortPress)
  {
    shortPress = false;
    Serial.println("Short press");
  }
  if(longPress)   /*Perform factory reset*/
  {
    Serial.println("Long press");
    /*Factory reset by delete all configurations*/
    LittleFS.remove("/config.txt");
    LittleFS.remove("/sched.txt");
    LittleFS.remove("/state.txt");
    ESP.reset();
  }

  if (!client) 
  {
    return;
  }
 
  /*Wait until the client sends some data*/
  Serial.println("new client");
  unsigned int u32FailSafeTimer = 1000;   /*Set 1s timeout*/
  while(!client.available())
  {
    u32FailSafeTimer--;
    delay(1);   /*Pause for 1ms*/
    if (0 == u32FailSafeTimer)   /*Fail-safe timer elapsed, get out from unexpected error*/
    {
      Serial.printf("timeout (RSSI: %d dBm)\n\n", WiFi.RSSI());
      client.flush();
      return;
    }
  }

  /*Read the first line of the request*/
  String request = client.readStringUntil('\r');
  String request_post, request_body;
  Serial.println(request);

  /*Read POST data from request body*/
  if (request.indexOf("POST") != -1)
  {
    request_body = client.readString();
  }
  client.flush();
 
  /*Match the request*/
  /*GET method*/
  if (request.indexOf("/ON") != -1)
  {
    Serial.println("RELAY=ON");
    if (TickBlink.active()) TickBlink.detach();
    if (TickSchedule.active()) TickSchedule.detach();
    digitalWrite(RELAY, RELAY_ON);
    file = LittleFS.open("/state.txt", "w");
    file.print(nenCtrlStateOn);
    file.close();
    enPage = nenPageSwitch;
  }
  if (request.indexOf("/OFF") != -1)
  {
    Serial.println("RELAY=OFF");
    if (TickBlink.active()) TickBlink.detach();
    if (TickSchedule.active()) TickSchedule.detach();
    digitalWrite(RELAY, RELAY_OFF);
    file = LittleFS.open("/state.txt", "w");
    file.print(nenCtrlStateOff);
    file.close();
    enPage = nenPageSwitch;
  }
  if (request.indexOf("/BLINK") != -1)
  {
    Serial.println("BLINK");
    if (TickSchedule.active()) TickSchedule.detach();
    TickBlink.attach_ms(TICK_INTERVAL_BLINK, vTickBlink);
    file = LittleFS.open("/state.txt", "w");
    file.print(nenCtrlStateBlink);
    file.close();
    enPage = nenPageSwitch;
  }
  if (request.indexOf("/TIMER") != -1)
  {
    Serial.println("TIMER");
    if (TickBlink.active()) TickBlink.detach();
    TickSchedule.attach(TICK_INTERVAL_SCHED, vTickSchedule);
    file = LittleFS.open("/state.txt", "w");
    file.print(nenCtrlStateSched);
    file.close();
    enPage = nenPageSwitch;
  }
  if (request.indexOf("/SETTING") != -1)
  {
    Serial.println("SETTING");
    enPage = nenPageSetting;
  }
  if (request.indexOf("/STATUS") != -1)
  {
    Serial.println("STATUS");
    enPage = nenPageStatus;
  }
  if (request.indexOf("/SCHEDULE") != -1)
  {
    Serial.println("SCHEDULE");
    enPage = nenPageSchedule;
  }
  if (request.indexOf("/RESET") != -1)
  {
    /*Factory reset by delete all configurations*/
    LittleFS.remove("/config.txt");
    LittleFS.remove("/sched.txt");
    LittleFS.remove("/state.txt");
    ESP.reset();
  }
  if (request.indexOf("/REBOOT") != -1)
  {
    ESP.reset();
  }

  /*POST method*/
  if (request.indexOf("/SAVE_CFG") != -1)
  {
    char * pu8Ret;
    Serial.println("SAVE_CFG");
    request_post = request_body.substring(request_body.indexOf("ssid="));
    file = LittleFS.open("/config.txt", "w");
    file.print(request_post);
    file.close();
    urldecode(request_post.substring(request_post.indexOf("ssid=")+5, request_post.indexOf("&pwrd="))).toCharArray(ssid, SSID_LEN_MAX);
    urldecode(request_post.substring(request_post.indexOf("&pwrd=")+6, request_post.indexOf("&ap_ssid="))).toCharArray(password, PWRD_LEN_MAX);
    urldecode(request_post.substring(request_post.indexOf("&ap_ssid=")+9, request_post.indexOf("&ap_pwrd="))).toCharArray(ap_ssid, SSID_LEN_MAX);
    urldecode(request_post.substring(request_post.indexOf("&ap_pwrd=")+9, request_post.indexOf("&clk_ntp="))).toCharArray(ap_password, PWRD_LEN_MAX);
    urldecode(request_post.substring(request_post.indexOf("&clk_ntp=")+9, request_post.indexOf("&clk_zone="))).toCharArray(clk_ntpsrv, NTPSRV_LEN_MAX);
    clk_zone = urldecode(request_post.substring(request_post.indexOf("&clk_zone=")+10)).toInt();
    /*Construct default SSID*/
    if (ap_ssid[0] == '\0') {
      char StrChipId[8];
      sprintf(StrChipId, "%06X", ESP.getChipId());
      strcpy(ap_ssid, "ESP-");
      strcat(ap_ssid, StrChipId);
    }
    /*Default NTP Server*/
    if (clk_ntpsrv[0] == '\0') {
      strcpy(clk_ntpsrv, DEFAULT_CLK_NTPSRV);
    }
    timeClient.setPoolServerName(clk_ntpsrv);
    timeClient.setTimeOffset(clk_zone*60l);
    if (timeClient.forceUpdate()) {   /*Syc with the new NTP server*/
      timeRTC.setTime(timeClient.getEpochTime());   /*Sync offline RTC*/
      Serial.println("NTP " + timeClient.getFormattedTime());
    } else {
      Serial.println("NTP Fail!");
    }
    enPage = nenPageSubmitted;
  }

  if (request.indexOf("/SAVE_SKD") != -1)
  {
    char * pu8Ret;
    Serial.println("SAVE_SKD");
    request_post = request_body.substring(request_body.indexOf("SKD=V1"));
    file = LittleFS.open("/sched.txt", "w");
    file.print(request_post);
    file.close();

    char match_pattern[10] = "&hr00=on";
    for (int hr=0; hr<24; hr++) {
      match_pattern[4] = hr % 10 + '0';
      match_pattern[3] = hr / 10 + '0';
      sched[hr] = (request_post.indexOf(match_pattern) != -1) ? 1 : 0;
    }
    enPage = nenPageSubmitted;
  }

  if (request.indexOf("/ADJ_TIME") != -1)
  {
    String time_str;
    int time_hrs, time_min, time_epoch;

    Serial.println("ADJ_TIME");
    time_str = urldecode(request_body.substring(request_body.indexOf("set_time=")+9));
    if (!time_str.isEmpty()) {
      Serial.println(time_str);
      time_hrs = time_str.substring(0, 2).toInt();
      time_min = time_str.substring(3, 5).toInt();
      time_epoch = time_hrs * 3600 + time_min * 60;   /*Ignore date*/
      timeRTC.setTime((time_epoch));
    }
    enPage = nenPageSetting;
  }

  /*Return the response*/
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println(""); //  this is a must


  /*Read html from FileSyetem*/
  /*Sidebar*/
  file = LittleFS.open("/sidebar.html", "r");
  if (!file) {
    Serial.println("File not found!");
    return; /*Abort*/
  }
  while (file.available()) {
    client.println(file.readString());
  }
  file.close();

  int numberOfNetworks;
  int checkbox_sequence[24] = {21, 22, 23,  0,  1,  2,  3,
                               20,                      4,
                               19,                      5,
                               18,                      6,
                               17,                      7,
                               16,                      8,
                               15, 14, 13, 12, 11, 10,  9};
  /*Body*/
  switch (enPage)
  {
    case nenPageSwitch:
      file = LittleFS.open("/index.html", "r");
      if (!file) {
        Serial.println("File not found!");
        return; /*Abort*/
      }
      while (file.available()) {
        client.println(file.readString());
      }
      file.close();
      break;
    case nenPageSetting:
      file = LittleFS.open("/setting.html", "r");
      if (!file) {
        Serial.println("File not found!");
        return; /*Abort*/
      }
      client.print(file.readStringUntil('#'));   /*Terminator character # is discarded*/
      client.print("<option value=\"");
      client.print(ssid);
      client.print("\">");
      client.print(ssid);
      client.print(" (saved)</option>");
      numberOfNetworks = WiFi.scanNetworks();
      for (int i=0; i < numberOfNetworks; i++) {
        client.print("<option value=\"");
        client.print(WiFi.SSID(i));
        client.print("\">");
        client.print(WiFi.SSID(i));
        client.print("</option>");
      }
      client.print(file.readStringUntil('#'));
      client.print(password);
      client.print(file.readStringUntil('#'));
      client.print(ap_ssid);
      client.print(file.readStringUntil('#'));
      client.print(ap_password);
      client.print(file.readStringUntil('#'));
      client.print(clk_ntpsrv);
      client.print(file.readStringUntil('#'));
      client.print("<option value=\"");
      client.print(clk_zone);
      client.print("\">");
      client.print((clk_zone >= 0)?"UTC+":"UTC");
      client.print(clk_zone / 60);
      if (clk_zone % 60) {
        client.print(":");
        client.print(abs(clk_zone) % 60);
      }
      client.print(" (saved)</option>");
      client.print(file.readStringUntil('#'));
      client.print(timeRTC.getTime().substring(0,5));   /*HH:MM*/
      client.println(file.readString());
      file.close();
      break;
    case nenPageStatus:
      file = LittleFS.open("/status.html", "r");
      if (!file) {
        Serial.println("File not found!");
        return; /*Abort*/
      }
      /*Wi-Fi*/
      client.print(file.readStringUntil('#'));   /*Terminator character # is discarded*/
      client.print(WiFi.RSSI());
      if (WiFi.RSSI() > -67) {client.print(" (good)");}
      else if (WiFi.RSSI() > -70) {client.print(" (fair)");}
      else if (WiFi.RSSI() > -80) {client.print(" (weak)");}
      else {client.print(" (poor)");}
      client.print(file.readStringUntil('#'));
      client.print(WiFi.SSID());
      client.print(file.readStringUntil('#'));
      client.print(WiFi.localIP());
      client.print(file.readStringUntil('#'));
      client.print(WiFi.macAddress());
      client.print(file.readStringUntil('#'));
      client.print(ap_ssid);
      client.print(file.readStringUntil('#'));
      client.print(WiFi.softAPIP());
      client.print(file.readStringUntil('#'));
      client.print(WiFi.softAPmacAddress());
      client.print(file.readStringUntil('#'));
      /*Clock*/
      client.print(clk_ntpsrv);
      client.print(file.readStringUntil('#'));
      client.print((clk_zone >= 0)?"UTC+":"UTC");
      client.print(clk_zone / 60);
      if (clk_zone % 60) {
        client.print(":");
        client.print(abs(clk_zone) % 60);
      }
      client.print(file.readStringUntil('#'));
      client.print(timeRTC.getTime());
      client.print(file.readStringUntil('#'));
      /*Control*/
      client.print(TickSchedule.active() ? "Scheduled" : "Manual");
      client.print(file.readStringUntil('#'));
      if (TickBlink.active()) {client.print("Blink");}
      else {client.print((digitalRead(RELAY) == RELAY_ON) ? "On" : "Off");}
      client.print("</td></tr><tr><td>Firmware Version:</td><td>vMAJOR.MINOR.PATCH");
      client.println(file.readString());
      file.close();
      break;
    case nenPageSchedule:
      file = LittleFS.open("/schedule.html", "r");
      if (!file) {
        Serial.println("File not found!");
        return; /*Abort*/
      }
      for (int index=0; index<24; index++) {
        client.print(file.readStringUntil('#'));
        if (sched[checkbox_sequence[index]]) client.print(" checked");
      }
      client.println(file.readString());
      file.close();
      break;
    case nenPageSubmitted:
      file = LittleFS.open("/submitted.html", "r");
      if (!file) {
        Serial.println("File not found!");
        return; /*Abort*/
      }
      while (file.available()) {
        client.println(file.readString());
      }
      file.close();
      break;
    default:
      break;
  }

  delay(1);
  Serial.println("Client disconnected");
  Serial.println("");
}


/*********
* Tickers
*********/
void vTickCheckStaStatus()
{
  IPAddress ip_unset(0,0,0,0);   /*(IP unset)*/
  if (WiFi.status() != WL_CONNECTED)
  {
    if (WiFi.softAPIP() == ip_unset)
    {
      enEvent = nenEvWiFiDisconnected;
    }
  }
  else if (WiFi.status() == WL_CONNECTED)
  {
    if (WiFi.softAPIP() != ip_unset)
    {
      enEvent = nenEvWiFiConnected;
    }
  }
}

void vTickSchedule()
{
  if (sched[timeRTC.getHour(true)]) {
    digitalWrite(RELAY, RELAY_ON);
  } else {
    digitalWrite(RELAY, RELAY_OFF);
  }
}

void vTickBlink()
{
  /*Toggle*/
  digitalWrite(RELAY,!digitalRead(RELAY));
}


/*******************************************************************************
* Source: https://circuits4you.com/2019/03/21/esp8266-url-encode-decode-example/
*******************************************************************************/
unsigned char h2int(char c)
{
    if (c >= '0' && c <='9'){
        return((unsigned char)c - '0');
    }
    if (c >= 'a' && c <='f'){
        return((unsigned char)c - 'a' + 10);
    }
    if (c >= 'A' && c <='F'){
        return((unsigned char)c - 'A' + 10);
    }
    return(0);
}

String urldecode(String str)
{
    String encodedString="";
    char c;
    char code0;
    char code1;
    for (int i =0; i < str.length(); i++){
        c=str.charAt(i);
      if (c == '+'){
        encodedString+=' ';
      }else if (c == '%') {
        i++;
        code0=str.charAt(i);
        i++;
        code1=str.charAt(i);
        c = (h2int(code0) << 4) | h2int(code1);
        encodedString+=c;
      } else{

        encodedString+=c;
      }

      yield();
    }
   return encodedString;
}


/*********************************************************
* Pin Change Interrupt
* Reference: https://github.com/Naguissa/ESP_INTERUPT_TEST
*********************************************************/
ICACHE_RAM_ATTR void pinInterruptHandler() {
  bool prev = pinStatus;
  pinStatus = digitalRead(FACTORY_RESET);

  if (prev == pinStatus) {
    return;   /*Skip, duplicate*/
  }
  if (pinStatus) {   /*Release - active low*/
    if (pinLastTrigger == 0) {
      return;   /*Release without press? Discard*/
    }
    if (millis() > pinLastTrigger + PIN_LONG_PRESS_MS) {   /*Long press*/
      longPress = true;
    } else {   /*Short press*/
      shortPress = true;
    }
    pinLastTrigger = 0;
  } else {   /*Pressed*/
     pinLastTrigger = millis();
  }
}
