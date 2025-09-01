/*
 * ============================================================
 *  PetFeeder - ESP8266 + A4988  (Cat Feeder Wi-Fi / OTA / Web UI)
 *  -> Planning journalier + Page Paramètres (gear icon)
 * ============================================================
 *
 * ✨ Fonctions :
 *  - WiFiManager : AP "<device_name>-SETUP" si non configuré
 *  - NTP fuseau : configurable (menu + champ avancé)
 *  - OTA : mot de passe configurable (défaut "croquettes")
 *          Hostname dynamique : "<device_name>-XXXX" (4 derniers digits MAC)
 *  - Web UI moderne (responsive) avec header UNIFIÉ sur toutes les pages
 *      Accueil "/" : Planning (ajout/édition/duplication/suppression + jours LMMJVSD + suppression globale),
 *                    actions (Feed / Unclog / Feed en avance / +5 / +15),
 *                    quota + distribué dans l’entête, graphique 7 jours
 *      Paramètres "/settings" : device_name, ota_pass, timezone (menu + avancé),
 *                               portion_steps, step_us_slow/fast, ramp_steps,
 *                               inversion sens, maintien couple, safe mode,
 *                               QUOTA/jour, logs (on/niveau),
 *                               MQTT / Webhook, Export/Import config, Reboot
 *      Logs "/logs" : lecture + effacer + télécharger
 *      Update "/update" : page OTA intégrée au design
 *  - API : /status (JSON), /feed[?n=1], /unclog, /feed_next, /feed_in?min=..&n=..
 *          /api/schedule (GET JSON, POST add), /api/schedule/update (POST),
 *          /api/schedule/delete?id=IDX|-1, /api/schedule/duplicate?id=IDX
 *  - Bouton physique : 1x = feed, 3x = débourrage, 5x = safe ON/OFF (GPIO4/D2 vers GND)
 *  - Rampe d'accélération (accel/palier/decel)
 *
 * ⚡ Câblage ESP8266 ↔ A4988 (logique 3,3 V OK)
 *  - DIR   ← GPIO14  (ex. D5)
 *  - STEP  ← GPIO12  (ex. D6)
 *  - EN    ← GPIO13  (ex. D7)        (A4988 : EN = LOW => activé)
 *  - SLEEP + RESET → 3,3 V           (ponter ensemble, driver réveillé en permanence)
 *  - MS1 / MS2 / MS3 : non connectés (full-step pour commencer)
 *  - VDD (logique A4988) → 3,3 V (ESP)
 *  - VMOT (puissance A4988) → 12 V
 *  - GND : commun ESP / A4988 / Alim 12 V
 *  - Condensateur 100–220 µF entre VMOT et GND, pattes courtes, proche du driver
 *
 * 🧵 Câblage Moteur (NEMA17 17HE12-1204S – docs couleurs IT)
 *  - Bobine A : A+ = Nero (Noir), A− = Blu (Bleu)  -> sur 1A / 1B du driver
 *  - Bobine B : B+ = Verde (Vert), B− = Rosso (Rouge) -> sur 2A / 2B du driver
 *  ⚠ Suis les libellés 1A/1B/2A/2B, pas l'ordre physique du clone A4988.
 *
 * 🔘 Bouton physique (déclenchement manuel)
 *  - Bouton poussoir entre GPIO4 (ex. D2) et GND
 *  - Pas de résistance externe (INPUT_PULLUP utilisé)
 *  - 1 clic = feed ; 3 clics = débourrage ; 5 clics = safe ON/OFF
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <time.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Updater.h>
#include <FS.h>

// ---------- PINS ----------
#define PIN_DIR   14  // D5
#define PIN_STEP  12  // D6
#define PIN_EN    13  // D7 (LOW=enable)
#define PIN_BTN    4  // D2 bouton -> GND

// ---------- FILES ----------
#define CFG_PATH    "/config.json"
#define SCHED_PATH  "/schedule.json"
#define LOG_PATH    "/logs.txt"
#define STATS_PATH  "/stats.json"

// ---------- SERVER / MQTT ----------
ESP8266WebServer server(80);
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------- TYPES / GLOBALS ----------
enum { DOW_MON=0, DOW_TUE, DOW_WED, DOW_THU, DOW_FRI, DOW_SAT, DOW_SUN };

struct SchedEvent { uint8_t hour; uint8_t minute; uint8_t rations; uint8_t daysMask; };
const uint8_t MAX_EVENTS = 32;
SchedEvent scheduleList[MAX_EVENTS];
uint8_t scheduleCount = 0;
uint16_t lastRunYDay[MAX_EVENTS], lastRunMinute[MAX_EVENTS];

struct DayStat { char date[11]; uint16_t r; };
DayStat last7[7]; uint8_t statCount=0;
uint16_t rationsToday=0; uint16_t quotaYDayRef=0xFFFF;

volatile uint32_t btnLastChange=0;
uint8_t btnClickCount=0; bool btnPrevState=true; uint32_t btnWindowStart=0;

String hostName = "petfeeder-esp8266";
char macSuffix[5]="0000";

// ---------- CONFIG ----------
struct Config {
  char device_name[32]     = "PetFeeder";
  char ota_pass[32]        = "croquettes";
  char tz[64]              = "CET-1CEST,M3.5.0,M10.5.0/3";
  uint16_t portion_steps   = 200;
  uint16_t step_us_slow    = 1200;
  uint16_t step_us_fast    = 600;
  uint16_t ramp_steps      = 120;
  bool     dir_invert      = false;
  bool     hold_enable     = true;
  bool     safe_mode       = false;
  uint8_t  daily_quota     = 0;      // 0 = illimité
  bool     logs_enable     = true;
  uint8_t  log_level       = 1;
  bool     mqtt_enable     = false;
  char     mqtt_host[64]   = "";
  uint16_t mqtt_port       = 1883;
  char     mqtt_topic[64]  = "petfeeder";
  char     mqtt_user[32]   = "";
  char     mqtt_pass[32]   = "";
  char     webhook_url[128]= "";
} cfg;

// ---------- UTILS ----------
inline uint8_t dowFromTm(int tm_wday){ return (tm_wday==0)?6:(uint8_t)(tm_wday-1); }
inline bool isDayActive(uint8_t mask, uint8_t dow){ return (mask >> dow) & 0x01; }

String macLast4(){ uint8_t m[6]; WiFi.macAddress(m); char b[5]; snprintf(b,sizeof(b),"%02X%02X",m[4],m[5]); return String(b); }
String formatDate(time_t t){ struct tm* tmNow=localtime(&t); char b[11]; if(!tmNow){strcpy(b,"0000-00-00");} else {snprintf(b,sizeof(b),"%04d-%02d-%02d",1900+tmNow->tm_year,1+tmNow->tm_mon,tmNow->tm_mday);} return String(b); }

// ---------- LOGS ----------
enum { LOG_ERR=0, LOG_INFO=1, LOG_DBG=2 };
void logTrimIfNeeded(){ File f=LittleFS.open(LOG_PATH,"r"); if(!f) return; size_t sz=f.size(); if(sz<=64*1024){ f.close(); return; } const size_t keep=32*1024; f.seek(sz-keep,SeekSet); String tail=f.readString(); f.close(); File w=LittleFS.open(LOG_PATH,"w"); if(!w) return; w.print(tail); w.close(); }
void logEvent(uint8_t level,const String& tag,const String& msg){
  if(!cfg.logs_enable || level>cfg.log_level) return;
  time_t now=time(nullptr); struct tm* tmNow=localtime(&now);
  char ts[32]; if(!tmNow){ strcpy(ts,"0000-00-00 00:00:00"); } else { snprintf(ts,sizeof(ts),"%04d-%02d-%02d %02d:%02d:%02d",1900+tmNow->tm_year,1+tmNow->tm_mon,tmNow->tm_mday,tmNow->tm_hour,tmNow->tm_min,tmNow->tm_sec); }
  File f=LittleFS.open(LOG_PATH,"a"); if(f){ f.printf("[%s] %s/%s: %s\n",ts,(level==0?"ERR":(level==1?"INF":"DBG")),tag.c_str(),msg.c_str()); f.close(); }
  logTrimIfNeeded();
}

// ---------- STATS ----------
void statsSave(); void statsEnsureToday();
void statsLoad(){ statCount=0; if(!LittleFS.exists(STATS_PATH)) return; File f=LittleFS.open(STATS_PATH,"r"); if(!f) return; StaticJsonDocument<2048> d; if(deserializeJson(d,f)){ f.close(); return; } f.close(); JsonArray a=d["days"].as<JsonArray>(); if(a.isNull()) return; for(JsonObject o:a){ if(statCount>=7) break; const char* dt=o["d"]|"0000-00-00"; uint16_t r=(uint16_t)(o["r"]|0); strncpy(last7[statCount].date,dt,sizeof(last7[statCount].date)); last7[statCount].date[10]='\0'; last7[statCount].r=r; statCount++; } }
void statsSave(){ StaticJsonDocument<2048> d; JsonArray a=d.createNestedArray("days"); for(uint8_t i=0;i<statCount;i++){ JsonObject o=a.createNestedObject(); o["d"]=last7[i].date; o["r"]=last7[i].r; } File f=LittleFS.open(STATS_PATH,"w"); if(!f) return; serializeJson(d,f); f.close(); }
void statsEnsureToday(){ String today=formatDate(time(nullptr)); for(uint8_t i=0;i<statCount;i++){ if(today.equals(last7[i].date)) return; } if(statCount<7){ strncpy(last7[statCount].date,today.c_str(),sizeof(last7[statCount].date)); last7[statCount].date[10]='\0'; last7[statCount].r=0; statCount++; statsSave(); return; } for(uint8_t i=1;i<7;i++) last7[i-1]=last7[i]; strncpy(last7[6].date,today.c_str(),sizeof(last7[6].date)); last7[6].date[10]='\0'; last7[6].r=0; statsSave(); }
void statsAdd(uint16_t r){ statsEnsureToday(); String today=formatDate(time(nullptr)); for(uint8_t i=0;i<statCount;i++){ if(today.equals(last7[i].date)){ last7[i].r += r; break; } } statsSave(); }

// ---------- SCHEDULE ----------
void scheduleSort(); bool scheduleSave(); bool scheduleLoad();
void scheduleAdd(uint8_t hh,uint8_t mm,uint8_t r,uint8_t daysMask){ if(scheduleCount>=MAX_EVENTS) return; scheduleList[scheduleCount++]={hh,mm,r,daysMask}; scheduleSort(); scheduleSave(); }
bool scheduleDelete(uint8_t idx){ if(idx>=scheduleCount) return false; for(uint8_t i=idx;i+1<scheduleCount;i++){ scheduleList[i]=scheduleList[i+1]; lastRunYDay[i]=lastRunYDay[i+1]; lastRunMinute[i]=lastRunMinute[i+1]; } scheduleCount--; scheduleSave(); return true; }
bool scheduleDuplicate(uint8_t idx, uint8_t &newIdx){ if(idx>=scheduleCount||scheduleCount>=MAX_EVENTS) return false; SchedEvent e=scheduleList[idx]; scheduleList[scheduleCount++]=e; scheduleSort(); scheduleSave(); for(uint8_t i=0;i<scheduleCount;i++){ if(scheduleList[i].hour==e.hour && scheduleList[i].minute==e.minute && scheduleList[i].rations==e.rations && scheduleList[i].daysMask==e.daysMask){ newIdx=i; return true; } } newIdx = scheduleCount-1; return true; }
void scheduleSort(){ for(uint8_t i=1;i<scheduleCount;i++){ SchedEvent k=scheduleList[i]; uint16_t km=k.hour*60+k.minute; int j=i-1; while(j>=0){ uint16_t jm=scheduleList[j].hour*60+scheduleList[j].minute; if(jm<=km) break; scheduleList[j+1]=scheduleList[j]; j--; } scheduleList[j+1]=k; } for(uint8_t i=0;i<scheduleCount;i++){ lastRunYDay[i]=0xFFFF; lastRunMinute[i]=0xFFFF; } }
bool scheduleSave(){ StaticJsonDocument<4096> d; JsonArray a=d.createNestedArray("events"); for(uint8_t i=0;i<scheduleCount;i++){ JsonObject o=a.createNestedObject(); o["hour"]=scheduleList[i].hour; o["minute"]=scheduleList[i].minute; o["rations"]=scheduleList[i].rations; o["days"]=scheduleList[i].daysMask; } File f=LittleFS.open(SCHED_PATH,"w"); if(!f) return false; bool ok=serializeJson(d,f)>0; f.close(); return ok; }
bool scheduleLoad(){ scheduleCount=0; for(uint8_t i=0;i<MAX_EVENTS;i++){ lastRunYDay[i]=0xFFFF; lastRunMinute[i]=0xFFFF; } if(!LittleFS.exists(SCHED_PATH)) return true; File f=LittleFS.open(SCHED_PATH,"r"); if(!f) return false; StaticJsonDocument<4096> d; DeserializationError e=deserializeJson(d,f); f.close(); if(e) return false; JsonArray a=d["events"].as<JsonArray>(); if(a.isNull()) return true; for(JsonObject o:a){ if(scheduleCount>=MAX_EVENTS) break; uint8_t hh=(uint8_t)(o["hour"]|0), mm=(uint8_t)(o["minute"]|0), rr=(uint8_t)(o["rations"]|1), dd=(uint8_t)(o["days"]|0x7F); if(hh<24&&mm<60&&rr>=1) scheduleList[scheduleCount++]={hh,mm,rr,dd}; } scheduleSort(); return true; }

// ---------- CONFIG FILE ----------
bool loadConfig(){ if(!LittleFS.exists(CFG_PATH)) return false; File f=LittleFS.open(CFG_PATH,"r"); if(!f) return false; StaticJsonDocument<4096> d; DeserializationError e=deserializeJson(d,f); f.close(); if(e) return false; strlcpy(cfg.device_name, d["device_name"]|"PetFeeder", sizeof(cfg.device_name)); strlcpy(cfg.ota_pass, d["ota_pass"]|"croquettes", sizeof(cfg.ota_pass)); strlcpy(cfg.tz, d["tz"]|"CET-1CEST,M3.5.0,M10.5.0/3", sizeof(cfg.tz)); cfg.portion_steps=(uint16_t)(d["portion_steps"]|200); cfg.step_us_slow=(uint16_t)(d["step_us_slow"]|1200); cfg.step_us_fast=(uint16_t)(d["step_us_fast"]|600); cfg.ramp_steps=(uint16_t)(d["ramp_steps"]|120); cfg.dir_invert=d["dir_invert"]|false; cfg.hold_enable=d["hold_enable"]|true; cfg.safe_mode=d["safe_mode"]|false; cfg.daily_quota=(uint8_t)(d["daily_quota"]|0); cfg.logs_enable=d["logs_enable"]|true; cfg.log_level=(uint8_t)(d["log_level"]|1); cfg.mqtt_enable=d["mqtt_enable"]|false; strlcpy(cfg.mqtt_host, d["mqtt_host"]|"", sizeof(cfg.mqtt_host)); cfg.mqtt_port=(uint16_t)(d["mqtt_port"]|1883); strlcpy(cfg.mqtt_topic, d["mqtt_topic"]|"petfeeder", sizeof(cfg.mqtt_topic)); strlcpy(cfg.mqtt_user, d["mqtt_user"]|"", sizeof(cfg.mqtt_user)); strlcpy(cfg.mqtt_pass, d["mqtt_pass"]|"", sizeof(cfg.mqtt_pass)); strlcpy(cfg.webhook_url, d["webhook_url"]|"", sizeof(cfg.webhook_url)); return true; }
bool saveConfig(){ StaticJsonDocument<4096> d; d["device_name"]=cfg.device_name; d["ota_pass"]=cfg.ota_pass; d["tz"]=cfg.tz; d["portion_steps"]=cfg.portion_steps; d["step_us_slow"]=cfg.step_us_slow; d["step_us_fast"]=cfg.step_us_fast; d["ramp_steps"]=cfg.ramp_steps; d["dir_invert"]=cfg.dir_invert; d["hold_enable"]=cfg.hold_enable; d["safe_mode"]=cfg.safe_mode; d["daily_quota"]=cfg.daily_quota; d["logs_enable"]=cfg.logs_enable; d["log_level"]=cfg.log_level; d["mqtt_enable"]=cfg.mqtt_enable; d["mqtt_host"]=cfg.mqtt_host; d["mqtt_port"]=cfg.mqtt_port; d["mqtt_topic"]=cfg.mqtt_topic; d["mqtt_user"]=cfg.mqtt_user; d["mqtt_pass"]=cfg.mqtt_pass; d["webhook_url"]=cfg.webhook_url; File f=LittleFS.open(CFG_PATH,"w"); if(!f) return false; bool ok=serializeJson(d,f)>0; f.close(); return ok; }

// ---------- MOTOR ----------
void driverEnable(bool on){ digitalWrite(PIN_EN, on?LOW:HIGH); }
void stepPulse(uint16_t us){ digitalWrite(PIN_STEP,HIGH); delayMicroseconds(us/2); digitalWrite(PIN_STEP,LOW); delayMicroseconds(us/2); }
void stepperRun(int32_t steps,bool dir,uint16_t usSlow,uint16_t usFast,uint16_t ramp){
  digitalWrite(PIN_DIR, dir?HIGH:LOW); driverEnable(true);
  uint32_t n=(steps>=0)?steps:-steps; if(n==0){ if(!cfg.hold_enable) driverEnable(false); return; }
  if(ramp*2>n) ramp=n/2;
  for(uint32_t i=0;i<ramp;i++){ uint16_t us=usSlow - ((uint32_t)(usSlow-usFast)*i)/ramp; stepPulse(us); }
  for(uint32_t i=0;i<n-2*ramp;i++){ stepPulse(usFast); }
  for(uint32_t i=0;i<ramp;i++){ uint16_t us=usFast + ((uint32_t)(usSlow-usFast)*i)/ramp; stepPulse(us); }
  if(!cfg.hold_enable) driverEnable(false);
}
void feedRations(uint8_t n){ if(n<1)n=1; if(cfg.safe_mode) return; uint32_t total=(uint32_t)cfg.portion_steps*n; bool dir=cfg.dir_invert?false:true; stepperRun(total,dir,cfg.step_us_slow,cfg.step_us_fast,cfg.ramp_steps); rationsToday+=n; statsAdd(n); logEvent(LOG_INFO,"FEED","dispense x"+String(n)); }
void unclog(){ uint16_t s=cfg.portion_steps/3; stepperRun(s,cfg.dir_invert,cfg.step_us_slow,cfg.step_us_fast,cfg.ramp_steps/2); delay(150); stepperRun(s,!cfg.dir_invert,cfg.step_us_slow,cfg.step_us_fast,cfg.ramp_steps/2); delay(150); stepperRun(s,cfg.dir_invert,cfg.step_us_slow,cfg.step_us_fast,cfg.ramp_steps/2); logEvent(LOG_INFO,"UNCLOG","done"); }

// ---------- QUOTA ----------
bool quotaAllow(uint8_t n){ if(cfg.daily_quota==0) return true; return (rationsToday+n) <= cfg.daily_quota; }

// ---------- MQTT / WEBHOOK ----------
void mqttReconnect(){ if(!cfg.mqtt_enable) return; if(mqtt.connected()) return; if(strlen(cfg.mqtt_host)==0) return; mqtt.setServer(cfg.mqtt_host,cfg.mqtt_port); String cid=hostName+"-"+String(ESP.getChipId(),HEX); if(strlen(cfg.mqtt_user)>0){ mqtt.connect(cid.c_str(),cfg.mqtt_user,cfg.mqtt_pass); } else { mqtt.connect(cid.c_str()); } if(mqtt.connected()){ String t = String(cfg.mqtt_topic)+"/cmd/#"; mqtt.subscribe(t.c_str()); } }
void mqttCallback(char* topic,byte* payload,unsigned int len){ String t(topic); String p; p.reserve(len+1); for(unsigned int i=0;i<len;i++) p+=(char)payload[i]; if(t.endsWith("/cmd/feed")){ long v=p.toInt(); if(v<1)v=1; if(v>20)v=20; if(quotaAllow((uint8_t)v) && !cfg.safe_mode) feedRations((uint8_t)v); } else if(t.endsWith("/cmd/unclog")){ unclog(); } else if(t.endsWith("/cmd/safe")){ cfg.safe_mode=(p=="1"||p=="on"||p=="true"); saveConfig(); } }
void mqttPublish(const String& sub,const String& val){ if(!cfg.mqtt_enable||!mqtt.connected()) return; String t=String(cfg.mqtt_topic)+"/"+sub; mqtt.publish(t.c_str(),val.c_str(),true); }
void webhookEvent(const String&,const String&){ /* optionnel */ }

// ---------- HTML DECL ----------
String htmlIndex(const String& host);
String htmlSettingsPage(bool saved,const String& toastMsg);
String htmlLogsPage();
String htmlUpdatePage(const String& msg);

// ---------- COMMON HEADER CSS & HTML ----------
String commonHeaderCSS(){
  return String(F(
    ":root{--bg:#0b0b0f;--card:#15151c;--muted:#9aa3af;--txt:#e7e9ee;--pri:#6366f1;--pri2:#8b5cf6;--axis:#a1a1aa;--bar:#8b5cf6}\n"
    ":root.light{--bg:#f5f6fb;--card:#ffffff;--muted:#6b7280;--txt:#111827;--pri:#4f46e5;--pri2:#7c3aed;--axis:#6b7280;--bar:#7c3aed}\n"
    "html,body{height:100%}\n"
    "body{margin:0;background:linear-gradient(135deg,var(--bg),var(--bg));color:var(--txt);font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,Inter,Montserrat,Inter,sans-serif}\n"
    "header{min-height:88px;padding:14px 16px;background:linear-gradient(90deg,var(--pri),var(--pri2));color:#fff;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap}\n"
    "header h1{margin:6px 0 0 0;font-size:18px;display:flex;align-items:center;gap:10px}\n"
    "header .actions{display:flex;gap:8px;align-items:center;flex-wrap:wrap}\n"
    "header a.icon,header button.icon{display:inline-flex;align-items:center;justify-content:center;width:36px;height:36px;border-radius:10px;background:rgba(255,255,255,.15);text-decoration:none;border:none;color:#fff;cursor:pointer}\n"
    "main{padding:16px;display:grid;gap:16px;max-width:980px;margin:auto}\n"
    ".card{background:var(--card);border:1px solid #1f2030;border-radius:16px;box-shadow:0 10px 30px rgba(0,0,0,.15);padding:16px}\n"
    ":root.light .card{border-color:#e5e7eb;box-shadow:0 8px 20px rgba(0,0,0,.06)}\n"
    "button,.btn,input,select{border-radius:12px;border:1px solid #313244;background:#0f1016;color:#e5e7eb;padding:10px 12px}\n"
    ":root.light button,:root.light .btn,:root.light input,:root.light select{background:#f3f4f6;border-color:#d1d5db;color:#111827}\n"
    ".btn{font:inherit;text-decoration:none;display:flex;align-items:center;justify-content:center;gap:8px;cursor:pointer}\n"
    "button.primary,.btn.primary{background:linear-gradient(90deg,var(--pri),var(--pri2));border:none;color:#fff}\n"
    ".pill{display:inline-flex;align-items:center;gap:8px;padding:6px 10px;border-radius:999px;border:1px solid #2b2e45;color:#f0f1ff;background:rgba(0,0,0,.18)}\n"
    ".row{display:flex;gap:12px;flex-wrap:wrap;align-items:center}\n"
    ".rowcol{display:grid;gap:6px}\n"
    ".actions-grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(180px,1fr))}\n"
    ".btn.warn{background:#f59e0b;color:#111827;border:none}\n"
    ".btn.danger{background:#dc2626;color:#fff;border:none}\n"
    ":root.light .btn.warn{background:#fbbf24;color:#111827}\n"
    ":root.light .btn.danger{background:#f87171;color:#fff}\n"
    ".toast{position:fixed;right:16px;top:16px;display:grid;gap:10px;z-index:9999}\n"
    ".toast .t{background:#111827;color:#e5e7eb;border:1px solid #2b2e45;padding:10px 14px;border-radius:12px;box-shadow:0 10px 20px rgba(0,0,0,.35)}\n"
    "svg,svg *{fill:currentColor!important;stroke:currentColor!important;stroke-width:0!important}\n"
  ));
}

String commonHeaderHTML(const String& title,const String& host){
  String h;
  h += F("<header><div style='min-width:260px'>");
  h += "<h1>"+title+"</h1>";
  h += "<div class='rowcol' style='margin-top:6px'>"
       "<div>Appareil : <b>"+host+"</b></div>"
       "<div>Distribué : <b id='distTxt'>0</b> &nbsp;|&nbsp; Quota : <span id='quotaTxt'>...</span></div>"
       "</div></div>";
  h += F("<div class='actions'>"
         "<a class='icon' href='/' title='Accueil'><svg width='20' height='20' viewBox='0 0 24 24'><path d='M10 19v-5h4v5h5v-8h3L12 3 2 11h3v8z'/></svg></a>"
         "<a class='icon' href='/settings' title='Paramètres'><svg width='20' height='20' viewBox='0 0 24 24'><path d='M19.14,12.94a7.48,7.48,0,0,0,.05-1l2.11-1.65a.5.5,0,0,0,.12-.64l-2-3.46a.5.5,0,0,0-.6-.22l-2.49,1a7.09,7.09,0,0,0-1.73-1l-.38-2.65A.5.5,0,0,0,13.72,3H10.28a.5.5,0,1,1-.49.41L9.41,6.06a7.09,7.09,0,0,0-1.73,1l-2.49-1a.5.5,0,0,0-.6.22l-2,3.46a.5.5,0,0,0,.12.64L4.86,11a7.48,7.48,0,0,0,0,2l-2.11,1.65a.5.5,0,0,0-.12.64l2,3.46a.5.5,0,0,0,.6.22l2.49,1a7.09,7.09,0,0,0,1.73,1l.38,2.65a.5.5,0,0,0,.49.41h3.44a.5.5,0,0,0,.49-.41l.38-2.65a.5.5,0,0,0,.6-.22l2-3.46a.5.5,0,0,0,.12-.64ZM12,15.5A3.5,3.5,0,1,1,15.5,12,3.5,3.5,0,0,1,12,15.5Z'/></svg></a>"
         "<a class='icon adv' style='display:none' href='/logs' title='Logs'><svg width='20' height='20' viewBox='0 0 24 24'><path d='M4 4h16v2H4zm0 6h16v2H4zm0 6h10v2H4z'/></svg></a>"
         "<a class='icon adv' style='display:none' href='/update' title='Mise à jour'><svg width='20' height='20' viewBox='0 0 24 24'><path d='M12 3v10.55A4 4 0 1 0 14 17V7h4l-6-6-6 6z'/></svg></a>"
         "<button id='theme' class='icon' title='Basculer thème' aria-label='Thème'><svg width='20' height='20' viewBox='0 0 24 24'><path d='M12 3a1 1 0 0 1 1 1v1a1 1 0 1 1-2 0V4a1 1 0 0 1 1-1zm7.07 2.93a1 1 0 0 1 0 1.41l-.7.7a1 1 0 1 1-1.41-1.41l.7-.7a1 1 0 0 1 1.41 0zM21 11a1 1 0 1 1 0 2h-1a1 1 0 1 1 0-2h1zM6.05 5.34a1 1 0 0 1 1.41 0l.7.7A1 1 0 0 1 6.75 7.46l-.7-.7a1 1 0 0 1 0-1.42zM12 18a1 1 0 0 1 1 1v1a1 1 0 1 1-2 0v-1a1 1 0 0 1 1-1zM4 11a1 1 0 1 1 0 2H3a1 1 0 1 1 0-2h1zm1.64 7.36a1 1 0 0 1 1.41 0l.7.7a1 1 0 1 1-1.41 1.41l-.7-.7a1 1 0 0 1 0-1.41zm12.02 0a1 1 0 0 1 1.41 1.41l-.7.7a1 1 0 1 1-1.41-1.41l.7-.7z'/></svg></button>"
         "</div>"
         "<div style='flex-basis:100%;display:flex;justify-content:flex-start;margin-top:8px'>"
         "<div class='pill'><svg width='16' height='16' viewBox='0 0 24 24'><path d='M12 1a11 11 0 1 0 11 11A11 11 0 0 0 12 1Zm1 11a1 1 0 0 1-1 1H7a1 1 0 1 1 0-2h4V5a1 1 0 1 1 2 0Z'/></svg><span id='nowFr'>--:--:-- --/--/----</span></div>"
         "</div></header>");
  return h;
}

// ---------- HTML INDEX ----------
String htmlIndex(const String& host){
  String items;
  for(uint8_t i=0;i<scheduleCount;i++){
    char hhmm[8]; snprintf(hhmm,sizeof(hhmm),"%02u:%02u",scheduleList[i].hour,scheduleList[i].minute);
    items += F("<div class='item' data-idx='");
    items += String((int)i);
    items += F("'><div class='time'>");
    items += hhmm;
    items += F("</div><div class='rats'>");
    items += String((int)scheduleList[i].rations);
    items += F(" ration"); if(scheduleList[i].rations>1) items+="s";
    items += F("</div><div class='dows mono'>");
    const char* lbl="LMMJVSD";
    for(uint8_t d=0; d<7; d++){
      bool on = (scheduleList[i].daysMask>>d)&1;
      items += "<span class='dowC "; if(!on) items+="off"; items+="'>"; items += String(lbl).substring(d,d+1); items += "</span>";
    }
    items += F("</div><div class='actions'>"
               "<button type='button' class='dup' title='Dupliquer' data-id='"); items+=String((int)i); items+=F("'>⧉</button>"
               "<button type='button' class='edit' title='Modifier' data-id='"); items+=String((int)i); items+=F("'>⚙</button>"
               "<button type='button' class='del' title='Supprimer' data-id='"); items+=String((int)i); items+=F("'>&times;</button>"
               "</div></div>");
  }

  String h; h.reserve(100000);
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<link rel='manifest' href='/manifest.json'>"
         "<script>if('serviceWorker' in navigator){addEventListener('load',()=>navigator.serviceWorker.register('/sw.js').catch(()=>{}));}</script>"
         "<title>PetFeeder</title><style>");
  h += commonHeaderCSS();
  h += F(
         ".grid{display:grid;gap:12px}"
         ".item{display:grid;grid-template-columns:auto auto 1fr auto;gap:10px;align-items:center;padding:10px;border:1px solid #26273b;border-radius:12px;background:#10121a}"
         ":root.light .item{background:#f9fafb;border-color:#e5e7eb}"
         ".item .time{font-weight:700;font-size:16px}"
         ".item .dows.mono{font-family:ui-monospace,Menlo,Consolas,monospace;letter-spacing:1px;color:var(--muted)}"
         ".dowC{display:inline-block;width:16px;text-align:center;margin:0 2px;padding:2px 0;border-radius:4px;border:1px solid #2b2e45}"
         ".dowC.off{opacity:.35}"
         ".item .actions{display:flex;gap:8px}"
         ".item .del,.item .edit,.item .dup{background:#161824;border:1px solid #2b2e45;color:#d1d5db;padding:6px 10px;border-radius:10px}"
         ":root.light .item .del,:root.light .item .edit,:root.light .item .dup{background:#fff;color:#374151;border-color:#e5e7eb}"
         ".item.done{opacity:.5;filter:saturate(.7)}"
         ".muted{color:var(--muted)}"
         ".next{display:flex;gap:10px;align-items:center;margin-top:8px}"
         "canvas{width:100%;max-width:920px;height:220px;display:block;background:#0e1017;border:1px solid #2b2e45;border-radius:12px}"
         ":root.light canvas{background:#fff;border-color:#e5e7eb}"
  );
  h += F("</style></head><body>");

  // HEADER unifié
  h += commonHeaderHTML("🐾 PetFeeder — Planificateur", host);

  // MAIN
  h += F("<main>");

  // Carte actions
  h += F("<section class='card'>"
         "<div class='row'>"
         "<button type='button' class='primary' id='btnFeed'>🍖 Distribuer 1</button>"
         "<button type='button' id='btnAdvance'>⏩ Distribuer en avance</button>"
         "<button type='button' id='btnUnclog'>🛠️ Débourrage</button>"
         "<button type='button' id='btnPlus5'>🕔 +5 min</button>"
         "<button type='button' id='btnPlus15'>🕒 +15 min</button>"
         "</div>"
         "<div class='next'><span class='muted'>Prochaine distribution :</span> <span class='when' id='nextWhen'>--:--</span></div>"
         "</section>");

  // Graph 7 jours
  h += F("<section class='card'>"
         "<div class='row' style='justify-content:space-between'>"
         "<div class='muted'>Historique 7 jours (rations)</div>"
         "</div>"
         "<canvas id='chart' width='940' height='260'></canvas>"
         "</section>");

  // Planning + boutons Ajouter / Tout supprimer
  h += F("<section class='card'>"
         "<div class='row' style='justify-content:space-between'>"
         "<h3 style='margin:0'>Planning du jour</h3>"
         "<div class='row'>"
         "<button type='button' id='btnAdd' class='primary'>➕ Ajouter un horaire</button>"
         "<button type='button' id='btnClearAll' class='btn'>🗑️ Supprimer tous les horaires</button>"
         "</div>"
         "</div>"
         "<div id='list' class='grid' style='margin-top:10px'>");
  h += items;
  h += F("</div></section>");

  // Modale édition
  h += F("<div id='dlg' style='position:fixed;inset:0;background:rgba(0,0,0,.45);display:none;align-items:center;justify-content:center;z-index:10000'>"
         "<div class='modal' style='background:var(--card);border:1px solid #2b2e45;border-radius:16px;padding:16px;min-width:280px;max-width:90vw;box-shadow:0 20px 60px rgba(0,0,0,.45)'>"
         "<h3 id='dlgTitle'>Nouvel horaire</h3>"
         "<input type='hidden' id='e_id' value='-1'>"
         "<div class='row'><div><label class='muted'>Heure</label><input id='e_h' type='number' min='0' max='23' value='12'></div>"
         "<div><label class='muted'>Minute</label><input id='e_m' type='number' min='0' max='59' value='00'></div></div>"
         "<div class='row'><div><label class='muted'>Rations</label><input id='e_r' type='number' min='1' max='20' value='1'></div></div>"
         "<label class='muted'>Jours</label>"
         "<div id='e_days' style='display:flex;gap:6px;flex-wrap:wrap;margin-top:8px'>"
         "<button type='button' class='dow' data-d='0'>L</button>"
         "<button type='button' class='dow' data-d='1'>M</button>"
         "<button type='button' class='dow' data-d='2'>M</button>"
         "<button type='button' class='dow' data-d='3'>J</button>"
         "<button type='button' class='dow' data-d='4'>V</button>"
         "<button type='button' class='dow' data-d='5'>S</button>"
         "<button type='button' class='dow' data-d='6'>D</button>"
         "</div>"
         "<div class='actions' style='display:flex;gap:10px;justify-content:flex-end;margin-top:12px'>"
         "<button type='button' id='btnCancel'>Annuler</button>"
         "<button type='button' id='btnSave' class='btn primary'>Enregistrer</button>"
         "</div>"
         "</div></div>");

  h += F("<div class='toast' id='toast'></div>");

  // JS
  h += F("<script>"
         "document.addEventListener('DOMContentLoaded',()=>{try{"
         "const $=s=>document.querySelector(s), $$=s=>document.querySelectorAll(s);"
         "const root=document.documentElement;"
         "const isExpert=localStorage.getItem('pf_expert')==='1'; document.querySelectorAll('.adv').forEach(e=>e.style.display=isExpert?'':'none');"
         "function applyTheme(t){ if(t==='light'){root.classList.add('light');} else {root.classList.remove('light');} localStorage.setItem('pf_theme',t);} "
         "applyTheme(localStorage.getItem('pf_theme')||'dark');"
         "const themeBtn=$('#theme'); if(themeBtn) themeBtn.onclick=()=>applyTheme(root.classList.contains('light')?'dark':'light');"
         "function two(n){return (n<10?'0':'')+n;}"
         "function updateClock(){const e=$('#nowFr'); if(!e) return; const d=new Date(); e.textContent=`${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())} ${two(d.getDate())}/${two(d.getMonth()+1)}/${d.getFullYear()}`;}"
         "setInterval(updateClock,1000); updateClock();"
         "function toast(msg){const box=$('#toast'); if(!box)return; const t=document.createElement('div'); t.className='t'; t.textContent=msg; box.appendChild(t); setTimeout(()=>{t.remove();},2200);}"
         "async function jget(u){const r=await fetch(u,{cache:'no-store'}); const ct=r.headers.get('content-type')||''; if(ct.includes('application/json')) return await r.json(); return {ok:r.ok}; }"
         "async function jpost(u,body){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}); const ct=r.headers.get('content-type')||''; if(ct.includes('application/json')) return await r.json(); return {ok:r.ok}; }"
         "function setDaysMask(mask){ $$('#e_days .dow').forEach((b,i)=>{ b.classList.toggle('on', !!(mask>>i & 1)); b.style.border='1px solid #2b2e45'; b.style.borderRadius='10px'; if(root.classList.contains('light')){ b.style.background=b.classList.contains('on')?'linear-gradient(90deg,var(--pri),var(--pri2))':'#f3f4f6'; b.style.color=b.classList.contains('on')?'#fff':'#111827'; } else { b.style.background=b.classList.contains('on')?'linear-gradient(90deg,var(--pri),var(--pri2))':'#0f1016'; b.style.color=b.classList.contains('on')?'#fff':'#e5e7eb'; } }); }"
         "function getDaysMask(){ let m=0; $$('#e_days .dow').forEach((b,i)=>{ if(b.classList.contains('on')) m|=(1<<i); }); if(m===0) m=0x7F; return m; }"
         "$$('#e_days .dow').forEach(b=>b.onclick=()=>{ b.classList.toggle('on'); setDaysMask(getDaysMask()); });"
         "function openDlg(title,id,h,m,r,mask){ $('#dlgTitle').textContent=title; $('#e_id').value=id; $('#e_h').value=h; $('#e_m').value=m; $('#e_r').value=r; setDaysMask(mask); $('#dlg').style.display='flex'; }"
         "function closeDlg(){ $('#dlg').style.display='none'; }"
         "const btnCancel=$('#btnCancel'); if(btnCancel) btnCancel.onclick=()=>closeDlg();"
         "const btnAdd=$('#btnAdd'); if(btnAdd) btnAdd.onclick=()=>openDlg('Nouvel horaire',-1,12,0,1,0x7F);"
         "$$('#list .edit').forEach(b=>b.onclick=()=>{const id=+b.dataset.id; const it=document.querySelector(`.item[data-idx='${id}']`); const hm=it.querySelector('.time').textContent.split(':'); const r=parseInt(it.querySelector('.rats').textContent); let mask=0; it.querySelectorAll('.dowC').forEach((s,i)=>{ if(!s.classList.contains('off')) mask|=(1<<i); }); openDlg('Modifier l\\'horaire',id,parseInt(hm[0]),parseInt(hm[1]),r,mask);});"
         "$$('#list .del').forEach(b=>b.onclick=async()=>{const id=+b.dataset.id; if(!confirm('Supprimer ce créneau ?')) return; await fetch('/api/schedule/delete?id='+id); toast('Supprimé'); setTimeout(()=>location.reload(),300); });"
         "$$('#list .dup').forEach(b=>b.onclick=async()=>{const id=+b.dataset.id; const r=await jget('/api/schedule/duplicate?id='+id); if(r.ok){ toast('Dupliqué'); const st=await jget('/status'); const ev=(st.events||[]).find(e=>e.id===r.new); if(ev){ openDlg('Modifier le clone',r.new,ev.hour,ev.minute,ev.rations,ev.days); } } else { toast('Échec duplication'); } });"
         "const btnSave=$('#btnSave'); if(btnSave) btnSave.onclick=async()=>{ const id=+$('#e_id').value; const h=$('#e_h').value, m=$('#e_m').value, r=$('#e_r').value, d=getDaysMask(); "
         "  if(id<0){ const rr=await jpost('/api/schedule',`hour=${h}&minute=${m}&rations=${r}&days=${d}`); toast(rr.ok?'Ajouté':'Erreur ajout'); }"
         "  else { const rr=await jpost('/api/schedule/update',`id=${id}&hour=${h}&minute=${m}&rations=${r}&days=${d}`); toast(rr.ok?'Modifié':'Erreur modif'); }"
         "  closeDlg(); setTimeout(()=>location.reload(),300); };"
         "const btnClearAll=$('#btnClearAll'); if(btnClearAll) btnClearAll.onclick=async()=>{ if(!confirm('Êtes-vous sûr de supprimer TOUS les horaires ?')) return; await jget('/api/schedule/delete?id=-1'); toast('Tous les horaires supprimés'); setTimeout(()=>location.reload(),300); };"
         "function drawChart(stats){ const c=document.getElementById('chart'); if(!c) return; const ctx=c.getContext('2d'); const cs=getComputedStyle(document.documentElement); const axis=cs.getPropertyValue('--axis').trim(); const barC=cs.getPropertyValue('--bar').trim(); ctx.clearRect(0,0,c.width,c.height); const W=c.width,H=c.height; const padL=40,padB=26,padT=10,padR=10; const plotW=W-padL-padR, plotH=H-padT-padB; const n=7; let vals=new Array(n).fill(0); let labels=new Array(n).fill(''); for(let i=0;i<Math.min(n,stats.length);i++){ vals[i]=stats[i].r||0; labels[i]=(stats[i].d||'').slice(5); } let vmax=0; vals.forEach(v=>{ if(v>vmax) vmax=v; }); if(vmax<3)vmax=3; const yTicks=Math.min(6,vmax); function y2px(v){ return padT + (plotH - (v/vmax)*plotH); } ctx.strokeStyle=axis; ctx.fillStyle=axis; ctx.font='12px system-ui,-apple-system,Segoe UI,Roboto,Inter,sans-serif'; ctx.beginPath(); ctx.moveTo(padL, padT); ctx.lineTo(padL, padT+plotH); ctx.lineTo(padL+plotW, padT+plotH); ctx.stroke(); for(let i=0;i<=yTicks;i++){ const v=Math.round(i*vmax/yTicks); const y=y2px(v); ctx.globalAlpha=0.12; ctx.beginPath(); ctx.moveTo(padL,y); ctx.lineTo(padL+plotW,y); ctx.stroke(); ctx.globalAlpha=1; ctx.fillText(String(v), 6, y+4); } const bw=plotW/n*0.6; for(let i=0;i<n;i++){ const x=padL + (i+0.5)*(plotW/n) - bw/2; const h= (vals[i]/vmax)*plotH; const y=padT+plotH-h; ctx.fillStyle=barC; ctx.fillRect(x,y,bw,h); ctx.fillStyle=axis; ctx.fillText(labels[i]||'', x, padT+plotH+18); } }"
         "async function refresh(){ try{ const s=await jget('/status'); if(!s||s.epoch===undefined) return; const quotaNum = s.daily_quota||0; const dist = s.rations_today||0; const quotaStr = quotaNum===0? 'illimité' : `${dist}/${quotaNum}`; const qTxt=$('#quotaTxt'); if(qTxt) qTxt.textContent=quotaStr; const dTxt=$('#distTxt'); if(dTxt) dTxt.textContent=String(dist); const now=new Date(s.epoch*1000); const cm=now.getHours()*60+now.getMinutes(); let best=1e9, hh='--', mm='--'; (s.events||[]).forEach(e=>{ if(e.active_today && !e.done_today){ const m=e.hour*60+e.minute; if(m>=cm && m<best){ best=m; hh=two(e.hour); mm=two(e.minute);} } }); $('#nextWhen').textContent=(best<1e9)?(`${hh}:${mm}`):'—'; (s.events||[]).forEach(e=>{ const it=document.querySelector(`.item[data-idx='${e.id}']`); if(it){ if(e.done_today && e.active_today) it.classList.add('done'); else it.classList.remove('done'); } }); drawChart(s.stats||[]); } catch(e){ console && console.warn && console.warn(e); } }"
         "const btnFeed=$('#btnFeed'); if(btnFeed) btnFeed.onclick=async()=>{const r=await jget('/feed?n=1'); toast((r&&r.ok)?'Distribué':'Action bloquée'); refresh();};"
         "const btnUnclog=$('#btnUnclog'); if(btnUnclog) btnUnclog.onclick=async()=>{const r=await jget('/unclog'); toast((r&&r.ok)?'Débourrage OK':'Action bloquée');};"
         "const btnAdvance=$('#btnAdvance'); if(btnAdvance) btnAdvance.onclick=async()=>{const r=await jget('/feed_next'); toast((r&&r.ok)?'Distribué en avance':'Aucun créneau dispo'); refresh();};"
         "const b5=$('#btnPlus5'); if(b5) b5.onclick=async()=>{const r=await jget('/feed_in?min=5&n=1'); toast((r&&r.ok)?'+5 min ajouté':'Erreur'); if(r&&r.ok){setTimeout(()=>location.reload(),300);} };"
         "const b15=$('#btnPlus15'); if(b15) b15.onclick=async()=>{const r=await jget('/feed_in?min=15&n=1'); toast((r&&r.ok)?'+15 min ajouté':'Erreur'); if(r&&r.ok){setTimeout(()=>location.reload(),300);} };"
         "refresh(); setInterval(refresh,15000);"
         "}catch(e){console.error(e);}});"
         "</script>");

  h += F("</main></body></html>");
  return h;
}

// ---------- TIMEZONES ----------
const char* TZ_PRESETS[][2] = {
  {"Europe/Paris","CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Rome" ,"CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/London","GMT0BST,M3.5.0/1,M10.5.0/2"},
  {"UTC","UTC0"},
};

// ---------- HTML SETTINGS ----------
String htmlSettingsPage(bool saved,const String& toastMsg){
  String opts; bool alreadySel=false;
  for(size_t i=0;i<sizeof(TZ_PRESETS)/sizeof(TZ_PRESETS[0]);i++){
    const char* label=TZ_PRESETS[i][0]; const char* value=TZ_PRESETS[i][1];
    bool sel = (!alreadySel) && (String(value)==String(cfg.tz));
    if(sel) alreadySel=true;
    opts+="<option value='"; opts+=value; opts+="'"; if(sel) opts+=" selected"; opts+=">"; opts+=label; opts+="</option>";
  }
  String h;
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Paramètres — PetFeeder</title><style>");
  h += commonHeaderCSS();
  h += F(".grid{display:grid;gap:12px}.g2{grid-template-columns:1fr 1fr}"
         "label{font-size:12px;color:var(--muted);display:block;margin-bottom:6px}"
         ".switchRow{display:flex;align-items:center;gap:8px}"
         ".switchRow input{appearance:none;width:40px;height:20px;background:#555;border-radius:10px;position:relative;cursor:pointer;outline:none}"
         ".switchRow input:before{content:'';position:absolute;width:18px;height:18px;top:1px;left:1px;background:#fff;border-radius:50%;transition:.2s}"
         ".switchRow input:checked{background:var(--pri)}"
         ".switchRow input:checked:before{transform:translateX(20px)}"
  );
  h += F("</style></head><body>");
  h += commonHeaderHTML("⚙ Paramètres", hostName);
  h += F("<main><form method='POST' action='/settings' class='grid'>");

  h += F("<section class='card grid g2'>"
         "<div><label>Nom de l'appareil</label><input name='device_name' value='"); h+=cfg.device_name;
  h += F("'></div><div class='adv' style='display:none'><label>Mot de passe OTA</label><input name='ota_pass' value='"); h+=cfg.ota_pass; h+=F("'></div>"
         "<div><label>Fuseau horaire (préréglages)</label><select name='tz_sel'>"); h+=opts; h+=F("</select></div>"
         "<div class='adv' style='display:none'><label>Fuseau horaire (avancé/POSIX)</label><input name='tz_adv' placeholder='ex: CET-1CEST,M3.5.0,M10.5.0/3'></div>"
         "<div><label>Quota/jour (rations, 0 = illimité)</label><input type='number' min='0' max='200' name='daily_quota' value='"); h+=String(cfg.daily_quota); h+=F("'></div>"
         "</section>");

  h += F("<section class='card grid g2 adv' style='display:none'>"
         "<div><label>Portion (pas / ration)</label><input type='number' min='10' max='20000' name='portion_steps' value='"); h+=String(cfg.portion_steps);
  h += F("'></div><div><label>Step µs (lent)</label><input type='number' min='100' max='5000' name='step_us_slow' value='"); h+=String(cfg.step_us_slow);
  h += F("'></div><div><label>Step µs (rapide)</label><input type='number' min='100' max='5000' name='step_us_fast' value='"); h+=String(cfg.step_us_fast);
  h += F("'></div><div><label>Rampe (pas)</label><input type='number' min='0' max='2000' name='ramp_steps' value='"); h+=String(cfg.ramp_steps);
  h += F("'></div><div><label><input type='checkbox' name='dir_invert' "); if(cfg.dir_invert) h+=F("checked"); h+=F("> Inversion sens</label></div>"
         "<div><label><input type='checkbox' name='hold_enable' "); if(cfg.hold_enable) h+=F("checked"); h+=F("> Maintien couple (EN LOW)</label></div>"
         "<div><label><input type='checkbox' name='safe_mode' "); if(cfg.safe_mode) h+=F("checked"); h+=F("> Mode Safe</label></div>"
         "</section>");

  h += F("<section class='card grid g2 adv' style='display:none'>"
         "<div><label><input type='checkbox' name='logs_enable' "); if(cfg.logs_enable) h+=F("checked"); h+=F("> Activer logs</label></div>"
         "<div><label>Niveau de logs</label><select name='log_level'>"
         "<option value='0' "); if(cfg.log_level==0) h+=F("selected"); h+=F(">Erreurs</option>"
         "<option value='1' "); if(cfg.log_level==1) h+=F("selected"); h+=F(">Infos</option>"
         "<option value='2' "); if(cfg.log_level==2) h+=F("selected"); h+=F(">Debug</option>"
         "</select></div>"
         "</section>");

  h += F("<section class='card grid g2 adv' style='display:none'>"
         "<div><label><input type='checkbox' name='mqtt_enable' "); if(cfg.mqtt_enable) h+=F("checked"); h+=F("> Activer MQTT</label></div>"
         "<div><label>Broker</label><input name='mqtt_host' value='"); h+=cfg.mqtt_host;
  h += F("'></div><div><label>Port</label><input type='number' name='mqtt_port' value='"); h+=String(cfg.mqtt_port);
  h += F("'></div><div><label>Topic</label><input name='mqtt_topic' value='"); h+=cfg.mqtt_topic;
  h += F("'></div><div><label>User</label><input name='mqtt_user' value='"); h+=cfg.mqtt_user;
  h += F("'></div><div><label>Password</label><input name='mqtt_pass' value='"); h+=cfg.mqtt_pass; h+=F("'></div>"
         "<div><label>Webhook URL</label><input name='webhook_url' value='"); h+=cfg.webhook_url; h+=F("'></div>"
         "</section>");

  h += F("<section class='card grid g2'>"
         "<div><a class='btn warn' href='/reboot'>⟲ Reboot</a></div>"
         "<div><a class='btn warn' href='/stats/clear'>🧹 Effacer historique 7j</a></div>"
         "<div><a class='btn danger' href='/factory'>🔄 Réinitialiser usine</a></div>"
         "<div class='switchRow'><label>Mode expert</label><input type='checkbox' id='expertToggle'></div>"
         "</section>");

  h += F("<section class='card'><div class='actions-grid'>"
         "<button type='submit' class='btn primary'>💾 Enregistrer</button>"
         "<a class='btn adv' style='display:none' href='/backup'>📤 Exporter config</a>"
         "<button type='button' id='btnImportCfg' class='btn adv' style='display:none'>📥 Importer config…</button>"
         "<input type='file' id='fres_settings' class='adv' style='display:none' accept='application/json'>"
         "</div></section>");

  h += F("</form>");

  h += F("<div class='toast' id='toast'></div>"
         "<script>"
         "document.addEventListener('DOMContentLoaded',()=>{"
         "const q=(s)=>document.querySelector(s); const root=document.documentElement;"
         "function applyTheme(t){if(t==='light'){root.classList.add('light');}else{root.classList.remove('light');} localStorage.setItem('pf_theme',t);} applyTheme(localStorage.getItem('pf_theme')||'dark');"
         "const themeBtn=document.querySelector('#theme'); if(themeBtn) themeBtn.onclick=()=>applyTheme(root.classList.contains('light')?'dark':'light');"
         "const expertToggle=q('#expertToggle'); const advEls=document.querySelectorAll('.adv');"
         "function applyExpert(on){advEls.forEach(e=>e.style.display=on?'':'none'); if(expertToggle) expertToggle.checked=on; localStorage.setItem('pf_expert',on?'1':'0');}"
         "applyExpert(localStorage.getItem('pf_expert')==='1'); if(expertToggle) expertToggle.onchange=()=>applyExpert(expertToggle.checked);"
         );
  h += F("const toastMsg='");
  h += toastMsg;
  h += F("'; if(toastMsg){const t=document.createElement('div');"
         "t.className='t'; t.textContent=toastMsg;"
         "q('#toast').appendChild(t); setTimeout(()=>t.remove(),2200); }"
         "q('#btnImportCfg').onclick=()=>q('#fres_settings').click();"
         "q('#fres_settings').onchange=async()=>{const f=q('#fres_settings').files[0]; if(!f) return; const txt=await f.text(); await fetch('/restore',{method:'POST',headers:{'Content-Type':'application/json'},body:txt}); location.href='/settings?saved=1';};"
         "function two(n){return (n<10?'0':'')+n;} function updateClock(){const e=document.querySelector('#nowFr'); if(!e)return; const d=new Date(); e.textContent=`${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())} ${two(d.getDate())}/${two(d.getMonth()+1)}/${d.getFullYear()}`;} setInterval(updateClock,1000); updateClock();"
         "fetch('/status').then(r=>r.json()).then(s=>{ const qTxt=document.querySelector('#quotaTxt'); const dTxt=document.querySelector('#distTxt'); const quotaNum = s.daily_quota||0; const dist=s.rations_today||0; const quotaStr = quotaNum===0? 'illimité' : `${dist}/${quotaNum}`; if(qTxt) qTxt.textContent=quotaStr; if(dTxt) dTxt.textContent=String(dist); }).catch(()=>{});"
         "});"
         "</script></main></body></html>");
  (void)saved; return h;
}

// ---------- HTML LOGS ----------
String htmlLogsPage(){
  String content; if(LittleFS.exists(LOG_PATH)){ File f=LittleFS.open(LOG_PATH,"r"); if(f){ content=f.readString(); f.close(); } }
  content.replace("&","&amp;"); content.replace("<","&lt;"); content.replace(">","&gt;");
  String h;
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Logs — PetFeeder</title><style>");
  h += commonHeaderCSS();
  h += F("pre{white-space:pre-wrap;word-wrap:break-word}");
  h += F("</style></head><body>");
  h += commonHeaderHTML("📝 Logs", hostName);
  h += F("<main><section class='card'><div class='row'><a href='/logs/download' class='btn'>📥 Télécharger</a><a href='/logs/clear' class='btn'>🧹 Effacer</a></div><pre>");
  h += content;
  h += F("</pre></section></main>"
         "<script>"
         "document.addEventListener('DOMContentLoaded',()=>{const root=document.documentElement;document.querySelectorAll('.adv').forEach(e=>e.style.display=localStorage.getItem('pf_expert')==='1'?'':'none');function applyTheme(t){if(t==='light'){root.classList.add('light');}else{root.classList.remove('light');} localStorage.setItem('pf_theme',t);} applyTheme(localStorage.getItem('pf_theme')||'dark');function two(n){return (n<10?'0':'')+n;} function updateClock(){const e=document.querySelector('#nowFr'); if(!e)return; const d=new Date(); e.textContent=`${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())} ${two(d.getDate())}/${two(d.getMonth()+1)}/${d.getFullYear()}`;} setInterval(updateClock,1000); updateClock(); const themeBtn=document.querySelector('#theme'); if(themeBtn) themeBtn.onclick=()=>applyTheme(root.classList.contains('light')?'dark':'light'); fetch('/status').then(r=>r.json()).then(s=>{ const qTxt=document.querySelector('#quotaTxt'); const dTxt=document.querySelector('#distTxt'); const quotaNum = s.daily_quota||0; const dist=s.rations_today||0; const quotaStr = quotaNum===0? 'illimité' : `${dist}/${quotaNum}`; if(qTxt) qTxt.textContent=quotaStr; if(dTxt) dTxt.textContent=String(dist); }).catch(()=>{});});"
         "</script></body></html>");
  return h;
}

// ---------- HTML UPDATE ----------
String htmlUpdatePage(const String& msg){
  String h;
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Update — PetFeeder</title><style>");
  h += commonHeaderCSS();
  h += F("input[type=file]{display:block;margin:10px 0;color:inherit}");
  h += F("</style></head><body>");
  h += commonHeaderHTML("⬆️ Mise à jour OTA", hostName);
  h += F("<main><section class='card'>"
         "<form method='POST' action='/update' enctype='multipart/form-data'>"
         "<label>Fichier firmware (.bin)</label><input type='file' name='firmware' accept='.bin' required>"
         "<button type='submit' class='btn primary'>Téléverser</button></form>");
  if(msg.length()){ h += "<p>"+msg+"</p>"; }
  h += F("</section></main>"
         "<script>"
         "document.addEventListener('DOMContentLoaded',()=>{const root=document.documentElement;document.querySelectorAll('.adv').forEach(e=>e.style.display=localStorage.getItem('pf_expert')==='1'?'':'none');function applyTheme(t){if(t==='light'){root.classList.add('light');}else{root.classList.remove('light');} localStorage.setItem('pf_theme',t);} applyTheme(localStorage.getItem('pf_theme')||'dark');function two(n){return (n<10?'0':'')+n;} function updateClock(){const e=document.querySelector('#nowFr'); if(!e)return; const d=new Date(); e.textContent=`${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())} ${two(d.getDate())}/${two(d.getMonth()+1)}/${d.getFullYear()}`;} setInterval(updateClock,1000); updateClock(); const themeBtn=document.querySelector('#theme'); if(themeBtn) themeBtn.onclick=()=>applyTheme(root.classList.contains('light')?'dark':'light'); fetch('/status').then(r=>r.json()).then(s=>{ const qTxt=document.querySelector('#quotaTxt'); const dTxt=document.querySelector('#distTxt'); const quotaNum = s.daily_quota||0; const dist=s.rations_today||0; const quotaStr = quotaNum===0? 'illimité' : `${dist}/${quotaNum}`; if(qTxt) qTxt.textContent=quotaStr; if(dTxt) dTxt.textContent=String(dist); }).catch(()=>{});});"
         "</script></body></html>");
  return h;
}

// ---------- HANDLERS ----------
void handleRoot(){ server.send(200,"text/html; charset=utf-8", htmlIndex(hostName)); }
void handleSettingsGet(){
  String msg="";
  if(server.hasArg("saved")) msg="Paramètres enregistrés";
  else if(server.hasArg("stats_cleared")) msg="Historique 7j supprimé";
  else if(server.hasArg("factory")) msg="Réinitialisation effectuée";
  server.send(200,"text/html; charset=utf-8", htmlSettingsPage(false,msg));
}

void handleReboot(){
  // Page avec compte à rebours et redirection automatique
  server.send(200,"text/html; charset=utf-8",
    "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{background:#0b0b0f;color:#e7e9ee;font-family:system-ui,-apple-system,Segoe UI,Roboto,Inter,sans-serif;display:grid;place-items:center;height:100vh;margin:0} .box{background:#15151c;border:1px solid #2b2e45;border-radius:16px;padding:24px;text-align:center;box-shadow:0 20px 60px rgba(0,0,0,.35)} .count{font-size:20px;margin-top:8px;color:#9aa3af}</style>"
    "<div class='box'><h2>Redémarrage… Patientez <span id='s'>10</span> secondes</h2><div class='count'>Retour automatique à l’accueil…</div></div>"
    "<script>let t=10; const s=document.getElementById('s'); const id=setInterval(()=>{t--; if(s) s.textContent=t; if(t<=0){clearInterval(id); location.href='/';}},1000);</script>"
  );
  delay(500);
  ESP.restart(); // le device va redémarrer pendant le compte à rebours; la redirection lancera "/" quand il sera revenu
}

void handleSettingsPost(){
  String oldName = String(cfg.device_name);
  String oldPass = String(cfg.ota_pass);
  String oldTz   = String(cfg.tz);

  if(server.hasArg("device_name")) { strlcpy(cfg.device_name, server.arg("device_name").c_str(), sizeof(cfg.device_name)); }
  if(server.hasArg("ota_pass") && server.arg("ota_pass").length()>0){
    strlcpy(cfg.ota_pass, server.arg("ota_pass").c_str(), sizeof(cfg.ota_pass));
  }

  if(server.hasArg("tz_adv") && server.arg("tz_adv").length()>0){
    strlcpy(cfg.tz, server.arg("tz_adv").c_str(), sizeof(cfg.tz));
  } else if(server.hasArg("tz_sel")){
    strlcpy(cfg.tz, server.arg("tz_sel").c_str(), sizeof(cfg.tz));
  }

  if(server.hasArg("portion_steps")) { long v=server.arg("portion_steps").toInt(); if(v<10)v=10; if(v>20000)v=20000; cfg.portion_steps=(uint16_t)v; }
  if(server.hasArg("step_us_slow"))  { long v=server.arg("step_us_slow").toInt();  if(v<100)v=100; if(v>5000)v=5000;   cfg.step_us_slow=(uint16_t)v; }
  if(server.hasArg("step_us_fast"))  { long v=server.arg("step_us_fast").toInt();  if(v<100)v=100; if(v>5000)v=5000;   cfg.step_us_fast=(uint16_t)v; }
  if(server.hasArg("ramp_steps"))    { long v=server.arg("ramp_steps").toInt();    if(v<0)v=0;    if(v>2000)v=2000;   cfg.ramp_steps=(uint16_t)v; }

  cfg.dir_invert  = server.hasArg("dir_invert");
  cfg.hold_enable = server.hasArg("hold_enable");
  cfg.safe_mode   = server.hasArg("safe_mode");

  if(server.hasArg("daily_quota"))   { long v=server.arg("daily_quota").toInt();   if(v<0)v=0;    if(v>200)v=200;     cfg.daily_quota=(uint8_t)v; }

  cfg.logs_enable = server.hasArg("logs_enable");
  if(server.hasArg("log_level"))     { long v=server.arg("log_level").toInt();     if(v<0)v=0;    if(v>2)v=2;         cfg.log_level=(uint8_t)v; }

  cfg.mqtt_enable = server.hasArg("mqtt_enable");
  if(server.hasArg("mqtt_host"))   strlcpy(cfg.mqtt_host,  server.arg("mqtt_host").c_str(),  sizeof(cfg.mqtt_host));
  if(server.hasArg("mqtt_port"))   { long v=server.arg("mqtt_port").toInt(); if(v<1)v=1; if(v>65535)v=65535; cfg.mqtt_port=(uint16_t)v; }
  if(server.hasArg("mqtt_topic"))  strlcpy(cfg.mqtt_topic, server.arg("mqtt_topic").c_str(), sizeof(cfg.mqtt_topic));
  if(server.hasArg("mqtt_user"))   strlcpy(cfg.mqtt_user,  server.arg("mqtt_user").c_str(),  sizeof(cfg.mqtt_user));
  if(server.hasArg("mqtt_pass"))   strlcpy(cfg.mqtt_pass,  server.arg("mqtt_pass").c_str(),  sizeof(cfg.mqtt_pass));
  if(server.hasArg("webhook_url")) strlcpy(cfg.webhook_url,server.arg("webhook_url").c_str(),sizeof(cfg.webhook_url));

  saveConfig(); logEvent(LOG_INFO,"SETTINGS","saved");

  // Redémarrer si hostname / pass / TZ changent
  if(oldName != String(cfg.device_name) || oldPass != String(cfg.ota_pass) || oldTz != String(cfg.tz)){
    handleReboot();
    return;
  }

  server.send(200,"text/html; charset=utf-8", htmlSettingsPage(true,"Paramètres enregistrés"));
}

void handleLogs(){ server.send(200,"text/html; charset=utf-8", htmlLogsPage()); }
void handleLogsDownload(){ if(!LittleFS.exists(LOG_PATH)){ server.send(200,"text/plain",""); return; } File f=LittleFS.open(LOG_PATH,"r"); server.streamFile(f,"text/plain"); f.close(); }
void handleLogsClear(){ LittleFS.remove(LOG_PATH); server.sendHeader("Location","/logs"); server.send(302,"text/plain",""); }

void handleStatsClear(){ LittleFS.remove(STATS_PATH); statsLoad(); statsEnsureToday(); server.sendHeader("Location","/settings?stats_cleared=1"); server.send(302,"text/plain",""); }

void handleFactory(){ LittleFS.remove(CFG_PATH); LittleFS.remove(SCHED_PATH); LittleFS.remove(STATS_PATH); loadConfig(); scheduleLoad(); statsLoad(); statsEnsureToday(); server.sendHeader("Location","/settings?factory=1"); server.send(302,"text/plain",""); }

void handleStatus(){
  StaticJsonDocument<4096> doc;
  time_t now=time(nullptr); struct tm* tmNow=localtime(&now);
  uint8_t todayDOW = tmNow ? dowFromTm(tmNow->tm_wday) : 0;
  uint16_t yday = tmNow ? (uint16_t)tmNow->tm_yday : 0xFFFF;

  doc["epoch"] = (uint32_t)now;
  doc["host"] = hostName;
  doc["device_name"]=cfg.device_name;
  doc["daily_quota"]=cfg.daily_quota;
  doc["rations_today"]=rationsToday;
  doc["portion_steps"]=cfg.portion_steps; doc["step_us_slow"]=cfg.step_us_slow; doc["step_us_fast"]=cfg.step_us_fast; doc["ramp_steps"]=cfg.ramp_steps;
  doc["dir_invert"]=cfg.dir_invert; doc["hold_enable"]=cfg.hold_enable; doc["safe_mode"]=cfg.safe_mode;

  JsonArray arr=doc.createNestedArray("events");
  for(uint8_t i=0;i<scheduleCount;i++){
    uint16_t sm=(uint16_t)(scheduleList[i].hour*60+scheduleList[i].minute);
    bool doneToday=(lastRunYDay[i]==yday && lastRunMinute[i]==sm);
    bool activeToday=isDayActive(scheduleList[i].daysMask,todayDOW);
    JsonObject o=arr.createNestedObject();
    o["id"]=i;
    o["hour"]=scheduleList[i].hour; o["minute"]=scheduleList[i].minute; o["rations"]=scheduleList[i].rations;
    o["days"]=scheduleList[i].daysMask; o["done_today"]=doneToday; o["active_today"]=activeToday;
  }
  JsonArray s = doc.createNestedArray("stats");
  for(uint8_t i=0;i<statCount;i++){ JsonObject o=s.createNestedObject(); o["d"]=last7[i].date; o["r"]=last7[i].r; }

  String out; serializeJson(doc,out); server.send(200,"application/json",out);
}

// API planning
void handleApiScheduleGet(){ StaticJsonDocument<4096> d; JsonArray a=d.createNestedArray("events"); for(uint8_t i=0;i<scheduleCount;i++){ JsonObject o=a.createNestedObject(); o["hour"]=scheduleList[i].hour; o["minute"]=scheduleList[i].minute; o["rations"]=scheduleList[i].rations; o["days"]=scheduleList[i].daysMask; } String out; serializeJson(d,out); server.send(200,"application/json",out); }
void handleApiScheduleAdd(){
  if(!(server.hasArg("hour")&&server.hasArg("minute")&&server.hasArg("rations"))){ server.send(400,"text/plain","Missing params"); return; }
  long hh=server.arg("hour").toInt(); long mm=server.arg("minute").toInt(); long rr=server.arg("rations").toInt();
  long dd= server.hasArg("days") ? server.arg("days").toInt() : 0x7F;
  if(hh<0||hh>23||mm<0||mm>59||rr<1||rr>20||dd<1||dd>127){ server.send(400,"text/plain","Bad values"); return; }
  scheduleAdd((uint8_t)hh,(uint8_t)mm,(uint8_t)rr,(uint8_t)dd); logEvent(LOG_INFO,"SCHEDULE","add");
  handleApiScheduleGet();
}
void handleApiScheduleUpdate(){
  if(!(server.hasArg("id")&&server.hasArg("hour")&&server.hasArg("minute")&&server.hasArg("rations"))){ server.send(400,"text/plain","Missing params"); return; }
  long id=server.arg("id").toInt(); long hh=server.arg("hour").toInt(); long mm=server.arg("minute").toInt(); long rr=server.arg("rations").toInt(); long dd=server.hasArg("days")?server.arg("days").toInt():0x7F;
  if(id<0||id>=scheduleCount||hh<0||hh>23||mm<0||mm>59||rr<1||rr>20||dd<1||dd>127){ server.send(400,"text/plain","Bad values"); return; }
  scheduleList[id].hour=(uint8_t)hh; scheduleList[id].minute=(uint8_t)mm; scheduleList[id].rations=(uint8_t)rr; scheduleList[id].daysMask=(uint8_t)dd;
  scheduleSort(); scheduleSave(); logEvent(LOG_INFO,"SCHEDULE","update"); handleApiScheduleGet();
}
void handleApiScheduleDelete(){ if(!server.hasArg("id")){ server.send(400,"text/plain","Missing id"); return; } long id=server.arg("id").toInt(); if(id==-1){ scheduleCount=0; scheduleSave(); server.send(200,"application/json","{\"ok\":true}"); logEvent(LOG_INFO,"SCHEDULE","clear"); return; } if(id<0||id>=scheduleCount){ server.send(400,"text/plain","Bad id"); return; } scheduleDelete((uint8_t)id); logEvent(LOG_INFO,"SCHEDULE","delete"); server.send(200,"application/json","{\"ok\":true}"); }
void handleApiScheduleDuplicate(){ if(!server.hasArg("id")){ server.send(400,"text/plain","Missing id"); return; } long id=server.arg("id").toInt(); if(id<0||id>=scheduleCount){ server.send(400,"text/plain","Bad id"); return; } uint8_t newIdx=255; if(!scheduleDuplicate((uint8_t)id,newIdx)){ server.send(500,"text/plain","Duplicate failed"); return; } StaticJsonDocument<128> d; d["ok"]=true; d["new"]=newIdx; String out; serializeJson(d,out); server.send(200,"application/json",out); logEvent(LOG_INFO,"SCHEDULE","duplicate"); }

// Actions
void handleFeed(){ uint8_t n=1; if(server.hasArg("n")){ long v=server.arg("n").toInt(); if(v<1)v=1; if(v>20)v=20; n=(uint8_t)v; } if(cfg.safe_mode){ server.send(200,"application/json","{\"ok\":false,\"message\":\"Mode Safe actif\"}"); return; } if(!quotaAllow(n)){ server.send(200,"application/json","{\"ok\":false,\"message\":\"Quota atteint\"}"); return; } feedRations(n); server.send(200,"application/json","{\"ok\":true}"); }
void handleUnclog(){ if(cfg.safe_mode){ server.send(200,"application/json","{\"ok\":false,\"message\":\"Mode Safe actif\"}"); return; } unclog(); server.send(200,"application/json","{\"ok\":true}"); }
void handleFeedNext(){
  if(cfg.safe_mode){ server.send(200,"application/json","{\"ok\":false,\"message\":\"Mode Safe actif\"}"); return; }
  time_t now=time(nullptr); struct tm* tmNow=localtime(&now);
  if(!tmNow){ server.send(503,"application/json","{\"ok\":false,\"message\":\"Heure indisponible\"}"); return; }
  uint16_t yday=(uint16_t)tmNow->tm_yday; uint16_t curMin=(uint16_t)(tmNow->tm_hour*60+tmNow->tm_min);
  uint8_t dow=dowFromTm(tmNow->tm_wday); int chosen=-1;
  for(uint8_t i=0;i<scheduleCount;i++){ if(!isDayActive(scheduleList[i].daysMask,dow)) continue; uint16_t sm=(uint16_t)(scheduleList[i].hour*60+scheduleList[i].minute); bool done=(lastRunYDay[i]==yday && lastRunMinute[i]==sm); if(sm>=curMin && !done){ chosen=(int)i; break; } }
  if(chosen<0){ server.send(200,"application/json","{\"ok\":false,\"message\":\"Aucune distribution restante aujourd'hui\"}"); return; }
  uint8_t r=scheduleList[chosen].rations; if(!quotaAllow(r)){ server.send(200,"application/json","{\"ok\":false,\"message\":\"Quota atteint\"}"); return; }
  feedRations(r); uint16_t sm=(uint16_t)(scheduleList[chosen].hour*60+scheduleList[chosen].minute); lastRunYDay[chosen]=yday; lastRunMinute[chosen]=sm;
  StaticJsonDocument<128> d; d["ok"]=true; d["hour"]=scheduleList[chosen].hour; d["minute"]=scheduleList[chosen].minute; d["rations"]=r; String out; serializeJson(d,out); server.send(200,"application/json",out);
}
void handleFeedIn(){ if(cfg.safe_mode){ server.send(200,"application/json","{\"ok\":false}"); return; } long m=server.hasArg("min")?server.arg("min").toInt():5; if(m<0)m=0; if(m>600)m=600; long rn=server.hasArg("n")?server.arg("n").toInt():1; if(rn<1)rn=1; if(rn>20)rn=20; time_t now=time(nullptr); struct tm* tmNow=localtime(&now); int h=tmNow?tmNow->tm_hour:0; int mi=tmNow?tmNow->tm_min:0; int tot=mi+(int)m; h+=tot/60; mi=tot%60; if(h>23){ server.send(200,"application/json","{\"ok\":false,\"message\":\"aujourd'hui terminé\"}"); return; } uint8_t today=dowFromTm(tmNow->tm_wday); scheduleAdd((uint8_t)h,(uint8_t)mi,(uint8_t)rn,(uint8_t)(1<<today)); server.send(200,"application/json","{\"ok\":true}"); }

// Backup / Restore
void handleBackup(){
  StaticJsonDocument<8192> d;
  File fc=LittleFS.open(CFG_PATH,"r"); if(fc){ StaticJsonDocument<4096> c; if(!deserializeJson(c,fc)) d["config"]=c.as<JsonVariant>(); fc.close(); }
  File fs=LittleFS.open(SCHED_PATH,"r"); if(fs){ StaticJsonDocument<4096> s; if(!deserializeJson(s,fs)) d["schedule"]=s.as<JsonVariant>(); fs.close(); }
  File ft=LittleFS.open(STATS_PATH,"r"); if(ft){ StaticJsonDocument<2048> s; if(!deserializeJson(s,ft)) d["stats"]=s.as<JsonVariant>(); ft.close(); }
  String out; serializeJson(d,out); server.sendHeader("Content-Disposition","attachment; filename=petfeeder_backup.json"); server.send(200,"application/json",out);
}
void handleRestore(){ if(!server.hasArg("plain")){ server.send(400,"text/plain","Missing body"); return; } StaticJsonDocument<8192> d; DeserializationError e=deserializeJson(d,server.arg("plain")); if(e){ server.send(400,"text/plain","Bad JSON"); return; } if(d.containsKey("config")){ File f=LittleFS.open(CFG_PATH,"w"); if(f){ serializeJson(d["config"],f); f.close(); loadConfig(); } } if(d.containsKey("schedule")){ File f=LittleFS.open(SCHED_PATH,"w"); if(f){ serializeJson(d["schedule"],f); f.close(); scheduleLoad(); } } server.sendHeader("Location","/settings?saved=1"); server.send(302,"text/plain",""); }

// Update
void handleUpdateGet(){ server.send(200,"text/html; charset=utf-8", htmlUpdatePage("")); }
void handleUpdatePost(){
  HTTPUpload& up = server.upload();
  if(up.status==UPLOAD_FILE_START){
    logEvent(LOG_INFO,"UPDATE","start "+String(up.filename));
    size_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if(!Update.begin(maxSketchSpace, U_FLASH)) { logEvent(LOG_ERR,"UPDATE","begin failed"); }
  } else if(up.status==UPLOAD_FILE_WRITE){
    Update.write(up.buf, up.currentSize);
  } else if(up.status==UPLOAD_FILE_END){
    if(Update.end(true)){ logEvent(LOG_INFO,"UPDATE","success size="+String(up.totalSize)); }
    else { logEvent(LOG_ERR,"UPDATE",String("failed: ")+Update.getError()); }
  }
  if(server.method()==HTTP_POST && !server.hasArg("plain")){
    String m = Update.hasError() ? "Échec de la mise à jour" : "Mise à jour OK, redémarrage…";
    server.send(200,"text/html; charset=utf-8", htmlUpdatePage(m));
    delay(1000); if(!Update.hasError()) ESP.restart();
  }
}

// Manifest / SW / Diag
void handleManifest(){ String m=F("{\"name\":\"PetFeeder\",\"short_name\":\"PetFeeder\",\"start_url\":\"/\",\"display\":\"standalone\",\"background_color\":\"#0b0b0f\",\"theme_color\":\"#6366f1\",\"icons\":[]}"); server.send(200,"application/json",m); }
void handleSW(){ String s=F("self.addEventListener('install',e=>self.skipWaiting());self.addEventListener('activate',e=>clients.claim());self.addEventListener('fetch',e=>{});"); server.send(200,"application/javascript",s); }
void handleDiag(){ StaticJsonDocument<256> d; d["flash_real"]=ESP.getFlashChipRealSize(); d["flash_ide"]=ESP.getFlashChipSize(); d["sketch_size"]=ESP.getSketchSize(); d["free_sketch"]=ESP.getFreeSketchSpace(); String out; serializeJson(d,out); server.send(200,"application/json",out); }

// ---------- TIMEZONE APPLY ----------
void applyTZ(){ setenv("TZ", cfg.tz, 1); tzset(); }

// ---------- BUTTON ISR ----------
void IRAM_ATTR btnISR(){ uint32_t now=millis(); if(now - btnLastChange < 40) return; bool s=digitalRead(PIN_BTN); if(s==LOW && btnPrevState==HIGH){ } else if(s==HIGH && btnPrevState==LOW){ if(btnWindowStart==0) btnWindowStart=now; btnClickCount++; } btnPrevState=s; btnLastChange=now; }

// ---------- SETUP / LOOP ----------
void setup(){
  pinMode(PIN_DIR,OUTPUT); pinMode(PIN_STEP,OUTPUT); pinMode(PIN_EN,OUTPUT); pinMode(PIN_BTN,INPUT_PULLUP);
  driverEnable(cfg.hold_enable); pinMode(LED_BUILTIN, OUTPUT);

  LittleFS.begin(); loadConfig(); scheduleLoad(); statsLoad(); statsEnsureToday();

  WiFi.mode(WIFI_STA);
  String mac4=macLast4(); mac4.toCharArray(macSuffix,sizeof(macSuffix));
  String base = cfg.device_name; base.toLowerCase(); for(size_t i=0;i<base.length();++i){ char c=base[i]; if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-')) base.setCharAt(i,'-');}
  if(base.length()==0) base="petfeeder";
  if(base.length()>24) base.remove(24);
  hostName = base + "-" + mac4;
  WiFi.hostname(hostName);

  WiFiManager wm; wm.setClass("invert"); wm.setHostname(hostName);
  String apName = base + "-SETUP";
  if(!wm.autoConnect(apName.c_str())){ ESP.restart(); }

  configTime(0,0,"pool.ntp.org","time.nist.gov"); applyTZ();

  ArduinoOTA.setHostname(hostName.c_str()); ArduinoOTA.setPassword(cfg.ota_pass);
  ArduinoOTA.onStart([](){ logEvent(LOG_INFO,"OTA","start"); });
  ArduinoOTA.onEnd([](){ logEvent(LOG_INFO,"OTA","end"); });
  ArduinoOTA.onError([](ota_error_t e){ logEvent(LOG_ERR,"OTA","err "+String((int)e)); });
  ArduinoOTA.begin();

  if(MDNS.begin(hostName.c_str())) MDNS.addService("http","tcp",80);

  mqtt.setServer(cfg.mqtt_host,cfg.mqtt_port); mqtt.setCallback(mqttCallback);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/reboot", HTTP_GET, handleReboot);
    server.on("/logs", HTTP_GET, handleLogs);
    server.on("/logs/download", HTTP_GET, handleLogsDownload);
    server.on("/logs/clear", HTTP_GET, handleLogsClear);
    server.on("/stats/clear", HTTP_GET, handleStatsClear);
    server.on("/factory", HTTP_GET, handleFactory);

    server.on("/status", HTTP_GET, handleStatus);
  server.on("/api/schedule", HTTP_GET, handleApiScheduleGet);
  server.on("/api/schedule", HTTP_POST, handleApiScheduleAdd);
  server.on("/api/schedule/update", HTTP_POST, handleApiScheduleUpdate);
  server.on("/api/schedule/delete", HTTP_GET, handleApiScheduleDelete);
  server.on("/api/schedule/duplicate", HTTP_GET, handleApiScheduleDuplicate);

  server.on("/feed", HTTP_GET, handleFeed);
  server.on("/unclog", HTTP_GET, handleUnclog);
  server.on("/feed_next", HTTP_GET, handleFeedNext);
  server.on("/feed_in", HTTP_GET, handleFeedIn);

  server.on("/backup", HTTP_GET, handleBackup);
  server.on("/restore", HTTP_POST, handleRestore);

  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, [](){}, handleUpdatePost);

  server.on("/manifest.json", HTTP_GET, handleManifest);
  server.on("/sw.js", HTTP_GET, handleSW);
  server.on("/diag", HTTP_GET, handleDiag);

  server.begin();
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), btnISR, CHANGE);

  logEvent(LOG_INFO,"BOOT","host="+hostName+" v4.0");
}

void loop(){
  ArduinoOTA.handle();
  server.handleClient();
  if(cfg.mqtt_enable){ if(!mqtt.connected()) mqttReconnect(); mqtt.loop(); }

  time_t now=time(nullptr); struct tm* tmNow=localtime(&now);
  if(tmNow){
    if(quotaYDayRef != (uint16_t)tmNow->tm_yday){ quotaYDayRef=(uint16_t)tmNow->tm_yday; rationsToday=0; statsEnsureToday(); }
    uint8_t dow=dowFromTm(tmNow->tm_wday);
    for(uint8_t i=0;i<scheduleCount;i++){
      if(!isDayActive(scheduleList[i].daysMask,dow)) continue;
      if(scheduleList[i].hour==tmNow->tm_hour && scheduleList[i].minute==tmNow->tm_min){
        uint16_t m=(uint16_t)(scheduleList[i].hour*60+scheduleList[i].minute);
        if(!(lastRunYDay[i]==quotaYDayRef && lastRunMinute[i]==m)){
          uint8_t r=scheduleList[i].rations;
          if(!cfg.safe_mode && quotaAllow(r)){ logEvent(LOG_INFO,"SCHEDULE",String("run ")+scheduleList[i].hour+":"+scheduleList[i].minute+" x"+r); webhookEvent("schedule", String("run ")+scheduleList[i].hour+":"+scheduleList[i].minute+" x"+r); feedRations(r); }
          else { logEvent(LOG_INFO,"SCHEDULE","blocked (SAFE/QUOTA)"); }
          lastRunYDay[i]=quotaYDayRef; lastRunMinute[i]=m;
        }
      }
    }
  }

  if(btnWindowStart && (millis()-btnWindowStart > 700)){
    uint8_t c=btnClickCount; btnClickCount=0; btnWindowStart=0;
    if(c==1){ if(!cfg.safe_mode && quotaAllow(1)) feedRations(1); }
    else if(c==3){ if(!cfg.safe_mode) unclog(); }
    else if(c==5){ cfg.safe_mode=!cfg.safe_mode; saveConfig(); }
  }
}
