/*
 * Pluviómetro de balancín con ESP32 DevKit V1
 *
 *  - Reed switch + imán en el balancín: cada vuelco = un pulso.
 *  - Área de captación configurable (rectangular o circular).
 *  - Cálculos:
 *        V  = vuelcos × ml_por_vuelco          (ml = cm³)
 *        h  = V / A                              (cm)  ->  ×10 = mm (= L/m²)
 *        V' = A' · h                             (proyección a otra superficie, en litros)
 *  - Configuración por comandos en el monitor serie y por web (WiFi).
 *  - Pantalla OLED SSD1306 128x64 I2C.
 *  - Sensor DHT11 de temperatura y humedad.
 */
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include "web_page.h"

// ---------------- Hardware ----------------
constexpr uint8_t PIN_REED = 27;          // Reed switch entre GPIO27 y GND
constexpr uint8_t PIN_SDA = 21;
constexpr uint8_t PIN_SCL = 22;
constexpr uint8_t OLED_ADDR = 0x3C;       // Algunas pantallas usan 0x3D
constexpr uint8_t PIN_DHT = 4;            // Pin de datos del DHT11
constexpr uint32_t DHT_INTERVALO_MS = 3000;
constexpr uint32_t DEBOUNCE_MS = 200;     // Tiempo mínimo entre vuelcos

// Punto de acceso propio cuando no hay WiFi configurada
const char *AP_SSID = "Pluviometro";
const char *AP_PASS = "pluvio123";        // mínimo 8 caracteres

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
Preferences prefs;
WebServer server(80);
DHT dht(PIN_DHT, DHT11);

// ---------------- Configuración ----------------
enum Forma : uint8_t { RECTANGULAR = 0, CIRCULAR = 1 };

struct Config {
  uint8_t forma = RECTANGULAR;
  float largo = 20.0f;       // cm
  float ancho = 20.0f;       // cm
  float diametro = 20.0f;    // cm
  float mlVuelco = 5.0f;     // ml que vuelca el balancín en cada movimiento
  float superficie = 0.0f;   // m² para proyectar V = A·h (0 = desactivado)
  String ssid;
  String pass;
} cfg;

// ---------------- Estado ----------------
volatile uint32_t vuelcosISR = 0;
volatile uint32_t ultimoPulsoMs = 0;
uint32_t vuelcos = 0;
uint32_t vuelcosGuardados = 0;
bool pantallaOk = false;
bool dhtOk = false;          // true si la última lectura fue válida
float temperatura = NAN;    // °C
float humedad = NAN;        // %
bool modoAP = false;
String lineaSerie;

void IRAM_ATTR onReed() {
  uint32_t ahora = millis();
  if (ahora - ultimoPulsoMs >= DEBOUNCE_MS) {
    vuelcosISR++;
    ultimoPulsoMs = ahora;
  }
}

// ---------------- Cálculos ----------------
float areaCm2() {
  if (cfg.forma == CIRCULAR) return PI * cfg.diametro * cfg.diametro / 4.0f;
  return cfg.largo * cfg.ancho;
}

float volumenMl() { return vuelcos * cfg.mlVuelco; }

float lluviaMm() {
  float a = areaCm2();
  return a > 0 ? (volumenMl() / a) * 10.0f : 0.0f;  // h = V/A en cm -> mm
}

// V = A·h  ->  m² × mm = litros
float volumenProyectadoL() { return cfg.superficie * lluviaMm(); }

String descripcionArea() {
  char buf[48];
  if (cfg.forma == CIRCULAR)
    snprintf(buf, sizeof(buf), "circular d=%.1f cm", cfg.diametro);
  else
    snprintf(buf, sizeof(buf), "%.1f x %.1f cm", cfg.largo, cfg.ancho);
  return buf;
}

// ---------------- Persistencia (NVS) ----------------
void cargarConfig() {
  prefs.begin("pluvio", true);
  cfg.forma = prefs.getUChar("forma", cfg.forma);
  cfg.largo = prefs.getFloat("largo", cfg.largo);
  cfg.ancho = prefs.getFloat("ancho", cfg.ancho);
  cfg.diametro = prefs.getFloat("diam", cfg.diametro);
  cfg.mlVuelco = prefs.getFloat("ml", cfg.mlVuelco);
  cfg.superficie = prefs.getFloat("sup", cfg.superficie);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  vuelcos = prefs.getUInt("vuelcos", 0);
  prefs.end();
  vuelcosGuardados = vuelcos;
}

void guardarConfig() {
  prefs.begin("pluvio", false);
  prefs.putUChar("forma", cfg.forma);
  prefs.putFloat("largo", cfg.largo);
  prefs.putFloat("ancho", cfg.ancho);
  prefs.putFloat("diam", cfg.diametro);
  prefs.putFloat("ml", cfg.mlVuelco);
  prefs.putFloat("sup", cfg.superficie);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.end();
}

void guardarVuelcos() {
  prefs.begin("pluvio", false);
  prefs.putUInt("vuelcos", vuelcos);
  prefs.end();
  vuelcosGuardados = vuelcos;
}

void resetearConteo() {
  noInterrupts();
  vuelcosISR = 0;
  interrupts();
  vuelcos = 0;
  guardarVuelcos();
}

// ---------------- Pantalla ----------------
String ipActual() {
  return modoAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

void dibujarPantalla() {
  if (!pantallaOk) return;
  char buf[32];
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Pantalla bicolor: las 16 filas de arriba son amarillas y el resto azul.
  // Franja amarilla (y 0-15): temperatura/humedad y área
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  if (dhtOk)
    snprintf(buf, sizeof(buf), "T:%.1fC  H:%.0f%%", temperatura, humedad);
  else
    snprintf(buf, sizeof(buf), "T:--.-C  H:--%%");
  oled.print(buf);
  oled.setCursor(0, 8);
  snprintf(buf, sizeof(buf), "A: %.1f cm2", areaCm2());
  oled.print(buf);

  // Zona azul (y 16-63): mediciones
  oled.setTextSize(2);
  oled.setCursor(0, 19);
  snprintf(buf, sizeof(buf), "%.2f mm", lluviaMm());
  oled.print(buf);

  oled.setTextSize(1);
  oled.setCursor(0, 38);
  snprintf(buf, sizeof(buf), "Vuelcos: %lu", (unsigned long)vuelcos);
  oled.print(buf);

  oled.setCursor(0, 47);
  if (cfg.superficie > 0)
    snprintf(buf, sizeof(buf), "V:%.0fml  %.1fL", volumenMl(), volumenProyectadoL());
  else
    snprintf(buf, sizeof(buf), "V: %.1f ml", volumenMl());
  oled.print(buf);

  oled.setCursor(0, 56);
  oled.print(modoAP ? "AP " : "IP ");
  oled.print(ipActual());

  oled.display();
}

// ---------------- Diagnóstico WiFi ----------------
volatile uint8_t motivoDesconexion = 0;

const char *textoMotivo(uint8_t motivo) {
  switch (motivo) {
    case WIFI_REASON_NO_AP_FOUND:
      return "no se encuentra la red (nombre mal escrito, red de 5 GHz o sin alcance)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "clave incorrecta";
    case WIFI_REASON_AUTH_EXPIRE:
      return "el router no respondio (senal debil o clave incorrecta)";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
      return "el router rechazo la conexion (filtro MAC o demasiados equipos)";
    default:
      return "desconocido";
  }
}

const char *textoSeguridad(wifi_auth_mode_t modo) {
  switch (modo) {
    case WIFI_AUTH_OPEN: return "abierta";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "otra";
  }
}

// Lista las redes que ve el ESP32 (solo 2.4 GHz) y marca la configurada
void escanearRedes() {
  Serial.println(F("Buscando redes (el ESP32 solo ve 2.4 GHz)..."));
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println(F("  No se encontro ninguna red."));
    return;
  }
  bool vista = false;
  for (int i = 0; i < n; i++) {
    bool esLaMia = cfg.ssid.length() && WiFi.SSID(i) == cfg.ssid;
    vista |= esLaMia;
    Serial.printf("  %c '%s'  %d dBm  canal %d  %s\n", esLaMia ? '>' : ' ',
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                  textoSeguridad(WiFi.encryptionType(i)));
  }
  if (cfg.ssid.length() && !vista)
    Serial.printf("  La red '%s' NO aparece en la lista.\n", cfg.ssid.c_str());
  WiFi.scanDelete();
}

// ---------------- Comandos serie ----------------
void imprimirAyuda() {
  Serial.println(F(
      "\n=== Comandos ===\n"
      "  ayuda                    Muestra esta ayuda\n"
      "  estado                   Muestra configuracion y mediciones\n"
      "  area <largo> <ancho>     Area rectangular en cm   (ej: area 20 20)\n"
      "  diametro <d>             Area circular en cm      (ej: diametro 15.5)\n"
      "  ml <valor>               ml por vuelco del balancin (ej: ml 5.2)\n"
      "  superficie <m2>          Proyecta V=A*h a esa superficie (0 = off)\n"
      "  reset                    Pone el conteo de vuelcos en 0\n"
      "  test                     Simula un vuelco\n"
      "  wifi <ssid>,<clave>      Conecta a tu red WiFi (se reinicia)\n"
      "  wifi borrar              Vuelve al modo punto de acceso\n"
      "  scan                     Lista las redes WiFi que ve el ESP32\n"));
}

void imprimirEstado() {
  Serial.println(F("\n=== Estado ==="));
  Serial.printf("  Area:          %s = %.2f cm2\n", descripcionArea().c_str(), areaCm2());
  Serial.printf("  ml/vuelco:     %.2f ml\n", cfg.mlVuelco);
  Serial.printf("  Vuelcos:       %lu\n", (unsigned long)vuelcos);
  Serial.printf("  Volumen V:     %.2f ml\n", volumenMl());
  Serial.printf("  Lluvia h:      %.2f mm (L/m2)\n", lluviaMm());
  if (cfg.superficie > 0)
    Serial.printf("  Proyeccion:    %.2f m2 x %.2f mm = %.2f L\n",
                  cfg.superficie, lluviaMm(), volumenProyectadoL());
  if (dhtOk)
    Serial.printf("  Temp/Humedad:  %.1f C  /  %.0f %%\n", temperatura, humedad);
  else
    Serial.println(F("  Temp/Humedad:  sin lectura del DHT11 (revisa cableado)"));
  Serial.printf("  WiFi:          %s  IP %s\n",
                modoAP ? "punto de acceso" : cfg.ssid.c_str(), ipActual().c_str());
}

void procesarComando(String linea) {
  linea.trim();
  if (linea.isEmpty()) return;

  int esp = linea.indexOf(' ');
  String cmd = (esp < 0 ? linea : linea.substring(0, esp));
  String args = (esp < 0 ? "" : linea.substring(esp + 1));
  cmd.toLowerCase();
  args.trim();

  // El WiFi se procesa antes porque la clave puede llevar comas o puntos
  if (cmd == "wifi") {
    if (args.equalsIgnoreCase("borrar")) {
      cfg.ssid = "";
      cfg.pass = "";
    } else {
      int coma = args.indexOf(',');
      if (coma <= 0) {
        Serial.println(F("Uso: wifi <ssid>,<clave>"));
        return;
      }
      cfg.ssid = args.substring(0, coma);
      cfg.pass = args.substring(coma + 1);
      cfg.ssid.trim();  // tolera "wifi MiRed, MiClave"
      cfg.pass.trim();
    }
    guardarConfig();
    guardarVuelcos();
    Serial.println(F("WiFi guardado. Reiniciando..."));
    delay(500);
    ESP.restart();
  }

  args.replace(',', '.');  // acepta "20,5" como decimal
  float a = 0, b = 0;
  int n = sscanf(args.c_str(), "%f %f", &a, &b);

  if (cmd == "ayuda" || cmd == "help" || cmd == "?") {
    imprimirAyuda();
  } else if (cmd == "scan") {
    escanearRedes();
  } else if (cmd == "estado") {
    imprimirEstado();
  } else if (cmd == "area") {
    if (n != 2 || a <= 0 || b <= 0) { Serial.println(F("Uso: area <largo_cm> <ancho_cm>")); return; }
    cfg.forma = RECTANGULAR; cfg.largo = a; cfg.ancho = b;
    guardarConfig();
    Serial.printf("Area: %s = %.2f cm2\n", descripcionArea().c_str(), areaCm2());
  } else if (cmd == "diametro") {
    if (n < 1 || a <= 0) { Serial.println(F("Uso: diametro <cm>")); return; }
    cfg.forma = CIRCULAR; cfg.diametro = a;
    guardarConfig();
    Serial.printf("Area: %s = %.2f cm2\n", descripcionArea().c_str(), areaCm2());
  } else if (cmd == "ml") {
    if (n < 1 || a <= 0) { Serial.println(F("Uso: ml <ml_por_vuelco>")); return; }
    cfg.mlVuelco = a;
    guardarConfig();
    Serial.printf("ml por vuelco: %.2f\n", cfg.mlVuelco);
  } else if (cmd == "superficie") {
    if (n < 1 || a < 0) { Serial.println(F("Uso: superficie <m2>  (0 = off)")); return; }
    cfg.superficie = a;
    guardarConfig();
    Serial.printf("Superficie de proyeccion: %.2f m2\n", cfg.superficie);
  } else if (cmd == "reset") {
    resetearConteo();
    Serial.println(F("Conteo reiniciado."));
  } else if (cmd == "test") {
    noInterrupts();
    vuelcosISR++;
    interrupts();
    Serial.println(F("Vuelco simulado."));
  } else {
    Serial.printf("Comando desconocido: '%s'. Escribe 'ayuda'.\n", cmd.c_str());
    return;
  }
  dibujarPantalla();
}

void leerSerie() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineaSerie.length()) procesarComando(lineaSerie);
      lineaSerie = "";
    } else if (lineaSerie.length() < 96) {
      lineaSerie += c;
    }
  }
}

// ---------------- Web ----------------
void handleEstado() {
  char clima[48] = "\"temperatura\":null,\"humedad\":null";
  if (dhtOk)
    snprintf(clima, sizeof(clima), "\"temperatura\":%.1f,\"humedad\":%.0f",
             temperatura, humedad);
  char json[480];
  snprintf(json, sizeof(json),
           "{\"vuelcos\":%lu,\"volumen_ml\":%.2f,\"lluvia_mm\":%.2f,"
           "\"area_cm2\":%.2f,\"forma\":\"%s\",\"largo\":%.2f,\"ancho\":%.2f,"
           "\"diametro\":%.2f,\"ml_vuelco\":%.2f,\"superficie\":%.2f,"
           "\"proyeccion_l\":%.2f,%s}",
           (unsigned long)vuelcos, volumenMl(), lluviaMm(), areaCm2(),
           cfg.forma == CIRCULAR ? "circular" : "rectangular", cfg.largo,
           cfg.ancho, cfg.diametro, cfg.mlVuelco, cfg.superficie,
           volumenProyectadoL(), clima);
  server.send(200, "application/json", json);
}

void handleConfig() {
  auto num = [](const char *k, float def) {
    if (!server.hasArg(k)) return def;
    String s = server.arg(k);
    s.replace(',', '.');
    float v = s.toFloat();
    return v > 0 ? v : def;
  };
  if (server.hasArg("forma"))
    cfg.forma = server.arg("forma") == "circular" ? CIRCULAR : RECTANGULAR;
  cfg.largo = num("largo", cfg.largo);
  cfg.ancho = num("ancho", cfg.ancho);
  cfg.diametro = num("diametro", cfg.diametro);
  cfg.mlVuelco = num("ml", cfg.mlVuelco);
  if (server.hasArg("superficie")) {
    String s = server.arg("superficie");
    s.replace(',', '.');
    cfg.superficie = max(0.0f, s.toFloat());
  }
  guardarConfig();
  dibujarPantalla();
  handleEstado();
}

void handleReset() {
  resetearConteo();
  dibujarPantalla();
  handleEstado();
}

void iniciarWiFi() {
  if (cfg.ssid.length()) {
    Serial.printf("Conectando a '%s' (clave de %u caracteres)", cfg.ssid.c_str(),
                  cfg.pass.length());
    WiFi.onEvent(
        [](WiFiEvent_t, WiFiEventInfo_t info) {
          motivoDesconexion = info.wifi_sta_disconnected.reason;
        },
        ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    if (pantallaOk) {
      oled.clearDisplay();
      oled.setCursor(0, 0);
      oled.print("Conectando WiFi...");
      oled.display();
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    uint32_t inicio = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
      delay(500);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      modoAP = false;
      Serial.printf("Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
      return;
    }
    Serial.printf("No se pudo conectar. Motivo %u: %s\n", motivoDesconexion,
                  textoMotivo(motivoDesconexion));
    WiFi.disconnect();
    escanearRedes();
    Serial.println(F("Se activa el punto de acceso."));
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  modoAP = true;
  Serial.printf("Punto de acceso '%s' (clave %s)  IP: %s\n", AP_SSID, AP_PASS,
                WiFi.softAPIP().toString().c_str());
}

void iniciarServidor() {
  server.on("/", [] { server.send_P(200, "text/html", PAGINA_WEB); });
  server.on("/api/estado", HTTP_GET, handleEstado);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.begin();
}

// ---------------- DHT11 ----------------
void leerDHT() {
  static uint32_t ultimaLectura = 0;
  if (millis() - ultimaLectura < DHT_INTERVALO_MS) return;
  ultimaLectura = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  bool ok = !isnan(t) && !isnan(h);
  bool cambio = ok != dhtOk || (ok && (t != temperatura || h != humedad));
  dhtOk = ok;
  if (ok) {
    temperatura = t;
    humedad = h;
  }
  if (cambio) dibujarPantalla();
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  cargarConfig();

  Wire.begin(PIN_SDA, PIN_SCL);
  pantallaOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!pantallaOk) Serial.println(F("OLED no encontrada (revisa cableado/direccion)."));
  else oled.clearDisplay();

  dht.begin();

  pinMode(PIN_REED, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_REED), onReed, FALLING);

  iniciarWiFi();
  iniciarServidor();

  imprimirAyuda();
  imprimirEstado();
  dibujarPantalla();
}

void loop() {
  leerSerie();
  server.handleClient();
  leerDHT();

  noInterrupts();
  uint32_t nuevos = vuelcosISR;
  vuelcosISR = 0;
  interrupts();

  if (nuevos) {
    vuelcos += nuevos;
    Serial.printf("Vuelco! total=%lu  V=%.2f ml  h=%.2f mm\n",
                  (unsigned long)vuelcos, volumenMl(), lluviaMm());
    dibujarPantalla();
  }

  // Guarda el conteo en flash como mucho cada 30 s para no desgastarla
  static uint32_t ultimoGuardado = 0;
  if (vuelcos != vuelcosGuardados && millis() - ultimoGuardado > 30000) {
    guardarVuelcos();
    ultimoGuardado = millis();
  }
}
