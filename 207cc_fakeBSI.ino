// ==================================================
// 207cc fakeBSI - oživenie pôvodného displeja (EMF) na stole
//
// Arduino + MCP2515 sa tvári ako BSI: posiela rámce, podľa ktorých
// displej zistí "zapaľovanie je zapnuté" a rozsvieti sa.
// Zároveň počúva, čo displej vysiela späť (0x167, 0x0DF, 0x3F6...),
// takže je hneď vidno, či žije - aj keby ostal tmavý.
//
// Zapojenie displeja (podľa wiki PSAVanCanBridge - pred pripojením
// 12 V si over, ktorý konektor máš):
//   12-pin (EMF-A/C):  1 = +12 V   3 = GND   9 = CAN-H   7 = CAN-L
//    6-pin (EMF-C):    1 = +12 V   3 = GND   4 = CAN-H   6 = CAN-L
// GND zdroja 12 V musí byť spojená s GND Arduina.
//
// Po štarte sa najprv overí SPI spojenie Arduino <-> MCP2515, až potom
// sa vysiela. Ovládanie cez Serial 115200 - príkaz "h" vypíše zoznam.
//
// --------------------------------------------------
// EN: 207cc fakeBSI - bringing the original display (EMF) to life on the bench
//
// Arduino + MCP2515 pretends to be the BSI: it sends the frames that tell
// the display "ignition is on", so it lights up. It also listens to what
// the display sends back (0x167, 0x0DF, 0x3F6...), so you can tell right
// away whether it is alive - even if the screen stays dark.
//
// Display wiring (from the PSAVanCanBridge wiki - check which connector
// you have before applying 12 V):
//   12-pin (EMF-A/C):  1 = +12 V   3 = GND   9 = CAN-H   7 = CAN-L
//    6-pin (EMF-C):    1 = +12 V   3 = GND   4 = CAN-H   6 = CAN-L
// The 12 V supply GND must be tied to the Arduino GND.
//
// On start the Arduino <-> MCP2515 SPI link is tested first; nothing is
// transmitted until it passes. Control over Serial at 115200 - command
// "h" prints the list (all Serial texts are in Slovak).
// ==================================================

#include <SPI.h>
#include <mcp_can.h>

#define CAN_CS 10
MCP_CAN CAN(CAN_CS);

// --------------------------------------------------
// Stav "auta" - mení sa príkazmi cez Serial
// EN: "Car" state - changed by Serial commands
// --------------------------------------------------

bool ignition = true;
bool lights = false;      // zapnuté svetlá = displej stmavne na "brightness" / lights on = display dims to "brightness"
bool economy = false;
bool txEnabled = true;    // false = ticho na zbernici (BSI "spí") / false = silent bus (BSI "asleep")
byte brightness = 15;     // 0..15
int outTemp = 21;         // °C

// --------------------------------------------------
// Rámce BSI -> displej (rozloženie podľa autowp.github.io)
// EN: BSI -> display frames (layout from autowp.github.io)
// --------------------------------------------------

// 0x036: zapaľovanie + podsvietenie
//   B2 bit7 = economy mode
//   B3      = jas 0-15, bit5 = svetlá zapnuté
//   B4      = režim siete: 1 = normál, 2 = čoskoro spánok
// EN: 0x036: ignition + backlight
//   B2 bit7 = economy mode
//   B3      = brightness 0-15, bit5 = lights on
//   B4      = network mode: 1 = normal, 2 = going to sleep soon
byte data036[8] = {
  0x0E, 0x00, 0x00, 0x0F,
  0x01, 0x00, 0x00, 0xA0
};

// 0x0F6: B0 bit3 = kľúč v polohe zapaľovania, B1 = teplota vody,
//   B2-4 = km, B5/B6 = vonkajšia teplota (T + 40) * 2
// EN: 0x0F6: B0 bit3 = key in ignition position, B1 = coolant temperature,
//   B2-4 = odometer, B5/B6 = outside temperature (T + 40) * 2
byte data0F6[8] = {
  0x88, 0x7E, 0x06, 0x01,
  0xA7, 0x6D, 0x6D, 0x28
};

// 0x1A1: info hláška - tento payload = žiadna hláška
// EN: 0x1A1: info message - this payload = no message
byte data1A1[8] = {
  0x7F, 0xFF, 0x00, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF
};

// 0x221: palubný počítač - okamžitá spotreba 6.5 l, dojazd 600 km
//   B0 bit3 = stlačené tlačidlo na páčke stieračov
// EN: 0x221: trip computer - instant consumption 6.5 l, range 600 km
//   B0 bit3 = wiper stalk button pressed
byte data221[7] = {
  0x00, 0x00, 0x41, 0x02,
  0x58, 0xFF, 0xFF
};

// 0x2A1 / 0x261: trip 1 / trip 2
//   B0 = priem. rýchlosť, B1-2 = km, B3-4 = priem. spotreba * 10
// EN: B0 = average speed, B1-2 = distance in km, B3-4 = average consumption * 10
byte data2A1[7] = {
  0x34, 0x00, 0x7B, 0x00,
  0x44, 0x00, 0x00
};

byte data261[7] = {
  0x2D, 0x04, 0xD2, 0x00,
  0x47, 0x00, 0x00
};

// 0x3E5: tlačidlá na rádiu (MENU/OK/ESC/šípky).
// Neposiela sa pravidelne, len na príkaz "k".
// EN: 0x3E5: radio faceplate buttons (MENU/OK/ESC/arrows).
// Not sent periodically, only on the "k" command.
byte data3E5[6] = {
  0x00, 0x00, 0x00,
  0x00, 0x00, 0x00
};

// --------------------------------------------------
// Pravidelne vysielané rámce
// EN: Periodically transmitted frames
// --------------------------------------------------

struct Periodic
{
  unsigned int id;
  byte len;
  byte *data;
  unsigned int interval;
  unsigned long last;
};

Periodic periodic[] = {
  { 0x036, 8, data036, 100, 0 },
  { 0x0F6, 8, data0F6, 500, 0 },
  { 0x1A1, 8, data1A1, 200, 0 },
  { 0x221, 7, data221, 1000, 0 },
  { 0x2A1, 7, data2A1, 1000, 0 },
  { 0x261, 7, data261, 1000, 0 }
};

const byte periodicCount =
  sizeof(periodic) / sizeof(periodic[0]);

// --------------------------------------------------
// Prijaté rámce - posledný payload podľa ID
// EN: Received frames - last payload per ID
// --------------------------------------------------

#define MAX_SEEN 16

struct Seen
{
  unsigned int id;
  byte len;
  byte data[8];
};

Seen seen[MAX_SEEN];
byte seenCount = 0;

bool verbose = false;     // true = vypisuj každý prijatý rámec / true = print every received frame

// --------------------------------------------------
// Diagnostika zbernice
// EN: Bus diagnostics
// --------------------------------------------------

enum
{
  BUS_UNKNOWN,
  BUS_NOACK,      // nikto nepotvrdzuje naše rámce / nobody ACKs our frames
  BUS_ACKONLY,    // displej potvrdzuje, ale nevysiela / display ACKs but sends nothing
  BUS_ALIVE,      // displej vysiela platné rámce / display sends valid frames
  BUS_ERRORS,     // rušenie alebo chyby na zbernici / noise or errors on the bus
  BUS_SILENT      // vysielanie vypnuté príkazom "z" / TX disabled by the "z" command
};

byte busState = BUS_UNKNOWN;

unsigned long txOk = 0;
unsigned long txFail = 0;
unsigned long rxCount = 0;
unsigned long lastTxOk = 0;
unsigned long lastRx = 0;
unsigned long lastCheck = 0;
byte lastRec = 0;

// tlačidlá sa po chvíli samy "pustia"
// EN: buttons "release" themselves after a moment
bool buttonDown = false;
bool tripDown = false;
unsigned long buttonAt = 0;
unsigned long tripAt = 0;

char line[40];
byte lineLen = 0;
unsigned long lastChar = 0;

// posledné prečítané CANSTAT / CANCTRL a príznak, že SPI vracia nezmysly
// EN: last CANSTAT / CANCTRL read and a flag that SPI is returning garbage
bool spiFault = false;
byte lastStat = 0;
byte lastCtrl = 0;

// --------------------------------------------------

void printHex(byte value)
{
  if (value < 0x10)
    Serial.print("0");

  Serial.print(value, HEX);
}

// --------------------------------------------------
// Priamy prístup k registrom MCP2515, mimo knižnice. Knižnica sama
// nepozná, že SPI vracia nezmysly: "Entering Configuration Mode
// Successful!" vypíše aj vtedy a nezmysly potom vyzerajú ako prijaté rámce.
// EN: Direct access to MCP2515 registers, bypassing the library. The library
// cannot tell that SPI is returning garbage: it still prints "Entering
// Configuration Mode Successful!" and the garbage then looks like received frames.
// --------------------------------------------------

byte mcpRead(byte address)
{
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CAN_CS, LOW);
  SPI.transfer(0x03);
  SPI.transfer(address);
  byte value = SPI.transfer(0x00);
  digitalWrite(CAN_CS, HIGH);
  SPI.endTransaction();

  return value;
}

void mcpWrite(byte address, byte value)
{
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CAN_CS, LOW);
  SPI.transfer(0x02);
  SPI.transfer(address);
  SPI.transfer(value);
  digitalWrite(CAN_CS, HIGH);
  SPI.endTransaction();
}

// --------------------------------------------------
// V normálnom režime je CANCTRL presne 0x07 a v CANSTAT sú bity 7-5,
// 4 a 0 nulové. Čokoľvek iné = SPI vracia nezmysly, alebo sa čip resetol.
// EN: In normal mode CANCTRL is exactly 0x07 and CANSTAT bits 7-5, 4 and 0
// are zero. Anything else = SPI returns garbage, or the chip has reset itself.
// --------------------------------------------------

bool spiSane()
{
  lastStat = mcpRead(0x0E);
  lastCtrl = mcpRead(0x0F);

  return lastCtrl == 0x07 && (lastStat & 0xF1) == 0x00;
}

// --------------------------------------------------
// Test SPI v troch krokoch, každý ukazuje na inú chybu zapojenia
// EN: SPI test in three steps, each pointing at a different wiring fault
// --------------------------------------------------

bool testSPI()
{
  const byte pattern[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
  bool idleOk = true;
  bool writeOk = true;
  bool silent = true;

  pinMode(CAN_CS, OUTPUT);
  digitalWrite(CAN_CS, HIGH);
  SPI.begin();

  // pull-up: keď MISO nikto nebudí, číta sa 0xFF namiesto náhodných hodnôt
  // EN: pull-up: with nobody driving MISO it reads 0xFF instead of random values
  pinMode(MISO, INPUT_PULLUP);

  Serial.println(F("Test SPI spojenia s MCP2515:"));

  // 1) čip nie je vybraný (CS = HIGH), takže MISO nesmie budiť
  // EN: 1) chip is not selected (CS = HIGH), so it must not drive MISO
  Serial.print(F("  bez CS:        "));
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

  for (byte i = 0; i < 8; i++)
  {
    byte value = SPI.transfer(0x00);
    printHex(value);
    Serial.print(" ");

    if (value != 0xFF)
      idleOk = false;
  }

  SPI.endTransaction();
  Serial.println(F(" (ma byt FF)"));

  // 2) po resete musí byť CANSTAT = 0x80 a CANCTRL = 0x87
  // EN: 2) after reset CANSTAT must be 0x80 and CANCTRL 0x87
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CAN_CS, LOW);
  SPI.transfer(0xC0);
  digitalWrite(CAN_CS, HIGH);
  SPI.endTransaction();
  delay(10);

  byte stat = mcpRead(0x0E);
  byte ctrl = mcpRead(0x0F);

  Serial.print(F("  po resete:     "));
  printHex(stat);
  Serial.print(" ");
  printHex(ctrl);
  Serial.println(F("  (ma byt 80 87)"));

  // 3) zápis do dát TXB0 a spätné čítanie
  // EN: 3) write to the TXB0 data bytes and read them back
  for (byte i = 0; i < 8; i++)
    mcpWrite(0x36 + i, pattern[i]);

  Serial.print(F("  zapis/citanie: "));

  for (byte i = 0; i < 8; i++)
  {
    byte value = mcpRead(0x36 + i);
    printHex(value);
    Serial.print(" ");

    if (value != pattern[i])
      writeOk = false;

    if (value != 0xFF)
      silent = false;
  }

  Serial.println(F(" (ma byt 11 22 .. 88)"));

  if (idleOk && stat == 0x80 && ctrl == 0x87 && writeOk)
  {
    Serial.println(F("  -> SPI OK"));
    return true;
  }

  Serial.print(F("  -> SPI CHYBA: "));

  if (!idleOk)
    Serial.println(F("modul vysiela, aj ked nie je vybrany - jeho pin CS nie je spojeny s D10"));
  else if (silent && stat == 0xFF && ctrl == 0xFF)
    Serial.println(F("ziadna odpoved - napajanie modulu, SCK (D13), SI (D11), SO (D12)"));
  else
    Serial.println(F("nespolahlive spojenie - vsetky styri SPI vodice, 5 V a GND"));

  return false;
}

// --------------------------------------------------
// MCP_STDEXT = prijímať len platné rámce (MCP_ANY ukladá aj úlomky
// rámcov s chybou). Skúša dokola, kým SPI neprejde testom.
// EN: MCP_STDEXT = receive valid frames only (MCP_ANY also stores fragments
// of errored frames). Retries until the SPI test passes.
// --------------------------------------------------

void connectMCP()
{
  for (;;)
  {
    while (!testSPI())
      delay(3000);

    if (CAN.begin(MCP_STDEXT, CAN_125KBPS, MCP_8MHZ) == CAN_OK)
      break;

    Serial.println(F("CAN ERROR"));
    delay(3000);
  }

  CAN.setMode(MCP_NORMAL);

  spiFault = false;
  busState = BUS_UNKNOWN;
  lastTxOk = txOk;
  lastRec = 0;
  lastCheck = millis();
}

// --------------------------------------------------

// Bez ACK od displeja ostane rámec visieť v MCP2515 a vráti sa chyba,
// takže návratová hodnota = "počúva na zbernici niekto?"
// EN: Without an ACK from the display the frame stays pending in the MCP2515
// and an error is returned, so the return value = "is anyone listening on the bus?"
bool sendFrame(unsigned int id, byte len, byte *data)
{
  if (CAN.sendMsgBuf(id, 0, len, data) == CAN_OK)
  {
    txOk++;
    return true;
  }

  txFail++;
  return false;
}

// --------------------------------------------------

void printFrame(unsigned int id, byte len, byte *data)
{
  if (id < 0x100)
    Serial.print("0");
  if (id < 0x10)
    Serial.print("0");
  Serial.print(id, HEX);
  Serial.print("  ");

  for (byte i = 0; i < len; i++)
  {
    printHex(data[i]);
    Serial.print(" ");
  }
}

// --------------------------------------------------
// Jednorazové rámce (príkazy) - s výpisom, či ich niekto potvrdil
// EN: One-shot frames (commands) - prints whether anyone acknowledged them
// --------------------------------------------------

void sendAndPrint(unsigned int id, byte len, byte *data)
{
  bool ok = sendFrame(id, len, data);

  Serial.print(F("TX "));
  printFrame(id, len, data);
  Serial.println(ok ? F(" OK") : F(" bez ACK"));
}

// --------------------------------------------------

const __FlashStringHelper *frameName(unsigned int id)
{
  switch (id)
  {
    case 0x167: return F("EMF stav");
    case 0x0DF: return F("EMF menu / popup");
    case 0x3F6: return F("EMF datum a cas");
    case 0x5E5: return F("EMF verzia");
    case 0x15B: return F("EMF zapis nastaveni");
    case 0x228: return F("EMF nastavenie hodin");
  }

  return F("?");
}

// --------------------------------------------------
// true = rámec je nový alebo sa zmenil, oplatí sa ho vypísať
// EN: true = the frame is new or has changed, worth printing
// --------------------------------------------------

bool rememberFrame(unsigned int id, byte len, byte *data)
{
  for (byte i = 0; i < seenCount; i++)
  {
    if (seen[i].id != id)
      continue;

    if (seen[i].len == len && memcmp(seen[i].data, data, len) == 0)
      return false;

    seen[i].len = len;
    memcpy(seen[i].data, data, len);

    // 0x3F6 je čas - mení sa každú sekundu, stačí ho vidieť raz
    // EN: 0x3F6 is the clock - it changes every second, seeing it once is enough
    return id != 0x3F6;
  }

  // plná tabuľka: radšej vypisovať stále, než nové ID zamlčať
  // EN: table full: better to print every time than to hide a new ID
  if (seenCount == MAX_SEEN)
    return true;

  seen[seenCount].id = id;
  seen[seenCount].len = len;
  memcpy(seen[seenCount].data, data, len);
  seenCount++;

  return true;
}

// --------------------------------------------------

void readCAN()
{
  unsigned long id;
  byte len;

  // knižnica kopíruje toľko bajtov, koľko udáva DLC (až 15)
  // EN: the library copies as many bytes as the DLC says (up to 15)
  byte buf[16];

  // najviac 8 rámcov naraz, aby záplava na zbernici nezastavila vysielanie
  // EN: at most 8 frames per call, so a flood on the bus cannot stall transmission
  for (byte n = 0; n < 8 && CAN.checkReceive() == CAN_MSGAVAIL; n++)
  {
    // nezmysly z rozpadnutého SPI vyzerajú ako prijaté rámce - najprv overiť
    // EN: garbage from a broken SPI link looks like received frames - verify first
    if (n == 0 && !spiSane())
    {
      spiFault = true;
      return;
    }

    if (CAN.readMsgBuf(&id, &len, buf) != CAN_OK)
      break;

    if (len > 8)
      len = 8;

    id &= 0x7FF;
    rxCount++;
    lastRx = millis();

    if (!rememberFrame(id, len, buf) && !verbose)
      continue;

    Serial.print(F("RX "));
    printFrame(id, len, buf);
    Serial.print(" ");
    Serial.print(frameName(id));

    // 0x167 B1 bit5 = displej je zhasnutý tlačidlom DARK
    // EN: 0x167 B1 bit5 = display is blanked by the DARK button
    if (id == 0x167 && len > 1 && (buf[1] & 0x20))
      Serial.print(F(" - rezim DARK, zrus prikazom: k k"));

    Serial.println();
  }
}

// --------------------------------------------------

void sendPeriodic()
{
  unsigned long now = millis();

  for (byte i = 0; i < periodicCount; i++)
  {
    if (now - periodic[i].last < periodic[i].interval)
      continue;

    periodic[i].last = now;
    sendFrame(periodic[i].id, periodic[i].len, periodic[i].data);

    // MCP2515 má len 2 prijímacie buffre - vyberať priebežne
    // EN: the MCP2515 has only 2 receive buffers - drain them as we go
    readCAN();
  }
}

// --------------------------------------------------

void updateFrames()
{
  data036[2] = economy ? 0x80 : 0x00;
  data036[3] = brightness | (lights ? 0x20 : 0x00);
  data036[4] = ignition ? 0x01 : 0x02;

  data0F6[0] = ignition ? 0x88 : 0x80;
  data0F6[5] = (outTemp + 40) * 2;
  data0F6[6] = data0F6[5];
}

// --------------------------------------------------

void printCarState()
{
  Serial.print(F("zapalovanie="));
  Serial.print(ignition ? F("ZAP") : F("VYP"));
  Serial.print(F(" svetla="));
  Serial.print(lights ? F("ZAP") : F("VYP"));
  Serial.print(F(" jas="));
  Serial.print(brightness);
  Serial.print(F(" economy="));
  Serial.print(economy ? F("ZAP") : F("VYP"));
  Serial.print(F(" teplota="));
  Serial.print(outTemp);
  Serial.print(F(" vysielanie="));
  Serial.println(txEnabled ? F("ZAP") : F("VYP"));
}

// --------------------------------------------------

void printCounters()
{
  Serial.print(F("TEC="));
  Serial.print(CAN.errorCountTX());
  Serial.print(F(" REC="));
  Serial.print(CAN.errorCountRX());
  Serial.print(F(" EFLG=0x"));
  Serial.println(CAN.getError(), HEX);
}

// --------------------------------------------------

void printStatus()
{
  Serial.print(F("[zbernica] "));

  switch (busState)
  {
    case BUS_ALIVE:
      Serial.println(F("DISPLEJ ZIJE - vysiela platne ramce."));
      break;

    case BUS_ACKONLY:
      Serial.println(F("Displej ramce potvrdzuje (ACK), sam zatial nevysiela."));
      break;

    case BUS_NOACK:
      Serial.println(F("ZIADNY ACK - ticho, ramce nikto neprijima."));
      Serial.println(F("  Skontroluj: 12 V + GND na displeji, spolocnu GND s Arduinom."));
      break;

    case BUS_ERRORS:
      Serial.println(F("CHYBY NA ZBERNICI - signal tam je, ale nie platne ramce."));
      Serial.println(F("  Skus postupne (po kazdom kroku reset Arduina):"));
      Serial.println(F("  1) prehodit CAN-H/CAN-L  2) jumper 120R opacne  3) spolocna GND"));
      break;

    case BUS_SILENT:
      Serial.println(F("Vysielanie je vypnute (prikaz z)."));
      break;

    default:
      Serial.println(F("zatial nezisteny stav"));
      break;
  }

  Serial.print(F("  TX ok="));
  Serial.print(txOk);
  Serial.print(F(" fail="));
  Serial.print(txFail);
  Serial.print(F("  RX="));
  Serial.print(rxCount);
  Serial.print("  ");
  printCounters();
}

// --------------------------------------------------
// Raz za sekundu vyhodnotí stav, vypisuje len zmenu.
// Čisté "nikto nepotvrdzuje" = TEC ostane stáť na 128..135 a REC je 0;
// čokoľvek iné pri neúspešnom vysielaní znamená rušenie na zbernici.
// EN: Evaluates the bus state once a second and prints only changes.
// A clean "nobody acknowledges" = TEC settles at 128..135 and REC is 0;
// anything else while transmission fails means disturbance on the bus.
// --------------------------------------------------

void checkBus()
{
  if (!spiSane())
  {
    spiFault = true;
    return;
  }

  byte state;
  byte tec = CAN.errorCountTX();
  byte rec = CAN.errorCountRX();
  bool noAck = txEnabled && txOk == lastTxOk;

  if (rxCount > 0 && millis() - lastRx < 3000)
    state = BUS_ALIVE;
  else if (rec > lastRec || rec >= 128 || (noAck && (tec < 128 || tec > 135)))
    state = BUS_ERRORS;
  else if (!txEnabled)
    state = BUS_SILENT;
  else if (noAck)
    state = BUS_NOACK;
  else
    state = BUS_ACKONLY;

  lastTxOk = txOk;
  lastRec = rec;

  if (state == busState)
    return;

  busState = state;
  printStatus();
}

// --------------------------------------------------
// EN: commands (the help text below is printed in Slovak)
//   i          ignition on/off
//   l          lights on/off (display dims)
//   b 0-15     brightness with lights on
//   t -40..87  outside temperature
//   e          economy mode on/off
//   k X        radio button: m=MENU o=OK e=ESC k=DARK u d l r=arrows
//   p          trip computer button (wiper stalk)
//   m 78       info message with the given code (hex), plain m = clear
//   x ID B0..  one-shot raw frame (hex), e.g. x 3E5 40 00 00 00 00 00
//   z          transmission on/off (silence = BSI asleep)
//   v          print every received frame / changes only
//   s          bus status
// --------------------------------------------------

void printHelp()
{
  Serial.println(F("Prikazy:"));
  Serial.println(F("  i         zapalovanie zap/vyp"));
  Serial.println(F("  l         svetla zap/vyp (displej stmavne)"));
  Serial.println(F("  b 0-15    jas pri zapnutych svetlach"));
  Serial.println(F("  t -40..87 vonkajsia teplota"));
  Serial.println(F("  e         economy mode zap/vyp"));
  Serial.println(F("  k X       tlacidlo radia: m=MENU o=OK e=ESC k=DARK u d l r=sipky"));
  Serial.println(F("  p         tlacidlo palubneho pocitaca (packa stieracov)"));
  Serial.println(F("  m 78      info hlaska s kodom (hex), samotne m = zrusit"));
  Serial.println(F("  x ID B0.. jednorazovy ramec (hex), napr. x 3E5 40 00 00 00 00 00"));
  Serial.println(F("  z         vysielanie zap/vyp (ticho = BSI spi)"));
  Serial.println(F("  v         vypis vsetkych prijatych ramcov / len zmien"));
  Serial.println(F("  s         stav zbernice"));
}

// --------------------------------------------------
// 0x3E5: B0 bit6 MENU | B2 bit6 OK, bit4 ESC, bit2 DARK
//        B5 bit6 hore, bit4 dole, bit2 vpravo, bit0 vľavo
// EN:    B5 bit6 up, bit4 down, bit2 right, bit0 left
// --------------------------------------------------

void pressButton(char button)
{
  memset(data3E5, 0, sizeof(data3E5));

  switch (button)
  {
    case 'm': data3E5[0] = 0x40; break;
    case 'o': data3E5[2] = 0x40; break;
    case 'e': data3E5[2] = 0x10; break;
    case 'k': data3E5[2] = 0x04; break;
    case 'u': data3E5[5] = 0x40; break;
    case 'd': data3E5[5] = 0x10; break;
    case 'r': data3E5[5] = 0x04; break;
    case 'l': data3E5[5] = 0x01; break;

    default:
      Serial.println(F("Tlacidla: m o e k u d l r"));
      return;
  }

  sendAndPrint(0x3E5, 6, data3E5);
  buttonAt = millis();
  buttonDown = true;
}

// --------------------------------------------------

void sendRaw(char *text)
{
  byte data[8];
  byte len = 0;
  char *end;

  unsigned int id = strtoul(text, &end, 16);

  if (end == text)
  {
    Serial.println(F("Pouzitie: x ID B0 B1 ... (hex)"));
    return;
  }

  while (len < 8)
  {
    text = end;
    unsigned long value = strtoul(text, &end, 16);

    if (end == text)
      break;

    data[len++] = value;
  }

  sendAndPrint(id, len, data);
}

// --------------------------------------------------

void handleCommand(char *text)
{
  char *arg = text + 1;

  while (*arg == ' ')
    arg++;

  switch (text[0])
  {
    case 'i': ignition = !ignition; break;
    case 'l': lights = !lights; break;
    case 'e': economy = !economy; break;
    case 'z': txEnabled = !txEnabled; break;

    case 'b':
      brightness = constrain(atoi(arg), 0, 15);
      break;

    case 't':
      outTemp = constrain(atoi(arg), -40, 87);
      break;

    case 'm':
      // EXPERIMENTÁLNE: marker 0xC6 a B3 sú odpozorované z jedinej
      // hlášky na aute (airbag, kód 0x78), pre iné kódy neoverené
      // EN: EXPERIMENTAL: marker 0xC6 and B3 were observed on a single message
      // in the car (airbag, code 0x78), not verified for other codes
      if (*arg)
      {
        data1A1[0] = 0x80;
        data1A1[1] = strtoul(arg, NULL, 16);
        data1A1[2] = 0xC6;
        data1A1[3] = 0x80;
      }
      else
      {
        data1A1[0] = 0x7F;
        data1A1[1] = 0xFF;
        data1A1[2] = 0x00;
        data1A1[3] = 0xFF;
      }
      sendAndPrint(0x1A1, 8, data1A1);
      return;

    case 'p':
      data221[0] |= 0x08;
      sendAndPrint(0x221, 7, data221);
      tripAt = millis();
      tripDown = true;
      return;

    case 'v':
      verbose = !verbose;
      Serial.println(verbose ? F("RX: vsetky ramce") : F("RX: len zmeny"));
      return;

    case 'k': pressButton(*arg); return;
    case 'x': sendRaw(arg); return;
    case 's': printStatus(); return;

    default:
      printHelp();
      return;
  }

  updateFrames();
  printCarState();
}

// --------------------------------------------------
// Riadok končí NL/CR, alebo 50 ms pauzou (Serial Monitor
// nastavený na "No line ending")
// EN: A line ends with NL/CR, or with a 50 ms pause (Serial Monitor
// set to "No line ending")
// --------------------------------------------------

void readSerial()
{
  while (Serial.available())
  {
    char c = Serial.read();
    lastChar = millis();

    if (c != '\n' && c != '\r')
    {
      if (lineLen < sizeof(line) - 1)
        line[lineLen++] = c;

      continue;
    }

    if (lineLen > 0)
    {
      line[lineLen] = 0;
      lineLen = 0;
      handleCommand(line);
    }
  }

  if (lineLen > 0 && millis() - lastChar >= 50)
  {
    line[lineLen] = 0;
    lineLen = 0;
    handleCommand(line);
  }
}

// --------------------------------------------------

void setup()
{
  Serial.begin(115200);

  while (!Serial)
    ;

  Serial.println();
  Serial.println(F("=============================="));
  Serial.println(F("Peugeot 207cc fake BSI"));
  Serial.println(F("=============================="));

  updateFrames();
  connectMCP();

  Serial.println();
  printHelp();
  Serial.println();
}

// --------------------------------------------------

void loop()
{
  // SPI vracia nezmysly alebo sa čip resetol: nič neposielať ani
  // nevypisovať, kým spojenie znova neprejde testom
  // EN: SPI returns garbage or the chip has reset: send and print nothing
  // until the link passes the test again
  if (spiFault)
  {
    Serial.print(F("MCP2515 neodpoveda spravne (CANSTAT="));
    printHex(lastStat);
    Serial.print(F(" CANCTRL="));
    printHex(lastCtrl);
    Serial.println(F(")"));

    if (lastStat == 0x80 && lastCtrl == 0x87)
      Serial.println(F("  = cip sa sam resetol, vypadok napajania 5 V"));

    connectMCP();
    return;
  }

  readCAN();
  readSerial();

  // až po príkazoch - tie si zapisujú vlastný millis()
  // EN: only after the commands - they store their own millis()
  unsigned long now = millis();

  // -----------------------------------------------
  // Pravidelné CAN správy
  // EN: Periodic CAN messages
  // -----------------------------------------------

  if (txEnabled)
    sendPeriodic();

  // -----------------------------------------------
  // Pustenie tlačidiel
  // EN: Releasing the buttons
  // -----------------------------------------------

  if (buttonDown && now - buttonAt >= 200)
  {
    buttonDown = false;
    memset(data3E5, 0, sizeof(data3E5));
    sendFrame(0x3E5, 6, data3E5);
  }

  if (tripDown && now - tripAt >= 300)
  {
    tripDown = false;
    data221[0] &= ~0x08;
    sendFrame(0x221, 7, data221);
  }

  // -----------------------------------------------
  // Diagnostika raz za sekundu
  // EN: Diagnostics once a second
  // -----------------------------------------------

  if (now - lastCheck >= 1000)
  {
    lastCheck = now;
    checkBus();
  }
}
