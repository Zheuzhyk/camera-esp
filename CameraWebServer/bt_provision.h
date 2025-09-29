#pragma once

#include <stdint.h>

// Окно BT-провижининга: поднять SPP и ждать команды от телефона/ПК.
// ttlMs — сколько миллисекунд держать окно открытым (например, 180000 = 3 минуты).
void BTProv_begin(uint32_t ttlMs);

// Вызывать в loop(): читает команды из BT и при "ssid=...;pass=..."
// пробует подключиться к Wi-Fi, сохраняет креды в NVS, шлёт OK/FAIL.
// По успеху автоматически закрывает BT-окно.
void BTProv_loop();

// Попытка подключиться к уже сохранённым в NVS Wi-Fi кредам.
// Возвращает true при успешном WL_CONNECTED в течение timeoutMs миллисекунд.
bool WiFi_trySaved(uint32_t timeoutMs);
