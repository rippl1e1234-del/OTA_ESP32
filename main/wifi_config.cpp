#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include <string>
#include <vector>

#include "NeonLED.h" 

extern "C" {
    #include "wifi_config.h"
}

#define TAG "WIFI_MGR"
#define AP_TIMEOUT_MS (5 * 60 * 1000)
#define FACTORY_RESET_PIN "1234"
#define DNS_PORT 53

static httpd_handle_t server = NULL;
static TimerHandle_t ap_timer = NULL;
static TaskHandle_t dns_task_handle = NULL;
static int dns_socket_fd = -1;

// --- DYNAMIC SSID ---
void get_device_ssid(char *buffer, size_t max_len) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(buffer, max_len, "SGLD%02X%02X%02X", mac[5], mac[4], mac[3]);
}

// --- HTML CONTENT (Updated) ---
static const char CAPTIVE_HTML[] = R"HTML(
<html>
<head>
  <title>Ripple Device Configuration</title>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; padding: 24px; background:#f4f4f4; margin: 0; }
    .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 30px; gap: 12px; }
    h2 { color:#333; text-align:center; margin: 0; flex: 1; }
    .factory-reset-btn { background: #dc3545; color: white; border: none; border-radius: 8px; width: 42px; height: 42px; cursor: pointer; display: inline-flex; align-items: center; justify-content: center; }
    .factory-reset-btn:hover { background: #b02a37; }
    form { max-width: 480px; margin: 0 auto; background:#fff; padding: 30px; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.08); }
    label { display:block; margin-bottom:8px; font-weight:bold; color: #333; }
    input[type="text"], input[type="password"] { width:100%; padding:12px; margin-bottom:20px; border:1px solid #ccc; border-radius:4px; box-sizing:border-box; font-size: 16px; }
    input:focus { outline: none; border-color: #007bff; box-shadow: 0 0 0 2px rgba(0,123,255,0.25); }
    .section-title { margin-top: 0; margin-bottom: 20px; font-weight: bold; border-bottom: 2px solid #007bff; padding-bottom: 8px; color: #007bff; }
    .note { font-size:0.9em; color:#666; margin-top: -10px; margin-bottom: 25px; font-style: italic; }
    .btn { background:#007bff; color:#fff; padding:14px 20px; border:none; border-radius:4px; cursor:pointer; width:100%; font-size:16px; font-weight: bold; }
    .btn:hover { background:#0056b3; }
    .pass-wrap { position: relative; margin-bottom: 20px; }
    .toggle-pass { position: absolute; right: 12px; top: 36%; transform: translateY(-50%); background: transparent; border: none; cursor: pointer; padding: 0; color: #666; }
    
    .svg-icon { width: 24px; height: 24px; display: inline-block; vertical-align: -3px; fill: #000; }
    .svg-icon svg { width: 100%; height: 100%; display: block; fill: inherit; }

    /* Modal Styles */
    .modal { display:none; position:fixed; inset:0; background:rgba(0,0,0,0.5); align-items:center; justify-content:center; padding:16px; z-index:9999; }
    .modal-card { background:#fff; width:100%; max-width:420px; border-radius:10px; padding:24px; box-shadow:0 8px 30px rgba(0,0,0,0.2); }
    .modal-card h3 { margin:0 0 15px 0; color: #333; }
    .modal-row { display:flex; gap:10px; margin-top:20px; }
    .modal-row .btn { width:100%; }
    .modal-status { margin-top:10px; font-size:0.95em; color:#444; }
    
    .btn-danger { background:#dc3545; }
    .btn-secondary { background:#6c757d; }
    .btn-success { background:#28a745; }
    
    .success-message { text-align: center; padding: 40px; background: #fff; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.08); max-width: 480px; margin: 50px auto; }
    .success-icon { width: 48px; height: 48px; margin: 0 auto 20px auto; color: #28a745; }
    .success-icon .svg-icon { width: 48px; height: 48px; fill: #28a745; }
    .success-text { font-size: 18px; margin-bottom: 20px; color: #333; }
    .reboot-notice { color: #dc3545; font-weight: bold; margin-top: 20px; }
  </style>

  <script>
  const ICONS = {
    warning: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 640"><path fill="#e97f1c" d="M320 64C334.7 64 348.2 72.1 355.2 85L571.2 485C577.9 497.4 577.6 512.4 570.4 524.5C563.2 536.6 550.1 544 536 544L104 544C89.9 544 76.8 536.6 69.6 524.5C62.4 512.4 62.1 497.4 68.8 485L284.8 85C291.8 72.1 305.3 64 320 64zM320 416C302.3 416 288 430.3 288 448C288 465.7 302.3 480 320 480C337.7 480 352 465.7 352 448C352 430.3 337.7 416 320 416zM320 224C301.8 224 287.3 239.5 288.6 257.7L296 361.7C296.9 374.2 307.4 384 319.9 384C332.5 384 342.9 374.3 343.8 361.7L351.2 257.7C352.5 239.5 338.1 224 319.8 224z"/></svg>`,
    gear: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><g transform="translate(1.4 1.4) scale(2.81)"><path fill="#ffffff" d="M 52.445 90 h -14.89 c -1.801 0 -3.266 -1.465 -3.266 -3.265 v -4.428 c -2.808 -0.805 -5.52 -1.931 -8.089 -3.357 l -3.136 3.135 c -1.232 1.234 -3.386 1.234 -4.619 0 L 7.916 71.555 c -1.273 -1.274 -1.273 -3.345 0 -4.619 l 3.137 -3.136 c -1.427 -2.57 -2.553 -5.282 -3.358 -8.09 H 3.266 C 1.465 55.711 0 54.246 0 52.445 v -14.89 c 0 -1.801 1.465 -3.266 3.266 -3.266 h 4.428 c 0.805 -2.809 1.931 -5.521 3.358 -8.09 l -3.136 -3.136 c -1.273 -1.273 -1.273 -3.345 0 -4.618 L 18.445 7.916 c 1.231 -1.233 3.384 -1.236 4.619 0 l 3.136 3.136 c 2.57 -1.427 5.282 -2.552 8.089 -3.358 V 3.266 C 34.289 1.466 35.754 0 37.555 0 h 14.89 c 1.801 0 3.266 1.465 3.266 3.266 v 4.427 c 2.807 0.805 5.519 1.931 8.089 3.358 l 3.136 -3.136 c 0.617 -0.617 1.437 -0.957 2.309 -0.957 c 0 0 0 0 0.001 0 c 0.872 0 1.693 0.34 2.309 0.958 l 10.528 10.528 c 1.273 1.274 1.272 3.346 0 4.618 l -3.136 3.137 c 1.427 2.57 2.553 5.283 3.357 8.09 h 4.428 c 1.801 0 3.266 1.465 3.266 3.266 v 14.89 c 0 1.8 -1.465 3.265 -3.266 3.265 h -4.428 c -0.805 2.808 -1.931 5.52 -3.357 8.09 l 3.136 3.136 c 1.272 1.274 1.272 3.345 0.001 4.618 l -10.53 10.529 c -1.231 1.234 -3.383 1.234 -4.619 0 L 63.8 78.949 c -2.57 1.427 -5.282 2.553 -8.089 3.357 v 4.428 C 55.711 88.534 54.246 90 52.445 90 z M 25.72 75.231 l 0.991 0.59 c 2.953 1.756 6.124 3.072 9.426 3.912 l 1.118 0.285 v 6.716 c 0 0.164 0.134 0.298 0.299 0.298 h 14.89 c 0.165 0 0.299 -0.134 0.299 -0.298 v -6.716 l 1.117 -0.285 c 3.3 -0.839 6.472 -2.155 9.426 -3.912 l 0.992 -0.59 l 4.754 4.755 c 0.146 0.147 0.281 0.144 0.422 0.001 l 10.53 -10.529 c 0.116 -0.116 0.116 -0.307 -0.001 -0.424 l -4.755 -4.753 l 0.59 -0.992 c 1.756 -2.954 3.072 -6.126 3.912 -9.427 l 0.285 -1.117 h 6.716 c 0.165 0 0.299 -0.134 0.299 -0.298 v -14.89 c 0 -0.165 -0.134 -0.299 -0.299 -0.299 h -6.716 l -0.285 -1.118 c -0.839 -3.301 -2.156 -6.473 -3.912 -9.426 l -0.59 -0.991 l 4.756 -4.755 c 0.116 -0.116 0.116 -0.307 -0.001 -0.424 L 69.457 10.014 c -0.144 -0.144 -0.278 -0.146 -0.423 0 l -4.754 4.754 l -0.992 -0.589 c -2.954 -1.757 -6.126 -3.073 -9.426 -3.913 l -1.117 -0.284 V 3.266 c 0 -0.165 -0.134 -0.299 -0.299 -0.299 h -14.89 c -0.165 0 -0.299 0.134 -0.299 0.299 v 6.716 l -1.118 0.284 c -3.302 0.84 -6.473 2.156 -9.426 3.912 l -0.991 0.59 l -4.755 -4.755 c -0.145 -0.144 -0.278 -0.144 -0.423 0 L 10.014 20.543 c -0.116 0.116 -0.116 0.306 0 0.423 l 4.755 4.755 l -0.59 0.991 c -1.756 2.953 -3.072 6.124 -3.912 9.426 l -0.284 1.118 H 3.266 c -0.165 0 -0.299 0.134 -0.299 0.299 v 14.89 c 0 0.164 0.134 0.298 0.299 0.298 h 6.716 l 0.284 1.117 c 0.84 3.301 2.156 6.473 3.913 9.427 l 0.589 0.992 l -4.755 4.754 c -0.116 0.117 -0.116 0.306 0 0.423 l 10.528 10.528 c 0.145 0.145 0.279 0.145 0.423 0 L 25.72 75.231 z M 45 69.452 c -13.482 0 -24.451 -10.969 -24.451 -24.451 S 31.517 20.549 45 20.549 S 69.451 31.518 69.451 45 S 58.482 69.452 45 69.452 z M 45 23.516 c -11.846 0 -21.484 9.638 -21.484 21.484 S 33.153 66.485 45 66.485 S 66.484 56.847 66.484 45 S 56.846 23.516 45 23.516 z" /></g></svg>`,
    eye: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 640"><path d="M320 96C239.2 96 174.5 132.8 127.4 176.6C80.6 220.1 49.3 272 34.4 307.7C31.1 315.6 31.1 324.4 34.4 332.3C49.3 368 80.6 420 127.4 463.4C174.5 507.1 239.2 544 320 544C400.8 544 465.5 507.2 512.6 463.4C559.4 419.9 590.7 368 605.6 332.3C608.9 324.4 608.9 315.6 605.6 307.7C590.7 272 559.4 220 512.6 176.6C465.5 132.9 400.8 96 320 96zM176 320C176 240.5 240.5 176 320 176C399.5 176 464 240.5 464 320C464 399.5 399.5 464 320 464C240.5 464 176 399.5 176 320zM320 256C320 291.3 291.3 320 256 320C244.5 320 233.7 317 224.3 311.6C223.3 322.5 224.2 333.7 227.2 344.8C240.9 396 293.6 426.4 344.8 412.7C396 399 426.4 346.3 412.7 295.1C400.5 249.4 357.2 220.3 311.6 224.3C316.9 233.6 320 244.4 320 256z"/></svg>`,
    eyeOff: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 640"><path d="M73 39.1C63.6 29.7 48.4 29.7 39.1 39.1C29.8 48.5 29.7 63.7 39 73.1L567 601.1C576.4 610.5 591.6 610.5 600.9 601.1C610.2 591.7 610.3 576.5 600.9 567.2L504.5 470.8C507.2 468.4 509.9 466 512.5 463.6C559.3 420.1 590.6 368.2 605.5 332.5C608.8 324.6 608.8 315.8 605.5 307.9C590.6 272.2 559.3 220.2 512.5 176.8C465.4 133.1 400.7 96.2 319.9 96.2C263.1 96.2 214.3 114.4 173.9 140.4L73 39.1zM236.5 202.7C260 185.9 288.9 176 320 176C399.5 176 464 240.5 464 320C464 351.1 454.1 379.9 437.3 403.5L402.6 368.8C415.3 347.4 419.6 321.1 412.7 295.1C399 243.9 346.3 213.5 295.1 227.2C286.5 229.5 278.4 232.9 271.1 237.2L236.4 202.5zM357.3 459.1C345.4 462.3 332.9 464 320 464C240.5 464 176 399.5 176 320C176 307.1 177.7 294.6 180.9 282.7L101.4 203.2C68.8 240 46.4 279 34.5 307.7C31.2 315.6 31.2 324.4 34.5 332.3C49.4 368 80.7 420 127.5 463.4C174.6 507.1 239.3 544 320.1 544C357.4 544 391.3 536.1 421.6 523.4L357.4 459.2z"/></svg>`,
    loader: `<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M21 12a9 9 0 1 1-9-9"></path></svg>`
  };
  
  function setIcon(el, svgMarkup) { if (el) el.innerHTML = svgMarkup; }

  window.addEventListener("load", function () {
    setIcon(document.getElementById("factory_icon"), ICONS.gear);
    setIcon(document.getElementById("warn_icon"), ICONS.warning);
    setIcon(document.getElementById("eye_icon"), ICONS.eyeOff);

    var pw = document.getElementById("password");
    var btn = document.getElementById("toggle_password");
    if (!pw || !btn) return;

    btn.addEventListener("click", function () {
      if (pw.disabled) return;
      var isHidden = (pw.type === "password");
      pw.type = isHidden ? "text" : "password";
      setIcon(document.getElementById("eye_icon"), isHidden ? ICONS.eye : ICONS.eyeOff);
      btn.setAttribute("aria-label", isHidden ? "Hide password" : "Show password");
    });
  });

  async function postForm(url, data) {
    const body = new URLSearchParams();
    for (const [k,v] of Object.entries(data||{})) body.append(k, String(v));
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body.toString()
    });
    let json = {};
    try { json = await res.json(); } catch (e) {}
    return { ok: res.ok && json && json.ok, status: res.status, json };
  }

  async function saveWiFiCredentials() {
    const ssid = document.getElementById('ssid').value.trim();
    const password = document.getElementById('password').value.trim();
    if (!ssid) { alert('Please enter WiFi SSID'); return false; }
    if (!password) { alert('Please enter WiFi Password'); return false; }
    
    const submitBtn = document.querySelector('button[type="submit"]');
    const originalText = submitBtn.innerHTML;
    submitBtn.disabled = true; submitBtn.innerHTML = 'Saving...';

    try {
      const formData = new FormData(document.querySelector('form'));
      const params = new URLSearchParams(formData);
      const response = await fetch('/save', { method: 'POST', body: params });
      
      if (response.ok) {
        showSaveSuccessPopup();
        submitBtn.innerHTML = originalText; submitBtn.disabled = false;
        return true;
      } else {
        const errorText = await response.text();
        alert('Error: ' + (errorText || 'Unknown'));
        submitBtn.disabled = false; submitBtn.innerHTML = originalText;
        return false;
      }
    } catch (error) {
      alert('Network error: ' + error.message);
      submitBtn.disabled = false; submitBtn.innerHTML = originalText;
      return false;
    }
  }

  function showSaveSuccessPopup() {
    const modalHTML = `
      <div class="modal" id="save_success_modal">
        <div class="modal-card">
          <p>WiFi Credentials Saved</p>
          <p class="reboot-notice">Your Wi-Fi credentials are being configured. Please complete the consumer registration by scanning the QR code on the product box.</p>
          <div class="modal-row">
            <button class="btn btn-success" type="button" id="confirm_reboot">OK</button>
          </div>
        </div>
      </div>`;
    const container = document.createElement('div');
    container.innerHTML = modalHTML;
    document.body.appendChild(container);
    
    const modal = document.getElementById('save_success_modal');
    modal.style.display = 'flex';

    document.getElementById('confirm_reboot').addEventListener('click', function() {
      document.querySelector('form').style.display = 'none';
      modal.style.display = 'none';
      showRebootingMessage();
      triggerDeviceReboot();
    });
  }

  function showRebootingMessage() {
    const html = `
      <div class="success-message">
        <div class="success-icon"><span class="svg-icon" aria-hidden="true">${ICONS.loader}</span></div>
        <h2>Device Rebooting</h2>
        <p class="success-text">The device is now rebooting with the new WiFi settings.</p>
      </div>`;
    const div = document.createElement('div');
    div.innerHTML = html;
    document.querySelector('.header').after(div);
  }

  function showSetupCompletedMessage() {
    const form = document.querySelector('form');
    if (form) form.style.display = 'none';
    const existingMessages = document.querySelectorAll('.success-message, .reboot-notice-container');
    existingMessages.forEach(msg => msg.remove());

    const messageHTML = `
      <div class="success-message">
        <h2>WiFi Setup Completed</h2>
        <p class="success-text">Your Wi-Fi credentials are being configured. Please complete the consumer registration by scanning the QR code on the product box.</p>
     </div>`;
    const messageContainer = document.createElement('div');
    messageContainer.innerHTML = messageHTML;
    document.querySelector('.header').after(messageContainer);
  }

  async function triggerDeviceReboot() {
    try { await fetch('/reboot', { method: 'POST' }); } catch (e) {}
    setTimeout(() => { showSetupCompletedMessage(); }, 3000);
  }

  window.addEventListener("load", function() {
    const frBtn = document.getElementById('factory_reset_btn');
    const frModal = document.getElementById('fr_modal');
    const frPw = document.getElementById('fr_pw');
    const frCancel = document.getElementById('fr_cancel');
    const frContinue = document.getElementById('fr_continue');
    const frNo = document.getElementById('fr_no');
    const frYes = document.getElementById('fr_yes');
    const frStepPw = document.getElementById('fr_step_pw');
    const frStepConfirm = document.getElementById('fr_step_confirm');
    const frStatus = document.getElementById('fr_status');
    const frStatus2 = document.getElementById('fr_status2');

    function frOpen() {
      if (!frModal) return;
      frModal.style.display = 'flex';
      frStepPw.style.display = ''; frStepConfirm.style.display = 'none';
      frStatus.textContent = ''; frStatus2.textContent = '';
      frPw.value = ''; frPw.focus();
    }
    function frClose() { if (frModal) frModal.style.display = 'none'; }

    if (frBtn) frBtn.addEventListener('click', frOpen);
    if (frCancel) frCancel.addEventListener('click', frClose);
    if (frNo) frNo.addEventListener('click', frClose);

    if (frContinue) frContinue.addEventListener('click', async () => {
      const pw = (frPw.value || '').trim();
      if (!pw) { frStatus.textContent = 'Password required.'; return; }
      frStatus.textContent = 'Checking...';
      const r = await postForm('/factory_check', { pw });
      if (!r.ok) {
        frStatus.textContent = (r.json && r.json.err) ? r.json.err : 'Invalid password.';
        return;
      }
      frStatus.textContent = '';
      frStepPw.style.display = 'none'; frStepConfirm.style.display = '';
    });

    if (frYes) frYes.addEventListener('click', async () => {
      const pw = (frPw.value || '').trim();
      frStatus2.textContent = 'Erasing EEPROM...';
      frYes.disabled = true;
      const r = await postForm('/factory_reset', { pw });
      if (!r.ok) { frStatus2.textContent = 'Failed.'; frYes.disabled = false; return; }
      frStatus2.textContent = 'Done. Rebooting...';
    });

    const form = document.querySelector('form');
    if (form) {
      form.addEventListener('submit', function(e) {
        e.preventDefault();
        saveWiFiCredentials();
      });
    }
  });
  </script>
</head>

<body>
  <div class="header">
    <h2>Ripple Device Configuration</h2>
    <button class="factory-reset-btn" type="button" id="factory_reset_btn" aria-label="Factory Reset">
      <span class="svg-icon" aria-hidden="true" id="factory_icon"></span>
    </button>
  </div>

  <form method="POST" action="/save">
    <input type="hidden" name="user_type" value="1">
    <input type="hidden" name="threshold" value="%THRESHOLD%">
    <input type="hidden" name="uid" value="%UID%">
    <input type="hidden" name="email" value="%EMAIL%">
    <input type="hidden" name="mobile" value="%MOBILE%">
    <input type="hidden" name="country_code" value="%CC%">
    <input type="hidden" name="name" value="%NAME%">
    <input type="hidden" name="flatno" value="%FLAT%">
    <input type="hidden" name="floor" value="%FLOOR%">
    <input type="hidden" name="block" value="%BLOCK%">
    <input type="hidden" name="tower" value="%TOWER%">
    <input type="hidden" name="address" value="%ADDRESS%">
    <input type="hidden" name="installedlocation" value="%LOC%">

    <div class="section-title">WiFi Configuration</div>
    
    <label for="ssid">WiFi Network Name (SSID)</label>
    <input type="text" id="ssid" name="ssid" placeholder="Enter your WiFi network name" required value="%SSID%">

    <label for="password">WiFi Password</label>
    <div class="pass-wrap">
      <input type="password" id="password" name="password" placeholder="Enter your WiFi password" required>
      <button type="button" class="toggle-pass" id="toggle_password" aria-label="Show password">
        <span class="svg-icon" aria-hidden="true" id="eye_icon"></span>
      </button>
    </div>

    <div class="note">Click Save to configure WiFi. A confirmation dialog will appear before the device reboots.</div>
    <button class="btn" type="submit">Save</button>
  </form>

  <div class="modal" id="fr_modal" aria-hidden="true">
    <div class="modal-card">
      <h3>Factory Reset Confirmation</h3>
      <div id="fr_step_pw">
        <p style="margin-bottom: 15px; color: #666;">Enter password to continue with factory reset:</p>
        <input type="password" id="fr_pw" placeholder="Enter password" style="width: 100%; padding: 10px; margin-bottom: 15px;">
        <div class="modal-row">
          <button class="btn btn-secondary" type="button" id="fr_cancel">Cancel</button>
          <button class="btn btn-danger" type="button" id="fr_continue">Continue</button>
        </div>
        <div class="modal-status" id="fr_status"></div>
      </div>
      <div id="fr_step_confirm" style="display:none;">
        <p style="color: #dc3545; font-weight: bold;">
          <span class="svg-icon" aria-hidden="true" id="warn_icon" style="margin-right:6px;"></span>
          Warning: This action cannot be undone!
        </p>
        <p>Are you sure you want to erase all stored data from the device?</p>
        <div class="modal-row">
          <button class="btn btn-secondary" type="button" id="fr_no">No, Keep Data</button>
          <button class="btn btn-danger" type="button" id="fr_yes">Yes, Erase All Data</button>
        </div>
        <div class="modal-status" id="fr_status2"></div>
      </div>
    </div>
  </div>
</body>
</html>
)HTML";

// --- UTILS ---
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10); else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10); else b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void get_post_param(char *buf, const char *key, char *out, size_t out_size) {
    char *ptr = strstr(buf, key);
    if(ptr) {
        ptr += strlen(key);
        if(*ptr == '=') ptr++;
        char *end = strchr(ptr, '&');
        size_t len = (end) ? (end - ptr) : strlen(ptr);
        if(len >= out_size) len = out_size - 1;
        char temp[100] = {0};
        strncpy(temp, ptr, len);
        url_decode(out, temp); 
    }
}

// --- HANDLERS ---
static esp_err_t root_get_handler(httpd_req_t *req) {
    std::string page = CAPTIVE_HTML;
    
    nvs_handle_t my_handle;
    char saved_ssid[33] = {0};
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t len = sizeof(saved_ssid);
        nvs_get_str(my_handle, "ssid", saved_ssid, &len);
        nvs_close(my_handle);
    }

    char uid_str[32];
    get_device_ssid(uid_str, sizeof(uid_str));

    auto replace_all = [&](const char* target, const char* value) {
        size_t pos = 0;
        while ((pos = page.find(target, pos)) != std::string::npos) {
            page.replace(pos, strlen(target), value);
            pos += strlen(value);
        }
    };

    // Replacements matching Captive.ino
    replace_all("%SSID%", saved_ssid); 
    replace_all("%UID%", uid_str);     
    
    // Default placeholders (Empty for now until variables are added)
    replace_all("%EMAIL%", "");
    replace_all("%NAME%", "");
    replace_all("%MOBILE%", "");
    replace_all("%CC%", "+91");
    replace_all("%FLAT%", "");
    replace_all("%FLOOR%", "");
    replace_all("%BLOCK%", "");
    replace_all("%TOWER%", "");
    replace_all("%ADDRESS%", "");
    replace_all("%LOC%", "");
    replace_all("%THRESHOLD%", "1500");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page.c_str(), page.length());
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[512]; // Increased buffer for extra fields
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    get_post_param(buf, "ssid", ssid, sizeof(ssid));
    get_post_param(buf, "password", pass, sizeof(pass));

    ESP_LOGI(TAG, "Saving SSID: %s", ssid);

    nvs_handle_t my_handle;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &my_handle));
    nvs_set_str(my_handle, "ssid", ssid);
    nvs_set_str(my_handle, "pass", pass);
    nvs_commit(my_handle);
    nvs_close(my_handle);

    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t factory_check_handler(httpd_req_t *req) {
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    char pw[20] = {0};
    get_post_param(buf, "pw", pw, sizeof(pw));

    if(strcmp(pw, FACTORY_RESET_PIN) == 0) {
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t factory_reset_handler(httpd_req_t *req) {
    ESP_LOGW(TAG, "FACTORY RESET TRIGGERED");
    nvs_flash_erase();
    nvs_flash_init(); 
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req) {
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// --- DNS SERVER ---
void dns_server_task(void *pvParameters) {
    uint8_t data[128];
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    dns_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);
    bind(dns_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    while (1) {
        int len = recvfrom(dns_socket_fd, data, sizeof(data), 0, (struct sockaddr *)&client_addr, &client_addr_len);
        if (len > 12) {
            data[2] = 0x81; data[3] = 0x80; 
            data[6] = 0x00; data[7] = 0x01; 
            data[8] = 0; data[9] = 0; data[10] = 0; data[11] = 0;
            
            int q_end = 12;
            while(q_end < len && data[q_end] != 0) q_end++;
            q_end += 5; 

            if (q_end + 16 <= sizeof(data)) {
                data[q_end++] = 0xC0; data[q_end++] = 0x0C;
                data[q_end++] = 0x00; data[q_end++] = 0x01;
                data[q_end++] = 0x00; data[q_end++] = 0x01;
                data[q_end++] = 0x00; data[q_end++] = 0x00; data[q_end++] = 0x00; data[q_end++] = 0x3C;
                data[q_end++] = 0x00; data[q_end++] = 0x04;
                data[q_end++] = 192; data[q_end++] = 168; data[q_end++] = 4; data[q_end++] = 1;
                sendto(dns_socket_fd, data, q_end, 0, (struct sockaddr *)&client_addr, client_addr_len);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void stop_dns_server() {
    if (dns_task_handle) { vTaskDelete(dns_task_handle); dns_task_handle = NULL; }
    if (dns_socket_fd >= 0) { close(dns_socket_fd); dns_socket_fd = -1; }
}

void ap_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "AP Timer Expired. Switching to STA.");
    
    // TURN LED BLUE (Attempting to connect)
    neon_set_status(NEON_STABLE);

    if (server) { httpd_stop(server); server = NULL; }
    stop_dns_server();
    
    if (is_wifi_credentials_saved()) {
        nvs_handle_t my_handle;
        if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
            char ssid[33]={0}, pass[65]={0};
            size_t len=33; nvs_get_str(my_handle, "ssid", ssid, &len);
            len=65; nvs_get_str(my_handle, "pass", pass, &len);
            nvs_close(my_handle);
            
            ESP_LOGI(TAG, "Read Credentials -> SSID: %s, PASS: %s", ssid, pass);

            wifi_config_t wifi_config = {};
            strcpy((char*)wifi_config.sta.ssid, ssid);
            strcpy((char*)wifi_config.sta.password, pass);
            
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            
            esp_wifi_disconnect(); 

            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            
            esp_wifi_connect(); 
        }
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
}

// --- PUBLIC FUNCTIONS ---
extern "C" void start_ap_provisioning_mode(void) {
    // TRIGGER MAGENTA LED
    neon_set_status(NEON_AP_MODE);

    char dynamic_ssid[32];
    get_device_ssid(dynamic_ssid, sizeof(dynamic_ssid));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, dynamic_ssid);
    wifi_config.ap.ssid_len = strlen(dynamic_ssid);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN; 
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // --- FIX: Prevent "Address in use" error ---
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    stop_dns_server(); 
    // -------------------------------------------

    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12; 
    config.stack_size = 12288;     

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
        httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL };
        httpd_uri_t uri_check = { .uri = "/factory_check", .method = HTTP_POST, .handler = factory_check_handler, .user_ctx = NULL };
        httpd_uri_t uri_reset = { .uri = "/factory_reset", .method = HTTP_POST, .handler = factory_reset_handler, .user_ctx = NULL };
        httpd_uri_t uri_reboot = { .uri = "/reboot", .method = HTTP_POST, .handler = reboot_handler, .user_ctx = NULL };

        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_save);
        httpd_register_uri_handler(server, &uri_check);
        httpd_register_uri_handler(server, &uri_reset);
        httpd_register_uri_handler(server, &uri_reboot);
        
        httpd_uri_t uri_captive1 = { .uri = "/generate_204", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
        httpd_uri_t uri_captive2 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_captive1);
        httpd_register_uri_handler(server, &uri_captive2);
    }

    if (ap_timer == NULL) {
        ap_timer = xTimerCreate("AP_Timer", pdMS_TO_TICKS(AP_TIMEOUT_MS), pdFALSE, (void*)0, ap_timer_callback);
    }
    if (ap_timer) xTimerStart(ap_timer, 0);
}

extern "C" bool is_wifi_credentials_saved(void) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) != ESP_OK) return false;
    size_t ssid_len = 0;
    nvs_get_str(my_handle, "ssid", NULL, &ssid_len);
    nvs_close(my_handle);
    return (ssid_len > 0);
}

extern "C" void wifi_init_and_check(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (is_wifi_credentials_saved()) {
        ESP_LOGI(TAG, "Connect to STA...");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        
        nvs_handle_t my_handle;
        nvs_open("storage", NVS_READONLY, &my_handle);
        char ssid[33]={0}, pass[65]={0};
        size_t len=33; nvs_get_str(my_handle, "ssid", ssid, &len);
        len=65; nvs_get_str(my_handle, "pass", pass, &len);
        nvs_close(my_handle);

        ESP_LOGI(TAG, "Read Credentials -> SSID: %s", ssid);

        wifi_config_t wifi_config = {};
        strcpy((char*)wifi_config.sta.ssid, ssid);
        strcpy((char*)wifi_config.sta.password, pass);

        // --- MAXIMUM COMPATIBILITY SETTINGS ---
        
        // 1. Scan Method: Search all channels aggressively
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        
        // 2. Sort Method: Connect to the strongest signal if multiple APs have same name
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

        // 3. Threshold: WIFI_AUTH_OPEN
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

        // 4. PMF (Protected Management Frames): Capable but NOT Required
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
        // --------------------------------------

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        
        // Note: esp_wifi_connect() is triggered automatically by the start event in main.c
    } else {
        start_ap_provisioning_mode();
    }
}