#pragma once

// LTE connection helpers. Do not modify without explicit confirmation.

#include <Arduino.h>
#include <MKRNB.h>

extern NB nbAccess;
extern NBModem modem;
extern NBClient client;

String sendAT(const char *cmd, unsigned long timeout = 2500);
void ensureURCsVerbose();
void ensureAPN(const String &apn);
bool attachPdp(const String &apn, int cid = 1);
bool checkStatus();
