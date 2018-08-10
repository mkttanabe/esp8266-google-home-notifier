#include <esp8266-google-home-notifier.h>

char data[1024];

boolean GoogleHomeNotifier::device(const char * name)
{
  return this->device(name, "en");
}

boolean GoogleHomeNotifier::device(const char * name, const char * locale)
{
  return GoogleHomeNotifier::device(name, locale, 10000);
}

boolean GoogleHomeNotifier::device(const char * name, const char * locale, int to)
{
  int timeout = millis() + to;
  int n;
  char hostString[20];
  uint64_t chipid;
#ifdef ARDUINO_ARCH_ESP8266
  chipid = ESP.getChipId();
#elif defined ARDUINO_ARCH_ESP32
  chipid = ESP.getEfuseMac();
#else
#error "ARDUINO_ARCH_ESP8266 or ARDUINO_ARCH_ESP32 has to be defined."
#endif
  sprintf(hostString, "ESP_%06X", chipid);

  if (strcmp(this->m_name, name) != 0) {
    int i = 0;
    if (!MDNS.begin(hostString)) {
      this->setLastError("Failed to set up MDNS responder.");
      return false;
    }
    do {
      n = MDNS.queryService("googlecast", "tcp");
      if (millis() > timeout) {
        this->setLastError("mDNS timeout.");
        return false;
      }
      delay(1);
      if (strcmp(name, "") != 0) {
        for(i = 0; i < n; i++) {
          if (strcmp(name, MDNS.txt(i, "fn").c_str()) == 0) {
            break;
          }
        }
      }
    } while (n <= 0 || i >= n);

    this->m_ipaddress = MDNS.IP(i);
    this->m_port = MDNS.port(i);
  }
  sprintf(this->m_name, "%s", name);
  sprintf(this->m_locale, "%s", locale);
  return true;
}

#ifdef TANABE
boolean GoogleHomeNotifier::ip(IPAddress ip, const char *locale)
{
  this->m_ipaddress = ip;
  this->m_port = 8009;
  sprintf(this->m_locale, "%s", locale);
  return true;
}

boolean GoogleHomeNotifier::notify(const char * phrase) {
  return this->cast(phrase, NULL, NULL);
}

boolean GoogleHomeNotifier::play(const char * mp3Url, void (*callbackFunc)(int)) {
  return this->cast(NULL, mp3Url, callbackFunc);
}

boolean GoogleHomeNotifier::cast(const char * phrase, const char * mp3Url, void (*callbackFunc)(int))
#else
boolean GoogleHomeNotifier::notify(const char * phrase)
#endif
{
  char error[128];
  String speechUrl;

#ifdef TANABE
  if (observerTaskEvent != NULL) {
	// pause the task
    xEventGroupClearBits(observerTaskEvent, BIT0);
    delay(200);
  }
  if (m_client) {
    disconnect();
  }

  // register callback function to get the playback status
  if (callbackFunc != NULL && mp3Url != NULL) {
    playEndCallback = callbackFunc;
  } else {
    playEndCallback = NULL;
  }

  if (mp3Url == NULL) {
    speechUrl = tts.getSpeechUrl(phrase, m_locale);
    delay(1);
    if (speechUrl.indexOf("https://") != 0) {
      this->setLastError("Failed to get TTS url.");
      return false;
    }
  } else {
    speechUrl = mp3Url;
  }
#else
  speechUrl = tts.getSpeechUrl(phrase, m_locale);
  delay(1);

  if (speechUrl.indexOf("https://") != 0) {
    this->setLastError("Failed to get TTS url.");
    return false;
  }
#endif

  delay(1);
  if(!m_client) m_client = new WiFiClientSecure();
  if (!m_client->connect(this->m_ipaddress, this->m_port)) {
    sprintf(error, "Failed to Connect to %d.%d.%d.%d:%d.", this->m_ipaddress[0], this->m_ipaddress[1], this->m_ipaddress[2], this->m_ipaddress[3], this->m_port);
    this->setLastError(error);
    disconnect();
    return false;
  }
  
  delay(1);
  if( this->connect() != true) {
    sprintf(error, "Failed to Open-Session. (%s)", this->getLastError());
    this->setLastError(error);
    disconnect();
    return false;
  }
   
  delay(1);
#ifdef TANABE
  if( this->_play(speechUrl.c_str()) != true) {
#else
  if( this->play(speechUrl.c_str()) != true) {
#endif
    sprintf(error, "Failed to play mp3 file. (%s)", this->getLastError());
    this->setLastError(error);
    disconnect();
    return false;
  }

#ifdef TANABE
  if (playEndCallback == NULL) {
    disconnect();
  }
#else
  disconnect();
#endif
  return true;
}

const IPAddress GoogleHomeNotifier::getIPAddress()
{
  return m_ipaddress;
}

const uint16_t GoogleHomeNotifier::getPort()
{
  return m_port;
}

#ifdef TANABE
// task to monitor the playback status
void observerTask(void* arg) {
  GoogleHomeNotifier *self = (GoogleHomeNotifier*)arg;
  extensions_api_cast_channel_CastMessage imsg;
  pb_istream_t istream;
  uint8_t pcktSize[4];
  uint8_t buffer[1024];
  uint32_t message_length;
  unsigned long lastPing = 0;
  int sts = 1;

  while (true) { // A
    PRLN("observerTask: waiting..");
    xEventGroupWaitBits(self->observerTaskEvent,  BIT0, false, true, portMAX_DELAY);
    PRLN("observerTask: triggered");

    while(true) { // B
      EventBits_t bits = xEventGroupGetBits(self->observerTaskEvent);
      if((bits & BIT0) != BIT0) {
          sts = -1;
          PRLN("observerTask: started another playback");
          break;
      }
      delay(200);
      if (millis() - lastPing >= 5000) {
        // send 'PING' per 5 seconds
        lastPing = millis();
        self->sendMessage(SOURCE_ID, DESTINATION_ID, CASTV2_NS_HEARTBEAT, CASTV2_DATA_PING);
      }
      // read message from Google Home
      memset(&imsg, 0, sizeof(imsg));
      int bytes = self->m_client->read(pcktSize, 4);
      if (bytes <= 0) {
        continue;
      }
      message_length = 0;
      for(int i=0;i<4;i++) {
        message_length |= pcktSize[i] << 8*(3 - i);
      }
      self->m_client->read(buffer, message_length);
      istream = pb_istream_from_buffer(buffer, message_length);

      imsg.payload_utf8.funcs.decode = &(GoogleHomeNotifier::decode_string);
      imsg.payload_utf8.arg = (void*)"body";

      if (pb_decode(&istream, extensions_api_cast_channel_CastMessage_fields, &imsg) != true){
        self->setLastError("Incoming message decoding");
        Serial.println("observerTask: pb_decode error");
        break;
      }
      //String json = String((char*)imsg.payload_utf8.arg);
      //PRLN(String("observerTask: ") + json);
      char *p = (char*)imsg.payload_utf8.arg;

      if (strstr(p, CASTV2_DATA_CLOSE)) {
        PRLN("observerTask: CASTV2_DATA_CLOSE");
        // another cast session has started
        self->disconnect();
        sts = 0; // stop playing
        break;
      } else if (strstr(p, "\"idleReason\":\"FINISHED\"")) {
        PRLN("observerTask: FINISHED");
        // playback end
        self->disconnect();
        sts = 1; // play next data
        break;
      } else if (strstr(p, "\"playerState\":\"PAUSED\"")) {
        PRLN("observerTask: PAUSED");
        // "OK, Google. stop"
        self->disconnect();
        sts = 0;
        break;
      } else if (strstr(p, "\"playerState\":\"PLAYING\"") && 
                 !strstr(p, "\"requestId\":0")) {
        PRLN("observerTask: SKIP");
        // "OK, Google. next"
        self->disconnect();
        sts = 1;
        break;
      } else if (strstr(p, CASTV2_DATA_PING)) { // 'PING'
        // send 'PONG'
        self->sendMessage(DESTINATION_ID, SOURCE_ID, CASTV2_NS_HEARTBEAT, CASTV2_DATA_PONG);
      }
    } // while B

    xEventGroupClearBits(self->observerTaskEvent, BIT0);
    if (self->playEndCallback != NULL) {
      PRLN("observerTask: start callback");
      (self->playEndCallback)(sts);
      PRLN("observerTask: done callback");
    }
  } // while A
}
#endif // TANABE

boolean GoogleHomeNotifier::sendMessage(const char* sourceId, const char* destinationId, const char* ns, const char* data)
{
#ifdef TANABE
  //PRLN(String("sendMessage: ") + ns);
#endif
  extensions_api_cast_channel_CastMessage message = extensions_api_cast_channel_CastMessage_init_default;

  message.protocol_version = extensions_api_cast_channel_CastMessage_ProtocolVersion_CASTV2_1_0;
  message.source_id.funcs.encode = &(GoogleHomeNotifier::encode_string);
  message.source_id.arg = (void*)sourceId;
  message.destination_id.funcs.encode = &(GoogleHomeNotifier::encode_string);
  message.destination_id.arg = (void*)destinationId;
  message.namespace_str.funcs.encode = &(GoogleHomeNotifier::encode_string);
  message.namespace_str.arg = (void*)ns;
  message.payload_type = extensions_api_cast_channel_CastMessage_PayloadType_STRING;
  message.payload_utf8.funcs.encode = &(GoogleHomeNotifier::encode_string);
  message.payload_utf8.arg = (void*)data;

  uint8_t* buf = NULL;
  uint32_t bufferSize = 0;
  uint8_t packetSize[4];
  boolean status;

  pb_ostream_t  stream;
  
  do
  {
    if (buf) {
      delete buf;
    }
    bufferSize += 1024;
    buf = new uint8_t[bufferSize];

    stream = pb_ostream_from_buffer(buf, bufferSize);
    status = pb_encode(&stream, extensions_api_cast_channel_CastMessage_fields, &message);
  } while(status == false && bufferSize < 10240);
  if (status == false) {
    char error[128];
    sprintf(error, "Failed to encode. (source_id=%s, destination_id=%s, namespace=%s, data=%s)", sourceId, destinationId, ns, data);
    this->setLastError(error);
    return false;
  }

  bufferSize = stream.bytes_written;
  for(int i=0;i<4;i++) {
    packetSize[3-i] = (bufferSize >> 8*i) & 0x000000FF;
  }
  m_client->write(packetSize, 4);
  m_client->write(buf, bufferSize);
  m_client->flush();

#ifdef TANABE
  // create the task to monitor the playback status
  if (playEndCallback != NULL && strcmp(ns, CASTV2_NS_MEDIA) == 0) {
    if (observerTaskHandle == NULL) {
        PRLN("sendMessage: TaskCreate");
        observerTaskEvent = xEventGroupCreate();
        xTaskCreatePinnedToCore(observerTask, "observerTask", 8192, this, 1, &observerTaskHandle, 1);
        delay(200);
    }
    xEventGroupSetBits(observerTaskEvent, BIT0);
  }
#endif

  delay(1);
  delete buf;
  return true;
}

boolean GoogleHomeNotifier::connect()
{
  // send 'CONNECT'
  if (this->sendMessage(SOURCE_ID, DESTINATION_ID, CASTV2_NS_CONNECTION, CASTV2_DATA_CONNECT) != true) {
    this->setLastError("'CONNECT' message encoding");
    return false;
  }
  delay(1);

  // send 'PING'
  if (this->sendMessage(SOURCE_ID, DESTINATION_ID, CASTV2_NS_HEARTBEAT, CASTV2_DATA_PING) != true) {
    this->setLastError("'PING' message encoding");
    return false;
  }
  delay(1);

  // send 'LAUNCH'
  sprintf(data, CASTV2_DATA_LAUNCH, APP_ID);
  if (this->sendMessage(SOURCE_ID, DESTINATION_ID, CASTV2_NS_RECEIVER, data) != true) {
    this->setLastError("'LAUNCH' message encoding");
    return false;
  }
  delay(1);

  // waiting for 'PONG' and Transportid
  int timeout = (int)millis() + 5000;
  while (m_client->available() == 0) {
    if (timeout < millis()) {
      this->setLastError("Listening timeout");
      return false;
    }
  }
  timeout = (int)millis() + 5000;
  extensions_api_cast_channel_CastMessage imsg;
  pb_istream_t istream;
  uint8_t pcktSize[4];
  uint8_t buffer[1024];

  uint32_t message_length;
  while(true) {
    delay(100);
    if (millis() > timeout) {
      this->setLastError("Incoming message decoding");
      return false;
    }
    // read message from Google Home
    m_client->read(pcktSize, 4);
    message_length = 0;
    for(int i=0;i<4;i++) {
      message_length |= pcktSize[i] << 8*(3 - i);
    }
    m_client->read(buffer, message_length);
    istream = pb_istream_from_buffer(buffer, message_length);

    imsg.source_id.funcs.decode = &(GoogleHomeNotifier::decode_string);
    imsg.source_id.arg = (void*)"sid";
    imsg.destination_id.funcs.decode = &(GoogleHomeNotifier::decode_string);
    imsg.destination_id.arg = (void*)"did";
    imsg.namespace_str.funcs.decode = &(GoogleHomeNotifier::decode_string);
    imsg.namespace_str.arg = (void*)"ns";
    imsg.payload_utf8.funcs.decode = &(GoogleHomeNotifier::decode_string);
    imsg.payload_utf8.arg = (void*)"body";
    /* Fill in the lucky number */

    if (pb_decode(&istream, extensions_api_cast_channel_CastMessage_fields, &imsg) != true){
      this->setLastError("Incoming message decoding");
      return false;
    }
    String json = String((char*)imsg.payload_utf8.arg);
    int pos = -1;

    // if the incoming message has the transportId, then break;
    if (json.indexOf(String("\"appId\":\"") + APP_ID + "\"") >= 0 &&
        (pos = json.indexOf("\"transportId\":")) >= 0
        ) {
      sprintf(this->m_transportid, "%s", json.substring(pos + 15, pos + 51).c_str());
      break;
    }
  }
  sprintf(this->m_clientid, "client-%d", millis());
  return true;
}

#ifdef TANABE
boolean GoogleHomeNotifier::_play(const char * mp3url)
#else
boolean GoogleHomeNotifier::play(const char * mp3url)
#endif
{
  // send 'CONNECT' again
  sprintf(data, CASTV2_DATA_CONNECT);
  if (this->sendMessage(this->m_clientid, this->m_transportid, CASTV2_NS_CONNECTION, CASTV2_DATA_CONNECT) != true) {
    this->setLastError("'CONNECT' message encoding");
    return false;
  }
  delay(1);

  // send URL of mp3
  sprintf(data, CASTV2_DATA_LOAD, mp3url);
  if (this->sendMessage(this->m_clientid, this->m_transportid, CASTV2_NS_MEDIA, data) != true) {
    this->setLastError("'LOAD' message encoding");
    return false;
  }
  delay(1);
  this->setLastError("");
  return true;
}

void GoogleHomeNotifier::disconnect() {
  if (m_client) {
    if (m_client->connected()) m_client->stop();
    delete m_client;
    m_client = NULL;
  }
}

bool GoogleHomeNotifier::encode_string(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  char *str = (char*) *arg;

  if (!pb_encode_tag_for_field(stream, field))
    return false;

  return pb_encode_string(stream, (uint8_t*)str, strlen(str));
}

bool GoogleHomeNotifier::decode_string(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
  uint8_t buffer[1024] = {0};

  /* We could read block-by-block to avoid the large buffer... */
  if (stream->bytes_left > sizeof(buffer) - 1)
    return false;

  if (!pb_read(stream, buffer, stream->bytes_left))
    return false;

  /* Print the string, in format comparable with protoc --decode.
    * Format comes from the arg defined in main().
    */
  *arg = (void***)buffer;
  return true;
}

const char * GoogleHomeNotifier::getLastError() {
  return m_lastError;
}

void GoogleHomeNotifier::setLastError(const char* lastError) {
  sprintf(m_lastError, "%s", lastError);
}
