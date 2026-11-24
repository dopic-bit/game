// ============================================================================
//  ОЧЕРЕДЬ — гонка на выживание за местом у кассы.
//  Чистый C++17, POSIX-терминал, собственная 1D-физика людей.
//
//  Сборка:  g++ -std=c++17 -O2 -o queue queue_game.cpp
//  Запуск:  ./queue        (--sim N  — бездействующий прогон N кадров для теста)
//
//  Управление:
//    a / d   — двигаться влево / вправо
//    SPACE   — ТОЛЧОК (лягнуть того, кто впереди)
//    b       — ЛОКТИ назад (отбиться от того, кто лезет сзади)
//    p       — УПРЕТЬСЯ (стать неподвижной глыбой на полсекунды)
//    q       — выйти
// ============================================================================

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// ----------------------------------------------------------------------------
// Настройки мира
// ----------------------------------------------------------------------------
static const int    W           = 80;   // ширина поля в символах
static const int    H           = 22;   // высота поля
static const int    COUNTER_X   = W - 7;// касса
static const float  DT          = 1.0f / 30.0f;
static const float  QUEUE_MIN   = 1.0f; // левая граница очереди
static const float  QUEUE_MAX   = (float)(COUNTER_X - 1);
static const float  PERSON_W    = 1.4f; // ширина одного человека
static const int    SERVE_NEEDED= 100;  // сколько "делов" нужно, чтобы пробить чек

// ANSI-цвета
static const char* C_RESET  = "\x1b[0m";
static const char* C_RED    = "\x1b[1;31m";
static const char* C_GREEN  = "\x1b[1;32m";
static const char* C_YELLOW = "\x1b[1;33m";
static const char* C_BLUE   = "\x1b[1;34m";
static const char* C_MAGENTA= "\x1b[1;35m";
static const char* C_CYAN   = "\x1b[1;36m";
static const char* C_WHITE  = "\x1b[1;37m";
static const char* C_GRAY   = "\x1b[1;90m";

// ----------------------------------------------------------------------------
// Человек
// ----------------------------------------------------------------------------
struct Person {
    std::string name;    // кто это
    char        glyph;   // буква на поле
    const char* color;
    float       x    = 0.0f;
    float       vx   = 0.0f;
    float       mass = 1.0f;
    float       speed = 1.0f; // личная прыть при подтягивании в очередь
    float       angry    = 0.0f;   // 0..100
    float       stun     = 0.0f;   // оглушение, сек
    float       serve    = 0.0f;   // прогресс обслуживания у кассы
    int         shoveCd  = 0;      // перезарядка толчка, в кадрах
    bool        isPlayer = false;
    bool        bracing  = false;  // упёрся
    bool        alive    = true;
};

// Ростерь NPC, которые подтягиваются в хвост
struct NpcKind {
    const char* name;
    char        glyph;
    const char* color;
    float       mass;
    float       speed;
};
static const NpcKind npcKinds[] = {
    {"Бабушка с авоськой", 'B', C_MAGENTA, 1.9f, 1.0f},  // тяжёлая, не сдвинешь
    {"Школьник",           'S', C_CYAN,    0.7f, 2.2f},  // лёгкий, но шустрый
    {"Тёзка с телефоном",  'T', C_BLUE,    1.0f, 1.2f},  // стоит зазумленный
    {"Дед-фантик",         'D', C_GREEN,   1.3f, 0.8f},  // медленный, но упорный
    {"Курьер",             'K', C_GRAY,    1.1f, 1.6f},  // вечно спешит
    {"Программист",        'P', C_YELLOW,  1.4f, 0.9f},  // прим ذегенерировал
};
static const int npcKindCount = sizeof(npcKinds) / sizeof(npcKinds[0]);

// ----------------------------------------------------------------------------
// Реплики — смешные и злые
// ----------------------------------------------------------------------------
static const char* angryLines[] = {
    "Тут стояла женщина в шубе!!!",
    "Я спрашивал — МНЕ СКАЗАЛИ СТОЯТЬ ЗДЕСЬ!",
    "Молодой человек, я вас запомнила.",
    "ЧЕЛОВЕК, ТЫ ВИДИШЬ, ЧТОЛИ, ОЧЕРЕДЬ?!",
    "Так, без физической агрессии. Почти без.",
    "Я на вас всех в книгу жалоб напишу!",
    "Только спросить! ...и постоять.",
    "За мной не занимали?! ЗА МНОЙ НЕ ЗАНИМАЛИ?!",
    "Бабушка активировала ЛОКТИ.",
    "Кто-то вызовите Терезу Мэй, тут беспредел!",
    "Мужик в пуховике начал договариваться с кассой.",
    "Школьник придумал схему и отступил.",
    "Кто-то 'на минутку отошёл' уже 15 минут.",
    "Вы что, очередь в прошлом не видели?",
    "Безобразие. Но место моё.",
};
static const char* hitLines[] = {
    "*ХРУСЬ*", "*ЛОПОТУХ*", "*БАМЦ*", "*ТЫДЫЩ*", "*ШЛЁП*", "*КРЯК*",
};

// ----------------------------------------------------------------------------
// Терминал: сырой режим, неблокирующий ввод
// ----------------------------------------------------------------------------
static termios g_oldTermios;
static bool    g_isTty = false;

static void terminalRawOn() {
    g_isTty = (isatty(STDIN_FILENO) != 0);
    if (!g_isTty) return;
    tcgetattr(STDIN_FILENO, &g_oldTermios);
    termios raw = g_oldTermios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    printf("\x1b[?1049h\x1b[?25l"); // alt screen + скрыть курсор
    fflush(stdout);
}
static void terminalOff() {
    if (g_isTty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_oldTermios);
        printf("\x1b[?25h\x1b[?1049l");
        fflush(stdout);
    }
}
static void onSig(int) { terminalOff(); std::_Exit(1); }

// Возвращает последний нажатый символ за кадр (или 0)
static int pollKey() {
    unsigned char ch = 0;
    ssize_t n;
    int last = 0;
    while ((n = read(STDIN_FILENO, &ch, 1)) == 1) last = ch;
    return last;
}

// ----------------------------------------------------------------------------
// Экран
// ----------------------------------------------------------------------------
struct Screen {
    char        cells[H][W + 1];
    int         fg[H][W];
    Screen() { clear(); }
    void clear() {
        for (int y = 0; y < H; ++y) {
            memset(cells[y], ' ', W);
            cells[y][W] = 0;
            for (int x = 0; x < W; ++x) fg[y][x] = 7;
        }
    }
    void put(int x, int y, char c, int col) {
        if (x >= 0 && x < W && y >= 0 && y < H) { cells[y][x] = c; fg[y][x] = col; }
    }
};
// строки с цветом поверх сетки (координаты в символах)
struct LineText { int x, y; std::string s; const char* color; };
static std::vector<LineText> g_texts;

static void drawText(int x, int y, const std::string& s, const char* color) {
    g_texts.push_back({x, y, s, color});
}

static const char* colorOf(int idx) {
    switch (idx) {
        case 1: return C_RED; case 2: return C_GREEN; case 3: return C_YELLOW;
        case 4: return C_BLUE; case 5: return C_MAGENTA; case 6: return C_CYAN;
        case 8: return C_GRAY; default: return C_WHITE;
    }
}

static int colorIndexOf(const char* c) {
    if (c == C_RED) return 1;
    if (c == C_GREEN) return 2;
    if (c == C_YELLOW) return 3;
    if (c == C_BLUE) return 4;
    if (c == C_MAGENTA) return 5;
    if (c == C_CYAN) return 6;
    if (c == C_GRAY) return 8;
    return 7;
}

// вписываем текстовые строки прямо в ячейки поля
static void mergeTexts(Screen& sc) {
    for (auto& t : g_texts) {
        int ci = colorIndexOf(t.color);
        for (size_t i = 0; i < t.s.size(); ++i)
            sc.put(t.x + (int)i, t.y, t.s[i], ci);
    }
}

static void present(const Screen& sc) {
    printf("\x1b[H");
    for (int y = 0; y < H; ++y) {
        printf("\x1b[K");
        for (int x = 0; x < W; ++x)
            printf("%s%c", colorOf(sc.fg[y][x]), sc.cells[y][x]);
        printf("%s\r\n", C_RESET);
    }
    printf("%s", C_RESET);
    fflush(stdout);
}

// ----------------------------------------------------------------------------
// Игра
// ----------------------------------------------------------------------------
struct Game {
    std::mt19937 rng;
    std::vector<Person> people;
    float elapsed = 0.0f;
    int   shovesDone = 0;
    int   shovesTaken = 0;
    int   served = 0;
    float stamina = 100.0f;
    std::string ticker;
    float tickerLeft = 0.0f;
    int   state = 0; // 0 = игра, 1 = победа, 2 = поражение
    std::string endReason;
    int   simFramesLeft = -1; // для --sim

    std::uniform_int_distribution<int> d100{0, 99};

    Game() : rng((unsigned)::time(nullptr)) {}

    void say(const std::string& s) { ticker = s; tickerLeft = 3.0f; }

    void spawnPeople() {
        people.clear();

        Person p;
        p.name = "Ты"; p.glyph = 'Q'; p.color = C_RED;
        p.isPlayer = true; p.mass = 1.4f;
        p.x = QUEUE_MAX - 4.0f * PERSON_W;
        people.push_back(p);

        for (int i = 0; i < 7; ++i) {
            const NpcKind& k = npcKinds[i % npcKindCount];
            Person q;
            q.name   = k.name;
            q.glyph  = k.glyph;
            q.color  = k.color;
            q.mass   = k.mass;
            q.speed  = k.speed;
            q.angry  = (float)(d100(rng) % 40);
            q.x      = QUEUE_MAX - (5 + i) * PERSON_W;
            people.push_back(q);
        }
    }

    Person* front() {
        Person* best = nullptr;
        for (auto& p : people)
            if (p.alive && (!best || p.x > best->x)) best = &p;
        return best;
    }
    Person* aheadOf(Person& p) {
        Person* best = nullptr;
        for (auto& q : people)
            if (q.alive && &q != &p && q.x > p.x && (!best || q.x < best->x)) best = &q;
        return best;
    }
    Person* behindOf(Person& p) {
        Person* best = nullptr;
        for (auto& q : people)
            if (q.alive && &q != &p && q.x < p.x && (!best || q.x > best->x)) best = &q;
        return best;
    }

    void shove(Person& src, Person& dst, float power) {
        dst.vx += power;
        dst.angry = std::min(100.0f, dst.angry + 28.0f);
        if (d100(rng) < 30) dst.stun = 0.35f;
        src.vx -= power * 0.25f;
        drawText((int)dst.x, 3, hitLines[d100(rng) % 5], C_YELLOW);
        say(std::string(src.name) + " применил грубую физику. " +
            hitLines[d100(rng) % 6]);
    }

    void npcThink(Person& p) {
        Person* a = aheadOf(p);
        if (!a) return;                    // ты первый — стой у кассы
        float gap = a->x - p.x - PERSON_W;
        float speed = (1.4f + p.angry * 0.02f) * p.speed;

        if (gap > 2.5f) {                  // подобраться поближе
            p.vx += (gap > 10 ? 4.0f * p.speed : speed) * DT;
        } else if (gap < 0.4f && p.angry > 55 && p.shoveCd <= 0) {
            shove(p, *a, 3.5f + p.angry * 0.03f);
            p.shoveCd = 40 + d100(rng) % 40;
            if (d100(rng) < 25) say(angryLines[d100(rng) % 15]);
        } else if (p.angry > 90 && p.shoveCd <= 0 && d100(rng) < 12) {
            // бешенство: рывок с прыжком вперёд
            p.vx += 8.0f;
            p.shoveCd = 60;
            say(std::string(p.name) + " пошёл на прорыв!!!");
        } else {
            // стоять СКУЧНО — злость капает, и чем дольше игра, тем быстрее
            p.angry = std::min(100.0f, p.angry + 0.08f + elapsed * 0.001f);
        }
    }

    void step() {
        if (state != 0) return;
        elapsed += DT;
        if (tickerLeft > 0) tickerLeft -= DT;

        int key = 0;
        if (simFramesLeft < 0) key = pollKey();
        if (key == 'q' || key == 'Q' || (simFramesLeft >= 0 && simFramesLeft-- <= 0)) {
            if (simFramesLeft >= 0) { state = 1; endReason = "sim done"; return; }
            terminalOff();
            printf("Пока!\n");
            std::exit(0);
        }

        Person& you = people[0];

        // --- ввод игрока ---
        if (you.stun <= 0 && state == 0) {
            if (key == 'a') you.vx -= 26.0f * DT;
            if (key == 'd') you.vx += 26.0f * DT;
            if (key == ' ') {
                Person* a = aheadOf(you);
                if (a && a->x - you.x - PERSON_W < 3.0f && stamina >= 12) {
                    shove(you, *a, 6.0f);
                    shovesDone++; stamina -= 12;
                }
            }
            if (key == 'b') {
                Person* b = behindOf(you);
                if (b && you.x - b->x - PERSON_W < 3.0f && stamina >= 12) {
                    b->vx -= 6.0f;
                    b->angry = std::min(100.0f, b->angry + 30.0f);
                    shovesTaken++; stamina -= 12;
                    say("Ты выставил ЛОКТИ. " + std::string(b->name) + " недоволен.");
                }
            }
            if (key == 'p' && stamina >= 20) { you.bracing = true; stamina -= 20; }
        }
        you.bracing = you.bracing && you.stun <= 0;
        if (you.bracing && d100(rng) < 2) you.bracing = false; // упор не вечен

        // --- NPCs ---
        for (size_t i = 1; i < people.size(); ++i) npcThink(people[i]);

        // --- физика ---
        for (auto& p : people) {
            if (p.stun > 0) p.stun -= DT;
            if (p.shoveCd > 0) p.shoveCd--;
            if (!p.isPlayer) p.vx *= 0.90f;         // трение
            else             p.vx *= 0.86f;
            if (p.bracing)  { p.mass = 12.0f; p.vx *= 0.2f; }
            else if (p.isPlayer) p.mass = 1.4f;
            p.x += p.vx * DT;
        }

        // столкновения (отсортируем, растолкаем)
        std::vector<Person*> sorted;
        for (auto& p : people) if (p.alive) sorted.push_back(&p);
        std::sort(sorted.begin(), sorted.end(),
                  [](Person* a, Person* b) { return a->x < b->x; });
        for (size_t i = 0; i + 1 < sorted.size(); ++i) {
            Person* A = sorted[i];
            Person* B = sorted[i + 1];
            float overlap = (A->x + PERSON_W) - B->x;
            if (overlap > 0.0f) {
                float totalMass = A->mass + B->mass;
                float push = overlap;
                A->x -= push * (B->mass / totalMass);
                B->x += push * (A->mass / totalMass);
                float rel = B->vx - A->vx;
                if (rel < 0) { // сближаются
                    float j = -rel * 0.5f;
                    A->vx -= j * (B->mass / totalMass) * 2;
                    B->vx += j * (A->mass / totalMass) * 2;
                    // толчея злит обоих
                    A->angry += 0.4f; B->angry += 0.4f;
                    B->angry = std::min(100.0f, B->angry);
                }
            }
        }

        // границы очереди
        for (auto& p : people) {
            if (p.x < QUEUE_MIN) { p.x = QUEUE_MIN; p.vx = 0; }
            if (p.x > QUEUE_MAX) { p.x = QUEUE_MAX; p.vx = 0; }
        }

        // обслуживание у кассы
        Person* f = front();
        if (f) {
            if (f->x >= QUEUE_MAX - 0.6f) {
                f->serve += DT * 55.0f;
                if (f->serve >= SERVE_NEEDED) {
                    if (f->isPlayer) { win("Ты пробил чек и гордо вышел с батоном!"); return; }
                    served++;
                    f->alive = false;
                    say(std::string(f->name) + " обслужен и растворился в закате.");
                    // новый человек подтягивается — тем злее, чем дольше идёт игра
                    const NpcKind& k = npcKinds[d100(rng) % npcKindCount];
                    Person n;
                    n.name   = k.name;
                    n.glyph  = k.glyph;
                    n.color  = k.color;
                    n.mass   = k.mass;
                    n.speed  = k.speed;
                    n.angry  = 30.0f + std::min(40.0f, elapsed);
                    n.x      = QUEUE_MIN;
                    people.push_back(n);
                }
            } else {
                f->serve = 0;
            }
        }

        // поражение: вытеснили из очереди
        for (auto& p : people) {
            if (p.isPlayer && p.x <= QUEUE_MIN + 0.01f && p.vx <= 0) {
                lose("Тебя вытеснили из очереди. Люди безжалостны.");
            }
        }

        if (stamina < 100.0f) stamina = std::min(100.0f, stamina + 14.0f * DT);
    }

    void win(const std::string& why)  { state = 1; endReason = why; }
    void lose(const std::string& why) { state = 2; endReason = why; }

    // ------------------------------------------------------------- рекорды
    float bestTime   = 0.0f;
    int   bestShoves = 0;
    bool  haveRecord = false;
    std::string recordPath = "queue_records.txt";

    void loadRecord() {
        if (const char* home = getenv("HOME")) recordPath = std::string(home) + "/.queue_records";
        FILE* fp = fopen(recordPath.c_str(), "r");
        if (!fp) return;
        float t = 0; int s = 0;
        if (fscanf(fp, "%f %d", &t, &s) == 2) { bestTime = t; bestShoves = s; haveRecord = true; }
        fclose(fp);
    }

    void saveRecord() {
        if (state != 1) return;                       // рекорд — только за победу
        bool better = !haveRecord || elapsed < bestTime ||
                      (elapsed == bestTime && shovesDone > bestShoves);
        if (!better) return;
        if (FILE* fp = fopen(recordPath.c_str(), "w")) {
            fprintf(fp, "%.1f %d\n", elapsed, shovesDone);
            fclose(fp);
        }
        bestTime = elapsed; bestShoves = shovesDone; haveRecord = true;
        say("НОВЫЙ РЕКОРД! Так его, очередь!");
    }

    // ------------------------------------------------------------------ drawn
    void render() {
        Screen sc;
        g_texts.clear();

        // рамка + касса
        drawText(10, 0, "====== ОЧЕРЕДЬ ======", C_WHITE);
        drawText(66, 0, "[ КАССА ]", C_GREEN);
        drawText(10, 1, "Q=ты  B=тяжёлая с авоськой  S=шустрый  D=упорный", C_GRAY);

        // земля
        for (int x = 0; x < W; ++x) sc.put(x, H - 1, '=', 8);

        // люди
        for (auto& p : people) {
            if (!p.alive) continue;
            int px = (int)p.x;
            sc.put(px, 8, p.glyph, p.isPlayer ? 1 : 4);
            if (p.angry > 70) sc.put(px, 7, '!', 1);
            else if (p.bracing) sc.put(px, 7, 'B', 6);
            else if (p.stun > 0) sc.put(px, 7, '@', 3);
            if (p.serve > 0) {
                int len = (int)(p.serve * 6 / SERVE_NEEDED);
                for (int i = 0; i < len && i < 6; ++i) sc.put(px + i, 5, '#', 2);
            }
        }

        // HUD
        char buf[128];
        snprintf(buf, sizeof buf, "Толчков: %d   Локтей получено: %d   Обслужено NPC: %d   Время: %.0fс",
                 shovesDone, shovesTaken, served, elapsed);
        drawText(0, H - 3, buf, C_WHITE);

        snprintf(buf, sizeof buf, "Силы: [");
        std::string bar = buf;
        int filled = (int)(stamina / 10);
        for (int i = 0; i < 10; ++i) bar += (i < filled) ? '#' : '.';
        bar += "]";
        drawText(0, H - 4, bar, stamina < 30 ? C_RED : C_GREEN);

        if (tickerLeft > 0) drawText(0, 2, ticker, C_YELLOW);

        if (state != 0) {
            drawText(20, 11, "==== ИТОГ ====", state == 1 ? C_GREEN : C_RED);
            drawText(20, 13, endReason, C_WHITE);
            snprintf(buf, sizeof buf, "Толчков сделано: %d, локтей получено: %d",
                     shovesDone, shovesTaken);
            drawText(20, 14, buf, C_WHITE);
            if (haveRecord) {
                snprintf(buf, sizeof buf, "Лучший забег: %.1fс (%d толчков)",
                         bestTime, bestShoves);
                drawText(20, 15, buf, C_YELLOW);
            }
        }
        mergeTexts(sc);
        present(sc);
        g_texts.clear();
    }
};

// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    signal(SIGINT, onSig);
    signal(SIGTERM, onSig);

    Game game;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--sim") && i + 1 < argc) game.simFramesLeft = atoi(argv[++i]);

    terminalRawOn();

    // проверка размера терминала
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && g_isTty) {
        if (ws.ws_col < W || ws.ws_row < H + 2) {
            terminalOff();
            printf("Терминал слишком маленький: нужно минимум %dx%d строк, у тебя %dx%d.\n",
                   W, H + 1, ws.ws_col, ws.ws_row);
            return 1;
        }
    }

    game.spawnPeople();
    game.loadRecord();
    if (game.haveRecord) {
        char rb[96];
        snprintf(rb, sizeof rb, "Рекорд: %.1fс. Побей его, очередь ждёт.", game.bestTime);
        game.say(rb);
    } else {
        game.say("Цель: пробиться к КАССЕ. Как? Физически.");
    }

    while (true) {
        game.step();
        if (game.state == 1 || game.state == 2) {
            game.render();
            if (game.simFramesLeft >= 0) break;
            // подождать нажатия
            usleep(300 * 1000);
            int k = 0;
            for (int t = 0; t < 40; ++t) { k = 0; unsigned char c; if (read(STDIN_FILENO, &c, 1) == 1) k = c; if (k) break; usleep(50 * 1000); }
            break;
        }
        game.render();
        if (game.simFramesLeft < 0) usleep((useconds_t)(DT * 1000 * 1000));
    }

    terminalOff();
    game.saveRecord();
    printf(game.state == 1 ? "Победа! %s\n" : "Проигрыш. %s\n", game.endReason.c_str());
    if (game.haveRecord)
        printf("Лучший забег: %.1fс, толчков: %d\n", game.bestTime, game.bestShoves);
    return game.state == 1 ? 0 : 2;
}
