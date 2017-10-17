/*******************************************************************
    Arduino Crypto Coin Display
    Github: https://github.com/witnessmenow/ArduinoCryptoCoinDisplay

    A project to display the values of any crypto coin
    that is avaiable through the CoinMarketCap.com API

    It will cycle through the different coins so it can be used to track
    multiple coins at once.

    What coins are displayed are configurable through Telegram messenger
    You can also set how much you hold of each coin so it will display
    how much you hold.

    Main Hardware (See readme of Github for wiring diagram):
    - ESP8266 (I used a Wemos D1 Mini Clone)
    - Nokia 5110 screen

    Written by Brian Lough (@witnessmenow)
    YoutTube: https://www.youtube.com/channel/UCezJOfu7OtqGzd5xrP3q6WA
 *******************************************************************/

#include <SPI.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include "FS.h"

// ----------------------------
// Additional Libraries - each one of these will need to be installed.
// ----------------------------

#include <Adafruit_GFX.h>
// Required for the screen
// Available on the library manager (Search for "Adafruit GFX")
// https://github.com/adafruit/Adafruit-GFX-Library

#include <Adafruit_PCD8544.h>
// Required for the screen, this is a forked version of this library though
// Download the zip of the repo and add that Sketch -> Include Library -> Add zip
// https://github.com/WereCatf/Adafruit-PCD8544-Nokia-5110-LCD-library

#include <CoinMarketCapApi.h>
// For Integrating with the CoinMarketCap.com API
// Available on the library manager (Search for "CoinMarket")
// https://github.com/witnessmenow/arduino-coinmarketcap-api

#include <UniversalTelegramBot.h>
// For sending/recieving Telegram Messages
// Available on the library manager (Search for "Universal Telegram")
// https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot

#include <ArduinoJson.h>
// Required by the CoinMarketCapApi Library for parsing the response
// Available on the library manager (Search for "arduino json")
// https://github.com/squix78/esp8266-oled-ssd1306

Adafruit_PCD8544 display = Adafruit_PCD8544(D6, D8, D4);

#define COINSHIP_CONFIG_NAME "telegramCoinship.json"
#define COINSHIP_HOLDINGS_NAME "coinHoldings.json"

// Generated Using this tool
// Options: Width 8, Height 16
// Note: I Did not use the last two rows
// http://www.introtoarduino.com/utils/pcd8544.html

static const unsigned char CURRENCY_SYMBOLS[][16] = {
  {
    B00111110, //EUR
    B00111111,
    B01110011,
    B01100001,
    B11111100,
    B11111100,
    B01100000,
    B01100000,
    B11111100,
    B11111100,
    B01100001,
    B01110011,
    B00111111,
    B00111110,
    B00000000,
    B00000000
  },
  {
    B00111110, //GBP
    B00111111,
    B01110011,
    B01100001,
    B01100000,
    B01100000,
    B11111100,
    B11111100,
    B01100000,
    B01100000,
    B01100000,
    B01100001,
    B11111111,
    B11111111,
    B00000000,
    B00000000
  },
  {
    B00011000, //USD
    B01111110,
    B11111111,
    B11011011,
    B11011001,
    B01111000,
    B01111100,
    B00111110,
    B00011110,
    B10011011,
    B11011011,
    B11111111,
    B01111110,
    B00011000,
    B00000000,
    B00000000
  }
};

//------- Replace the following! ------
char ssid[] = "WiFiName";       // your network SSID (name)
char password[] = "password";  // your network key

#define BOT_TOKEN "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" //Telegram
#define CHAT_ID "-123456789" //Telegram chat Id

char currency[10] = "eur";


WiFiClientSecure secureClient;

CoinMarketCapApi api(secureClient);
UniversalTelegramBot bot(BOT_TOKEN, secureClient);

// CoinMarketCap's limit is "no more than 10 per minute"
// Make sure to factor in if you are requesting more than one coin.
unsigned long coinbaseDelay = 60000; // Time between api requests
unsigned long coinbaseDue = 0;

unsigned long telegramDelay = 1000;
unsigned long telegramDue;

unsigned long screenChangeDelay = 10000;
unsigned long screenChangeDue;

struct Holding {
  String tickerId;
  String symbol;
  float amount;
  bool inUse;
  CMCTickerResponse lastResponse;
};

int currentIndex = -1;
bool haveAnyHoldings;

int screenContrast = 35;
bool showValues = true;

int currencySymbolIndex = 0;

#define MAX_HOLDINGS 10

Holding holdings[MAX_HOLDINGS];

void setup()   {
  Serial.begin(115200);

  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount FS");
    return;
  }

  loadConfig();
  display.begin();
  Serial.println(screenContrast);
  display.setContrast(screenContrast);

  // Set WiFi to station mode and disconnect from an AP if it was Previously
  // connected
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Attempt to connect to Wifi network:
  Serial.print("Connecting Wifi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  IPAddress ip = WiFi.localIP();
  Serial.println(ip);

  loadHoldings();
  bot.sendMessage(CHAT_ID, "Starting Up", "Markdown");

}

bool loadConfig() {
  File configFile = SPIFFS.open(COINSHIP_CONFIG_NAME, "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  configFile.readBytes(buf.get(), size);

  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    Serial.println("Failed to parse config file");
    return false;
  }

  strcpy(currency, json["currency"]);
  currencySymbolIndex = getCurrencySymbolIndex();
  screenContrast = json["screenContrast"].as<int>();
  showValues = json["showValues"].as<bool>();

  return true;
}

bool saveConfig() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["currency"] = currency;
  json["screenContrast"] = screenContrast;
  json["showValues"] = showValues;

  File configFile = SPIFFS.open(COINSHIP_CONFIG_NAME, "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return false;
  }

  json.printTo(configFile);
  return true;
}

bool loadHoldings() {
  File configFile = SPIFFS.open(COINSHIP_HOLDINGS_NAME, "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  configFile.readBytes(buf.get(), size);

  StaticJsonBuffer<500> jsonBuffer;
  Serial.println(buf.get());
  JsonArray& holdingsArray = jsonBuffer.parseArray(buf.get());

  if (!holdingsArray.success()) {
    Serial.println("Failed to parse config file");
    return false;
  }

  for (int i = 0; i < holdingsArray.size(); i++) {
    holdings[i].tickerId = holdingsArray[i]["tickerId"].as<String>();
    holdings[i].symbol = holdingsArray[i]["symbol"].as<String>();
    holdings[i].amount = holdingsArray[i]["amount"].as<float>();
    holdings[i].inUse = true;
  }
  return true;
}

bool saveHoldings() {
  StaticJsonBuffer<500> jsonBuffer;
  JsonArray& holdingsArray = jsonBuffer.createArray();
  for (int i = 0; i < MAX_HOLDINGS; i++) {
    if (holdings[i].inUse) {
      JsonObject& holdingObject = holdingsArray.createNestedObject();
      holdingObject["tickerId"] = holdings[i].tickerId;
      holdingObject["symbol"] = holdings[i].symbol;
      holdingObject["amount"] = holdings[i].amount;
    }
  }

  File configFile = SPIFFS.open(COINSHIP_HOLDINGS_NAME, "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return false;
  }

  holdingsArray.printTo(configFile);
  return true;
}

String removeCurrentWord(String text) {
  text.trim();
  int nextSpaceIndex = text.indexOf(" ");
  if (nextSpaceIndex > -1) {
    text.remove(0, nextSpaceIndex + 1);
  } else {
    text = "";
  }

  return text;
}
String getNextWord(String text) {
  text.trim();
  int nextSpaceIndex = text.indexOf(" ");
  if (nextSpaceIndex > -1) {
    return text.substring(0, nextSpaceIndex + 1);
  }
  return text;
}

int getHoldingIndexBySymbol(String symbol) {
  symbol.trim();
  for (int i = 0; i < MAX_HOLDINGS; i++) {
    if (symbol.equalsIgnoreCase(holdings[i].symbol)) {
      return i;
    }
  }

  return -1;
}

int getHoldingIndexByTickerId(String tickerId) {
  tickerId.trim();
  for (int i = 0; i < MAX_HOLDINGS; i++) {
    if (tickerId.equalsIgnoreCase(holdings[i].tickerId)) {
      return i;
    }
  }

  return -1;
}

int getNextFreeHoldingIndex() {

  for (int i = 0; i < MAX_HOLDINGS; i++) {
    if (!holdings[i].inUse) {
      return i;
    }
  }

  return -1;
}

int getNextInUseTickerIndex(int index) {
  for (int i = index + 1; i < MAX_HOLDINGS; i++) {
    if (holdings[i].inUse) {
      return i;
    }
  }

  for (int j = 0; j <= index; j++) {
    if (holdings[j].inUse) {
      return j;
    }
  }

  return -1;
}

void populateHolding(int index, CMCTickerResponse tickerResponse) {
  holdings[index].tickerId = tickerResponse.id;
  holdings[index].symbol = tickerResponse.symbol;
  holdings[index].amount = 0;
  holdings[index].inUse = true;
  holdings[index].lastResponse = tickerResponse;
}

void handleSetCommand(String text, String chatId) {
  String message;
  message.reserve(150);
  text.trim();
  if (text.indexOf(" ") == text.lastIndexOf(" ")) {
    // Not enough parameters recieved
    message = F("Usage: /set ticker value");
    message = message + F("\n\nExample: /set eth 5.55");
  } else {
    // Remove the command word (/set)
    text = removeCurrentWord(text);
    String ticker = getNextWord(text);
    int holdingIndex = getHoldingIndexBySymbol(ticker);
    if (holdingIndex == -1) {
      message = "Could not find holding with the symbol: " + ticker;
      message = message + F("\n\nUse /add tickerId to add new tickers.");
      message = message + F("\n\nExample: /add ethereum");
    } else {
      // Remove the ticker
      text = removeCurrentWord(text);
      float oldValue = holdings[holdingIndex].amount;
      holdings[holdingIndex].amount = getNextWord(text).toFloat();
      saveHoldings();
      message = "Set Holding for " + holdings[holdingIndex].symbol + " from " + String(oldValue) + " to " + String(holdings[holdingIndex].amount);
    }
  }

  bot.sendMessage(chatId, message, "Markdown");
}

void handleAddCommand(String text, String chatId) {
  String message;
  message.reserve(150);
  text.trim();
  if (text.indexOf(" ") == -1 || text.indexOf(" ") != text.lastIndexOf(" ")) {
    // Wrong number of parameters recieved
    message = "Usage: /add tickerID";
    message = message + "\n\n" + "Example: /add ethereum";
    message = message + "\n\n" + "Get tickerID from the coinmarketcap.com website";
  } else {
    // Remove the command word (/add)
    text = removeCurrentWord(text);
    String tickerId = getNextWord(text);
    int holdingIndex = getHoldingIndexByTickerId(tickerId);
    if (holdingIndex != -1) {
      message = tickerId + " already exists.";
    } else {
      // Remove the ticker
      CMCTickerResponse tickerResponse = api.GetTickerInfo(tickerId, currency);
      if (tickerResponse.error == "") {
        int holdingIndex = getNextFreeHoldingIndex();
        populateHolding(holdingIndex, tickerResponse);
        saveHoldings();
        message = "Added " + String(tickerResponse.name) + " (" + String(tickerResponse.symbol) + ")";
        message = message + F("\n\nUse the /set command to add a holding");
        message = message + F("\n\nExample: /set ") + tickerResponse.symbol + F(" 5.5");
      } else {
        message = "Error Adding ticker: " + String(tickerResponse.error);
      }
    }
  }

  bot.sendMessage(chatId, message, "Markdown");
}

void handleHoldingCommand(String chatId) {
  String message;
  message.reserve(150);
  bool haveHoldings;
  message = "Holdings: \n";
  for (int i = 0; i < MAX_HOLDINGS; i++) {
    if (holdings[i].inUse) {
      haveHoldings = true;
      message = message + "\n" + holdings[i].lastResponse.id + " (" + holdings[i].lastResponse.symbol + "): " + String(holdings[i].amount);
    }
  }

  if (!haveHoldings) {
    message = "No holdings found.";
    message = message + "\n\n" + "Use the /add command to add a ticker";
  }

  bot.sendMessage(chatId, message, "Markdown");
}

void handleDeleteCommand(String text, String chatId) {
  String message;
  message.reserve(150);
  text.trim();
  if (text.indexOf(" ") == -1 || text.indexOf(" ") != text.lastIndexOf(" ")) {
    // Wrong number of parameters recieved
    message = "Usage: /delete tickerID";
    message = message + "\n\n" + "Example: /delete ethereum";
    message = message + "\n\n" + "Use the /holdings command to see a list of your current tickers";
  } else {
    // Remove the command word (/add)
    text = removeCurrentWord(text);
    String tickerId = getNextWord(text);
    int holdingIndex = getHoldingIndexByTickerId(tickerId);
    if (holdingIndex == -1) {
      message = tickerId + " is not one of your tickers";
      message = message + "\n\n" + "Use the /holdings command to see a list of your current tickers";
    } else {
      // Remove the ticker
      holdings[holdingIndex] = Holding();
      screenChangeDue = 0;
      saveHoldings();
      message = "Removed " + tickerId;
    }
  }

  bot.sendMessage(chatId, message, "Markdown");
}

void handleSettingsMessage(String chat_id) {
  String settings = "Current Settings:\n";
  settings.reserve(200);
  settings = settings + F("Screen Contrast: ");
  settings = settings + String(screenContrast);
  settings = settings + F("\nSet using /contrast value (e.g. /contrast 40)\n\n");

  settings = settings + F("Display Holdings: ");
  settings = settings + String(showValues);
  settings = settings + F("\nSet using /show value (e.g. /show false)\n\n");

  settings = settings + F("Currency: ");
  settings = settings + String(currency);
  settings = settings + F("\nSet using /currency value (e.g. /currency gbp)\n\n");
  bot.sendMessage(chat_id, settings, "Markdown");
}

void handleCurrencyMessage(String text, String chatId) {
  String message;
  message.reserve(150);
  text.trim();
  if (text.indexOf(" ") == -1 || text.indexOf(" ") != text.lastIndexOf(" ")) {
    // Wrong number of parameters recieved
    message = "Usage: /currency value";
    message = message + "\n\n" + "Example: /currency gbp";
    message = message + "\n\n" + "Use the /settings to see your current settings";
  } else {
    // Remove the command word (/currency)
    text = removeCurrentWord(text);
    String value = getNextWord(text);
    if (value.length() != 3) {
      message = text + " is not a valid currency Value";
      message = message + F("\nCurrency value should only be 3 characters long");
      message = message + F("\n\nUsage: /currency value");
      message = message + F("\n\nExample: /currency gbp");
    } else {
      value.toLowerCase();
      value.toCharArray(currency, 10);
      currencySymbolIndex = getCurrencySymbolIndex();
      if (saveConfig()) {
        message = "Set currency to " + value;
        message = message + "\n\n" + "NOTE: If it's not a valid currency on coinmarketcap.com it will default to USD";
      } else {
        message = "Set currency but failed to save config";
      }
    }
  }

  bot.sendMessage(chatId, message, "Markdown");
}

void handleContrastMessage(String text, String chatId) {
  String message;
  message.reserve(150);
  text.trim();
  if (text.indexOf(" ") == -1 || text.indexOf(" ") != text.lastIndexOf(" ")) {
    // Wrong number of parameters recieved
    message = "Usage: /contrast value";
    message = message + "\n\n" + "Example: /contrast 40";
    message = message + "\n\n" + "Use the /settings to see your current settings";
  } else {
    // Remove the command word (/contrast)
    text = removeCurrentWord(text);
    int value = getNextWord(text).toFloat();
    if (value < 1) {
      message = text + " is not a valid contrast";
      message = message + "\n\n" + "Usage: /contrast value";
      message = message + "\n\n" + "Example: /contrast 40";
    } else {
      // Remove the ticker
      screenContrast = value;
      display.setContrast(screenContrast);
      if (saveConfig()) {
        message = "Set contrast to " + text;
      } else {
        message = "Set contrast but failed to save config";
      }
    }
  }

  bot.sendMessage(chatId, message, "Markdown");
}

void handleShowMessage(String text, String chatId) {
  String message;
  message.reserve(150);
  text.trim();
  if (text.indexOf(" ") == -1 || text.indexOf(" ") != text.lastIndexOf(" ")) {
    // Wrong number of parameters recieved
    message = F("Usage: /show value");
    message = message + F("\n\nExample: /show false");
    message = message + F("\n\nUse the /settings to see your current settings");
  } else {
    // Remove the command word (/contrast)
    text = removeCurrentWord(text);
    String value = getNextWord(text);
    bool validValue = value.equalsIgnoreCase("true") || value.equalsIgnoreCase("false");
    if (!validValue ) {
      message = value + " is not a true or false value";
      message = message + F("\n\nUsage: /show value");
      message = message + F("\n\nExample: /show false");
    } else {

      showValues = value.equalsIgnoreCase("true");
      if (saveConfig()) {
        message = "Set Display Holdings to " + value;
      } else {
        message = F("Set Display Holdings but failed to save config");
      }
    }
  }

  bot.sendMessage(chatId, message, "Markdown");
}

void handleNewMessages(int numNewMessages) {
  bool auth;
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    auth = chat_id == CHAT_ID;
    String text = bot.messages[i].text;
    if (auth)
    {
      if (text.startsWith(F("/settings"))) {
        handleSettingsMessage(chat_id);
      } else if (text.startsWith(F("/set"))) {
        handleSetCommand(text, chat_id);
      } else if (text.startsWith(F("/add"))) {
        handleAddCommand(text, chat_id);
      } else if (text.startsWith(F("/holdings"))) {
        handleHoldingCommand(chat_id);
      } else if (text.startsWith(F("/delete"))) {
        handleDeleteCommand(text, chat_id);
      } else if (text.startsWith(F("/currency"))) {
        handleCurrencyMessage(text, chat_id);
      } else if (text.startsWith(F("/show"))) {
        handleShowMessage(text, chat_id);
      } else if (text.startsWith(F("/contrast"))) {
        handleContrastMessage(text, chat_id);
      } else if (text == "/start" || text == "/help") {
        String welcome = F("Welcome to the moon\n");
        welcome = welcome + F("/add tickerId : add ticker to your holding list\n");
        welcome = welcome + F("/set ticker value : sets the value you hold of that ticker\n");
        welcome = welcome + F("/holdings : displays a list of your holdings\n");
        welcome = welcome + F("/delete tickerId : removes ticker from holding list\n");
        welcome = welcome + F("/settings : shows current settings and how to set them\n");
        bot.sendMessage(chat_id, welcome, "Markdown");
      }
    }
  }
}

int getCurrencySymbolIndex() {
  Serial.println(currency);
  if (strcmp(currency, "eur") == 0 ) {
    return 0;
  } else if (strcmp(currency, "gbp") == 0 ) {
    return 1;
  } else if (strcmp(currency, "usd") == 0) {
    return 2;
  }

  Serial.println(F("I dont have a symbol for your choosen currecny, defaulting to a $ symbol"));
  return 2;
}

void displayHolderInfo(Holding holding) {

  display.clearDisplay();
  display.setRotation(2);

  // Ticker Symbol
  display.setTextSize(1);
  display.setTextColor(BLACK);
  display.setCursor(33, 0);
  display.println(holding.symbol);

  // Price
  double price = holding.lastResponse.price_currency;
  if (price == 0) {
    price = holding.lastResponse.price_usd;
  }
  int majorCurrencyValue = (int)price;

  // If the value of the coin is over 10 $/£/€ we want round after the decimal to 2
  int roundingFactor = 100;
  int digitsAfterDecimal = 2;

  String leadingZeros;
  leadingZeros.reserve(5);
  if (majorCurrencyValue < 10) {
    roundingFactor = 100000;
    digitsAfterDecimal = 5;
  }

  int roundedCurrency = (int)roundf(holding.lastResponse.price_currency * roundingFactor);
  int minorCurrencyValue = roundedCurrency - (majorCurrencyValue * roundingFactor);

  int temp = 10;
  for (int i = 0; i < digitsAfterDecimal - 1; i++) {
    if (minorCurrencyValue < temp) {
      leadingZeros = leadingZeros + F("0");
    }
    temp = temp * 10;
  }

  display.setTextSize(2);
  int y = display.getCursorY();
  display.drawBitmap(0, y, CURRENCY_SYMBOLS[currencySymbolIndex], 8, 16, 1);
  display.setCursor(11, y);
  display.print(majorCurrencyValue);
  display.setTextSize(1);
  int x = display.getCursorX();
  display.println();
  y = display.getCursorY();
  display.setCursor(x, y);
  display.print(F("."));
  display.print(leadingZeros);
  display.println(minorCurrencyValue);

  // Precent Change
  display.print(F("7d: "));
  display.print(holding.lastResponse.percent_change_7d);
  display.println(F("%"));

  if (showValues && holding.amount > 0) {
    // Hold
    display.print(F("Hold: "));
    display.println(holding.amount);

    // Value
    float holdingValue = holding.amount * holding.lastResponse.price_currency;
    display.print(F("Value: "));
    display.println(holdingValue, 4);
  } else {
    display.print(F("24h: "));
    display.print(holding.lastResponse.percent_change_24h);
    display.println(F("%"));

    display.print(F("1h: "));
    display.print(holding.lastResponse.percent_change_1h);
    display.println(F("%"));
  }

  display.display();
}

void changeActiveHolding() {
  Serial.println(F("Changing active holding"));
  int nextIndex = getNextInUseTickerIndex(currentIndex);
  if (nextIndex > -1) {
    haveAnyHoldings = true;
    currentIndex = nextIndex;
    Serial.print(F("new active holding: "));
    Serial.print(holdings[currentIndex].tickerId);
    CMCTickerResponse tickerResponse = api.GetTickerInfo(holdings[currentIndex].tickerId, currency);
    if (tickerResponse.error == "") {
      holdings[currentIndex].lastResponse = tickerResponse;
    } else {
      // Display error?
      Serial.println(tickerResponse.error);
    }
  } else {
    Serial.println(F("No active holdings"));
    haveAnyHoldings = false;
  }
}

void displayNoHoldingsMessage() {
  display.clearDisplay();
  display.setRotation(2);

  // Ticker Symbol
  display.setTextSize(1);
  display.setTextColor(BLACK);
  display.println(F("You have no holdings"));

  display.println(F("Use Telegram bot to /add some"));
  display.display();
}

void loop() {
  unsigned long timeNow = millis();
  if (timeNow > screenChangeDue)  {
    Serial.printf("loop heap size: %u\n", ESP.getFreeHeap());
    changeActiveHolding();
    screenChangeDue = timeNow + screenChangeDelay;
  }

  timeNow = millis();
  if (timeNow > telegramDue)  {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    while (numNewMessages) {
      Serial.println(F("got response"));
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }

    telegramDue = timeNow + telegramDelay;
  }
  if (haveAnyHoldings) {
    displayHolderInfo(holdings[currentIndex]);
  } else {
    displayNoHoldingsMessage();
  }
  delay(100);
}
