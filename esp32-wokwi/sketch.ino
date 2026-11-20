/*
 * EcoBio-Matrix · دماغ الوحدة (ESP32) · المرحلة 2
 * ------------------------------------------------------------------
 * - يقرأ الحساسات (كل ثانية في وضع العرض، كل دقيقة في الواقع)، وكل قراءة = وسيط 10 عينات.
 * - يطبّق قواعد الحماية السبع محليًا داخل ESP32 (تشتغل حتى لو انقطع الإنترنت).
 * - يتحكم بالصمامين ومضخة الهواء ومضخة الكلور عن طريق مرحّلات (Relays).
 * - يرسل كل قراءة لجدول sensor_readings وكل حدث لجدول events في Supabase (REST + مفتاح anon).
 * - لو انقطع الإنترنت: يحفظ القراءات والأحداث في ذاكرة الجهاز ويرسلها بعدين بوقتها الأصلي.
 *
 * مجرّب على محاكي Wokwi فقط، مو على قطع حقيقية.
 * معادلات التحويل تحت خطية للعرض على المحاكي. على القطع الحقيقية تُستبدل بمعايرة لكل حساس.
 *
 * المكتبات: OneWire و DallasTemperature (موجودة في libraries.txt).
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <stdarg.h>

// ============================== 1) الإعدادات ==============================
#define DEMO_MODE 1  // 1 = وضع العرض على Wokwi: قراءة كل ثانية، وانتظار 10 ثواني قبل الرجوع
                     // 0 = الواقع: قراءة كل دقيقة

const char* WIFI_SSID = "Wokwi-GUEST";  // شبكة المحاكي (مفتوحة بدون كلمة مرور)
const char* WIFI_PASS = "";

// من المرحلة 3 (Supabase → زر Connect، أو Settings → API Keys). يقبل المفتاح الجديد sb_publishable_...
// أو المفتاح القديم anon (يبدأ بـ eyJ). الاثنين عامّين، وصلاحيتهم إضافة وقراءة بس (RLS).
const char* SUPABASE_URL      = "https://vnwptdfnleongvkbiydc.supabase.co";
const char* SUPABASE_ANON_KEY = "sb_publishable_A-NSuIONi17ERzZtFdh3Yg_eM0yMuHV";

#if DEMO_MODE
const uint32_t READ_PERIOD_MS = 1000;    // قراءة كل ثانية (وضع العرض)
const uint32_t RETURN_WAIT_MS = 10000;   // قاعدة 7: الرجوع بعد 10 ثواني (التصميم المعتمد للعرض)
const int      WIFI_CHANNEL   = 6;       // قناة Wokwi-GUEST، تختصر وقت الاتصال في المحاكي
#else
const uint32_t READ_PERIOD_MS = 60000;   // قراءة كل دقيقة (التصميم المعتمد)
const uint32_t RETURN_WAIT_MS = 600000;  // 10 دقائق، تقديري (افتراض، يتحدد في خطة الإثبات)
const int      WIFI_CHANNEL   = 0;       // 0 = يدوّر على القناة تلقائيًا
#endif

// ============================== 2) حدود قواعد الحماية ==============================
const float PH_MIN = 6.5, PH_MAX = 8.4;          // الدليل الإرشادي للمياه الرمادية 1429هـ
const float TURB_MAX_NTU = 5.0;                   // معايير المياه المعالجة 2025: عكارة ≤ 5 NTU
const float UV_MIN_PCT = 70.0;                    // التصميم المعتمد: شدة UV أقل من 70% = خطر
const float CL_MIN = 0.2, CL_MAX = 0.5;           // كلور متبقي بين 0.2 و0.5 ملغ/لتر (دليل الوزارة)
const float LEVEL_MAX_PCT = 90.0;                 // التصميم المعتمد: التخزين فوق 90%
const float ORP1_JUMP_MV = 150.0;                 // تقديري (افتراض للعرض): «ارتفاع مفاجئ» = +150 mV فوق المعتاد
const float CL_TARGET = (CL_MIN + CL_MAX) / 2.0;  // مضخة الكلور تشتغل لما الكلور تحت منتصف المدى (0.35)
const int   ADC_RAIL_LOW = 10, ADC_RAIL_HIGH = 4085;  // تقديري: قراءة على طرف المدى = سلك مفصول أو قصر
const int   STUCK_N = 30;                         // تقديري: 30 قراءة متطابقة بالضبط = حساس عالق (في الواقع بس)
const bool  STUCK_CHECK = !DEMO_MODE;             // في المحاكي المقاومات ثابتة بدون تشويش، فهالفحص يشتغل في الواقع بس

// ============================== 3) معادلات التحويل ==============================
// ADC في ESP32 دقته 12 بت: قراءة خام من 0 إلى 4095 تقابل 0 إلى 3.3 فولت. x = الخام ÷ 4095
//   pH         = 14 × x                  (0 إلى 14)
//   ORP1 (mV)  = −1000 + 2000 × x        (−1000 إلى +1000)
//   DO (mg/L)  = 20 × x                  (0 إلى 20)
//   العكارة    = 20 × x NTU              (0 إلى 20)
//   UV (%)     = 100 × x                 (0 إلى 100)
//   ORP2 (mV)  = 400 + 600 × x           (400 إلى 1000)
// الكلور المتبقي من ORP2 (خط بين نقطتين): 650 mV ↔ 0.2 و 750 mV ↔ 0.5 ملغ/لتر
//   الكلور = 0.2 + (ORP2 − 650) × 0.003   ← تقديري: افتراض للعرض، ويتغير مع pH. لازم معايرة بعدّة DPD.
// المسافة (سم) = زمن الصدى (µs) ÷ 58      ← من ورقة بيانات HC-SR04
// مستوى التخزين (%) = 100 × (160 − المسافة) ÷ 160   ← عمق التخزين 1.6 م (الأبعاد المعتمدة)، والحساس في السقف
const float ORP2_CAL_MV1 = 650, ORP2_CAL_CL1 = 0.2;  // تقديري (افتراض للعرض)
const float ORP2_CAL_MV2 = 750, ORP2_CAL_CL2 = 0.5;  // تقديري (افتراض للعرض)
const float TANK_DEPTH_CM = 160.0;

// ============================== 4) التوصيلات ==============================
// المقاومات المتغيرة على ADC1 بس، لأن ADC2 ما يشتغل والـ WiFi شغّال.
const int PIN_PH = 36, PIN_ORP1 = 39, PIN_DO = 34, PIN_TURB = 35, PIN_UV = 32, PIN_ORP2 = 33;
const int PIN_ONEWIRE = 25;                 // DS18B20 (حرارة المفاعل)
const int PIN_TRIG = 26, PIN_ECHO = 27;     // HC-SR04 (مستوى التخزين)
const int PIN_VALVE1 = 21, PIN_VALVE2 = 19, PIN_AIR = 18, PIN_CL = 17;  // المرحّلات
const int PIN_BTN_FAULT = 23;               // زر «عطل حساس»
const int PIN_BTN_NET = 22;                 // زر «قطع الإنترنت» (لعرض قاعدة 6)

// المرحّل (Active-HIGH، مثل مرحّل Wokwi الافتراضي): IN = HIGH → يشتغل (COM↔NO = لمبة خضراء).
// IN = LOW أو مفصول → يطفي (COM↔NC = لمبة حمراء). يعني لو انقطعت الكهرباء أو علّق الجهاز،
// المرحّل يطفي والصمام يرجع للصرف تلقائيًا (Fail-safe).
// لو مرحّلك الحقيقي Active-LOW، اقلب القيمتين.
const uint8_t RELAY_ON = HIGH, RELAY_OFF = LOW;

// ============================== 5) الحالة ==============================
struct Channel {
  const char* en;      // الاسم في رسائل Serial
  const char* ar;      // الاسم في سجل الأحداث
  int pin;
  float lo, hi;        // القيمة = lo + (hi − lo) × x
  int raw, lastRaw, sameCount;
  float value;         // NAN لو الحساس معطل
  bool fault;          // true = الحساس معطل (قاعدة 3)
  const char* faultEn;
  const char* faultAr;
};
enum { I_PH, I_ORP1, I_DO, I_TURB, I_UV, I_ORP2, N_CH };
Channel CH[N_CH] = {
  {"pH",   "pH",           PIN_PH,   0,     14},
  {"ORP1", "ORP1",         PIN_ORP1, -1000, 1000},
  {"DO",   "الأكسجين DO",  PIN_DO,   0,     20},
  {"Turb", "العكارة",      PIN_TURB, 0,     20},
  {"UV",   "شدة UV",       PIN_UV,   0,     100},
  {"ORP2", "ORP2",         PIN_ORP2, 400,   1000},
};

enum Cause : uint16_t {
  C_BOOT = 1, C_PH = 2, C_ORP1 = 4, C_TURB = 8, C_UV = 16, C_CL = 32, C_LEVEL = 64, C_FAULT = 128
};
struct Valve {
  const char* id;      // valve1 / valve2
  const char* en;
  int pin;
  bool sewer;          // true = محوّل للصرف (لمبة حمراء)
  uint16_t causes;
  uint32_t clearSince; // متى صارت كل الأسباب سليمة (0 = لسا فيه سبب)
};
Valve V1 = {"valve1", "Valve 1", PIN_VALVE1, true, C_BOOT, 0};
Valve V2 = {"valve2", "Valve 2", PIN_VALVE2, true, C_BOOT, 0};

struct Reading {
  uint32_t seq, ms;
  float ph, orp1, dox, temp, turb, uv, orp2, level;
  bool v1Sewer, v2Sewer, air, cl, safe;
};
struct EventRec {
  uint32_t seq, ms;
  char type[16];
  char reason[240];
  char valve[8];
  float value;
};

// مخزن دائري: لو امتلى يمسح الأقدم.
template <typename T, int N>
struct Ring {
  T buf[N];
  int head = 0, count = 0;
  uint32_t dropped = 0;
  void push(const T& x) {
    if (count == N) { head = (head + 1) % N; count--; dropped++; }
    buf[(head + count) % N] = x;
    count++;
  }
  const T& at(int i) const { return buf[(head + i) % N]; }
  void popThrough(uint32_t seq) {
    while (count > 0 && buf[head].seq <= seq) { head = (head + 1) % N; count--; }
  }
};
const int READ_BUF = 600;   // في العرض قرابة 10 دقائق، وفي الواقع قرابة 10 ساعات
const int EVENT_BUF = 100;
Ring<Reading, READ_BUF> readQ;
Ring<EventRec, EVENT_BUF> eventQ;
uint32_t readSeq = 0, eventSeq = 0;

SemaphoreHandle_t qLock, logLock;
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature ds(&oneWire);

float tempC = NAN, levelPct = NAN, distCm = NAN, clMgl = NAN;
int tempSame = 0; float tempLast = NAN;
bool tempFault = false, levelFault = false;
const char* tempWhyEn = ""; const char* tempWhyAr = "";
const char* levelWhyEn = ""; const char* levelWhyAr = "";

bool safeMode = false, airOn = true, clOn = false;
bool bootPending = true;

// خط الأساس لـ ORP1: وسيط آخر 10 قراءات (يتجمد وقت الإنذار)
float orpHist[10]; int orpCount = 0, orpIdx = 0;
bool orp1Alarm = false; float orp1Base = NAN;

volatile bool btnFaultEvt = false, btnNetEvt = false;
volatile uint32_t btnFaultT = 0, btnNetT = 0;
bool faultInjected = false;      // زر «عطل حساس»: كأن سلك حساس pH انفصل
volatile bool netCut = false;    // زر «قطع الإنترنت»

volatile bool wifiUp = false, timeOk = false;
volatile uint32_t sentReadings = 0, sentEvents = 0;
volatile int lastHttp = 0;
bool supabaseReady = false;

// ============================== 6) أدوات صغيرة ==============================
void say(const char* fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  xSemaphoreTake(logLock, portMAX_DELAY);
  Serial.println(b);
  xSemaphoreGive(logLock);
}

// يشيل أي حرف UTF-8 مقصوص (لو انقص النص)، عشان Supabase ما يرفض الطلب
void utf8Clean(char* s) {
  char* w = s;
  const uint8_t* r = (const uint8_t*)s;
  while (*r) {
    uint8_t c = *r;
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    bool ok = len > 0;
    for (int i = 1; ok && i < len; i++) if ((r[i] & 0xC0) != 0x80) ok = false;
    if (ok) { for (int i = 0; i < len; i++) *w++ = r[i]; r += len; }
    else r++;
  }
  *w = 0;
}

void addEvent(const char* type, const char* reasonAr, const char* valve, float value) {
  if (!supabaseReady) return;   // بدون Supabase الأحداث تنطبع في Serial بس
  EventRec e;
  memset(&e, 0, sizeof(e));
  e.ms = millis();
  strncpy(e.type, type, sizeof(e.type) - 1);
  strncpy(e.reason, reasonAr, sizeof(e.reason) - 1);
  utf8Clean(e.reason);
  if (valve) strncpy(e.valve, valve, sizeof(e.valve) - 1);
  e.value = value;
  xSemaphoreTake(qLock, portMAX_DELAY);
  e.seq = ++eventSeq;
  eventQ.push(e);
  xSemaphoreGive(qLock);
}

void sortInts(int* a, int n) {
  for (int i = 1; i < n; i++) {
    int v = a[i], j = i - 1;
    while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
}
void sortFloats(float* a, int n) {
  for (int i = 1; i < n; i++) {
    float v = a[i]; int j = i - 1;
    while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
}
float medianFloats(const float* src, int n) {
  float t[10];
  for (int i = 0; i < n; i++) t[i] = src[i];
  sortFloats(t, n);
  return (n % 2) ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2.0;
}

// وسيط 10 عينات من مدخل تناظري
int medianRaw(int pin) {
  int s[10];
  for (int i = 0; i < 10; i++) { s[i] = analogRead(pin); delay(2); }
  sortInts(s, 10);
  return (s[4] + s[5]) / 2;
}

const char* onOff(bool b) { return b ? "ON" : "OFF"; }
const char* route(bool sewer) { return sewer ? "SEWER" : "NORMAL"; }

// ============================== 7) قراءة الحساسات ==============================
void readAnalogs() {
  for (int i = 0; i < N_CH; i++) {
    Channel& c = CH[i];
    int raw = medianRaw(c.pin);
    if (i == I_PH && faultInjected) raw = 0;   // محاكاة عطل: سلك pH مفصول
    c.raw = raw;
    c.fault = false;
    c.faultEn = ""; c.faultAr = "";
    if (raw <= ADC_RAIL_LOW || raw >= ADC_RAIL_HIGH) {
      c.fault = true;
      c.faultEn = "impossible reading (at the end of the range: wire cut or short?)";
      c.faultAr = "قراءة مستحيلة (سلك مفصول أو قصر)";
    }
    if (STUCK_CHECK) {
      c.sameCount = (raw == c.lastRaw) ? c.sameCount + 1 : 0;
      if (c.sameCount >= STUCK_N && !c.fault) {
        c.fault = true;
        c.faultEn = "stuck reading (no change at all)";
        c.faultAr = "قراءة ثابتة ما تتغير";
      }
    }
    c.lastRaw = raw;
    c.value = c.fault ? NAN : c.lo + (c.hi - c.lo) * (raw / 4095.0);
  }
  float orp2 = CH[I_ORP2].value;
  if (isnan(orp2)) clMgl = NAN;
  else {
    float k = (ORP2_CAL_CL2 - ORP2_CAL_CL1) / (ORP2_CAL_MV2 - ORP2_CAL_MV1);
    clMgl = ORP2_CAL_CL1 + (orp2 - ORP2_CAL_MV1) * k;
    if (clMgl < 0) clMgl = 0;
  }
}

void readLevel() {
  float d[10]; int n = 0;
  for (int i = 0; i < 10; i++) {
    digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    unsigned long us = pulseIn(PIN_ECHO, HIGH, 30000UL);  // 30 ms ≈ 5 م كحد أقصى
    if (us > 0) d[n++] = us / 58.0;
    delay(15);
  }
  levelFault = false; levelWhyEn = ""; levelWhyAr = "";
  if (n < 5) {
    levelFault = true;
    levelWhyEn = "no echo";
    levelWhyAr = "ما فيه صدى";
    distCm = NAN; levelPct = NAN;
    return;
  }
  distCm = medianFloats(d, n);
  float lv = 100.0 * (TANK_DEPTH_CM - distCm) / TANK_DEPTH_CM;
  levelPct = constrain(lv, 0.0f, 100.0f);
}

void readTemp() {
  float t = ds.getTempCByIndex(0);
  tempFault = false; tempWhyEn = ""; tempWhyAr = "";
  if (t == DEVICE_DISCONNECTED_C) {
    tempFault = true; tempWhyEn = "not responding"; tempWhyAr = "ما يرد (مفصول)";
  } else if (t < -55 || t > 125) {
    tempFault = true; tempWhyEn = "impossible reading"; tempWhyAr = "قراءة مستحيلة";
  } else if (STUCK_CHECK) {
    tempSame = (t == tempLast) ? tempSame + 1 : 0;
    if (tempSame >= STUCK_N) { tempFault = true; tempWhyEn = "stuck reading"; tempWhyAr = "قراءة ثابتة ما تتغير"; }
  }
  tempLast = t;
  tempC = tempFault ? NAN : t;
}

// ============================== 8) قواعد الحماية ==============================
// القاعدة 1: pH برا الحد أو ORP1 ارتفع فجأة → صمام 1 للصرف
// القاعدة 2: العكارة عالية، أو UV أقل من 70%، أو الكلور برا الحد → صمام 2 للصرف
// القاعدة 3: حساس معطل → وضع آمن: الصمامان للصرف + تنبيه
// القاعدة 4: التخزين فوق 90% → إيقاف التغذية للتخزين (صمام 2 للصرف) والفائض للصرف
// القاعدة 5: انقطاع الكهرباء → المرحّلات تطفي → الصمامان للصرف (من التوصيل نفسه)
// القاعدة 6: انقطاع الإنترنت → القواعد هنا تكمل، والقراءات تنحفظ وتنرسل بعدين (مهمة الشبكة)
// القاعدة 7: الرجوع للمسار الطبيعي بعد فترة انتظار من القراءات السليمة

void orp1Update(float orp) {
  if (isnan(orp)) return;
  if (!orp1Alarm) {
    if (orpCount >= 10) {
      float base = medianFloats(orpHist, 10);
      if (orp - base > ORP1_JUMP_MV) { orp1Alarm = true; orp1Base = base; return; }  // نجمّد خط الأساس
    }
    orpHist[orpIdx] = orp; orpIdx = (orpIdx + 1) % 10; if (orpCount < 10) orpCount++;
  } else if (orp - orp1Base <= ORP1_JUMP_MV) {
    orp1Alarm = false;   // رجع قريب من المعتاد
  }
}

void describe(uint16_t bit, char* en, size_t ne, char* ar, size_t na, float* val) {
  float ph = CH[I_PH].value, orp1 = CH[I_ORP1].value, turb = CH[I_TURB].value,
        uv = CH[I_UV].value, orp2 = CH[I_ORP2].value;
  switch (bit) {
    case C_PH:
      snprintf(en, ne, "pH %.2f is outside 6.5-8.4", ph);
      snprintf(ar, na, "pH برا الحد المسموح (بين 6.5 و8.4): القراءة %.2f", ph);
      *val = ph; break;
    case C_ORP1:
      snprintf(en, ne, "ORP1 jumped to %.0f mV (usual %.0f mV): chlorine or strong cleaner?", orp1, orp1Base);
      snprintf(ar, na, "ORP1 ارتفع فجأة إلى %.0f mV (المعتاد %.0f mV): احتمال كلور أو منظف قوي", orp1, orp1Base);
      *val = orp1; break;
    case C_TURB:
      snprintf(en, ne, "turbidity %.1f NTU is above 5 NTU", turb);
      snprintf(ar, na, "العكارة عالية: %.1f NTU (الحد 5 NTU)", turb);
      *val = turb; break;
    case C_UV:
      snprintf(en, ne, "UV intensity %.0f%% is below 70%%", uv);
      snprintf(ar, na, "شدة UV ضعيفة: %.0f%% (الحد الأدنى 70%%)", uv);
      *val = uv; break;
    case C_CL:
      snprintf(en, ne, "residual chlorine ~%.2f mg/L (est. from ORP2 %.0f mV) is outside 0.2-0.5", clMgl, orp2);
      snprintf(ar, na, "الكلور المتبقي برا الحد (بين 0.2 و0.5 ملغ/لتر): %.2f تقديري من ORP2", clMgl);
      *val = clMgl; break;
    case C_LEVEL:
      snprintf(en, ne, "storage %.0f%% is above 90%%: stop feeding storage, overflow to sewer", levelPct);
      snprintf(ar, na, "التخزين فوق 90%%: %.0f%%. إيقاف التغذية والفائض للصرف", levelPct);
      *val = levelPct; break;
    default:
      snprintf(en, ne, "?"); snprintf(ar, na, "?"); *val = NAN;
  }
}

void updateValve(Valve& v, uint16_t causes) {
  uint16_t added = causes & ~v.causes;
  v.causes = causes;
  if (causes) {
    v.clearSince = 0;
    if (!v.sewer) {
      v.sewer = true;
    }
    // حدث لكل سبب جديد (العطل والتشغيل لهم حدث خاص يغطي الصمامين)
    for (uint16_t bit = C_PH; bit <= C_LEVEL; bit <<= 1) {
      if (!(added & bit)) continue;
      char en[200], ar[240]; float val;
      describe(bit, en, sizeof(en), ar, sizeof(ar), &val);
      say(">>> [EVENT] %s -> SEWER (red): %s", v.en, en);
      addEvent("divert", ar, v.id, val);
    }
    return;
  }
  if (!v.sewer) return;
  if (v.clearSince == 0) {
    v.clearSince = millis();
    say("... %s: all checks OK, back to NORMAL in %lu s if it stays OK", v.en, (unsigned long)(RETURN_WAIT_MS / 1000));
  } else if (millis() - v.clearSince >= RETURN_WAIT_MS) {
    v.sewer = false;
    v.clearSince = 0;
    char ar[240];
    snprintf(ar, sizeof(ar), "رجع للمسار الطبيعي بعد %lu ثانية من القراءات السليمة", (unsigned long)(RETURN_WAIT_MS / 1000));
    say(">>> [EVENT] %s -> NORMAL (green): readings OK for %lu s", v.en, (unsigned long)(RETURN_WAIT_MS / 1000));
    addEvent("restore", ar, v.id, NAN);
  }
}

void applyOutputs() {
  digitalWrite(PIN_VALVE1, V1.sewer ? RELAY_OFF : RELAY_ON);
  digitalWrite(PIN_VALVE2, V2.sewer ? RELAY_OFF : RELAY_ON);
  digitalWrite(PIN_AIR, airOn ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_CL, clOn ? RELAY_ON : RELAY_OFF);
}

void evaluate() {
  // القاعدة 3: الحساسات المعطلة
  char faultsEn[400] = "", faultsAr[400] = "";
  int nf = 0;
  for (int i = 0; i < N_CH; i++) {
    if (!CH[i].fault) continue;
    size_t le = strlen(faultsEn), la = strlen(faultsAr);
    snprintf(faultsEn + le, sizeof(faultsEn) - le, "%s%s: %s", nf ? "; " : "", CH[i].en, CH[i].faultEn);
    snprintf(faultsAr + la, sizeof(faultsAr) - la, "%s%s: %s", nf ? "، " : "", CH[i].ar, CH[i].faultAr);
    nf++;
  }
  if (tempFault) {
    size_t le = strlen(faultsEn), la = strlen(faultsAr);
    snprintf(faultsEn + le, sizeof(faultsEn) - le, "%sTemp: %s", nf ? "; " : "", tempWhyEn);
    snprintf(faultsAr + la, sizeof(faultsAr) - la, "%sالحرارة: %s", nf ? "، " : "", tempWhyAr);
    nf++;
  }
  if (levelFault) {
    size_t le = strlen(faultsEn), la = strlen(faultsAr);
    snprintf(faultsEn + le, sizeof(faultsEn) - le, "%sLevel: %s", nf ? "; " : "", levelWhyEn);
    snprintf(faultsAr + la, sizeof(faultsAr) - la, "%sالمستوى: %s", nf ? "، " : "", levelWhyAr);
    nf++;
  }
  bool nowSafe = nf > 0;
  if (nowSafe && !safeMode) {
    char ar[480];   // ينقص لـ 240 بايت داخل addEvent بدون ما يكسر حرف عربي
    snprintf(ar, sizeof(ar), "حساس معطل (%s). وضع آمن: الصمامان على الصرف", faultsAr);
    say("!!! [EVENT] SAFE MODE: %s -> both valves to SEWER", faultsEn);
    addEvent("safe_mode", ar, "both", NAN);
  } else if (!nowSafe && safeMode) {
    say("... SAFE MODE ended: sensors OK again (valves return after the waiting period)");
    addEvent("safe_mode_end", "الحساسات رجعت سليمة، والصمامان يرجعون بعد فترة الانتظار", "both", NAN);
  }
  safeMode = nowSafe;

  // بداية التشغيل: نبدأ على الصرف لين تمر أول قراءة كاملة سليمة
  if (bootPending && !safeMode) bootPending = false;

  orp1Update(CH[I_ORP1].value);

  uint16_t c1 = 0, c2 = 0;
  float ph = CH[I_PH].value;
  if (!isnan(ph) && (ph < PH_MIN || ph > PH_MAX)) c1 |= C_PH;
  if (orp1Alarm) c1 |= C_ORP1;
  float turb = CH[I_TURB].value, uv = CH[I_UV].value;
  if (!isnan(turb) && turb > TURB_MAX_NTU) c2 |= C_TURB;
  if (!isnan(uv) && uv < UV_MIN_PCT) c2 |= C_UV;
  if (!isnan(clMgl) && (clMgl < CL_MIN || clMgl > CL_MAX)) c2 |= C_CL;
  if (!isnan(levelPct) && levelPct > LEVEL_MAX_PCT) c2 |= C_LEVEL;
  if (safeMode) { c1 |= C_FAULT; c2 |= C_FAULT; }
  if (bootPending) { c1 |= C_BOOT; c2 |= C_BOOT; }

  updateValve(V1, c1);
  updateValve(V2, c2);

  // مضخة الهواء تبقى شغالة دايم (حتى في الوضع الآمن) عشان البكتيريا ما تموت.
  airOn = true;
  // مضخة الكلور: تشتغل لو الكلور تحت منتصف المدى، وتطفي في الوضع الآمن.
  clOn = !safeMode && !isnan(clMgl) && clMgl < CL_TARGET;
  applyOutputs();
}

// ============================== 9) مهمة الشبكة (على النواة 0) ==============================
// منفصلة عن القراءة والقواعد، فبطء الإنترنت أو انقطاعه ما يأخر قرار الحماية أبد.
bool isoFromMs(uint32_t ms, char* out, size_t n) {
  if (!timeOk) return false;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  int64_t nowMs = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  int64_t t = nowMs - (int64_t)(uint32_t)(millis() - ms);
  time_t s = (time_t)(t / 1000);
  struct tm tm;
  gmtime_r(&s, &tm);
  snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(t % 1000));
  return true;
}

void jNum(String& s, const char* k, float v, int dec) {
  s += '"'; s += k; s += "\":";
  if (isnan(v)) s += "null"; else s += String(v, dec);
  s += ',';
}
void jStr(String& s, const char* k, const char* v) {
  s += '"'; s += k; s += "\":";
  if (!v || !*v) { s += "null,"; return; }
  s += '"';
  for (const char* p = v; *p; p++) {
    if (*p == '"' || *p == '\\') s += '\\';
    if ((uint8_t)*p >= 0x20) s += *p;
  }
  s += "\",";
}
void jBool(String& s, const char* k, bool v) {
  s += '"'; s += k; s += "\":"; s += v ? "true" : "false"; s += ',';
}
void jTs(String& s, uint32_t ms, bool withTs) {
  char ts[32];
  if (withTs && isoFromMs(ms, ts, sizeof(ts))) jStr(s, "ts", ts);
}
void jClose(String& s) {
  if (s.endsWith(",")) s.remove(s.length() - 1);
  s += '}';
}

// اتصال واحد يبقى مفتوح (keep-alive) بدل مصافحة TLS جديدة مع كل إرسال.
// المصافحة ثقيلة على المعالج (خصوصًا في المحاكي)، فإعادة استخدام الاتصال تخلي الإرسال أسرع بكثير.
WiFiClientSecure tlsClient;
HTTPClient http;
bool httpReady = false;

int postJson(const char* table, const String& body) {
  if (!httpReady) {
    tlsClient.setInsecure();  // للعرض: بدون تحقق من شهادة الموقع. المفتاح anon عام أصلًا وصلاحياته إضافة وقراءة بس.
    http.setReuse(true);
    httpReady = true;
  }
  String url = String(SUPABASE_URL) + "/rest/v1/" + table;
  if (!http.begin(tlsClient, url)) return -1;
  http.setTimeout(15000);
  http.setConnectTimeout(20000);   // مصافحة TLS في المحاكي أبطأ من القطعة الحقيقية
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  // المفتاح الجديد (sb_publishable_...) مو JWT، وSupabase يطلبه في apikey بس (لا يرسل كـ Bearer).
  // المفتاح القديم anon هو JWT (يبدأ بـ eyJ)، فنرسله في الاثنين مثل مكتبة supabase-js.
  if (strncmp(SUPABASE_ANON_KEY, "eyJ", 3) == 0)
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");
  int code = http.POST(body);
  if (code < 200 || code >= 300) {
    String resp = code > 0 ? http.getString() : http.errorToString(code);
    say("[NET] %s: HTTP %d %s", table, code, resp.substring(0, 160).c_str());
  }
  http.end();                 // مع setReuse يبقى الاتصال مفتوح للمرة الجاية
  if (code < 0) tlsClient.stop();  // اتصال خربان: نبدأ اتصال جديد المرة الجاية
  lastHttp = code;
  return code;
}

static Reading rBatch[30];
static EventRec eBatch[10];

bool flushEvents() {
  int n = 0;
  xSemaphoreTake(qLock, portMAX_DELAY);
  n = eventQ.count < 10 ? eventQ.count : 10;
  for (int i = 0; i < n; i++) eBatch[i] = eventQ.at(i);
  xSemaphoreGive(qLock);
  if (n == 0) return true;
  bool withTs = timeOk;   // نفس المفاتيح في كل صفوف الدفعة
  String body = "[";
  for (int i = 0; i < n; i++) {
    const EventRec& e = eBatch[i];
    String o = "{";
    jTs(o, e.ms, withTs);
    jStr(o, "type", e.type);
    jStr(o, "reason", e.reason);
    jStr(o, "valve", e.valve);
    jNum(o, "value", e.value, 2);
    jStr(o, "decided_by", "rule");
    jClose(o);
    body += o;
    if (i < n - 1) body += ',';
  }
  body += ']';
  int code = postJson("events", body);
  if (code < 200 || code >= 300) return false;
  xSemaphoreTake(qLock, portMAX_DELAY);
  eventQ.popThrough(eBatch[n - 1].seq);
  xSemaphoreGive(qLock);
  sentEvents += n;
  return true;
}

bool flushReadings() {
  int n = 0;
  xSemaphoreTake(qLock, portMAX_DELAY);
  n = readQ.count < 30 ? readQ.count : 30;
  for (int i = 0; i < n; i++) rBatch[i] = readQ.at(i);
  xSemaphoreGive(qLock);
  if (n == 0) return true;
  bool withTs = timeOk;   // نفس المفاتيح في كل صفوف الدفعة
  String body;
  body.reserve(n * 330 + 4);
  body = "[";
  for (int i = 0; i < n; i++) {
    const Reading& r = rBatch[i];
    String o = "{";
    jTs(o, r.ms, withTs);
    jNum(o, "ph", r.ph, 2);
    jNum(o, "orp_in_mv", r.orp1, 0);
    jNum(o, "do_mgl", r.dox, 2);
    jNum(o, "temp_c", r.temp, 2);
    jNum(o, "turbidity", r.turb, 2);
    jNum(o, "uv_pct", r.uv, 1);
    jNum(o, "orp_cl_mv", r.orp2, 0);
    jNum(o, "level_pct", r.level, 1);
    jStr(o, "valve1", r.v1Sewer ? "sewer" : "normal");
    jStr(o, "valve2", r.v2Sewer ? "sewer" : "normal");
    jBool(o, "air_pump", r.air);
    jBool(o, "cl_pump", r.cl);
    jBool(o, "safe_mode", r.safe);
    jClose(o);
    body += o;
    if (i < n - 1) body += ',';
  }
  body += ']';
  int code = postJson("sensor_readings", body);
  if (code < 200 || code >= 300) return false;
  xSemaphoreTake(qLock, portMAX_DELAY);
  readQ.popThrough(rBatch[n - 1].seq);
  xSemaphoreGive(qLock);
  sentReadings += n;
  return true;
}

void netTask(void*) {
  bool everUp = false, wasUp = false;
  uint32_t upSince = 0, retryAt = 0, backoff = 2000, lastBegin = 0;
  for (;;) {
    if (netCut) {
      if (WiFi.status() == WL_CONNECTED) WiFi.disconnect();
      lastBegin = 0;
    } else if (WiFi.status() != WL_CONNECTED) {
      if (lastBegin == 0 || millis() - lastBegin > 15000) {
        lastBegin = millis();
        say("[NET] connecting to WiFi \"%s\" ...", WIFI_SSID);
        WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
      }
    }
    bool up = !netCut && WiFi.status() == WL_CONNECTED;
    if (up != wasUp) {
      wasUp = up;
      wifiUp = up;
      if (up) {
        upSince = millis();
        say("[NET] WiFi connected, IP %s", WiFi.localIP().toString().c_str());
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        if (everUp) {
          int pending;
          xSemaphoreTake(qLock, portMAX_DELAY);
          pending = readQ.count;
          xSemaphoreGive(qLock);
          char ar[240];
          snprintf(ar, sizeof(ar), "رجع الإنترنت: بنرسل %d قراءة محفوظة بوقتها الأصلي", pending);
          say(">>> [EVENT] INTERNET BACK: sending %d saved readings", pending);
          addEvent("net_online", ar, nullptr, NAN);
        }
        everUp = true;
        backoff = 2000; retryAt = 0;
      } else if (everUp) {
        tlsClient.stop();   // الاتصال القديم مات مع الشبكة
        say(">>> [EVENT] INTERNET LOST: rules keep running, readings are saved on the device");
        addEvent("net_offline", "انقطع الإنترنت: القواعد شغالة، والقراءات تنحفظ في الجهاز وتنرسل بعدين", nullptr, NAN);
      }
    }
    if (up && !timeOk) {
      if (time(nullptr) > 1700000000) {
        timeOk = true;
        char ts[32];
        isoFromMs(millis(), ts, sizeof(ts));
        say("[NET] clock synced (UTC %s)", ts);
      }
    }
    // نرسل بعد ما يتزامن الوقت (عشان كل قراءة توصل بوقتها الصحيح)، أو بعد 15 ثانية لو ما تزامن
    bool canSend = up && supabaseReady && (timeOk || millis() - upSince > 15000) && millis() >= retryAt;
    if (canSend) {
      bool ok = flushEvents() && flushReadings();
      if (ok) backoff = 2000;
      else {
        retryAt = millis() + backoff;
        say("[NET] send failed, data kept. retry in %lu s", (unsigned long)(backoff / 1000));
        backoff = backoff * 2 > 30000 ? 30000 : backoff * 2;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

// ============================== 10) الأزرار ==============================
void IRAM_ATTR onBtnFault() {
  uint32_t t = millis();
  if (t - btnFaultT > 300) { btnFaultT = t; btnFaultEvt = true; }
}
void IRAM_ATTR onBtnNet() {
  uint32_t t = millis();
  if (t - btnNetT > 300) { btnNetT = t; btnNetEvt = true; }
}
void handleButtons() {
  if (btnFaultEvt) {
    btnFaultEvt = false;
    faultInjected = !faultInjected;
    if (faultInjected) say("[BUTTON] sensor fault simulated: pH probe wire cut (press again to fix it)");
    else say("[BUTTON] sensor fault fixed: pH probe connected again");
  }
  if (btnNetEvt) {
    btnNetEvt = false;
    netCut = !netCut;
    if (netCut) say("[BUTTON] internet cut simulated (press again to bring it back)");
    else say("[BUTTON] internet back on");
  }
}

// ============================== 11) دورة القراءة ==============================
void cycle() {
  uint32_t t0 = millis();
  ds.requestTemperatures();          // يبدأ قياس الحرارة (يحتاج قرابة 750 ms)
  readAnalogs();
  readLevel();
  while (millis() - t0 < 760) delay(5);
  readTemp();
  evaluate();

  Reading r;
  r.ms = t0;
  r.ph = CH[I_PH].value; r.orp1 = CH[I_ORP1].value; r.dox = CH[I_DO].value; r.temp = tempC;
  r.turb = CH[I_TURB].value; r.uv = CH[I_UV].value; r.orp2 = CH[I_ORP2].value; r.level = levelPct;
  r.v1Sewer = V1.sewer; r.v2Sewer = V2.sewer; r.air = airOn; r.cl = clOn; r.safe = safeMode;
  int queued;
  uint32_t dropped;
  xSemaphoreTake(qLock, portMAX_DELAY);
  r.seq = ++readSeq;
  if (supabaseReady) readQ.push(r);
  queued = readQ.count + eventQ.count;
  dropped = readQ.dropped;
  xSemaphoreGive(qLock);

  char ph[8], orp1[8], dox[8], tc[8], turb[8], uv[8], orp2[8], cl[8], lv[8];
  #define FMT(buf, v, f) do { if (isnan(v)) strcpy(buf, "--"); else snprintf(buf, sizeof(buf), f, v); } while (0)
  FMT(ph, r.ph, "%.2f"); FMT(orp1, r.orp1, "%+.0f"); FMT(dox, r.dox, "%.1f"); FMT(tc, r.temp, "%.1f");
  FMT(turb, r.turb, "%.1f"); FMT(uv, r.uv, "%.0f"); FMT(orp2, r.orp2, "%.0f"); FMT(cl, clMgl, "%.2f");
  FMT(lv, r.level, "%.0f");
  #undef FMT
  char net[64];
  if (!supabaseReady) snprintf(net, sizeof(net), "%s, Supabase not set", wifiUp ? "online" : "offline");
  else snprintf(net, sizeof(net), "%s, sent %lu, waiting %d%s", wifiUp ? "online" : "OFFLINE",
                (unsigned long)sentReadings, queued, dropped ? " (buffer full!)" : "");
  say("#%lu pH %s | ORP1 %s mV | DO %s mg/L | T %s C | Turb %s NTU | UV %s %% | ORP2 %s mV (Cl ~%s mg/L est.) | Storage %s %%",
       (unsigned long)r.seq, ph, orp1, dox, tc, turb, uv, orp2, cl, lv);
  say("   V1 %s | V2 %s | AIR %s | CL pump %s | SAFE %s | net: %s",
       route(V1.sewer), route(V2.sewer), onOff(airOn), onOff(clOn), safeMode ? "YES" : "no", net);
}

// ============================== 12) setup و loop ==============================
void setup() {
  // أول شي: كل المرحّلات مطفية = الصمامان على الصرف (Fail-safe) قبل أي شي ثاني
  int relays[] = {PIN_VALVE1, PIN_VALVE2, PIN_AIR, PIN_CL};
  for (int p : relays) { pinMode(p, OUTPUT); digitalWrite(p, RELAY_OFF); }

  Serial.begin(115200);
  qLock = xSemaphoreCreateMutex();
  logLock = xSemaphoreCreateMutex();
  analogReadResolution(12);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BTN_FAULT, INPUT_PULLUP);
  pinMode(PIN_BTN_NET, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_FAULT), onBtnFault, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_NET), onBtnNet, FALLING);
  ds.begin();
  ds.setResolution(12);
  ds.setWaitForConversion(false);

  supabaseReady = strstr(SUPABASE_URL, "YOUR-") == nullptr && strstr(SUPABASE_ANON_KEY, "YOUR-") == nullptr;

  say("");
  say("==============================================================");
  say(" EcoBio-Matrix | ESP32 protection unit | %s mode", DEMO_MODE ? "DEMO (1 reading/s)" : "REAL (1 reading/min)");
  say(" Tested on the Wokwi simulator only, not on real hardware.");
  say(" Rules: pH 6.5-8.4 | ORP1 jump > %.0f mV | Turb <= 5 NTU | UV >= 70%% | Cl 0.2-0.5 mg/L | Storage <= 90%%",
       ORP1_JUMP_MV);
  say(" Return to normal after %lu s of good readings", (unsigned long)(RETURN_WAIT_MS / 1000));
  say(" Supabase: %s", supabaseReady ? SUPABASE_URL : "not set yet (Phase 3). Rules still run locally.");
  say("==============================================================");
  say(">>> [EVENT] BOOT: both valves start on SEWER until the readings are OK (fail-safe)");
  addEvent("boot", "تشغيل الجهاز: الصمامان على الصرف لين تثبت القراءات (Fail-safe)", "both", NAN);

  WiFi.mode(WIFI_STA);
  // أولوية 0 (نفس مهمة الخمول IDLE) عشان مصافحة TLS الطويلة ما تحرم IDLE0 من وقته
  // ويعيد تشغيل الجهاز (Task watchdog). وقرار الحماية على النواة 1 ما يتأثر أبد.
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 0, nullptr, 0);
}

void loop() {
  static uint32_t next = 0;
  handleButtons();
  if ((int32_t)(millis() - next) >= 0) {
    next = millis() + READ_PERIOD_MS;
    cycle();
    // ما نحتاج انتظار طويل: الدورة نفسها تاخذ قرابة 0.8 ثانية
  }
  delay(10);
}
