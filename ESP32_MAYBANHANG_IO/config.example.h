#pragma once
#include <Arduino.h>

// Copy file nay thanh config.h va dien thong tin cua ban.
// KHONG upload config.h len GitHub Public.

// WiFi nha
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// WiFi do ESP32 phat khi khong ket noi duoc WiFi nha
const char* AP_SSID = "MayBanHang";
const char* AP_PASS = "CHANGE_THIS_AP_PASSWORD"; // It nhat 8 ky tu

const char* HOSTNAME = "maybanhang";

// PIN chu may xac nhan da nhan tien tren web
const char* ADMIN_PIN = "CHANGE_THIS_ADMIN_PIN";

// Thong tin hien thi va tao ma QR.
// Dien dung thong tin cua nha cung cap thanh toan cua ban.
const char* ACCOUNT_NAME = "YOUR_ACCOUNT_NAME";
const char* QR_GUID = "YOUR_QR_GUID";
const char* QR_BANK_BIN = "YOUR_BANK_BIN";
const char* QR_ACCOUNT = "YOUR_BANK_ACCOUNT";
const char* QR_SERVICE = "YOUR_QR_SERVICE";
const char* QR_REF_05 = "YOUR_QR_REFERENCE_05";
const char* QR_REF_06 = "YOUR_QR_REFERENCE_06";

const uint8_t QR_VERSION = 10; // QR version 10 (57x57)
