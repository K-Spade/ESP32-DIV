#include "SnakeGame.h"

#include <Arduino.h>
#include "config.h"
#include "shared.h"
#include "utils.h"

namespace SnakeGame {
namespace {

constexpr int16_t CELL = 10;
constexpr int16_t GRID_W = 23;
constexpr int16_t GRID_H = 23;
constexpr int16_t BOARD_X = 5;
constexpr int16_t BOARD_Y = 51;
constexpr int16_t BOARD_W = GRID_W * CELL;
constexpr int16_t BOARD_H = GRID_H * CELL;
constexpr uint16_t MAX_LEN = GRID_W * GRID_H;
constexpr uint16_t START_MS = 140;
constexpr uint16_t MIN_MS = 42;

struct Point { uint8_t x; uint8_t y; };
enum class Direction : uint8_t { Up, Down, Left, Right };

Point snake[MAX_LEN];
Point food{};
uint16_t snakeLen = 0;
uint16_t score = 0;
uint16_t intervalMs = START_MS;
uint32_t nextMoveMs = 0;
Direction requested = Direction::Right;
Direction travelling = Direction::Right;
bool gameOver = false;
bool won = false;

int16_t px(uint8_t x) { return BOARD_X + x * CELL; }
int16_t py(uint8_t y) { return BOARD_Y + y * CELL; }

void drawCell(const Point& p, uint16_t color, bool inset = true) {
  const int i = inset ? 1 : 0;
  tft.fillRect(px(p.x) + i, py(p.y) + i, CELL - i, CELL - i, color);
}

void drawHeader() {
  tft.fillRect(0, 20, TFT_WIDTH, 27, UI_FG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(MK_SIGNAL_GREEN, UI_FG);
  tft.setCursor(7, 26);
  tft.print("SNAKE");
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.setCursor(128, 26);
  tft.printf("Score: %u", (unsigned)score);
}

bool onSnake(const Point& p) {
  for (uint16_t i = 0; i < snakeLen; ++i) {
    if (snake[i].x == p.x && snake[i].y == p.y) return true;
  }
  return false;
}

bool spawnFood() {
  if (snakeLen >= MAX_LEN) return false;
  for (uint16_t attempt = 0; attempt < MAX_LEN * 2u; ++attempt) {
    Point p{(uint8_t)random(GRID_W), (uint8_t)random(GRID_H)};
    if (!onSnake(p)) { food = p; return true; }
  }
  for (uint8_t y = 0; y < GRID_H; ++y) {
    for (uint8_t x = 0; x < GRID_W; ++x) {
      Point p{x, y};
      if (!onSnake(p)) { food = p; return true; }
    }
  }
  return false;
}

bool opposite(Direction a, Direction b) {
  return (a == Direction::Up && b == Direction::Down) ||
         (a == Direction::Down && b == Direction::Up) ||
         (a == Direction::Left && b == Direction::Right) ||
         (a == Direction::Right && b == Direction::Left);
}

void setDirection(Direction d) {
  if (!opposite(travelling, d)) requested = d;
}

void drawBoard() {
  tft.fillRect(0, 47, TFT_WIDTH, 238, FEATURE_BG);
  tft.drawRect(BOARD_X - 1, BOARD_Y - 1, BOARD_W + 2, BOARD_H + 2, UI_LINE);
  drawCell(food, RED);
  for (uint16_t i = snakeLen; i > 0; --i) {
    drawCell(snake[i - 1], i == 1 ? UI_WARN : MK_SIGNAL_GREEN);
  }
}

void reset(Direction d = Direction::Right) {
  snakeLen = 4;
  const uint8_t cx = GRID_W / 2;
  const uint8_t cy = GRID_H / 2;
  snake[0] = {cx, cy};
  for (uint8_t i = 1; i < snakeLen; ++i) {
    if (d == Direction::Up) snake[i] = {cx, (uint8_t)(cy + i)};
    else if (d == Direction::Down) snake[i] = {cx, (uint8_t)(cy - i)};
    else if (d == Direction::Left) snake[i] = {(uint8_t)(cx + i), cy};
    else snake[i] = {(uint8_t)(cx - i), cy};
  }
  requested = travelling = d;
  score = 0;
  intervalMs = START_MS;
  gameOver = won = false;
  spawnFood();
  drawHeader();
  drawBoard();
  nextMoveMs = millis() + intervalMs;
}

void drawEnd() {
  const int y = 118;
  tft.fillRoundRect(18, y, 204, 76, 5, UI_FG);
  tft.drawRoundRect(18, y, 204, 76, 5, won ? UI_OK : UI_WARN);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(won ? UI_OK : UI_WARN, UI_FG);
  tft.drawString(won ? "YOU WIN" : "GAME OVER", TFT_WIDTH / 2, y + 9, 2);
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.drawString(String("Score: ") + score, TFT_WIDTH / 2, y + 34, 2);
  tft.drawString("Direction: retry  Center: exit", TFT_WIDTH / 2, y + 56, 1);
  tft.setTextDatum(TL_DATUM);
}

void move() {
  Point n = snake[0];
  if (requested == Direction::Up) --n.y;
  else if (requested == Direction::Down) ++n.y;
  else if (requested == Direction::Left) --n.x;
  else ++n.x;

  if (n.x >= GRID_W || n.y >= GRID_H) {
    gameOver = true; drawEnd(); return;
  }
  const bool growing = n.x == food.x && n.y == food.y;
  const uint16_t collisionLen = growing ? snakeLen : snakeLen - 1;
  for (uint16_t i = 0; i < collisionLen; ++i) {
    if (snake[i].x == n.x && snake[i].y == n.y) {
      gameOver = true; drawEnd(); return;
    }
  }

  const Point oldHead = snake[0];
  const Point oldTail = snake[snakeLen - 1];
  if (growing) {
    for (uint16_t i = snakeLen; i > 0; --i) snake[i] = snake[i - 1];
    ++snakeLen;
  } else {
    for (uint16_t i = snakeLen - 1; i > 0; --i) snake[i] = snake[i - 1];
    drawCell(oldTail, FEATURE_BG, false);
  }
  snake[0] = n;
  travelling = requested;
  drawCell(oldHead, MK_SIGNAL_GREEN);
  drawCell(snake[0], UI_WARN);

  if (growing) {
    score += 10;
    const uint16_t rawReduction = (score / 50u) * 10u;
    const uint16_t reduction = rawReduction > START_MS - MIN_MS
        ? START_MS - MIN_MS : rawReduction;
    intervalMs = START_MS - reduction;
    drawHeader();
    if (!spawnFood()) { won = gameOver = true; drawEnd(); return; }
    drawCell(food, RED);
  }
}

bool directionPressed(Direction& d) {
  if (isButtonPressedEdge(BTN_UP)) { d = Direction::Up; return true; }
  if (isButtonPressedEdge(BTN_DOWN)) { d = Direction::Down; return true; }
  if (isButtonPressedEdge(BTN_LEFT)) { d = Direction::Left; return true; }
  if (isButtonPressedEdge(BTN_RIGHT)) { d = Direction::Right; return true; }
  return false;
}

}  // namespace

void setup() {
  pauseBackgroundRadioTasks();
  neopixelClear();
  setTouchButtonInputEnabled(true);
  setTouchNavLabels("Left", "Down", "Exit", "Up", "Right");
  randomSeed(micros());
  featureClearContent(FEATURE_BG);
  drawStatusBar(readBatteryVoltage(), true);
  resetTouchNavHeldState();
  maintainTouchNavBar();
  reset();
}

void loop() {
  maintainTouchNavBar();
  if (featureExitButtonPressed()) {
    feature_exit_requested = true;
    return;
  }
  Direction d = Direction::Right;
  if (directionPressed(d)) {
    if (gameOver) reset(d);
    else setDirection(d);
  }
  if (!gameOver && (int32_t)(millis() - nextMoveMs) >= 0) {
    move();
    nextMoveMs = millis() + intervalMs;
  }
  delay(3);
}

void shutdown() {
  setTouchNavLabels(nullptr, nullptr, nullptr, nullptr, nullptr);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
}

}  // namespace SnakeGame
