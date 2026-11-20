/*
  MAY BAN HANG - PHAN ESP32 (WiFi + web + QR chuyen khoan)

  ESP32 KHONG dieu khien phan cung. Moi thu (LCD, keypad, xu, servo...) do STM32 lo.
  ESP32 lam 3 viec:
      1) Phat trang web cho khach (chon san pham, tra bang xu hoac chuyen khoan QR)
      2) Gui phim ao tu web cho STM32, nhan trang thai tu STM32 qua UART
      3) Tao ma QR MoMo/VietQR co san so tien + noi dung

  XAC NHAN DA NHAN TIEN (thu cong): chu may kiem tra tien trong app MoMo, roi
      - nhan phim D tren may, hoac
      - mo web, nhap ADMIN_PIN duoi ma QR va bam "Xac nhan da nhan tien".

  Mo bang dien thoai:  http://<IP>  hoac  http://maybanhang.local
  (IP hien tren LCD luc khoi dong va trong Serial Monitor 115200)
  Neu khong vao duoc WiFi sau 15 giay, ESP32 tu phat WiFi rieng:
      ten va mat khau AP duoc khai bao trong config.h, mo http://192.168.4.1

  THU VIEN CAN CAI (Library Manager):
      - QRCode   (Richard Moore)
    WiFi, WebServer, ESPmDNS co san trong ESP32 core.
    Board: "ESP32 Dev Module" (DOIT ESP32 DEVKIT V1).

  DAY NOI VOI STM32:
      ESP32 GPIO16 (RX2)  <-  STM32 PA9  (TX)
      ESP32 GPIO17 (TX2)  ->  STM32 PA10 (RX)
      GND ESP32 noi GND STM32
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "qrcode_rm.h"

#include "config.h"  

// ================== UART TOI STM32 ==================
#define LINK Serial2
const int LINK_RX = 16;
const int LINK_TX = 17;

char rxBuf[96];
uint8_t rxLen = 0;

// Trang thai may (phan anh tu STM32)
int mState = 0, mSel = -1, mCoins = 0, mPhase = 0, mBankSeq = 0;
unsigned long mVpc = 1000;                      // dong moi xu
int mPrices[4] = {0, 0, 0, 0};
uint32_t lastLinkMs = 0;

bool linkUp()
{
  return lastLinkMs != 0 && (uint32_t)(millis() - lastLinkMs) < 2500UL;
}

void handleLine(char* line)
{
  if (line[0] == 'S' && line[1] == ',') {
    int st, sel, coins, ph, seq, p0, p1, p2, p3;
    unsigned long vpc;
    if (sscanf(line, "S,%d,%d,%d,%d,%d,%lu,%d,%d,%d,%d",
               &st, &sel, &coins, &ph, &seq, &vpc, &p0, &p1, &p2, &p3) == 10) {
      mState = st; mSel = sel; mCoins = coins; mPhase = ph; mBankSeq = seq;
      mVpc = vpc;
      mPrices[0] = p0; mPrices[1] = p1; mPrices[2] = p2; mPrices[3] = p3;
      lastLinkMs = millis();
      if (lastLinkMs == 0) lastLinkMs = 1;
    }
  }
}

void readLink()
{
  while (LINK.available()) {
    char c = (char)LINK.read();

    if (c == '\n' || c == '\r') {
      if (rxLen > 0) {
        rxBuf[rxLen] = 0;
        handleLine(rxBuf);
        rxLen = 0;
      }
    }
    else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = c;
    }
    else {
      rxLen = 0;
    }
  }
}

// ================== WIFI + WEB ==================
WebServer server(80);
bool apMode = false;
bool netReady = false;

uint32_t lastKeyMs = 0;
const uint32_t KEY_GAP_MS = 150;

// ================== TAO MA QR VIETQR ==================
String tlv(const char* id, const String& value)
{
  char len[4];
  snprintf(len, sizeof(len), "%02u", (unsigned)value.length());
  return String(id) + len + value;
}

uint16_t crc16ccitt(const String& s)
{
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < s.length(); i++) {
    crc ^= (uint16_t)((uint8_t)s[i]) << 8;
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// Chuoi VietQR dong: so tien (tag 54) + noi dung (tag 62/08) + CRC
String buildVietQR(uint32_t amount, const String& memo)
{
  String account = tlv("00", QR_BANK_BIN) + tlv("01", QR_ACCOUNT);
  String merchant = tlv("00", QR_GUID) + tlv("01", account) + tlv("02", QR_SERVICE);
  String extra = tlv("05", QR_REF_05) + tlv("06", QR_REF_06) + tlv("08", memo);

  String p = tlv("00", "01");
  p += tlv("01", "12");
  p += tlv("38", merchant);
  p += tlv("53", "704");
  p += tlv("54", String(amount));
  p += tlv("58", "VN");
  p += tlv("62", extra);
  p += "6304";

  char crc[5];
  snprintf(crc, sizeof(crc), "%04X", crc16ccitt(p));
  p += crc;
  return p;
}

String qrBits;            // ma QR dang chuoi '0'/'1'
uint8_t qrSize = 0;

bool makeQr(const String& text)
{
  static uint8_t qrData[700];
  QRCode qr;

  if (qrcode_initText(&qr, qrData, QR_VERSION, ECC_LOW, text.c_str()) != 0) {
    return false;
  }

  qrSize = qr.size;
  qrBits = "";
  qrBits.reserve((size_t)qr.size * qr.size);
  for (uint8_t y = 0; y < qr.size; y++) {
    for (uint8_t x = 0; x < qr.size; x++) {
      qrBits += qrcode_getModule(&qr, x, y) ? '1' : '0';
    }
  }
  return true;
}

// ================== DON CHUYEN KHOAN ==================
bool orderActive = false;
int orderSeq = -1;
String orderCode;
uint32_t orderAmount = 0;
bool paidConfirmed = false;
uint32_t lastPaidSendMs = 0;

void sendPaid()
{
  LINK.print("PAID\n");
  lastPaidSendMs = millis();
  Serial.println("DA GUI PAID CHO STM32");
}

bool newOrder()
{
  if (mSel < 0 || mSel > 3) {
    return false;
  }

  orderAmount = (uint32_t)mPrices[mSel] * (uint32_t)mVpc;
  if (orderAmount == 0) {
    return false;
  }

  char c[12];
  snprintf(c, sizeof(c), "MBH%06lu", (unsigned long)(esp_random() % 1000000UL));
  orderCode = c;

  String payload = buildVietQR(orderAmount, orderCode);
  Serial.print("DON MOI ");
  Serial.print(orderCode);
  Serial.print(" - ");
  Serial.print(orderAmount);
  Serial.println(" VND");

  if (!makeQr(payload)) {
    Serial.println("LOI TAO MA QR");
    return false;
  }

  orderActive = true;
  paidConfirmed = false;
  return true;
}

void bankTick()
{
  if (!linkUp()) {
    return;                      // mat lien lac STM32: giu nguyen don, khong tao don moi
  }

  if (mState != 4) {
    if (orderActive) {
      orderActive = false;
      paidConfirmed = false;
      qrBits = "";
    }
    orderSeq = -1;
    return;
  }

  if (!orderActive && mBankSeq != orderSeq) {
    orderSeq = mBankSeq;
    newOrder();
  }

  // Chu may da xac nhan nhung STM32 chua chuyen trang thai -> gui lai moi giay (phong mat goi)
  if (orderActive && paidConfirmed && (uint32_t)(millis() - lastPaidSendMs) >= 1000UL) {
    sendPaid();
  }
}

// Gui dia chi web cho STM32 hien tren LCD (moi 5 giay)
void sendIp()
{
  static uint32_t last = 0;
  if (last != 0 && (uint32_t)(millis() - last) < 5000UL) {
    return;
  }
  last = millis();
  if (last == 0) last = 1;

  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LINK.print("IP,");
  LINK.print(apMode ? 'A' : 'W');
  LINK.print(',');
  LINK.print(ip);
  LINK.print('\n');
}

// ================== TRANG WEB + API ==================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Máy bán hàng</title>
<style>
:root{
  --bg:#e8eef0; --card:#f8fafb; --ink:#13232a; --muted:#576a72; --line:#cbd7db;
  --head:#2b4a56; --head-ink:#eaf2f5;
  --brand:#1f6f86; --brand-ink:#ffffff;
  --ok:#1c8f5f; --bad:#c43a34; --warn:#b97d00; --coin:#c9a24a;
  --p1:#d65a31; --p2:#2f9e8f; --p3:#7a5bd6; --p4:#d9a521;
  color-scheme:light;
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    --bg:#0d1519; --card:#15222a; --ink:#e4eef1; --muted:#90a5ad; --line:#27393f;
    --head:#1b323c; --head-ink:#e4eef1;
    --brand:#5cb9d3; --brand-ink:#04222c;
    --ok:#3cc08c; --bad:#ff7a70; --warn:#e0a82e; --coin:#d4b05a;
    color-scheme:dark;
  }
}
*{box-sizing:border-box}
html,body{margin:0}
body{
  background:var(--bg);color:var(--ink);
  font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
  min-height:100vh;
  padding-top:env(safe-area-inset-top,0px);
  padding-bottom:max(24px,env(safe-area-inset-bottom,0px));
}
button{font:inherit;color:inherit}
:focus-visible{outline:3px solid var(--brand);outline-offset:2px}
.wrap{max-width:480px;margin:0 auto;display:grid;gap:16px;padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px))}
header{background:var(--head);color:var(--head-ink);margin:0 calc(-1 * max(16px,env(safe-area-inset-left,0px)));padding:16px max(16px,env(safe-area-inset-right,0px)) 16px max(16px,env(safe-area-inset-left,0px));display:flex;justify-content:space-between;align-items:center;gap:12px}
h1{margin:0;font-size:20px;font-weight:700;letter-spacing:.02em}
.pill{display:inline-flex;align-items:center;gap:8px;font-size:13px;white-space:nowrap}
.pill i{width:10px;height:10px;border-radius:50%;background:var(--bad);display:block}
.pill.on i{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.banner{display:none;border:1px solid var(--bad);color:var(--bad);border-radius:12px;padding:10px 14px;font-size:14px}
.banner.show{display:block}

.steps{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;list-style:none;margin:0;padding:0}
.steps li{border-top:4px solid var(--line);padding-top:6px;font-size:13px;color:var(--muted);line-height:1.3}
.steps li.done{border-color:var(--ok);color:var(--ink)}
.steps li.on{border-color:var(--brand);color:var(--ink);font-weight:600}

.products{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.prod{
  position:relative;text-align:left;background:var(--card);border:2px solid var(--line);border-radius:16px;
  padding:14px 14px 14px 18px;cursor:pointer;display:grid;gap:2px;min-height:92px;overflow:hidden;
}
.prod::before{content:"";position:absolute;left:0;top:0;bottom:0;width:6px;background:var(--c)}
.prod b{font-size:17px}
.prod span{color:var(--muted);font-size:14px}
.prod .price{font-size:22px;font-weight:700;color:var(--ink);font-variant-numeric:tabular-nums}
.prod.sel{border-color:var(--brand);box-shadow:0 0 0 3px color-mix(in srgb,var(--brand) 25%,transparent)}
.prod:disabled{cursor:default;opacity:.55}
.prod.sel:disabled{opacity:1}

.panel{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:18px;display:grid;gap:12px}
.panel h2{margin:0;font-size:20px;line-height:1.25}
.panel p{margin:0;color:var(--muted)}
.coins{display:grid;gap:8px}
.coins .num{font-size:34px;font-weight:700;font-variant-numeric:tabular-nums;line-height:1}
.coins .num small{font-size:16px;font-weight:500;color:var(--muted)}
.dots{display:flex;flex-wrap:wrap;gap:8px}
.dot{width:26px;height:26px;border-radius:50%;border:2px solid var(--line)}
.dot.on{background:var(--coin);border-color:var(--coin)}
.bar{height:10px;border-radius:6px;background:var(--line);overflow:hidden}
.bar i{display:block;height:100%;background:var(--brand);width:0;transition:width .4s}
.bar.indet i{width:40%;animation:slide 1.2s ease-in-out infinite alternate}
@keyframes slide{from{margin-left:0}to{margin-left:60%}}
.btns{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.btns.one{grid-template-columns:1fr}
.btns .btn:nth-child(3):last-child{grid-column:1/-1}
.qrbox{display:grid;gap:8px;justify-items:center;text-align:center}
.qrbox canvas{width:100%;max-width:280px;height:auto;image-rendering:pixelated;border-radius:10px;background:#fff;border:1px solid var(--line)}
.qrbox a{color:var(--brand);font-size:14px}
.bank{width:100%;display:grid;gap:4px;background:var(--bg);border-radius:12px;padding:10px 12px;font-size:14px;text-align:left}
.bank div{display:flex;justify-content:space-between;gap:12px}
.bank span{color:var(--muted)}
.bank b{text-align:right;word-break:break-word}
.methods{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.method{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:10px 12px;font-size:14px;line-height:1.35}
.method b{display:block;font-size:15px}
.method span{color:var(--muted)}
.admin{width:100%;display:grid;gap:6px;border-top:1px dashed var(--line);padding-top:10px;text-align:left}
.pinrow{display:grid;grid-template-columns:110px 1fr;gap:8px}
.pinrow input{font:inherit;color:var(--ink);background:var(--bg);border:2px solid var(--line);border-radius:12px;padding:10px 12px;min-height:52px;width:100%}
.btn{border-radius:12px;border:2px solid var(--line);background:transparent;padding:14px 12px;font-weight:600;cursor:pointer;min-height:52px}
.btn.primary{background:var(--brand);border-color:var(--brand);color:var(--brand-ink)}
.btn.ghost{color:var(--bad);border-color:var(--bad)}
.btn:disabled{opacity:.4;cursor:default}
.note{font-size:13px;color:var(--muted)}
.ok{color:var(--ok)}
footer{text-align:center;color:var(--muted);font-size:12px}
@media (prefers-reduced-motion:reduce){.bar i,.bar.indet i{transition:none;animation:none}}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>Máy bán hàng</h1>
    <div class="pill" id="pill"><i></i><span id="pillTxt">Đang kết nối…</span></div>
  </header>

  <div class="banner" id="banner" role="alert"></div>

  <ol class="steps" id="steps">
    <li>1. Chọn sản phẩm</li>
    <li>2. Thanh toán</li>
    <li>3. Nhận hàng</li>
  </ol>

  <div class="products" id="products"></div>

  <div class="methods" aria-label="Hai hình thức thanh toán">
    <div class="method"><b>🪙 Bỏ xu</b><span>Bỏ xu vào khe trên máy</span></div>
    <div class="method"><b>📱 Chuyển khoản QR</b><span>MoMo / app ngân hàng</span></div>
  </div>

  <section class="panel" id="panel" aria-live="polite"></section>

  <footer id="foot"></footer>
</div>

<script>
const $ = id => document.getElementById(id);
const NAMES = ['Sản phẩm 1', 'Sản phẩm 2', 'Sản phẩm 3', 'Sản phẩm 4'];
const fmt = n => Number(n).toLocaleString('vi-VN') + 'đ';
let QR = null, qrLoading = false, pinDraft = '';
let S = null, linkOk = false, pending = false, pendTimer = 0, lastKey = '', lastSig = '';

/* Pure function: machine status -> what the screen shows. */
function model(s) {
  const m = { step: 0, canPick: false, sel: -1, title: '', text: '', coins: null, bar: null, qr: null, buttons: [], note: '' };
  if (!s || !s.online) return m;
  const sel = s.selected, valid = sel >= 0 && sel <= 3;
  const price = valid ? s.prices[sel] : 0, have = s.coins, vnd = valid ? s.vnd[sel] : 0;
  m.sel = valid ? sel : -1;
  switch (s.state) {
    case 0:
      m.step = 1; m.canPick = true;
      m.title = 'Chọn sản phẩm';
      m.text = 'Chạm vào một sản phẩm để bắt đầu. Bạn có thể trả bằng xu hoặc chuyển khoản QR.';
      break;
    case 1:
      m.step = 1; m.canPick = true;
      m.title = NAMES[sel] + ' · ' + price + ' xu / ' + fmt(vnd);
      m.text = 'Chọn cách thanh toán: bỏ xu hoặc chuyển khoản QR. Chạm sản phẩm khác nếu muốn đổi.';
      m.buttons = [
        { k: 'B', label: '🪙 Trả bằng xu', kind: 'primary' },
        { k: 'A', label: '📱 Chuyển khoản QR', kind: 'primary' },
        { k: 'C', label: 'Hủy', kind: 'ghost' }
      ];
      break;
    case 2: {
      const enough = have >= price;
      m.step = 2;
      m.title = enough ? 'Đã đủ xu' : 'Bỏ xu vào khe trên máy';
      m.text = enough ? 'Nhấn Mua để nhận ' + NAMES[sel].toLowerCase() + '.'
                      : NAMES[sel] + ' giá ' + price + ' xu. Còn thiếu ' + (price - have) + ' xu.';
      m.coins = { have, need: price, enough };
      m.buttons = [{ k: '#', label: 'Mua', kind: 'primary', disabled: !enough }, { k: 'C', label: 'Hủy giao dịch', kind: 'ghost' }];
      m.note = 'Hủy giao dịch sẽ không hoàn lại xu đã bỏ.';
      break;
    }
    case 4:
      m.step = 2;
      m.title = 'Chuyển khoản ' + fmt(vnd);
      m.text = 'Quét mã QR bằng MoMo hoặc app ngân hàng. Giữ nguyên số tiền và nội dung.';
      m.qr = { order: s.order };
      m.buttons = [{ k: 'C', label: 'Hủy giao dịch', kind: 'ghost' }];
      m.note = 'Máy sẽ nhả hàng sau khi chủ máy xác nhận đã nhận tiền.';
      break;
    default: {
      m.step = 3;
      const p = s.phase;
      if (p === 4) { m.title = 'Hoàn tất. Cảm ơn quý khách!'; m.text = 'Chúc bạn một ngày vui vẻ.'; m.bar = 1; }
      else if (p === 3) { m.title = 'Hãy lấy sản phẩm ở khay'; m.text = 'Máy sẽ đóng ngăn sau khi bạn lấy hàng.'; m.bar = 0.8; }
      else if (p === 2) { m.title = 'Đang chờ hàng rơi xuống khay'; m.text = 'Vui lòng đợi trong giây lát.'; m.bar = 0.5; }
      else { m.title = 'Đang nhả sản phẩm'; m.text = 'Vui lòng đợi trong giây lát.'; m.bar = 0.2; }
      break;
    }
  }
  return m;
}

function banner() {
  if (!linkOk) return 'Không kết nối được tới ESP32. Kiểm tra điện thoại đã vào đúng WiFi chưa.';
  if (S && !S.online) return 'ESP32 không nhận được tín hiệu từ STM32. Kiểm tra dây nối UART và nguồn.';
  return '';
}

/* ---- DOM ---- */
const prodEls = [];
for (let i = 0; i < 4; i++) {
  const b = document.createElement('button');
  b.type = 'button'; b.className = 'prod';
  b.style.setProperty('--c', 'var(--p' + (i + 1) + ')');
  b.innerHTML = '<b>' + NAMES[i] + '</b><span>Giá</span><div class="price">–</div>';
  b.addEventListener('click', () => press(String(i + 1)));
  $('products').appendChild(b); prodEls.push(b);
}
const stepEls = Array.from($('steps').children);

function render() {
  const msg = banner();
  $('banner').textContent = msg; $('banner').classList.toggle('show', !!msg);
  const on = linkOk && S && S.online;
  $('pill').classList.toggle('on', !!on);
  $('pillTxt').textContent = on ? 'Máy sẵn sàng' : (linkOk ? 'Máy không phản hồi' : 'Mất kết nối');

  const m = model(on ? S : null);
  stepEls.forEach((el, i) => {
    el.classList.toggle('on', i + 1 === m.step);
    el.classList.toggle('done', m.step > 0 && i + 1 < m.step);
  });
  prodEls.forEach((el, i) => {
    el.classList.toggle('sel', i === m.sel);
    el.disabled = !m.canPick || pending;
    el.querySelector('.price').textContent = on ? S.prices[i] + ' xu' : '–';
    el.querySelector('span').textContent = on ? fmt(S.vnd[i]) : 'Giá';
    el.setAttribute('aria-pressed', String(i === m.sel));
  });

  const sig = JSON.stringify([m, pending, msg]);
  if (sig === lastSig) return;
  lastSig = sig;
  const p = $('panel');
  p.textContent = '';
  const h = document.createElement('h2'); h.textContent = on ? m.title : 'Chưa có dữ liệu từ máy';
  p.appendChild(h);
  if (m.text || !on) { const t = document.createElement('p'); t.textContent = on ? m.text : 'Trang sẽ tự cập nhật khi kết nối lại.'; p.appendChild(t); }
  if (m.qr) p.appendChild(qrBlock(m.qr.order));
  if (m.coins) {
    const c = document.createElement('div'); c.className = 'coins';
    const n = document.createElement('div'); n.className = 'num';
    n.innerHTML = m.coins.have + ' <small>/ ' + m.coins.need + ' xu đã bỏ</small>';
    const d = document.createElement('div'); d.className = 'dots';
    for (let i = 0; i < m.coins.need; i++) { const x = document.createElement('i'); x.className = 'dot' + (i < m.coins.have ? ' on' : ''); d.appendChild(x); }
    c.appendChild(n); c.appendChild(d); p.appendChild(c);
  }
  if (m.bar !== null) {
    const b = document.createElement('div'); b.className = 'bar'; const f = document.createElement('i');
    f.style.width = (m.bar * 100) + '%'; b.appendChild(f); p.appendChild(b);
  }
  if (m.buttons.length) {
    const r = document.createElement('div'); r.className = 'btns' + (m.buttons.length === 1 ? ' one' : '');
    m.buttons.forEach(bt => {
      const b = document.createElement('button'); b.type = 'button';
      b.className = 'btn ' + bt.kind; b.textContent = bt.label; b.disabled = !!bt.disabled || pending;
      b.addEventListener('click', () => press(bt.k));
      r.appendChild(b);
    });
    p.appendChild(r);
  }
  if (m.note) { const n = document.createElement('p'); n.className = 'note'; n.textContent = m.note; p.appendChild(n); }
}

function drawQr(q) {
  const n = q.size, z = 4, cell = 4, dim = (n + 2 * z) * cell;
  const cv = document.createElement('canvas'); cv.width = dim; cv.height = dim;
  const g = cv.getContext('2d');
  g.fillStyle = '#fff'; g.fillRect(0, 0, dim, dim);
  g.fillStyle = '#000';
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++)
    if (q.bits.charAt(y * n + x) === '1') g.fillRect((x + z) * cell, (y + z) * cell, cell, cell);
  return cv;
}

function qrBlock(order) {
  const box = document.createElement('div'); box.className = 'qrbox';
  if (QR && QR.order === order) {
    const cv = drawQr(QR); box.appendChild(cv);
    const info = document.createElement('div'); info.className = 'bank';
    [['Người nhận', QR.name], ['Số tiền', fmt(QR.amount)], ['Nội dung', QR.memo]].forEach(r => {
      const d = document.createElement('div');
      const a = document.createElement('span'); a.textContent = r[0];
      const b = document.createElement('b'); b.textContent = r[1];
      d.appendChild(a); d.appendChild(b); info.appendChild(d);
    });
    box.appendChild(info);
    const dl = document.createElement('a'); dl.textContent = 'Lưu ảnh QR để quét từ thư viện ảnh';
    dl.href = cv.toDataURL('image/png'); dl.download = 'qr-thanh-toan.png';
    box.appendChild(dl);

    const adm = document.createElement('div'); adm.className = 'admin';
    const lb = document.createElement('p'); lb.className = 'note';
    lb.textContent = 'Dành cho chủ máy: sau khi đã nhận tiền, nhập PIN để xác nhận.';
    const row = document.createElement('div'); row.className = 'pinrow';
    const inp = document.createElement('input');
    inp.type = 'password'; inp.inputMode = 'numeric'; inp.autocomplete = 'off';
    inp.placeholder = 'PIN'; inp.value = pinDraft;
    inp.setAttribute('aria-label', 'PIN chủ máy');
    inp.addEventListener('input', () => { pinDraft = inp.value; });
    const bt = document.createElement('button'); bt.type = 'button'; bt.className = 'btn primary';
    bt.textContent = 'Xác nhận đã nhận tiền';
    const msg = document.createElement('p'); msg.className = 'note'; msg.setAttribute('role', 'status');
    bt.addEventListener('click', () => confirmPay(inp.value, msg, bt));
    row.appendChild(inp); row.appendChild(bt);
    adm.appendChild(lb); adm.appendChild(row); adm.appendChild(msg);
    box.appendChild(adm);
  } else {
    const t = document.createElement('p'); t.textContent = 'Đang tạo mã QR…'; box.appendChild(t);
    loadQr(order);
  }
  return box;
}

async function confirmPay(pin, msg, bt) {
  if (!pin) { msg.textContent = 'Hãy nhập PIN.'; return; }
  bt.disabled = true;
  try {
    const r = await fetch('/api/confirm', {
      method: 'POST', cache: 'no-store',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'pin=' + encodeURIComponent(pin)
    });
    const j = await r.json().catch(() => ({}));
    if (r.ok && j.ok) { msg.textContent = 'Đã xác nhận, máy đang nhả hàng…'; pinDraft = ''; setTimeout(poll, 300); }
    else if (r.status === 429) msg.textContent = 'Nhập sai nhiều lần. Thử lại sau 30 giây.';
    else if (r.status === 403) msg.textContent = 'Sai PIN.';
    else msg.textContent = 'Không có giao dịch chuyển khoản nào đang chờ.';
  } catch (e) { msg.textContent = 'Không gửi được tới máy.'; }
  bt.disabled = false;
}

async function loadQr(order) {
  if (qrLoading) return;
  qrLoading = true;
  try { const q = await api('/api/qr'); if (q.order === order) { QR = q; lastSig = ''; } }
  catch (e) {}
  finally { qrLoading = false; }
}

async function api(path, opt) {
  const c = new AbortController(), t = setTimeout(() => c.abort(), 2500);
  try {
    const r = await fetch(path, Object.assign({ cache: 'no-store', signal: c.signal }, opt || {}));
    if (!r.ok) throw new Error(r.status);
    return await r.json();
  } finally { clearTimeout(t); }
}

async function poll() {
  try {
    S = await api('/api/status'); linkOk = true;
    const k = S.state + '/' + S.selected + '/' + S.coins + '/' + S.phase;
    if (pending && k !== lastKey) { pending = false; clearTimeout(pendTimer); }
    lastKey = k;
    $('foot').textContent = S.ip + ' · WiFi ' + S.rssi + ' dBm';
  } catch (e) { S = null; linkOk = false; }
  render();
}

async function press(k) {
  if (pending) return;
  pending = true; clearTimeout(pendTimer);
  pendTimer = setTimeout(() => { pending = false; render(); }, 1200);
  render();
  try { await api('/api/key?k=' + encodeURIComponent(k), { method: 'POST' }); } catch (e) {}
  setTimeout(poll, 300);
}

render();
poll();
setInterval(poll, 400);
</script>
</body>
</html>
)rawliteral";

String statusJson(bool sent)
{
  String j = "{";
  j += "\"online\":" + String(linkUp() ? "true" : "false");
  j += ",\"sent\":" + String(sent ? "true" : "false");
  j += ",\"state\":" + String(mState);
  j += ",\"selected\":" + String(mSel);
  j += ",\"coins\":" + String(mCoins);
  j += ",\"phase\":" + String(mPhase);
  j += ",\"prices\":[";
  for (int i = 0; i < 4; i++) {
    if (i) j += ",";
    j += String(mPrices[i]);
  }
  j += "]";
  j += ",\"vnd\":[";
  for (int i = 0; i < 4; i++) {
    if (i) j += ",";
    j += String((uint32_t)mPrices[i] * (uint32_t)mVpc);
  }
  j += "]";
  j += ",\"order\":" + String(orderActive ? orderSeq : 0);
  j += ",\"rssi\":" + String(apMode ? 0 : WiFi.RSSI());
  j += ",\"ip\":\"" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\"";
  j += "}";
  return j;
}

void sendJson(const String& j)
{
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleRoot()
{
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleStatus()
{
  sendJson(statusJson(false));
}

void handleKey()
{
  if (!server.hasArg("k")) {
    server.send(400, "application/json", "{\"error\":\"thieu tham so k\"}");
    return;
  }

  String k = server.arg("k");

  if (k.length() != 1 || k[0] == 0 || strchr("1234ABC#", k[0]) == NULL) {
    server.send(400, "application/json", "{\"error\":\"phim khong hop le\"}");
    return;
  }

  bool sent = false;

  if ((uint32_t)(millis() - lastKeyMs) >= KEY_GAP_MS) {
    lastKeyMs = millis();
    LINK.print("K,");
    LINK.print(k[0]);
    LINK.print('\n');
    sent = true;
    Serial.print("PHIM AO: ");
    Serial.println(k);
  }

  sendJson(statusJson(sent));
}

void handleQr()
{
  if (!orderActive || qrBits.length() == 0) {
    server.send(404, "application/json", "{\"error\":\"khong co giao dich chuyen khoan\"}");
    return;
  }

  String j = "{";
  j += "\"order\":" + String(orderSeq);
  j += ",\"size\":" + String((int)qrSize);
  j += ",\"amount\":" + String(orderAmount);
  j += ",\"memo\":\"" + orderCode + "\"";
  j += ",\"name\":\"" + String(ACCOUNT_NAME) + "\"";
  j += ",\"bits\":\"" + qrBits + "\"";
  j += "}";
  sendJson(j);
}

uint8_t pinFails = 0;
uint32_t pinLockUntil = 0;

// Chu may nhap dung PIN tren web -> bao STM32 da nhan tien.
void handleConfirm()
{
  if ((int32_t)(millis() - pinLockUntil) < 0) {
    server.send(429, "application/json", "{\"ok\":false,\"error\":\"locked\"}");
    return;
  }

  if (!linkUp() || mState != 4) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"no_order\"}");
    return;
  }

  if (server.arg("pin") == String(ADMIN_PIN)) {
    pinFails = 0;
    paidConfirmed = true;
    sendPaid();
    Serial.println("WEB: CHU MAY XAC NHAN DA NHAN TIEN");
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    pinFails++;
    if (pinFails >= 5) {
      pinFails = 0;
      pinLockUntil = millis() + 30000UL;
    }
    server.send(403, "application/json", "{\"ok\":false,\"error\":\"wrong_pin\"}");
  }
}

void handleNotFound()
{
  server.send(404, "text/plain", "Khong tim thay");
}

// ================== WIFI ==================
void startWifi()
{
  Serial.print("DANG KET NOI WIFI ");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    WiFi.setSleep(false);
    Serial.print("DA KET NOI. Mo trinh duyet: http://");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(HOSTNAME)) {
      Serial.print("Hoac: http://");
      Serial.print(HOSTNAME);
      Serial.println(".local");
    }
  } else {
    apMode = true;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.println("KHONG VAO DUOC WIFI -> PHAT WIFI RIENG");
    Serial.print("Ten: ");
    Serial.print(AP_SSID);
    Serial.print("  Mat khau: ");
    Serial.println(AP_PASS);
    Serial.print("Mo trinh duyet: http://");
    Serial.println(WiFi.softAPIP());
  }
}

// ================== SETUP / LOOP ==================
void setup()
{
  Serial.begin(115200);

  LINK.setRxBufferSize(1024);
  LINK.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);

  startWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/key", HTTP_POST, handleKey);
  server.on("/api/qr", HTTP_GET, handleQr);
  server.on("/api/confirm", HTTP_POST, handleConfirm);
  server.onNotFound(handleNotFound);
  server.begin();

  netReady = true;
  Serial.println("ESP32 SAN SANG!");
}

void loop()
{
  readLink();

  if (netReady) {
    sendIp();
  }

  bankTick();
  server.handleClient();
  delay(2);
}
