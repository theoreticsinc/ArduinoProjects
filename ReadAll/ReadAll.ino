#include <SPI.h>
#include <MFRC522.h>

// Reassigned pins placed directly adjacent to SPI pins (50-52) on Mega 2560
#define SS_PIN 53   // Slave Select (Right next to SCK 52)
#define RST_PIN 48  // Reset Pin (Adjacent on double-row header)

MFRC522 rfid(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;
// Data arrays for test writing
byte block5Data[16] = "Hello Block 5!! ";
byte block10Data[16] = "Data for Block10";

// Common default MIFARE keys to attempt fallback authentication
// Expanded list of common MIFARE Classic keys
byte knownKeys[][6] = {

  // Standard / factory
  {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},

  // Common NXP / MAD / application keys
  {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
  {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
  {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
  {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},

  // Blank
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},

  // Frequently seen test/example keys
  {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F},
  {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC},
  {0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF},

  // Sequential patterns
  {0x01, 0x02, 0x03, 0x04, 0x05, 0x06},
  {0x06, 0x05, 0x04, 0x03, 0x02, 0x01},

  // Common hexadecimal patterns
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x11},
  {0x22, 0x22, 0x22, 0x22, 0x22, 0x22},
  {0x33, 0x33, 0x33, 0x33, 0x33, 0x33},
  {0x44, 0x44, 0x44, 0x44, 0x44, 0x44},
  {0x55, 0x55, 0x55, 0x55, 0x55, 0x55},
  {0x66, 0x66, 0x66, 0x66, 0x66, 0x66},
  {0x77, 0x77, 0x77, 0x77, 0x77, 0x77},
  {0x88, 0x88, 0x88, 0x88, 0x88, 0x88},
  {0x99, 0x99, 0x99, 0x99, 0x99, 0x99},

  // Common ASCII-ish patterns
  {0x10, 0x10, 0x10, 0x10, 0x10, 0x10},
  {0x20, 0x20, 0x20, 0x20, 0x20, 0x20},
  {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA},

  // Other frequently used examples
  {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56},
  {0xCA, 0xFE, 0xBA, 0xBE, 0xCA, 0xFE},
  {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD}
};

// Update array size calculation automatically
const byte NUMBER_OF_KEYS = sizeof(knownKeys) / sizeof(knownKeys[0]);

void setup() {
  Serial.begin(9600);

  SPI.begin();
  rfid.PCD_Init();

  delay(50);  // Power and RF stabilization delay

  // Set Rx Gain to 38dB (default is 48dB max) to prevent signal saturation on sensitive tags
  rfid.PCD_SetAntennaGain(rfid.RxGain_38dB);

  Serial.println(F("Mega 2560 + RC522 (Enhanced Compatibility Setup) Ready..."));
}

/**
 * Authenticates a block by trying known keys across Key A and Key B.
 * Resets crypto engine state between attempts.
 */
bool authenticateWithFallback(byte blockAddress, MFRC522::MIFARE_Key *activeKey, byte *keyTypeUsed) {
  MFRC522::StatusCode status;

  for (byte k = 0; k < NUMBER_OF_KEYS; k++) {
    MFRC522::MIFARE_Key testKey;
    for (byte i = 0; i < 6; i++) {
      testKey.keyByte[i] = knownKeys[k][i];
    }

    // Always clear prior crypto lock before new authentication attempt
    rfid.PCD_StopCrypto1();
    delay(5);

    // Try Key A authentication
    status = (MFRC522::StatusCode)rfid.PCD_Authenticate(
      MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddress, &testKey, &(rfid.uid));
    if (status == MFRC522::STATUS_OK) {
      *activeKey = testKey;
      *keyTypeUsed = MFRC522::PICC_CMD_MF_AUTH_KEY_A;
      return true;
    }

    // Clear crypto state before switching to Key B
    rfid.PCD_StopCrypto1();
    delay(5);

    // Try Key B authentication
    status = (MFRC522::StatusCode)rfid.PCD_Authenticate(
      MFRC522::PICC_CMD_MF_AUTH_KEY_B, blockAddress, &testKey, &(rfid.uid));
    if (status == MFRC522::STATUS_OK) {
      *activeKey = testKey;
      *keyTypeUsed = MFRC522::PICC_CMD_MF_AUTH_KEY_B;
      return true;
    }
  }

  return false;
}

/**
 * Writes 16 bytes to a MIFARE Classic block using fallback key authentication.
 */
bool writeBlock(byte blockAddress, byte dataToWrite[16]) {
  MFRC522::StatusCode status;
  MFRC522::MIFARE_Key activeKey;
  byte keyType;

  Serial.print(F("\n--- Writing to Block "));
  Serial.print(blockAddress);
  Serial.println(F(" ---"));

  if (!authenticateWithFallback(blockAddress, &activeKey, &keyType)) {
    Serial.println(F("Write Auth Failed: No matching key found."));
    return false;
  }
  delay(10);  // Post-auth pacing delay

  // Write 16 Bytes to Block
  status = (MFRC522::StatusCode)rfid.MIFARE_Write(blockAddress, dataToWrite, 16);
  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("Write Failed: "));
    Serial.println(rfid.GetStatusCodeName(status));
    return false;
  }

  Serial.print(F("Successfully Written: "));
  Serial.println((char *)dataToWrite);

  delay(15);  // EEPROM write cycle delay
  return true;
}

/**
 * Reads 16 bytes from a MIFARE Classic block.
 * Authenticates using fallback only if the sector is not yet authenticated.
 */
bool readBlock(byte blockAddress, byte bufferOut[18]) {
  MFRC522::StatusCode status;
  MFRC522::MIFARE_Key activeKey;
  byte keyType;
  byte size = 18;

  Serial.print(F("\n--- Target Block "));
  Serial.print(blockAddress);
  Serial.println(F(" ---"));

  // Attempt to read directly (works if sector is already authenticated)
  status = (MFRC522::StatusCode) rfid.MIFARE_Read(blockAddress, bufferOut, &size);

  // If read fails, authenticate the sector and try reading again
  if (status != MFRC522::STATUS_OK) {
    if (!authenticateWithFallback(blockAddress, &activeKey, &keyType)) {
      Serial.println(F("Read Auth Failed: No matching key found."));
      return false;
    }
    delay(10); // Post-auth pacing delay

    status = (MFRC522::StatusCode) rfid.MIFARE_Read(blockAddress, bufferOut, &size);
    if (status != MFRC522::STATUS_OK) {
      Serial.print(F("Read Failed: "));
      Serial.println(rfid.GetStatusCodeName(status));
      return false;
    }
  }

  // Print raw bytes in HEX
  Serial.print(F("Read Back (HEX) : "));
  for (byte i = 0; i < 16; i++) {
    Serial.print(bufferOut[i] < 0x10 ? " 0" : " ");
    Serial.print(bufferOut[i], HEX);
  }
  Serial.println();

  // Print raw bytes in ASCII Text
  Serial.print(F("Read Back (Text): "));
  for (byte i = 0; i < 16; i++) {
    Serial.write(bufferOut[i]);
  }
  Serial.println();

  return true;
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }
  MFRC522::PICC_Type piccType = rfid.PICC_GetType(rfid.uid.sak);
  Serial.print(F("Card Type: "));
  Serial.println(rfid.PICC_GetTypeName(piccType));

  if (piccType != MFRC522::PICC_TYPE_MIFARE_MINI && piccType != MFRC522::PICC_TYPE_MIFARE_1K && piccType != MFRC522::PICC_TYPE_MIFARE_4K) {
    Serial.println(F("Error: This card is not a MIFARE Classic tag!"));
    return;
  }
  Serial.println(F("\n=========================================="));
  Serial.print(F("Card Detected! UID:"));
  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.println();

  byte readBuffer[18];

  // --- WRITE OPERATIONS ---
  //writeBlock(14, block10Data);
  //delay(10);

  // Clear crypto state between sequential block tasks
  rfid.PCD_StopCrypto1();
  delay(15);

  // --- READ OPERATIONS ---
  readBlock(4, readBuffer);
  delay(10);
  readBlock(5, readBuffer);
  delay(10);
  readBlock(6, readBuffer);
  delay(10);

  readBlock(8, readBuffer);
  delay(10);
  readBlock(9, readBuffer);
  delay(10);
  readBlock(10, readBuffer);
  delay(10);

  readBlock(12, readBuffer);
  delay(10);
  readBlock(13, readBuffer);
  delay(10);
  readBlock(14, readBuffer);

  // Final session cleanup
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  Serial.println(F("\nDone! Remove card..."));
  delay(2000);
}