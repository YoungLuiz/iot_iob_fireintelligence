/*
  Fire Intelligence · Estação de campo simulada (ESP32 no Wokwi)
  Global Solution FIAP 2026.2 · Physical Computing: IoT & IoB

  O que a estação faz
  - IoT: lê temperatura e umidade do ar (DHT22) e publica a cada 10 s via MQTT.
  - IoB: conta passagens de pessoas na entrada de uma trilha (PIR), sem identificar ninguém,
         e registra relatos de fumaça feitos por brigadistas ou moradores (botão).
  - Feedback: LED vermelho + bipe confirmam que o relato foi enviado.
    Sem conexão, o relato fica na fila e o LED pisca até ser enviado.

  O que a estação NÃO faz
  - Não classifica risco nem usa limiares ambientais. Ela só mede e relata.
  - Todas as leituras são SIMULADAS (Wokwi) e vão marcadas com "simulado": true.

  Bibliotecas (libraries.txt): DHT sensor library for ESPx, PubSubClient,
  LiquidCrystal I2C, ArduinoJson
*/
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

// ---------- Configuração --------------------------------------------------
const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_SENHA  = "";
const char* MQTT_HOST   = "broker.hivemq.com";
const int   MQTT_PORTA  = 1883;

const char* EQUIPE      = "equipe-caos";      // nome da equipe (evita colisão no broker público)
const char* ESTACAO_ID  = "EST-CHAPADA-01";

const unsigned long INTERVALO_LEITURA_MS = 10000;

// ---------- Pinos ---------------------------------------------------------
const int PINO_DHT    = 15;
const int PINO_PIR    = 27;
const int PINO_BOTAO  = 4;
const int PINO_LED_OK = 25;   // verde: conectado ao broker
const int PINO_LED_RL = 26;   // vermelho: relato
const int PINO_BUZZER = 13;

// ---------- Objetos -------------------------------------------------------
DHTesp dht;
WiFiClient wifi;
PubSubClient mqtt(wifi);
LiquidCrystal_I2C lcd(0x27, 16, 2);

String topicoLeituras, topicoEventos, topicoStatus;

// ---------- Estado --------------------------------------------------------
unsigned long ultimoEnvio = 0, ultimaTelaMs = 0, ultimoBotaoMs = 0;
unsigned long seq = 0;
int  passagensIntervalo = 0;
long passagensTotal = 0;
long relatosTotal = 0;
int  relatosPendentes = 0;          // relatos feitos sem conexão
bool pirAnterior = false;
bool botaoAnterior = HIGH;
float tempAtual = NAN, umidAtual = NAN;
bool telaAlterna = false;

// ---------- Conexão -------------------------------------------------------
void conectarWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);   // canal 6 acelera a conexão no Wokwi
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.println(" ok");
}

bool conectarMQTT() {
  if (mqtt.connected()) return true;
  String clientId = String("fireintel-") + ESTACAO_ID + "-" + String(random(0xffff), HEX);
  // "last will": se a estação cair, o broker publica "offline"
  if (mqtt.connect(clientId.c_str(), topicoStatus.c_str(), 1, true, "offline")) {
    mqtt.publish(topicoStatus.c_str(), "online", true);
    Serial.println("MQTT conectado");
    return true;
  }
  Serial.printf("MQTT falhou (rc=%d)\n", mqtt.state());
  return false;
}

// ---------- Publicações ---------------------------------------------------
void publicarLeitura() {
  TempAndHumidity th = dht.getTempAndHumidity();
  tempAtual = th.temperature; umidAtual = th.humidity;

  // Ordem pensada para leitura humana: quem, o quê, ambiente (IoT), pessoas (IoB), controle
  JsonDocument doc;
  doc["estacao"] = ESTACAO_ID;
  doc["tipo"] = "leitura";
  doc["seq"] = ++seq;
  if (isnan(tempAtual)) doc["temperatura_c"] = nullptr; else doc["temperatura_c"] = round(tempAtual * 10) / 10.0;
  if (isnan(umidAtual)) doc["umidade_pct"] = nullptr;   else doc["umidade_pct"]   = round(umidAtual * 10) / 10.0;
  doc["passagens_intervalo"] = passagensIntervalo;   // IoB: fluxo de pessoas, anônimo
  doc["passagens_total"] = passagensTotal;
  doc["relatos_total"] = relatosTotal;
  doc["intervalo_s"] = INTERVALO_LEITURA_MS / 1000;
  doc["uptime_s"] = millis() / 1000;
  doc["simulado"] = true;

  char buf[512];
  serializeJsonPretty(doc, buf, sizeof(buf));   // JSON indentado: fica legível no app MQTT
  bool publicou = mqtt.connected() && mqtt.publish(topicoLeituras.c_str(), buf);
  if (publicou) {
    Serial.printf("[IoT] Leitura publicada    | seq:%lu | T:%.1fC U:%.0f%% | Pass:%d\n",
      seq, tempAtual, umidAtual, passagensIntervalo);
    passagensIntervalo = 0;     // só zera se a leitura saiu
  } else if (!mqtt.connected()) {
    Serial.printf("[IoT] Leitura NAO enviada  | sem conexao MQTT (estado=%d)\n", mqtt.state());
  } else {
    Serial.printf("[IoT] Leitura NAO enviada  | falha ao publicar (%u bytes)\n", (unsigned)strlen(buf));
  }
}

bool publicarRelato() {
  JsonDocument doc;
  doc["estacao"] = ESTACAO_ID;
  doc["tipo"] = "relato_fumaca";
  doc["origem"] = "botao_campo";
  doc["relatos_total"] = relatosTotal;
  doc["uptime_s"] = millis() / 1000;
  doc["simulado"] = true;
  char buf[300];
  serializeJsonPretty(doc, buf, sizeof(buf));
  bool ok = mqtt.connected() && mqtt.publish(topicoEventos.c_str(), buf);
  if (ok) Serial.printf("[IoB] Relato enviado       | total: %ld\n", relatosTotal);
  else    Serial.printf("[IoB] Relato na fila       | total: %ld (sem conexao)\n", relatosTotal);
  return ok;
}

void confirmarRelato() {        // feedback ao usuário (parte "behavior" do IoB)
  for (int i = 0; i < 2; i++) {
    digitalWrite(PINO_LED_RL, HIGH); tone(PINO_BUZZER, 1500, 120); delay(150);
    digitalWrite(PINO_LED_RL, LOW);  delay(100);
  }
}

// ---------- Tela ----------------------------------------------------------
void atualizarTela() {
  lcd.clear();
  lcd.setCursor(0, 0);
  if (isnan(tempAtual)) lcd.print("Sensor sem dado");
  else lcd.printf("T:%.1fC U:%.0f%%", tempAtual, umidAtual);
  lcd.setCursor(0, 1);
  if (telaAlterna) lcd.printf("Pass:%ld Rel:%ld", passagensTotal, relatosTotal);
  else if (relatosPendentes > 0) lcd.printf("Fila: %d relato", relatosPendentes);
  else lcd.print(mqtt.connected() ? "MQTT ok  SIMUL." : "Sem conexao");
  telaAlterna = !telaAlterna;
}

// ---------- Setup / loop --------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PINO_PIR, INPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  pinMode(PINO_LED_OK, OUTPUT);
  pinMode(PINO_LED_RL, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);
  dht.setup(PINO_DHT, DHTesp::DHT22);

  lcd.init(); lcd.backlight();
  lcd.print("Fire Intelligence");
  lcd.setCursor(0, 1); lcd.print(ESTACAO_ID);

  String base = String("fireintel/") + EQUIPE + "/estacoes/" + ESTACAO_ID;
  topicoLeituras = base + "/leituras";
  topicoEventos  = base + "/eventos";
  topicoStatus   = base + "/status";

  randomSeed(esp_random());

  Serial.println();
  Serial.println("=========================================");
  Serial.println("   FIRE INTELLIGENCE · Estacao de Campo  ");
  Serial.println("=========================================");
  Serial.printf ("   Estacao  : %s\n", ESTACAO_ID);
  Serial.printf ("   Equipe   : %s\n", EQUIPE);
  Serial.printf ("   Broker   : %s:%d\n", MQTT_HOST, MQTT_PORTA);
  Serial.println("-----------------------------------------");
  Serial.println("   Pinos configurados:");
  Serial.printf ("     DHT22    -> GPIO %d\n", PINO_DHT);
  Serial.printf ("     PIR      -> GPIO %d\n", PINO_PIR);
  Serial.printf ("     Botao    -> GPIO %d\n", PINO_BOTAO);
  Serial.printf ("     LED Mov  -> GPIO %d\n", PINO_LED_OK);
  Serial.printf ("     LED Rel  -> GPIO %d\n", PINO_LED_RL);
  Serial.printf ("     Buzzer   -> GPIO %d\n", PINO_BUZZER);
  Serial.println("-----------------------------------------");
  Serial.println("   IoT  : temperatura e umidade (DHT22)");
  Serial.println("   IoB  : passagens (PIR) + relatos (botao)");
  Serial.println("=========================================");
  Serial.println();

  conectarWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setBufferSize(768);   // o padrão (256 bytes) é pequeno para o JSON formatado
  conectarMQTT();

  Serial.println();
  Serial.println("-----------------------------------------");
  Serial.printf ("   Topico leituras : %s\n", topicoLeituras.c_str());
  Serial.printf ("   Topico eventos  : %s\n", topicoEventos.c_str());
  Serial.printf ("   Topico status   : %s\n", topicoStatus.c_str());
  Serial.println("-----------------------------------------");
  Serial.println("   Publicando a cada 10 s. Aguarde...");
  Serial.println("=========================================");
  Serial.println();
}

void loop() {
  unsigned long agora = millis();

  // conexão (tenta de novo a cada volta, sem travar a contagem)
  static unsigned long ultimaTentativa = 0;
  if ((WiFi.status() != WL_CONNECTED || !mqtt.connected()) && agora - ultimaTentativa > 5000) {
    ultimaTentativa = agora;
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    else conectarMQTT();
  }
  mqtt.loop();
  digitalWrite(PINO_LED_OK, mqtt.connected() ? HIGH : LOW);

  // PIR: conta uma passagem a cada borda de subida
  bool pir = digitalRead(PINO_PIR) == HIGH;
  if (pir && !pirAnterior) {
    passagensIntervalo++; passagensTotal++;
    Serial.printf("[IoB] Passagem detectada  |  total: %ld\n", passagensTotal);
  }
  pirAnterior = pir;

  // Botão de relato, com debounce de 300 ms
  bool botao = digitalRead(PINO_BOTAO);
  if (botao == LOW && botaoAnterior == HIGH && agora - ultimoBotaoMs > 300) {
    ultimoBotaoMs = agora;
    relatosTotal++;
    if (publicarRelato()) confirmarRelato(); else relatosPendentes++;
  }
  botaoAnterior = botao;

  // Fila de relatos: envia quando a conexão voltar
  if (relatosPendentes > 0 && mqtt.connected() && publicarRelato()) {
    relatosPendentes--; confirmarRelato();
  }
  if (relatosPendentes > 0) digitalWrite(PINO_LED_RL, (agora / 300) % 2);   // pisca = pendente

  if (agora - ultimoEnvio >= INTERVALO_LEITURA_MS) { ultimoEnvio = agora; publicarLeitura(); }
  if (agora - ultimaTelaMs >= 2500) { ultimaTelaMs = agora; atualizarTela(); }
  delay(20);
}
