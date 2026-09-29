# Mi Fitness API: основание (адаптер и учётные данные) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Собрать фундамент сервиса: форк шаблона под AGPL, крипта Xiaomi с побайтовой сверкой против Python, хранение ротируемого токена в Postgres под libsodium, клиент облака с логином и пагинацией.

**Architecture:** Три изолированных модуля под `src/xiaomi/`. `Crypto` это чистые функции без сети и состояния. `HttpTransport` это шов: интерфейс с реализацией на libcurl и подделкой в тестах, поэтому весь клиент проверяется без сети. `CloudClient` знает HTTP и подпись, но не знает ни домена, ни базы. Токен живёт в Postgres зашифрованным, ключ приходит из Secret.

**Tech Stack:** C++20, Drogon, libpqxx, libsodium, libcurl, OpenSSL, nlohmann-json, gtest. Шаблон `cpp-rapid-rest-template` v1.4.0.

**Spec:** `docs/superpowers/specs/2026-09-29-mi-fitness-api-cpp-design.md`

## Global Constraints

1. Лицензия проекта AGPL-3.0-only. Файл `LICENSE` заменяется, заголовки новых файлов лицензию не дублируют.
2. Шаблон разворачивается один раз командой `./scripts/init-project.sh --minimal mi-fitness-api ghcr.io/moveeeax tarassov.me`.
3. Каждый маршрут обязан присутствовать в контроллере, в `Api::get_endpoints()` и в `docs/openapi.yaml`. Гейты `scripts/check-routes-registered.sh` и `scripts/check-openapi-drift.sh` роняют CI на расхождении.
4. Тесты без Postgres и Redis лежат в `tests/unit/`, тесты с базой в `tests/integration/`. Каталог определяет бакет, имя набора не должно встречаться в двух бакетах.
5. Стиль кода задан `.clang-format`, проверяется `make lint-format`. Заголовок файла несёт блок `@file` и `@brief`, как в существующих файлах шаблона.
6. RC4 у Xiaomi отбрасывает 1024 байта потока после инициализации ключевого расписания. Без этого расшифровка даёт мусор.
7. Base64 для подписи стандартный, с паддингом. `Utils::Base64` в шаблоне это base64url без паддинга, для протокола Xiaomi он не подходит.
8. Регион `cn` и пустая строка дают хост `https://hlth.io.mi.com`, остальные `https://<region>.hlth.io.mi.com`. Кандидаты: `ru, cn, de, i2, sg, us`.
9. Значения токена, `ssecurity` и полный `user_id` не попадают ни в логи, ни в ответы, ни в тексты ошибок. Маскирование: шесть звёзд и два последних символа.
10. Собирать и запускать тесты локально нельзя: ни компилятора, ни Docker в рабочем окружении нет. Единственный исполнитель тестов это GitHub Actions. Воркфлоу `ci.yml` шаблона уже делает всё нужное: `make test` в compose с Postgres и Redis, проверку формата, gitleaks, дрейф OpenAPI, гейт разделения бакетов.
11. Из этого следует ритм работы: каждая задача даёт два прогона CI. Первый на коммите с падающим тестом, он обязан быть красным и именно по той причине, которую ждёт шаг. Второй на коммите с реализацией, он обязан быть зелёным. Красный прогон это результат шага, а не авария.
12. Работа идёт в ветке на каждую задачу, слияние в `main` после зелёного CI. Прямой пуш в `main` запрещён, иначе красные прогоны окажутся в истории основной ветки.

## Review Focus

1. Пустой или битый `ssecurity`: `key[i % key.size()]` при нулевой длине это неопределённое поведение в C++, тогда как Python падал исключением. Ожидание: внятная ошибка `MiFitnessAuthError`, а не порча памяти. Тест в задаче 2.
2. Токен с не-ASCII символами (ровно случай многоточия из DevTools): заголовок `Cookie` обязан быть ASCII. Ожидание: отказ при загрузке учётных данных с указанием позиции символа, а не непонятная ошибка libcurl в момент запроса. Тест в задаче 3.
3. Ответ логина без префикса `&&&START&&&`, с отсутствующими полями, с `location` на посторонний хост или без куки `serviceToken`. Ожидание: отказ логина, ни одна из этих ситуаций не считается успехом. Тесты в задаче 4.
4. `has_more: true` с повторяющимся `next_key` и диапазон, требующий больше страниц, чем разрешено. Ожидание: остановка с ошибкой вместо бесконечного цикла или молчаливой потери данных. Тесты в задаче 5.
5. Ротация токена при недоступной базе: Xiaomi уже выдал новый токен, а записать его некуда. Ожидание: сессия в памяти продолжает работать на новом токене, ошибка записи логируется, следующий старт честно падает на авторизации вместо тихого использования устаревшего значения. Тест в задаче 4.

---

### Task 1: Форк шаблона под AGPL

**Files:**
- Create: весь скелет из `cpp-rapid-rest-template` в корне репозитория
- Modify: `LICENSE`, `project.env`, `docker/.env` (флаги модулей), `README.md`
- Modify: `docs/superpowers/` остаётся как есть, `init-project.sh` его не трогает

**Interfaces:**
- Consumes: ничего
- Produces: собирающийся проект с именем `mi-fitness-api`, зелёный CI на первом же пуше

- [ ] **Step 1: Скопировать шаблон, не затащив его историю**

```bash
cd /Users/moveeeax/Public/github/mi-fitness-api
rsync -a --exclude '.git' --exclude 'docs/superpowers' \
  /Users/moveeeax/Public/github/cpp-rapid-rest-template/ ./
git status --short | head -20
```

- [ ] **Step 2: Прогнать бутстрап в режиме проверки, затем всерьёз**

```bash
./scripts/init-project.sh --dry-run --minimal mi-fitness-api ghcr.io/moveeeax tarassov.me
./scripts/init-project.sh --minimal mi-fitness-api ghcr.io/moveeeax tarassov.me
```

Ожидание: скрипт заканчивается проверкой и не находит ни одного оставшегося токена `cpp-rapid-rest-template` или `tarassov.me` там, где их быть не должно. Каталог `_reference` и контент-модуль удалены.

- [ ] **Step 3: Заменить лицензию на AGPL-3.0-only**

```bash
curl -fsSL https://www.gnu.org/licenses/agpl-3.0.txt -o LICENSE
grep -n "MIT" README.md CONTRIBUTING.md THIRD_PARTY_NOTICES.md | head
```

В `README.md` и `THIRD_PARTY_NOTICES.md` заменить упоминание MIT на AGPL-3.0-only и добавить абзац: код портирован с `shkyyy18/mi_fitness_data_bridge` (AGPL-3.0-only), сам шаблон `cpp-rapid-rest-template` под MIT, его условия сохранены в `THIRD_PARTY_NOTICES.md`.

- [ ] **Step 4: Выключить ненужные модули шаблона**

В `docker/.env` и в значениях Helm выставить `MESSAGING_ENABLED=false`, `KAFKA_PRODUCER_ENABLED=false`, `KAFKA_CONSUMER_ENABLED=false`, `MAIL_ENABLED=false`, оставить `JOBS_ENABLED=true`, `RATE_LIMIT_ENABLED=true`, `DB_MIGRATIONS_ENABLED=true`.

- [ ] **Step 5: Убедиться, что база собирается и тесты зелёные**

```bash
git checkout -b task-1-bootstrap
git add -A && git commit -m "chore: форк шаблона"
gh repo create moveeeax/mi-fitness-api --private --source=. --remote=origin --push
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
```

Ожидание: все работы CI зелёные на неизменённом коде шаблона. Если нет, дальше идти нельзя: сломан форк, а не порт. Смотреть `build-and-test`, `lint-format`, `openapi-drift`, `gate-selftest`.

- [ ] **Step 6: Коммит**

```bash
git add -A
git commit -m "chore: форк cpp-rapid-rest-template под AGPL

Шаблон развёрнут через init-project.sh --minimal: без демо-материала,
без контент-модуля. Kafka и почта выключены настройками. Лицензия
AGPL-3.0-only, потому что порт делается с AGPL-исходника."
```

---

### Task 2: Крипта Xiaomi с золотыми векторами

**Files:**
- Create: `tools/gen_crypto_vectors.py`
- Create: `tests/fixtures/xiaomi_crypto_vectors.json`
- Create: `src/xiaomi/Crypto.hpp`, `src/xiaomi/Crypto.cpp`
- Create: `tests/unit/test_xiaomi_crypto.cpp`

**Interfaces:**
- Consumes: ничего
- Produces:
  - `std::string Xiaomi::Crypto::rc4(std::string_view key, std::string_view payload)`
  - `std::string Xiaomi::Crypto::signed_nonce(std::string_view ssecurity_b64, std::string_view nonce_raw)`
  - `std::string Xiaomi::Crypto::make_nonce(std::int64_t minutes_since_epoch, std::string_view random8)`
  - `std::string Xiaomi::Crypto::signature(std::string_view method, std::string_view path, std::string_view data, std::optional<std::string_view> rc4_hash, std::string_view signed_nonce_raw)`
  - `std::string Xiaomi::Crypto::b64_encode(std::string_view raw)` и `b64_decode`
  - `struct Xiaomi::MiFitnessAuthError : std::runtime_error` — отказ авторизации, очередью не ретраится
  - `struct Xiaomi::MiFitnessProtocolError : std::runtime_error` — нарушение формата или контракта пагинации

- [ ] **Step 1: Снять золотые векторы с Python-реализации**

```python
# tools/gen_crypto_vectors.py
"""Snapshot the upstream crypto as fixed input/output pairs.

Run once against the Python bridge's venv. The C++ port must reproduce
these byte for byte; that is the only cheap way to verify a hand-ported
RC4 variant and signature scheme without a live Xiaomi account.
"""
import base64, hashlib, json, pathlib, sys

sys.path.insert(0, "/Users/moveeeax/Public/github/mi_fitness_data_bridge/src")
from mi_fitness_mcp.adapters.mi_fitness_cloud import (  # noqa: E402
    _gen_signature, _gen_signed_nonce, _rc4_crypt,
)

KEYS = [b"\x01" * 32, bytes(range(32)), b"short-key"]
PAYLOADS = [b"", b"a", b"{\"start_time\":1,\"end_time\":2}", bytes(range(256))]
vectors = {"rc4": [], "signed_nonce": [], "signature": []}

for key in KEYS:
    for payload in PAYLOADS:
        vectors["rc4"].append({
            "key_b64": base64.b64encode(key).decode(),
            "payload_b64": base64.b64encode(payload).decode(),
            "cipher_b64": base64.b64encode(_rc4_crypt(key, payload)).decode(),
        })

for ss in (b"\x00" * 16, b"secret-material!"):
    for nonce in (b"\x00" * 12, bytes(range(12))):
        vectors["signed_nonce"].append({
            "ssecurity_b64": base64.b64encode(ss).decode(),
            "nonce_b64": base64.b64encode(nonce).decode(),
            "signed_nonce_b64": base64.b64encode(_gen_signed_nonce(ss, nonce)).decode(),
        })

sn = _gen_signed_nonce(b"secret-material!", bytes(range(12)))
for data in ('{"key":"steps"}', ""):
    for rc4_hash in (None, "QUJD"):
        values = {"data": data} | ({"rc4_hash__": rc4_hash} if rc4_hash else {})
        vectors["signature"].append({
            "method": "POST",
            "path": "/app/v1/data/get_fitness_data_by_time",
            "data": data,
            "rc4_hash": rc4_hash,
            "signed_nonce_b64": base64.b64encode(sn).decode(),
            "signature": _gen_signature(
                "POST", "/app/v1/data/get_fitness_data_by_time", values, sn),
        })

out = pathlib.Path("tests/fixtures/xiaomi_crypto_vectors.json")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(vectors, indent=2) + "\n")
print(f"{out}: {sum(len(v) for v in vectors.values())} vectors")
```

Запуск:

```bash
/Users/moveeeax/Public/github/mi_fitness_data_bridge/.venv/bin/python tools/gen_crypto_vectors.py
```

Ожидание: файл создан, в нём 12 векторов RC4, 4 на `signed_nonce` и 4 на подпись.

- [ ] **Step 2: Написать падающий тест на векторах**

```cpp
/**
 * @file test_xiaomi_crypto.cpp
 * @brief Порт крипты Xiaomi против золотых векторов Python-реализации.
 */

#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "xiaomi/Crypto.hpp"

namespace {

nlohmann::json load_vectors() {
    std::ifstream in("tests/fixtures/xiaomi_crypto_vectors.json");
    if (!in) throw std::runtime_error("fixture not found; run tools/gen_crypto_vectors.py");
    nlohmann::json j;
    in >> j;
    return j;
}

}  // namespace

TEST(XiaomiCrypto, Rc4MatchesUpstreamVectors) {
    for (const auto& v : load_vectors()["rc4"]) {
        const auto key = Xiaomi::Crypto::b64_decode(v["key_b64"].get<std::string>());
        const auto payload = Xiaomi::Crypto::b64_decode(v["payload_b64"].get<std::string>());
        const auto expected = v["cipher_b64"].get<std::string>();
        EXPECT_EQ(Xiaomi::Crypto::b64_encode(Xiaomi::Crypto::rc4(key, payload)), expected);
    }
}

TEST(XiaomiCrypto, Rc4IsItsOwnInverse) {
    const std::string key = "secret-material!";
    const std::string clear = R"({"start_time":1,"end_time":2})";
    EXPECT_EQ(Xiaomi::Crypto::rc4(key, Xiaomi::Crypto::rc4(key, clear)), clear);
}

TEST(XiaomiCrypto, SignedNonceMatchesUpstreamVectors) {
    for (const auto& v : load_vectors()["signed_nonce"]) {
        const auto nonce = Xiaomi::Crypto::b64_decode(v["nonce_b64"].get<std::string>());
        EXPECT_EQ(Xiaomi::Crypto::b64_encode(Xiaomi::Crypto::signed_nonce(
                      v["ssecurity_b64"].get<std::string>(), nonce)),
                  v["signed_nonce_b64"].get<std::string>());
    }
}

TEST(XiaomiCrypto, SignatureMatchesUpstreamVectors) {
    for (const auto& v : load_vectors()["signature"]) {
        const auto sn = Xiaomi::Crypto::b64_decode(v["signed_nonce_b64"].get<std::string>());
        std::optional<std::string_view> hash;
        std::string hash_storage;
        if (!v["rc4_hash"].is_null()) {
            hash_storage = v["rc4_hash"].get<std::string>();
            hash = hash_storage;
        }
        EXPECT_EQ(Xiaomi::Crypto::signature(v["method"].get<std::string>(),
                                            v["path"].get<std::string>(),
                                            v["data"].get<std::string>(), hash, sn),
                  v["signature"].get<std::string>());
    }
}

// Пустой ключ в Python давал исключение, в C++ дал бы деление на ноль и UB.
TEST(XiaomiCrypto, EmptyKeyIsRejected) {
    EXPECT_THROW(Xiaomi::Crypto::rc4("", "payload"), Xiaomi::MiFitnessAuthError);
}

// Nonce: 8 байт случайности плюс 4 байта big-endian с номером минуты.
TEST(XiaomiCrypto, NonceLayoutIsTwelveBytesBigEndianMinutes) {
    const std::string nonce = Xiaomi::Crypto::make_nonce(0x01020304, std::string(8, '\x00'));
    ASSERT_EQ(nonce.size(), 12u);
    EXPECT_EQ(static_cast<unsigned char>(nonce[8]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(nonce[9]), 0x02);
    EXPECT_EQ(static_cast<unsigned char>(nonce[10]), 0x03);
    EXPECT_EQ(static_cast<unsigned char>(nonce[11]), 0x04);
}
```

- [ ] **Step 3: Убедиться, что тест падает**

Run:

```bash
git push -u origin HEAD
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
```

Работы CI: `build-and-test`.
Expected: FAIL на отсутствии `xiaomi/Crypto.hpp`.

- [ ] **Step 4: Реализовать крипту**

```cpp
// src/xiaomi/Crypto.cpp (ключевая часть)
std::string rc4(std::string_view key, std::string_view payload) {
    if (key.empty()) {
        throw MiFitnessAuthError("rc4: empty key (missing or malformed ssecurity)");
    }
    std::array<unsigned char, 256> s{};
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<unsigned char>(i);
    std::size_t j = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        j = (j + s[i] + static_cast<unsigned char>(key[i % key.size()])) % 256;
        std::swap(s[i], s[j]);
    }
    std::size_t x = 0;
    std::size_t y = 0;
    const auto next = [&]() -> unsigned char {
        x = (x + 1) % 256;
        y = (y + s[x]) % 256;
        std::swap(s[x], s[y]);
        return s[(s[x] + s[y]) % 256];
    };
    // Апстрим отбрасывает 1024 байта потока. Это не украшение: без отброса
    // расшифровка ответа даёт мусор.
    for (int i = 0; i < 1024; ++i) next();
    std::string out;
    out.reserve(payload.size());
    for (const char c : payload) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(c) ^ next()));
    }
    return out;
}

std::string signed_nonce(std::string_view ssecurity_b64, std::string_view nonce_raw) {
    const std::string ss = b64_decode(ssecurity_b64);
    std::string buf;
    buf.reserve(ss.size() + nonce_raw.size());
    buf.append(ss).append(nonce_raw);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(buf.data()), buf.size(), digest);
    return std::string(reinterpret_cast<const char*>(digest), SHA256_DIGEST_LENGTH);
}

std::string signature(std::string_view method, std::string_view path, std::string_view data,
                      std::optional<std::string_view> rc4_hash,
                      std::string_view signed_nonce_raw) {
    std::string base;
    base.append(method).append("&").append(path).append("&data=").append(data);
    if (rc4_hash) base.append("&rc4_hash__=").append(*rc4_hash);
    base.append("&").append(b64_encode(signed_nonce_raw));
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(base.data()), base.size(), digest);
    return b64_encode(std::string(reinterpret_cast<const char*>(digest), SHA_DIGEST_LENGTH));
}

std::string make_nonce(std::int64_t minutes_since_epoch, std::string_view random8) {
    if (random8.size() != 8) throw MiFitnessAuthError("make_nonce: need exactly 8 random bytes");
    std::string nonce(random8);
    const auto value = static_cast<std::uint32_t>(minutes_since_epoch);
    for (int shift = 24; shift >= 0; shift -= 8) {
        nonce.push_back(static_cast<char>((value >> shift) & 0xff));
    }
    return nonce;
}
```

`b64_encode` и `b64_decode` реализуются на стандартном алфавите с паддингом через `EVP_EncodeBlock` и `EVP_DecodeBlock` с поправкой на длину. Брать `Utils::Base64` нельзя: там base64url без паддинга.

- [ ] **Step 5: Тесты зелёные**

Run:

```bash
git push -u origin HEAD
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
```

Работы CI: `build-and-test`.
Expected: PASS, все 20 векторов сходятся.

- [ ] **Step 6: Коммит**

```bash
git add tools/gen_crypto_vectors.py tests/fixtures/xiaomi_crypto_vectors.json \
        src/xiaomi/Crypto.hpp src/xiaomi/Crypto.cpp tests/unit/test_xiaomi_crypto.cpp
git commit -m "feat(xiaomi): крипта протокола с побайтовой сверкой против Python

RC4 с отбросом 1024 байт потока, signed_nonce, подпись и стандартный
base64. Золотые векторы сняты с апстрима скриптом, поэтому ошибка
переноса алгоритма видна без сети и без живого аккаунта."
```

---

### Task 3: Учётные данные: Secret плюс шифрование в Postgres

**Files:**
- Create: `migrations/0NN_xiaomi_credentials.sql` (номер даёт `make new-migration SLUG=xiaomi_credentials`)
- Create: `src/xiaomi/Credentials.hpp`, `src/xiaomi/Credentials.cpp`
- Create: `src/repositories/CredentialsRepository.hpp`, `.cpp`
- Create: `tests/unit/test_xiaomi_credentials.cpp`
- Create: `tests/integration/test_credentials_repository.cpp`

**Interfaces:**
- Consumes: `Xiaomi::Crypto::b64_decode`, `Xiaomi::MiFitnessAuthError`
- Produces:
  - `struct Xiaomi::Credentials { std::string user_id; std::string pass_token; std::string region; }`
  - `void Xiaomi::validate_pass_token(std::string_view token)`
  - `std::string Xiaomi::mask_account_id(std::string_view value)`
  - `Xiaomi::Sealed Xiaomi::seal(std::string_view plain, std::string_view key_b64)` и `std::string Xiaomi::unseal(const Sealed&, std::string_view key_b64)`
  - `std::optional<Credentials> CredentialsRepository::load()`, `void CredentialsRepository::store(const Credentials&)`

- [ ] **Step 1: Миграция**

```bash
make new-migration SLUG=xiaomi_credentials
```

Содержимое:

```sql
CREATE TABLE xiaomi_credentials (
    user_id           text        PRIMARY KEY,
    pass_token_sealed bytea       NOT NULL,
    nonce             bytea       NOT NULL,
    region            text        NOT NULL DEFAULT 'cn',
    rotated_at        timestamptz NOT NULL DEFAULT now()
);
COMMENT ON TABLE xiaomi_credentials IS
    'Живой passToken: Xiaomi ротирует его при каждом логине, Secret хранит только сид.';
```

- [ ] **Step 2: Падающий юнит-тест на валидацию, маскирование и шифрование**

```cpp
/**
 * @file test_xiaomi_credentials.cpp
 * @brief Валидация токена, маскирование идентификатора, secretbox без базы.
 */

#include <gtest/gtest.h>

#include "xiaomi/Credentials.hpp"

namespace {
constexpr const char* kKey = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";  // 32 нулевых байта
}

// Ровно тот случай, который однажды стоил часа: DevTools отдаёт значение с
// многоточием в середине, а заголовок Cookie обязан быть ASCII.
TEST(XiaomiCredentials, RejectsNonAsciiToken) {
    const std::string token = std::string("AXSU") + "\xe2\x80\xa6" + "CqGAl";
    try {
        Xiaomi::validate_pass_token(token);
        FAIL() << "ожидался отказ";
    } catch (const Xiaomi::MiFitnessAuthError& e) {
        EXPECT_NE(std::string(e.what()).find("position 4"), std::string::npos);
    }
}

TEST(XiaomiCredentials, RejectsSemicolonAndWhitespace) {
    EXPECT_THROW(Xiaomi::validate_pass_token("abc;def"), Xiaomi::MiFitnessAuthError);
    EXPECT_THROW(Xiaomi::validate_pass_token("abc def"), Xiaomi::MiFitnessAuthError);
    EXPECT_THROW(Xiaomi::validate_pass_token(""), Xiaomi::MiFitnessAuthError);
}

TEST(XiaomiCredentials, AcceptsRealisticToken) {
    EXPECT_NO_THROW(Xiaomi::validate_pass_token(std::string(347, 'A')));
}

TEST(XiaomiCredentials, MaskKeepsOnlyLastTwoCharacters) {
    EXPECT_EQ(Xiaomi::mask_account_id("4236479152"), "******52");
    EXPECT_EQ(Xiaomi::mask_account_id("42"), "******");
    EXPECT_EQ(Xiaomi::mask_account_id(""), "");
}

TEST(XiaomiCredentials, SealRoundTrips) {
    const std::string plain(347, 'T');
    const auto sealed = Xiaomi::seal(plain, kKey);
    EXPECT_NE(sealed.ciphertext, plain);
    EXPECT_EQ(sealed.nonce.size(), 24u);
    EXPECT_EQ(Xiaomi::unseal(sealed, kKey), plain);
}

TEST(XiaomiCredentials, UnsealWithWrongKeyFails) {
    const auto sealed = Xiaomi::seal("secret", kKey);
    const std::string other = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=";
    EXPECT_THROW(Xiaomi::unseal(sealed, other), Xiaomi::MiFitnessAuthError);
}

TEST(XiaomiCredentials, RejectsKeyOfWrongLength) {
    EXPECT_THROW(Xiaomi::seal("secret", "QUJD"), Xiaomi::MiFitnessAuthError);
}
```

- [ ] **Step 3: Проверить падение**

Run:

```bash
git push -u origin HEAD
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
```

Работы CI: `build-and-test`.
Expected: FAIL на отсутствии `xiaomi/Credentials.hpp`.

- [ ] **Step 4: Реализация**

`validate_pass_token` отвергает пустое значение, любой байт больше 0x7f, пробельные символы и точку с запятой, и сообщает позицию первого плохого символа, но никогда само значение. `mask_account_id` повторяет формат Python-моста: шесть звёзд и два последних символа, при длине до двух только звёзды, при пустом входе пустая строка. `seal` использует `crypto_secretbox_easy` с ключом длиной `crypto_secretbox_KEYBYTES` из base64 и случайным nonce длиной `crypto_secretbox_NONCEBYTES`, `unseal` при неудаче бросает `MiFitnessAuthError` без деталей.

- [ ] **Step 5: Интеграционный тест репозитория на живой базе**

```cpp
/**
 * @file test_credentials_repository.cpp
 * @brief Посев из окружения, чтение обратно, перезапись ротированного значения.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "repositories/CredentialsRepository.hpp"
#include "test_helpers.hpp"

namespace {
// 32 нулевых байта в base64: ключ шифрования для тестов.
constexpr const char* kTestKeyB64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
}  // namespace

class CredentialsRepositoryTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "credentials_repo_test_config.json"; }
};

TEST_F(CredentialsRepositoryTest, RoundTripAndRotate) {
    CredentialsRepository repo(kTestKeyB64);
    EXPECT_FALSE(repo.load().has_value());

    repo.store({"4236479152", std::string(347, 'S'), "cn"});
    auto loaded = repo.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->user_id, "4236479152");
    EXPECT_EQ(loaded->pass_token, std::string(347, 'S'));
    EXPECT_EQ(loaded->region, "cn");

    // Ротация перезаписывает строку, а не добавляет вторую.
    repo.store({"4236479152", std::string(347, 'R'), "cn"});
    EXPECT_EQ(repo.load()->pass_token, std::string(347, 'R'));
    EXPECT_EQ(count_rows("xiaomi_credentials"), 1);
}

TEST_F(CredentialsRepositoryTest, TokenIsNotStoredInClear) {
    CredentialsRepository repo(kTestKeyB64);
    repo.store({"4236479152", std::string(347, 'R'), "cn"});
    const auto sealed = fetch_sealed_token();
    EXPECT_EQ(sealed.find(std::string(20, 'R')), std::string::npos);
    EXPECT_GE(sealed.size(), 347u + 16u);  // secretbox добавляет тег аутентичности
}

TEST_F(CredentialsRepositoryTest, WrongKeyFailsLoudlyOnLoad) {
    CredentialsRepository writer(kTestKeyB64);
    writer.store({"4236479152", std::string(347, 'S'), "cn"});
    CredentialsRepository reader("AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=");
    EXPECT_THROW(reader.load(), Xiaomi::MiFitnessAuthError);
}
```

Вспомогательные методы `count_rows` и `fetch_sealed_token` добавляются в тот же тестовый класс через `TestHelpers::CoreBackedTest` и запрос к базе, как это делает `tests/api/test_admin_billing_api.cpp`.

- [ ] **Step 6: Тесты зелёные и коммит**

```bash
git push
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
git add migrations src/xiaomi/Credentials.* src/repositories/CredentialsRepository.* \
        tests/unit/test_xiaomi_credentials.cpp tests/integration/test_credentials_repository.cpp
git commit -m "feat(xiaomi): токен в Postgres под secretbox, сид из Secret

Xiaomi ротирует passToken при каждом логине, поэтому Secret держит только
сид и ключ шифрования, а живое значение лежит в базе зашифрованным.
Валидация отвергает не-ASCII: обрезанное значение из DevTools ломало
заголовок Cookie непонятной ошибкой в момент запроса."
```

---

### Task 4: Логин в облако Xiaomi через шов HttpTransport

**Files:**
- Create: `src/xiaomi/HttpTransport.hpp` (интерфейс), `src/xiaomi/CurlTransport.hpp`, `.cpp`
- Create: `src/xiaomi/CloudClient.hpp`, `src/xiaomi/CloudClient.cpp`
- Create: `tests/unit/test_xiaomi_login.cpp`
- Create: `tests/FakeHttpTransport.hpp`

**Interfaces:**
- Consumes: `Xiaomi::Crypto::*`, `Xiaomi::Credentials`, `Xiaomi::validate_pass_token`
- Produces:
  - `struct Xiaomi::HttpRequest { std::string method, url, body; std::vector<std::pair<std::string,std::string>> headers; }`
  - `struct Xiaomi::HttpResponse { long status; std::string body; std::vector<std::pair<std::string,std::string>> headers; }`
  - `class Xiaomi::HttpTransport { virtual HttpResponse send(const HttpRequest&) = 0; }`
  - `class Xiaomi::CloudClient` с конструктором `CloudClient(HttpTransport&, Credentials, std::function<void(const Credentials&)> on_rotate)`, методами `void login()`, `const Credentials& credentials() const`, `void set_max_pages(int)`
  - `bool Xiaomi::is_allowed_login_redirect(std::string_view url)`

- [ ] **Step 1: Падающие тесты логина**

```cpp
/**
 * @file test_xiaomi_login.cpp
 * @brief Двухступенчатый логин на подделке транспорта: без сети и без аккаунта.
 */

#include <gtest/gtest.h>

#include "FakeHttpTransport.hpp"
#include "xiaomi/CloudClient.hpp"

namespace {

std::string login_body(const std::string& token = "NEWTOKEN") {
    return std::string("&&&START&&&") + R"({"passToken":")" + token +
           R"(","userId":4236479152,"ssecurity":"c2VjcmV0LW1hdGVyaWFsIQ==",)" +
           R"("location":"https://account.xiaomi.com/pass/end"})";
}

Xiaomi::Credentials seed() { return {"4236479152", std::string(347, 'S'), "cn"}; }

}  // namespace

TEST(XiaomiLogin, TwoStepLoginStoresRotatedToken) {
    FakeHttpTransport t;
    t.reply({200, login_body(), {}});
    t.reply({200, "", {{"set-cookie", "serviceToken=abc; Path=/"}}});
    std::optional<Xiaomi::Credentials> rotated;
    Xiaomi::CloudClient c(t, seed(), [&](const auto& cr) { rotated = cr; });

    c.login();

    ASSERT_TRUE(rotated.has_value());
    EXPECT_EQ(rotated->pass_token, "NEWTOKEN");
    ASSERT_EQ(t.requests().size(), 2u);
    EXPECT_NE(t.requests().at(0).url.find("sid=miothealth"), std::string::npos);
    EXPECT_EQ(t.requests().at(1).url, "https://account.xiaomi.com/pass/end");
}

TEST(XiaomiLogin, MissingStartPrefixIsRejected) {
    FakeHttpTransport t;
    t.reply({200, R"({"passToken":"x","userId":1,"ssecurity":"QQ==","location":"https://mi.com/x"})", {}});
    Xiaomi::CloudClient c(t, seed(), [](const auto&) {});
    EXPECT_THROW(c.login(), Xiaomi::MiFitnessAuthError);
}

TEST(XiaomiLogin, MissingRequiredFieldIsRejected) {
    FakeHttpTransport t;
    t.reply({200, std::string("&&&START&&&") + R"({"userId":1,"ssecurity":"QQ=="})", {}});
    Xiaomi::CloudClient c(t, seed(), [](const auto&) {});
    EXPECT_THROW(c.login(), Xiaomi::MiFitnessAuthError);
}

TEST(XiaomiLogin, ForeignRedirectHostIsRefused) {
    EXPECT_TRUE(Xiaomi::is_allowed_login_redirect("https://account.xiaomi.com/pass/end"));
    EXPECT_TRUE(Xiaomi::is_allowed_login_redirect("https://sts.api.io.mi.com/x"));
    EXPECT_FALSE(Xiaomi::is_allowed_login_redirect("https://evil.example.com/x"));
    EXPECT_FALSE(Xiaomi::is_allowed_login_redirect("http://account.xiaomi.com/x"));
    EXPECT_FALSE(Xiaomi::is_allowed_login_redirect("https://xiaomi.com.evil.test/x"));
    EXPECT_FALSE(Xiaomi::is_allowed_login_redirect("https://account.xiaomi.com/\nx"));
}

TEST(XiaomiLogin, MissingServiceTokenCookieIsRejected) {
    FakeHttpTransport t;
    t.reply({200, login_body(), {}});
    t.reply({200, "", {{"set-cookie", "somethingElse=1"}}});
    Xiaomi::CloudClient c(t, seed(), [](const auto&) {});
    EXPECT_THROW(c.login(), Xiaomi::MiFitnessAuthError);
}

// Ротация уже случилась, а записать её некуда: сессия обязана продолжить
// работать на новом токене, а не потерять его вместе с исключением.
TEST(XiaomiLogin, RotationPersistFailureDoesNotLoseTheNewToken) {
    FakeHttpTransport t;
    t.reply({200, login_body("ROTATED"), {}});
    t.reply({200, "", {{"set-cookie", "serviceToken=abc"}}});
    Xiaomi::CloudClient c(t, seed(), [](const auto&) {
        throw std::runtime_error("postgres unavailable");
    });
    EXPECT_NO_THROW(c.login());
    EXPECT_EQ(c.credentials().pass_token, "ROTATED");
}

// Ни токен, ни ssecurity не должны попасть в текст ошибки.
TEST(XiaomiLogin, ErrorTextCarriesNoSecrets) {
    FakeHttpTransport t;
    t.reply({200, std::string("&&&START&&&") + R"({"passToken":"SUPERSECRET","userId":1})", {}});
    Xiaomi::CloudClient c(t, seed(), [](const auto&) {});
    try {
        c.login();
        FAIL() << "ожидался отказ";
    } catch (const Xiaomi::MiFitnessAuthError& e) {
        EXPECT_EQ(std::string(e.what()).find("SUPERSECRET"), std::string::npos);
    }
}
```

`FakeHttpTransport` держит очередь заготовленных ответов, запоминает все запросы и бросает, если запросов пришло больше, чем заготовлено.

- [ ] **Step 2: Проверить падение**

Run:

```bash
git push -u origin HEAD
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
```

Работы CI: `build-and-test`.
Expected: FAIL на отсутствии `xiaomi/CloudClient.hpp`.

- [ ] **Step 3: Реализация логина**

Порядок обязателен и повторяет апстрим: разобрать ответ целиком, проверить префикс и все четыре поля, проверить типы, отвергнуть значения с пробелами, управляющими символами и точкой с запятой, декодировать `ssecurity`, проверить `location` по списку разрешённых хостов, и только после этого обновить состояние в памяти и вызвать `on_rotate`. Вызов `on_rotate` оборачивается в `try/catch`: ошибка записи логируется и не роняет логин. Затем запрос по `location` без автоматического следования редиректам (`CURLOPT_FOLLOWLOCATION` в ноль, как в `src/webhooks/Webhooks.hpp`), сбор всех `set-cookie` и проверка наличия непустого `serviceToken`.

- [ ] **Step 4: Тесты зелёные**

Run:

```bash
git push -u origin HEAD
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
```

Работы CI: `build-and-test`.
Expected: PASS.

- [ ] **Step 5: Реализовать CurlTransport и проверить формат заголовков**

Тест в `tests/unit/test_xiaomi_login.cpp`: `CurlTransport` собирает заголовок `Cookie: userId=<id>; passToken=<token>` ровно в этом порядке и с этим разделителем.

- [ ] **Step 6: Коммит**

```bash
git add src/xiaomi/HttpTransport.hpp src/xiaomi/CurlTransport.* src/xiaomi/CloudClient.* \
        tests/FakeHttpTransport.hpp tests/unit/test_xiaomi_login.cpp
git commit -m "feat(xiaomi): двухступенчатый логин с ротацией токена

Транспорт вынесен за интерфейс, поэтому логин целиком проверяется без
сети. Новый passToken сохраняется до запроса по редиректу: иначе сетевая
ошибка теряет уже выданный токен. Редирект принимается только на домены
xiaomi.com и mi.com."
```

---

### Task 5: Подписанный запрос данных и пагинация

**Files:**
- Modify: `src/xiaomi/CloudClient.hpp`, `src/xiaomi/CloudClient.cpp`
- Create: `src/xiaomi/Regions.hpp`
- Create: `tests/unit/test_xiaomi_fetch.cpp`
- Modify: `tests/FakeHttpTransport.hpp` — добавить `reply_login()` (готовый двухступенчатый успешный логин) и `reply_encrypted(std::string_view json)`, который шифрует переданный JSON тем же RC4 на `signed_nonce` текущей сессии и кодирует base64, чтобы клиент расшифровал его как настоящий ответ

**Interfaces:**
- Consumes: всё из задач 2 и 4
- Produces:
  - `std::string Xiaomi::host_for_region(std::string_view region)`
  - `inline constexpr std::array<std::string_view, 6> Xiaomi::kKnownRegions`
  - `nlohmann::json Xiaomi::CloudClient::post_signed(std::string_view path, const nlohmann::json& payload)`
  - `std::vector<nlohmann::json> Xiaomi::CloudClient::fetch_key(std::string_view key, std::string_view start_date, std::string_view end_date, std::optional<std::string_view> region)`
  - `std::pair<std::int64_t, std::int64_t> Xiaomi::range_to_timestamps(std::string_view start_date, std::string_view end_date, std::string_view region)`

- [ ] **Step 1: Падающие тесты**

```cpp
/**
 * @file test_xiaomi_fetch.cpp
 * @brief Хосты регионов, границы диапазона, подпись запроса и курсорная пагинация.
 */

#include <gtest/gtest.h>

#include "FakeHttpTransport.hpp"
#include "xiaomi/CloudClient.hpp"
#include "xiaomi/Regions.hpp"

TEST(XiaomiRegions, HostMapping) {
    EXPECT_EQ(Xiaomi::host_for_region("cn"), "https://hlth.io.mi.com");
    EXPECT_EQ(Xiaomi::host_for_region(""), "https://hlth.io.mi.com");
    EXPECT_EQ(Xiaomi::host_for_region("de"), "https://de.hlth.io.mi.com");
    EXPECT_EQ(Xiaomi::kKnownRegions.size(), 6u);
}

// Регион cn считает границы в UTC+8, остальные в UTC. Ошибка здесь сдвигает
// сутки и портит суточные агрегаты.
TEST(XiaomiRegions, RangeBoundsUseRegionOffset) {
    // 2026-09-22 00:00:00 +08:00 == 1790006400, конец суток == 1790092799.
    const auto cn = Xiaomi::range_to_timestamps("2026-09-22", "2026-09-22", "cn");
    EXPECT_EQ(cn.first, 1790006400);
    EXPECT_EQ(cn.second, 1790092799);
    // 2026-09-22 00:00:00 UTC == 1790035200, ровно на восемь часов позже.
    const auto utc = Xiaomi::range_to_timestamps("2026-09-22", "2026-09-22", "de");
    EXPECT_EQ(utc.first, 1790035200);
    EXPECT_EQ(utc.first - cn.first, 8 * 3600);
}

TEST(XiaomiFetch, PaginatesUntilHasMoreIsFalse) {
    FakeHttpTransport t;
    t.reply_login();
    t.reply_encrypted(R"({"data_list":[{"a":1}],"has_more":true,"next_key":"k1"})");
    t.reply_encrypted(R"({"data_list":[{"a":2}],"has_more":false,"next_key":null})");
    Xiaomi::CloudClient c(t, {"4236479152", std::string(347, 'S'), "cn"}, [](const auto&) {});
    c.login();
    const auto items = c.fetch_key("steps", "2026-09-22", "2026-09-23", std::nullopt);
    EXPECT_EQ(items.size(), 2u);
}

TEST(XiaomiFetch, RepeatedCursorIsTreatedAsALoop) {
    FakeHttpTransport t;
    t.reply_login();
    t.reply_encrypted(R"({"data_list":[{"a":1}],"has_more":true,"next_key":"same"})");
    t.reply_encrypted(R"({"data_list":[{"a":2}],"has_more":true,"next_key":"same"})");
    Xiaomi::CloudClient c(t, {"4236479152", std::string(347, 'S'), "cn"}, [](const auto&) {});
    c.login();
    EXPECT_THROW(c.fetch_key("steps", "2026-09-22", "2026-09-23", std::nullopt),
                 Xiaomi::MiFitnessProtocolError);
}

TEST(XiaomiFetch, PageCeilingStopsRunawayPagination) {
    FakeHttpTransport t;
    t.reply_login();
    for (int i = 0; i < 300; ++i) {
        t.reply_encrypted(R"({"data_list":[{"a":1}],"has_more":true,"next_key":"k)" +
                          std::to_string(i) + R"("})");
    }
    Xiaomi::CloudClient c(t, {"4236479152", std::string(347, 'S'), "cn"}, [](const auto&) {});
    c.login();
    c.set_max_pages(200);
    EXPECT_THROW(c.fetch_key("steps", "2026-01-01", "2026-09-29", std::nullopt),
                 Xiaomi::MiFitnessProtocolError);
    EXPECT_LE(t.requests().size(), 202u);
}

TEST(XiaomiFetch, RequestCarriesBothSignatureFields) {
    FakeHttpTransport t;
    t.reply_login();
    t.reply_encrypted(R"({"data_list":[],"has_more":false})");
    Xiaomi::CloudClient c(t, {"4236479152", std::string(347, 'S'), "cn"}, [](const auto&) {});
    c.login();
    c.fetch_key("steps", "2026-09-22", "2026-09-22", std::nullopt);
    const auto& body = t.requests().back().body;
    EXPECT_NE(body.find("data="), std::string::npos);
    EXPECT_NE(body.find("rc4_hash__="), std::string::npos);
    EXPECT_NE(body.find("signature="), std::string::npos);
}
```

- [ ] **Step 2: Проверить падение**

Run:

```bash
git push -u origin HEAD
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
```

Работы CI: `build-and-test`.
Expected: FAIL на отсутствии `Regions.hpp` и методов клиента.

- [ ] **Step 3: Реализация**

`post_signed` собирает `form["data"]` компактным JSON без пробелов, считает `rc4_hash__` от неизменённой формы, шифрует каждое значение RC4 и кодирует base64, затем считает `signature` от зашифрованных значений, отправляет `application/x-www-form-urlencoded` и расшифровывает ответ. `fetch_key` крутит цикл по `next_key`, ведёт множество виденных курсоров и счётчик страниц, при повторе курсора или превышении потолка бросает `MiFitnessProtocolError`.

- [ ] **Step 4: Тесты зелёные**

Run:

```bash
git push -u origin HEAD
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
```

Работы CI: `build-and-test`.
Expected: PASS.

- [ ] **Step 5: Коммит**

```bash
git add src/xiaomi/Regions.hpp src/xiaomi/CloudClient.* tests/unit/test_xiaomi_fetch.cpp
git commit -m "feat(xiaomi): подписанный запрос данных и курсорная пагинация

Границы диапазона считаются в поясе региона: для cn это UTC+8, ошибка
здесь сдвигает сутки. Петля курсора и потолок страниц дают явную ошибку
вместо бесконечного цикла."
```

---

### Task 6: Маршрут проверки связи

**Files:**
- Create: `src/api/XiaomiController.hpp`, `.cpp` (через `./scripts/new-endpoint.sh`)
- Modify: `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Create: `tests/api/test_xiaomi_probe.cpp`

**Interfaces:**
- Consumes: `CloudClient`, `CredentialsRepository`
- Produces: `GET /api/v1/xiaomi/probe?key=steps&from=&to=` отдаёт `{"region":"cn","account":"******52","key":"steps","records":N}`

- [ ] **Step 1: Сгенерировать заготовку маршрута**

```bash
./scripts/new-endpoint.sh XiaomiController Get /api/v1/xiaomi/probe --with-test --patch-openapi
```

- [ ] **Step 2: Падающий тест**

```cpp
/**
 * @file test_xiaomi_probe.cpp
 * @brief Маршрут проверки связи: подделка транспорта вместо облака Xiaomi.
 */

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "FakeHttpTransport.hpp"
#include "api/XiaomiController.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

class XiaomiProbeTest : public TestHelpers::CoreBackedTest {
protected:
    FakeHttpTransport transport;
    Api::XiaomiController controller{transport};

    std::string config_file_name() const override { return "xiaomi_probe_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        seed_credentials("4236479152", std::string(347, 'S'), "cn");
    }

    HttpResponsePtr probe(const std::string& query) {
        HttpResponsePtr resp;
        controller.probe(TestHelpers::authed_get("/api/v1/xiaomi/probe?" + query),
                         [&](const HttpResponsePtr& r) { resp = r; });
        return resp;
    }
};

TEST_F(XiaomiProbeTest, ReturnsMaskedAccountAndCount) {
    transport.reply_login();
    transport.reply_encrypted(R"({"data_list":[{"a":1},{"a":2}],"has_more":false})");

    auto resp = probe("key=steps&from=2026-09-22&to=2026-09-22");
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    const auto body = json::parse(resp->body());
    EXPECT_EQ(body["data"]["account"], "******52");
    EXPECT_EQ(body["data"]["region"], "cn");
    EXPECT_EQ(body["data"]["records"], 2);
    // Полный идентификатор аккаунта в ответе не появляется.
    EXPECT_EQ(std::string(resp->body()).find("4236479152"), std::string::npos);
}

TEST_F(XiaomiProbeTest, RejectsUnknownKey) {
    auto resp = probe("key=nonsense&from=2026-09-22&to=2026-09-22");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_TRUE(transport.requests().empty()) << "до облака дойти не должно";
}

TEST_F(XiaomiProbeTest, RejectsReversedRange) {
    auto resp = probe("key=steps&from=2026-09-23&to=2026-09-22");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(XiaomiProbeTest, RejectsMalformedDate) {
    auto resp = probe("key=steps&from=22.09.2026&to=2026-09-22");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

// Ошибка авторизации отдаётся как 502 с признаком auth, а не как 500.
TEST_F(XiaomiProbeTest, AuthFailureIsReportedAsUpstreamAuthError) {
    transport.reply({200, R"(no start prefix)", {}});
    auto resp = probe("key=steps&from=2026-09-22&to=2026-09-22");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k502BadGateway);
    EXPECT_EQ(json::parse(resp->body())["error"]["kind"], "auth");
}
```

Хелпер `TestHelpers::authed_get` и метод `seed_credentials` добавляются по образцу `TestHelpers::authed` из `tests/test_fixtures.hpp`: запрос с валидным ключом API, посев строки в `xiaomi_credentials` тем же ключом шифрования, что и в конфиге теста.

- [ ] **Step 3: Проверить падение и реализовать**

Run:

```bash
git push -u origin HEAD
gh run watch "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --exit-status --compact
```

Работы CI: `build-and-test` (бакет с базой входит в `make test`).
Expected: FAIL. Реализация: разбор и валидация параметров, отказ на неизвестном ключе и перевёрнутом диапазоне, вызов `fetch_key`, ответ с замаскированным идентификатором и количеством записей. Сами записи маршрут не отдаёт: это проверка связи, а не выгрузка.

- [ ] **Step 4: Проверить гейты маршрутов**

Гейты маршрутов живут в работе `openapi-drift` того же воркфлоу, отдельного запуска не требуют. Проверять её вывод в том же прогоне:

```bash
gh run view "$(gh run list --limit 1 --json databaseId -q '.[0].databaseId')" --log-failed | head -40
```

- [ ] **Step 5: Отметить, что живая проверка ждёт деплоя**

Локально поднять сервис нечем, поэтому проверка крипты на настоящем облаке Xiaomi делается в кластере, в задаче 8. Здесь достаточно зелёного CI: маршрут отвечает на подделке транспорта, гейты маршрутов и OpenAPI пройдены.

- [ ] **Step 6: Коммит**

```bash
git add src/api/XiaomiController.* src/api/Endpoints.hpp docs/openapi.yaml \
        tests/api/test_xiaomi_probe.cpp
git commit -m "feat(api): маршрут проверки связи с облаком Xiaomi

Отдаёт регион, замаскированный идентификатор аккаунта и число записей по
одному ключу. Записи наружу не идут: это проверка транспорта и подписи."
```

---

---

### Task 7: База и роль в кластере Postgres

**Files:**
- Create: `deploy/db/secret-postgresql-mi-fitness.example.yaml`
- Create: `deploy/db/database.yaml`
- Create: `deploy/db/README.md`

**Interfaces:**
- Consumes: существующий кластер CloudNativePG `postgresql` в namespace `db`
- Produces: база `mi_fitness`, роль `mi-fitness`, секрет `postgresql-mi-fitness` в namespace `db`, адрес `postgresql-rw.db.svc.cluster.local:5432`

Кластер общий, в нём уже живут роли `tarassov-me` и `tgw` и база `tgw_archive`. Поэтому роль добавляется точечным патчем в массив, а не перезаписью `spec.managed.roles`, и после патча состав ролей проверяется целиком.

- [ ] **Step 1: Завести секрет с паролем роли**

```bash
kubectl -n db create secret generic postgresql-mi-fitness \
  --type=kubernetes.io/basic-auth \
  --from-literal=username=mi-fitness \
  --from-literal=password="$(openssl rand -base64 32 | tr -d '/+=' | head -c 32)"
kubectl -n db get secret postgresql-mi-fitness -o jsonpath='{.type}{"\n"}'
```

Ожидание: тип `kubernetes.io/basic-auth`, как у `postgresql-tgw`. Пароль в терминал не печатать.

- [ ] **Step 2: Добавить роль в кластер, не затронув существующие**

```bash
kubectl -n db patch cluster postgresql --type=json -p='[{"op":"add","path":"/spec/managed/roles/-","value":{
  "name":"mi-fitness","comment":"mi-fitness-api db user","ensure":"present","login":true,
  "superuser":false,"createdb":false,"createrole":false,"inherit":false,"replication":false,
  "bypassrls":false,"connectionLimit":-1,"passwordSecret":{"name":"postgresql-mi-fitness"}}}]'
kubectl -n db get cluster postgresql -o jsonpath='{range .spec.managed.roles[*]}{.name}{"\n"}{end}'
```

Ожидание: в выводе три роли, `tarassov-me`, `tgw` и `mi-fitness`. Если какая-то пропала, патч ушёл заменой вместо добавления, надо откатить.

- [ ] **Step 3: Создать базу декларативно**

```yaml
# deploy/db/database.yaml
apiVersion: postgresql.cnpg.io/v1
kind: Database
metadata:
  name: mi-fitness
  namespace: db
spec:
  cluster:
    name: postgresql
  name: mi_fitness
  owner: mi-fitness
  ensure: present
  # Удаление ресурса не должно уносить данные здоровья вместе с собой.
  databaseReclaimPolicy: retain
```

```bash
kubectl apply -f deploy/db/database.yaml
kubectl -n db get database mi-fitness -o custom-columns='NAME:.metadata.name,PG:.status.applied,MSG:.status.message'
```

Ожидание: `applied` равно `true`, сообщение пустое.

- [ ] **Step 4: Проверить подключение ролью, а не суперпользователем**

```bash
kubectl -n db run psql-check --rm -i --restart=Never \
  --image=ghcr.io/cloudnative-pg/postgresql:18.3-system-trixie \
  --env=PGPASSWORD="$(kubectl -n db get secret postgresql-mi-fitness -o jsonpath='{.data.password}' | base64 -d)" \
  -- psql -h postgresql-rw -U mi-fitness -d mi_fitness \
  -c 'select current_user, current_database();' \
  -c 'create table probe(x int); drop table probe;'
```

Ожидание: `mi-fitness | mi_fitness` и успешное создание с удалением таблицы, то есть владение базой действительно есть.

- [ ] **Step 5: Записать в репозиторий образец и заметку**

`deploy/db/secret-postgresql-mi-fitness.example.yaml` содержит структуру секрета с `REPLACE_ME` вместо пароля. `deploy/db/README.md` описывает, что кластер общий, что роль добавляется патчем-добавлением, и что `databaseReclaimPolicy: retain` оставляет базу живой при удалении ресурса.

- [ ] **Step 6: Коммит**

```bash
git add deploy/db
git commit -m "feat(deploy): база mi_fitness и роль в общем кластере CNPG

Роль добавляется патчем-добавлением в spec.managed.roles: кластер общий,
там уже живут tarassov-me и tgw. Политика retain у базы, чтобы удаление
ресурса не уносило данные."
```

---

### Task 8: Деплой в отдельный namespace и живая проверка крипты

**Files:**
- Modify: `helm/mi-fitness-api/values.yaml` (значения по умолчанию остаются примерами)
- Create: `deploy/values-prod.yaml`
- Create: `deploy/secret-app.example.yaml`
- Create: `deploy/README.md`

**Interfaces:**
- Consumes: образ из CI, база и роль из задачи 7, маршрут проверки связи из задачи 6
- Produces: работающий сервис в namespace `mi-fitness-api`, подтверждённый живым ответом облака Xiaomi

- [ ] **Step 1: Namespace и секрет приложения**

```bash
kubectl create namespace mi-fitness-api
kubectl -n mi-fitness-api create secret generic mi-fitness-app \
  --from-literal=MI_FITNESS_USER_ID=4236479152 \
  --from-literal=MI_FITNESS_PASS_TOKEN='<свежий токен из браузера>' \
  --from-literal=MI_FITNESS_TOKEN_KEY="$(openssl rand -base64 32)" \
  --from-literal=DATABASE_PASSWORD="$(kubectl -n db get secret postgresql-mi-fitness -o jsonpath='{.data.password}' | base64 -d)" \
  --from-literal=API_KEY="$(openssl rand -base64 24 | tr -d '/+=' | head -c 32)"
```

Токен берётся свежим из браузера, а не из локальной ключницы Python-моста: два держателя одного токена воюют между собой, и Python-мост на этот момент ещё работает.

- [ ] **Step 2: Значения Helm без единого PVC**

```yaml
# deploy/values-prod.yaml
image:
  repository: ghcr.io/moveeeax/mi-fitness-api
  tag: sha-REPLACE
externalDatabase:
  host: postgresql-rw.db.svc.cluster.local
  port: 5432
  name: mi_fitness
  user: mi-fitness
  poolSize: 5
  replicaHost: postgresql-ro.db.svc.cluster.local
  migrationsEnabled: false
migrations:
  runAsInitContainer: true
externalRedis:
  url: "tcp://redis.db.svc.cluster.local:6379"
ingress:
  enabled: true
  className: nginx
  annotations:
    cert-manager.io/cluster-issuer: letsencrypt-prod
    nginx.ingress.kubernetes.io/ssl-redirect: "true"
  hosts:
    - host: mi-fitness-api.tarassov.me
  tls:
    - hosts: [mi-fitness-api.tarassov.me]
      secretName: mi-fitness-api-tls
```

Пароль базы и секреты Xiaomi приходят из `mi-fitness-app` через `envFrom`, в значениях их нет. Проверить, что чарт не создаёт ни одного `PersistentVolumeClaim`:

```bash
helm template mi-fitness-api helm/mi-fitness-api -f deploy/values-prod.yaml | grep -c PersistentVolumeClaim
```

Ожидание: ноль. Если не ноль, найти включённый подчарт или блок `persistence` и выключить.

- [ ] **Step 3: Развернуть API и воркер**

```bash
helm upgrade --install mi-fitness-api helm/mi-fitness-api \
  -n mi-fitness-api -f deploy/values-prod.yaml --wait --timeout 5m
kubectl -n mi-fitness-api get deploy,pod,ingress
kubectl -n mi-fitness-api logs -l app.kubernetes.io/name=mi-fitness-api --tail=30
```

Ожидание: под готов, init-контейнер миграций отработал один раз, в логах нет попыток подключиться к Kafka и к почте.

- [ ] **Step 4: Убедиться, что миграция создала таблицу учётных данных**

```bash
kubectl -n db run psql-check --rm -i --restart=Never \
  --image=ghcr.io/cloudnative-pg/postgresql:18.3-system-trixie \
  --env=PGPASSWORD="$(kubectl -n db get secret postgresql-mi-fitness -o jsonpath='{.data.password}' | base64 -d)" \
  -- psql -h postgresql-rw -U mi-fitness -d mi_fitness -c '\dt'
```

Ожидание: среди таблиц есть `xiaomi_credentials` и служебные таблицы миграций шаблона.

- [ ] **Step 5: Живая проверка крипты на настоящем облаке**

```bash
API_KEY=$(kubectl -n mi-fitness-api get secret mi-fitness-app -o jsonpath='{.data.API_KEY}' | base64 -d)
curl -s -H "X-API-Key: $API_KEY" \
  'https://mi-fitness-api.tarassov.me/api/v1/xiaomi/probe?key=steps&from=2026-09-22&to=2026-09-22' | jq
```

Ожидание: `records` больше нуля, `account` равен `******52`, `region` равен `cn`. Это единственная точка всего плана, где порт крипты подтверждается настоящим облаком, а не векторами.

Разбор отказов: ошибка расшифровки означает промах в переносе RC4 или в порядке подписи, а не сеть. `kind: auth` означает, что токен в секрете уже мёртв, надо взять свежий и пересоздать секрет. Пустое `records` при коде 200 означает, что за эти сутки записей нет, и это не ошибка.

- [ ] **Step 6: Коммит**

```bash
git add deploy helm
git commit -m "feat(deploy): сервис в отдельном namespace без PVC

Состояние только в Postgres из namespace db и в Redis, тома не нужны.
Миграции идут init-контейнером, чтобы реплики не гонялись за ними на
старте. Живая проверка probe подтверждает порт крипты на настоящем облаке."
```

## Что идёт следующими планами

1. План 2, данные: нормализация восьми типов с семью правилами из спеки, репозитории, `SyncService`, воркер, `sync_runs`, идемпотентность через `ON CONFLICT`, сверка чисел с Python-мостом.
2. План 3, витрина и переход: десять маршрутов REST, экспорт JSON и CSV, расписание через `Tasks::`, пауза и удаление Python-моста вместе с его PVC. Инфраструктура деплоя к этому моменту уже стоит: она въехала в задачи 7 и 8 этого плана, потому что без развёрнутого сервиса крипту не проверить ничем.
