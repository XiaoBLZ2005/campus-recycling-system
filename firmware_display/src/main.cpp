#include <Arduino.h>
#include <esp32_smartdisplay.h>

#include "fonts/fonts.h"

LV_IMAGE_DECLARE(waiting_screen_background);

namespace {

constexpr uint32_t COLOR_BG = 0xD8EBF3;
constexpr uint32_t COLOR_INK = 0x1C2B34;
constexpr uint32_t COLOR_CARD = 0xECFBFE;
constexpr uint32_t COLOR_CARD_ALT = 0xE0F5FA;
constexpr uint32_t COLOR_CYAN = 0x65D5E6;
constexpr uint32_t COLOR_YELLOW = 0xF9DEA2;
constexpr uint32_t COLOR_PINK = 0xF0A5AD;
constexpr uint32_t COLOR_MUTED = 0x657C87;
constexpr uint32_t COLOR_WHITE = 0xFFFFFF;
constexpr uint32_t COLOR_GREEN = 0x39C99A;
constexpr uint32_t DEPOSIT_AUTO_SETTLE_TIMEOUT_MS = 60UL * 1000UL;
constexpr uint32_t SETTLEMENT_CONFIRM_TIMEOUT_MS = 15UL * 1000UL;

struct DashboardState {
  String cardId = "2026 1286";
  String studentName = "小明";
  String category = "纸类";
  String statusTitle = "请刷学生卡";
  String statusMain = "请刷卡";
  float weightKg = 0.48f;
  int bottles = 2;
  int totalPoints = 128;
  int transactionPoints = 12;
};

DashboardState state;
String serialLine;

lv_obj_t *cardIdLabel = nullptr;
lv_obj_t *studentStateLabel = nullptr;
lv_obj_t *statusMainLabel = nullptr;
lv_obj_t *pointsLabel = nullptr;
lv_obj_t *transactionLabel = nullptr;
lv_obj_t *weightLabel = nullptr;
lv_obj_t *bottlesLabel = nullptr;
lv_obj_t *countdownPill = nullptr;
lv_obj_t *countdownLabel = nullptr;
lv_obj_t *stepCircles[3] = {nullptr, nullptr, nullptr};
lv_obj_t *stepNumbers[3] = {nullptr, nullptr, nullptr};
lv_obj_t *waitingScreen = nullptr;

String lastWeightText;
int lastBottleCount = -1;
int lastTransactionPoints = -1;
String lastStatusTitle;
String lastStatusText;
String lastCardIdText;
String lastStudentNameText;
bool returnCountdownActive = false;
uint32_t returnCountdownStartedMs = 0;
int lastCountdownSeconds = -1;

enum class DepositPhase {
  Waiting,
  InProgress,
  Uploading,
  Completed,
};

DepositPhase depositPhase = DepositPhase::Waiting;
uint32_t depositStartedMs = 0;
uint32_t settlementStartedMs = 0;
String activeCardId;
float absoluteWeightKg = 0.0f;
int absoluteBottleCount = 0;
float sessionWeightBaselineKg = 0.0f;
int sessionBottleBaseline = 0;
float waitingBaselineKg = 0.0f;
bool waitingBaselineValid = false;
bool hasCardEventId = false;
uint32_t lastCardEventId = 0;
String lastFallbackCardId;
uint32_t lastFallbackCardMs = 0;
constexpr uint32_t CARD_FALLBACK_COOLDOWN_MS = 1200;

void setWaitingScreenVisible(bool visible) {
  if (waitingScreen == nullptr) {
    return;
  }
  if (visible) {
    lv_obj_remove_flag(waitingScreen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(waitingScreen);
  } else {
    lv_obj_add_flag(waitingScreen, LV_OBJ_FLAG_HIDDEN);
  }
}

void applySoftShadow(lv_obj_t *object) {
  lv_obj_set_style_shadow_color(object, lv_color_hex(0x1C2B34),
                                LV_PART_MAIN);
  lv_obj_set_style_shadow_width(object, 10, LV_PART_MAIN);
  lv_obj_set_style_shadow_offset_y(object, 3, LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(object, LV_OPA_20, LV_PART_MAIN);
}

lv_obj_t *makeObject(lv_obj_t *parent, int32_t x, int32_t y,
                     int32_t width, int32_t height) {
  lv_obj_t *object = lv_obj_create(parent);
  lv_obj_remove_style_all(object);
  lv_obj_set_pos(object, x, y);
  lv_obj_set_size(object, width, height);
  lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
  lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  return object;
}

lv_obj_t *createShell(lv_obj_t *parent, int32_t x, int32_t y,
                      int32_t width, int32_t height,
                      uint32_t fillColor) {
  lv_obj_t *outer = makeObject(parent, x, y, width, height);
  lv_obj_set_style_radius(outer, 20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(outer, lv_color_hex(COLOR_INK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, LV_PART_MAIN);
  applySoftShadow(outer);

  lv_obj_t *inner = makeObject(outer, 5, 5, width - 10, height - 10);
  lv_obj_set_style_radius(inner, 15, LV_PART_MAIN);
  lv_obj_set_style_bg_color(inner, lv_color_hex(fillColor),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(inner, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(inner, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(inner, lv_color_hex(COLOR_INK),
                                LV_PART_MAIN);
  return inner;
}

lv_obj_t *createLabel(lv_obj_t *parent, const char *text,
                      const lv_font_t *font, uint32_t color,
                      int32_t x, int32_t y, int32_t width = 0) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  if (font != nullptr) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  }
  lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_pos(label, x, y);
  if (width > 0) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  }
  return label;
}

lv_obj_t *createPill(lv_obj_t *parent, int32_t x, int32_t y,
                     int32_t width, int32_t height, uint32_t fill,
                     uint32_t border) {
  lv_obj_t *pill = makeObject(parent, x, y, width, height);
  lv_obj_set_style_radius(pill, height / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(pill, lv_color_hex(fill), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(pill, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(pill, lv_color_hex(border),
                                LV_PART_MAIN);
  return pill;
}

lv_obj_t *makeColorObject(lv_obj_t *parent, int32_t x, int32_t y,
                          int32_t width, int32_t height, uint32_t fill,
                          int32_t radius = 0, int32_t borderWidth = 0,
                          uint32_t borderColor = 0) {
  lv_obj_t *object = makeObject(parent, x, y, width, height);
  lv_obj_set_style_radius(object, radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(object, lv_color_hex(fill), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_MAIN);
  if (borderWidth > 0) {
    lv_obj_set_style_border_width(object, borderWidth, LV_PART_MAIN);
    lv_obj_set_style_border_color(object, lv_color_hex(borderColor),
                                  LV_PART_MAIN);
  }
  return object;
}

void drawBinIcon(lv_obj_t *parent, int32_t x, int32_t y, int32_t size) {
  const int32_t border = 2;

  lv_obj_t *body = makeObject(parent, x, y, size, size);
  lv_obj_set_style_radius(body, size / 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(body, lv_color_hex(COLOR_WHITE),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(body, border, LV_PART_MAIN);
  lv_obj_set_style_border_color(body, lv_color_hex(COLOR_INK),
                                LV_PART_MAIN);

  lv_obj_t *cap = makeObject(parent, x + size / 3, y - 5,
                            size / 3, 11);
  lv_obj_set_style_radius(cap, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(cap, lv_color_hex(COLOR_CYAN),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(cap, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(cap, lv_color_hex(COLOR_INK),
                                LV_PART_MAIN);

  lv_obj_t *line = makeObject(parent, x + size / 5, y + size / 5,
                             size * 3 / 5, 2);
  lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_INK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);

  const int32_t eyeWidth = size / 10;
  const int32_t eyeHeight = size / 9;
  lv_obj_t *leftEye =
      makeObject(parent, x + size / 3, y + size / 2, eyeWidth, eyeHeight);
  lv_obj_t *rightEye =
      makeObject(parent, x + size * 3 / 5, y + size / 2, eyeWidth, eyeHeight);
  lv_obj_set_style_radius(leftEye, eyeWidth / 2, LV_PART_MAIN);
  lv_obj_set_style_radius(rightEye, eyeWidth / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(leftEye, lv_color_hex(COLOR_INK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(rightEye, lv_color_hex(COLOR_INK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(leftEye, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(rightEye, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t *mouth = makeObject(parent, x + size * 2 / 5,
                               y + size * 3 / 4, size / 4, 2);
  lv_obj_set_style_bg_color(mouth, lv_color_hex(COLOR_INK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(mouth, LV_OPA_COVER, LV_PART_MAIN);
}

void drawPaperIcon(lv_obj_t *parent, int32_t x, int32_t y) {
  lv_obj_t *back = makeObject(parent, x + 8, y, 38, 14);
  lv_obj_set_style_radius(back, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(back, lv_color_hex(COLOR_CYAN),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(back, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(back, lv_color_hex(COLOR_INK),
                                LV_PART_MAIN);

  lv_obj_t *front = makeObject(parent, x, y + 7, 44, 14);
  lv_obj_set_style_radius(front, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(front, lv_color_hex(COLOR_WHITE),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(front, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(front, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(front, lv_color_hex(COLOR_INK),
                                LV_PART_MAIN);
}

void drawBottleIcon(lv_obj_t *parent, int32_t x, int32_t y) {
  lv_obj_t *neck = makeObject(parent, x + 13, y, 10, 9);
  lv_obj_set_style_radius(neck, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(neck, lv_color_hex(COLOR_CYAN),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(neck, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(neck, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(neck, lv_color_hex(COLOR_INK),
                                LV_PART_MAIN);

  lv_obj_t *bottle = makeObject(parent, x, y + 8, 36, 28);
  lv_obj_set_style_radius(bottle, 12, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bottle, lv_color_hex(COLOR_PINK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bottle, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(bottle, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(bottle, lv_color_hex(COLOR_INK),
                                LV_PART_MAIN);
}

String formatCardId(const String &id) {
  if (id.length() == 8) {
    return id.substring(0, 4) + " " + id.substring(4);
  }
  return id;
}

String formatWeightText() {
  String weightText = String(state.weightKg, 2);
  if (weightText.length() > 4) {
    weightText = String(state.weightKg, 1);
  }
  return weightText + " kg";
}

void updateSensorDashboard() {
  const String studentText =
      state.studentName.length() > 0 ? state.studentName : state.statusTitle;
  if (studentText != lastStudentNameText) {
    lv_obj_set_style_text_font(
        studentStateLabel,
        state.studentName.length() > 0 ? &lv_font_simsun_16_cjk : &cjk_28,
        LV_PART_MAIN);
    lv_label_set_text(studentStateLabel, studentText.c_str());
    lastStudentNameText = studentText;
  }

  const String cardIdText = String("卡号 ") + state.cardId;
  if (cardIdText != lastCardIdText) {
    lv_label_set_text(cardIdLabel, cardIdText.c_str());
    lastCardIdText = cardIdText;
  }

  const String weightText = formatWeightText();
  if (weightText != lastWeightText) {
    lv_label_set_text(weightLabel, weightText.c_str());
    lastWeightText = weightText;
  }

  if (state.bottles != lastBottleCount) {
    lv_label_set_text_fmt(bottlesLabel, "%d 件", state.bottles);
    lastBottleCount = state.bottles;
  }

  if (state.transactionPoints != lastTransactionPoints) {
    lv_label_set_text_fmt(transactionLabel, "本次 +%d",
                          state.transactionPoints);
    lastTransactionPoints = state.transactionPoints;
  }

  if (state.statusMain != lastStatusText) {
    lv_label_set_text(statusMainLabel, state.statusMain.c_str());
    lastStatusText = state.statusMain;
  }
}

void updateDashboard() {
  const String studentText =
      state.studentName.length() > 0 ? state.studentName : state.statusTitle;
  lv_obj_set_style_text_font(
      studentStateLabel,
      state.studentName.length() > 0 ? &lv_font_simsun_16_cjk : &cjk_28,
      LV_PART_MAIN);
  lv_label_set_text(studentStateLabel, studentText.c_str());
  lv_label_set_text(cardIdLabel,
                    (String("卡号 ") + state.cardId).c_str());
  lv_label_set_text(statusMainLabel, state.statusMain.c_str());

  lv_label_set_text_fmt(pointsLabel, "%d", state.totalPoints);
  lastStatusTitle = state.statusTitle;
  lastStatusText = state.statusMain;
  lastStudentNameText = studentText;
  lastCardIdText = String("卡号 ") + state.cardId;
  lastTransactionPoints = -1;
  lastBottleCount = -1;
  lastWeightText = "";
  updateSensorDashboard();
}

void updateTransactionPoints() {
  const float nonNegativeKg =
      state.weightKg > 0.0f ? state.weightKg : 0.0f;
  state.transactionPoints =
      state.bottles * 5 +
      static_cast<int>(lroundf(nonNegativeKg * 10.0f));
}

void setStepIndicator(int activeStep) {
  for (int index = 0; index < 3; ++index) {
    if (stepCircles[index] == nullptr) {
      continue;
    }
    const bool active = index == activeStep;
    lv_obj_set_style_bg_color(
        stepCircles[index],
        lv_color_hex(active ? COLOR_CYAN : COLOR_WHITE),
        LV_PART_MAIN);
    if (stepNumbers[index] != nullptr) {
      lv_obj_set_style_text_color(
          stepNumbers[index],
          lv_color_hex(active ? COLOR_WHITE : COLOR_INK),
          LV_PART_MAIN);
    }
  }
}

void cancelReturnCountdown() {
  returnCountdownActive = false;
  lastCountdownSeconds = -1;
  if (countdownPill != nullptr) {
    lv_obj_add_flag(countdownPill, LV_OBJ_FLAG_HIDDEN);
  }
}

void showCountdownSeconds(int seconds) {
  if (countdownPill == nullptr || countdownLabel == nullptr) {
    return;
  }
  lv_obj_remove_flag(countdownPill, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(countdownLabel, "%d 秒", seconds);
  lastCountdownSeconds = seconds;
}

void startReturnCountdown() {
  returnCountdownActive = true;
  returnCountdownStartedMs = millis();
  showCountdownSeconds(5);
}

void beginDepositSession() {
  setWaitingScreenVisible(false);
  cancelReturnCountdown();
  sessionWeightBaselineKg =
      waitingBaselineValid ? waitingBaselineKg : absoluteWeightKg;
  waitingBaselineValid = false;
  sessionBottleBaseline = absoluteBottleCount;
  activeCardId = state.cardId;
  state.studentName = "";
  state.weightKg = 0.0f;
  state.bottles = 0;
  updateTransactionPoints();
  // The card event only contains the UID. Leave the name area blank until
  // STUDENT_INFO arrives from the server with the canonical student name.
  state.statusTitle = "";
  state.statusMain = "正在投放";
  depositPhase = DepositPhase::InProgress;
  depositStartedMs = millis();
  showCountdownSeconds(
      static_cast<int>(DEPOSIT_AUTO_SETTLE_TIMEOUT_MS / 1000UL));
  setStepIndicator(1);
  updateSensorDashboard();
}

void finishDepositSession() {
  state.weightKg = absoluteWeightKg - sessionWeightBaselineKg;
  if (state.weightKg < 0.0f) {
    state.weightKg = 0.0f;
  }
  state.bottles = absoluteBottleCount - sessionBottleBaseline;
  if (state.bottles < 0) {
    state.bottles = 0;
  }
  updateTransactionPoints();
  state.statusTitle = "投放完成";
  state.statusMain = "正在上传本次积分";
  depositPhase = DepositPhase::Uploading;
  settlementStartedMs = millis();
  cancelReturnCountdown();
  setStepIndicator(2);
  updateSensorDashboard();
}

void loadDemoState() {
  state = DashboardState{};
  updateTransactionPoints();
  updateDashboard();
}

void loadResetState() {
  state.cardId = "--";
  state.studentName = "";
  state.category = "待回收";
  state.statusTitle = "请刷学生卡";
  state.statusMain = "请刷卡";
  state.weightKg = 0.0f;
  state.bottles = 0;
  state.totalPoints = 0;
  state.transactionPoints = 0;
  depositPhase = DepositPhase::Waiting;
  activeCardId = "";
  absoluteWeightKg = 0.0f;
  absoluteBottleCount = 0;
  sessionWeightBaselineKg = 0.0f;
  sessionBottleBaseline = 0;
  waitingBaselineKg = 0.0f;
  waitingBaselineValid = false;
  cancelReturnCountdown();
  setStepIndicator(0);
  updateTransactionPoints();
  updateDashboard();
  setWaitingScreenVisible(true);
}

void createHeader(lv_obj_t *screen) {
  lv_obj_t *header = createShell(screen, 16, 10, 768, 64, 0xDFF5FC);
  lv_obj_set_style_bg_grad_color(header, lv_color_hex(0xF7FEFF),
                                 LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(header, LV_GRAD_DIR_VER, LV_PART_MAIN);
  drawBinIcon(header, 18, 8, 42);

  createLabel(header, "校园智能分类回收站", &cjk_28, COLOR_INK,
              79, -1);
  createLabel(header, "SMART RECYCLING STATION",
              &lv_font_montserrat_14, COLOR_MUTED, 80, 34);

  countdownPill =
      createPill(header, 572, 7, 168, 34, 0xF1FCFF, COLOR_INK);
  countdownLabel =
      createLabel(countdownPill, "5 秒", &cjk_16, COLOR_MUTED, 0, 0);
  lv_obj_center(countdownLabel);
  lv_obj_add_flag(countdownPill, LV_OBJ_FLAG_HIDDEN);
}

void createStudentCard(lv_obj_t *screen) {
  lv_obj_t *panel = createShell(screen, 16, 82, 468, 280, COLOR_CARD);
  lv_obj_set_style_bg_grad_color(panel, lv_color_hex(0xF8FEFF),
                                 LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_VER, LV_PART_MAIN);

  createLabel(panel, "学生信息", &cjk_22, COLOR_INK, 14, 7);

  lv_obj_t *divider = makeObject(panel, 14, 41, 430, 2);
  lv_obj_set_style_bg_color(divider, lv_color_hex(COLOR_INK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);

  drawBinIcon(panel, 22, 66, 66);
  createLabel(panel, "学生姓名", &student_label_16, COLOR_MUTED, 106, 56);
  studentStateLabel =
      createLabel(panel, state.statusTitle.c_str(), &cjk_28,
                  COLOR_INK,
                  106, 78, 300);
  cardIdLabel =
      createLabel(panel, "", &cjk_16, COLOR_MUTED, 106, 113, 300);

  lv_obj_t *statusOuter =
      makeObject(panel, 14, 139, 430, 120);
  lv_obj_set_style_radius(statusOuter, 15, LV_PART_MAIN);
  lv_obj_set_style_bg_color(statusOuter, lv_color_hex(COLOR_INK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(statusOuter, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t *statusInner =
      makeObject(statusOuter, 4, 4, 422, 112);
  lv_obj_set_style_radius(statusInner, 11, LV_PART_MAIN);
  lv_obj_set_style_bg_color(statusInner, lv_color_hex(COLOR_CARD),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(statusInner, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(statusInner, lv_color_hex(0xE5F8FC),
                                 LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(statusInner, LV_GRAD_DIR_HOR,
                               LV_PART_MAIN);

  lv_obj_t *accent =
      makeObject(statusInner, 4, 6, 4, 100);
  lv_obj_set_style_radius(accent, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_PINK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN);

  createLabel(statusInner, "当前状态", &cjk_16, COLOR_MUTED, 20, 8);
  statusMainLabel =
      createLabel(statusInner, "", &cjk_22, COLOR_INK, 20, 43, 382);

}

void createPointsCard(lv_obj_t *screen) {
  lv_obj_t *panel = createShell(screen, 492, 82, 292, 124, COLOR_YELLOW);
  lv_obj_set_style_bg_grad_color(panel, lv_color_hex(0xFFF1C9),
                                 LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_VER, LV_PART_MAIN);
  createLabel(panel, "账户积分", &cjk_22, COLOR_INK, 16, 7);
  createPill(panel, 171, 9, 102, 27, 0xFFF7DD, 0xD7BD78);
  transactionLabel =
      createLabel(panel, "本次 +12", &cjk_16, 0x7A673A, 178, 13, 95);

  pointsLabel =
      createLabel(panel, "128", &lv_font_montserrat_48, COLOR_INK, 16, 34);
  createLabel(panel, "分", &cjk_22, COLOR_INK, 140, 53);
  createLabel(panel, "累计积分", &cjk_16, COLOR_MUTED, 18, 82);

  lv_obj_t *diamond = makeObject(panel, 228, 62, 28, 28);
  lv_obj_set_style_radius(diamond, 9, LV_PART_MAIN);
  lv_obj_set_style_border_width(diamond, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(diamond, lv_color_hex(COLOR_PINK),
                                LV_PART_MAIN);
  lv_obj_set_style_transform_rotation(diamond, 450, LV_PART_MAIN);
}

void createTodayCard(lv_obj_t *screen) {
  lv_obj_t *panel = createShell(screen, 492, 214, 292, 148, COLOR_CARD);
  lv_obj_set_style_bg_grad_color(panel, lv_color_hex(0xF8FEFF),
                                 LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_VER, LV_PART_MAIN);

  lv_obj_t *paperCard =
      createShell(panel, 4, 10, 136, 116, 0xE5F8FC);
  createLabel(paperCard, "纸皮", &cjk_16, COLOR_MUTED, 9, 8);
  drawPaperIcon(paperCard, 80, 10);
  weightLabel =
      createLabel(paperCard, "0.48 kg", &lv_font_montserrat_32, COLOR_INK,
                  3, 46, 123);

  lv_obj_t *bottleCard =
      createShell(panel, 144, 10, 134, 116, 0xFFF3F4);
  createLabel(bottleCard, "塑料瓶", &cjk_16, COLOR_MUTED, 9, 8);
  drawBottleIcon(bottleCard, 78, 5);
  bottlesLabel =
      createLabel(bottleCard, "2 件", &cjk_28, COLOR_INK, 9, 54);
}

void createSteps(lv_obj_t *screen) {
  lv_obj_t *panel = createShell(screen, 16, 370, 768, 94, 0xBFD9E5);
  lv_obj_set_style_bg_grad_color(panel, lv_color_hex(0xD8EDF5),
                                 LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_HOR, LV_PART_MAIN);
  createLabel(panel, "投放指引", &cjk_22, COLOR_INK, 14, 26);

  lv_obj_t *firstCircle = makeObject(panel, 148, 22, 32, 32);
  stepCircles[0] = firstCircle;
  lv_obj_set_style_radius(firstCircle, 16, LV_PART_MAIN);
  lv_obj_set_style_bg_color(firstCircle, lv_color_hex(COLOR_CYAN),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(firstCircle, LV_OPA_COVER, LV_PART_MAIN);
  applySoftShadow(firstCircle);
  stepNumbers[0] =
      createLabel(firstCircle, "1", &cjk_16, COLOR_WHITE, 11, 5);
  createLabel(panel, "刷卡识别", &cjk_22, COLOR_INK, 188, 25);

  lv_obj_t *secondCircle = makeObject(panel, 376, 22, 32, 32);
  stepCircles[1] = secondCircle;
  lv_obj_set_style_radius(secondCircle, 16, LV_PART_MAIN);
  lv_obj_set_style_bg_color(secondCircle, lv_color_hex(COLOR_WHITE),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(secondCircle, LV_OPA_COVER, LV_PART_MAIN);
  stepNumbers[1] =
      createLabel(secondCircle, "2", &cjk_16, COLOR_INK, 11, 5);
  createLabel(panel, "投放称重", &cjk_22, COLOR_INK, 416, 25);

  lv_obj_t *thirdCircle = makeObject(panel, 604, 22, 32, 32);
  stepCircles[2] = thirdCircle;
  lv_obj_set_style_radius(thirdCircle, 16, LV_PART_MAIN);
  lv_obj_set_style_bg_color(thirdCircle, lv_color_hex(COLOR_WHITE),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(thirdCircle, LV_OPA_COVER, LV_PART_MAIN);
  stepNumbers[2] =
      createLabel(thirdCircle, "3", &cjk_16, COLOR_INK, 11, 5);
  createLabel(panel, "积分到账", &cjk_22, COLOR_INK, 644, 25);

}

void createWaitingScreen() {
  lv_obj_t *screen = lv_screen_active();
  waitingScreen = makeObject(screen, 0, 0, 800, 480);
  lv_obj_t *background = lv_image_create(waitingScreen);
  lv_image_set_src(background, &waiting_screen_background);
  lv_obj_set_pos(background, 0, 0);
  lv_obj_set_size(background, 800, 480);

  lv_obj_add_flag(waitingScreen, LV_OBJ_FLAG_HIDDEN);
}

void createDashboard() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t *borderTop = makeObject(screen, 16, 5, 768, 3);
  lv_obj_t *borderBottom = makeObject(screen, 16, 470, 768, 3);
  lv_obj_set_style_bg_color(borderTop, lv_color_hex(COLOR_INK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(borderBottom, lv_color_hex(COLOR_INK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(borderTop, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(borderBottom, LV_OPA_COVER, LV_PART_MAIN);

  createHeader(screen);
  createStudentCard(screen);
  createPointsCard(screen);
  createTodayCard(screen);
  createSteps(screen);
}

void printHelp() {
  Serial.println();
  Serial.println("Campus Recycling dashboard commands:");
  Serial.println("CARD|<card-id>[|<points>]");
  Serial.println("MEASURE|<paper|plastic>|<kg>|<bottles>");
  Serial.println("SENSOR|<kg-or-dash>|<ir-count>");
  Serial.println("STUDENT_INFO|<card-id>|<total>|<name>");
  Serial.println("STUDENT_POINTS|<card-id>|<total>");
  Serial.println("POINTS|<total>");
  Serial.println("DEMO");
  Serial.println("RESET");
  Serial.println();
}

void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) {
    return;
  }
  if (line.equalsIgnoreCase("HELP")) {
    printHelp();
    return;
  }
  if (line.equalsIgnoreCase("DEMO")) {
    loadDemoState();
    return;
  }
  if (line.equalsIgnoreCase("RESET")) {
    loadResetState();
    return;
  }

  const int first = line.indexOf('|');
  const String command = first >= 0 ? line.substring(0, first) : line;

  if (command.equalsIgnoreCase("CARD_EVENT") && first > 0) {
    const int second = line.indexOf('|', first + 1);
    if (second > first && second + 1 < line.length()) {
      const String cardId =
          formatCardId(line.substring(first + 1, second));
      const uint32_t eventId =
          static_cast<uint32_t>(line.substring(second + 1).toInt());
      Serial.printf(
          "DISPLAY CARD_EVENT RX id=%lu t=%lu\n",
          static_cast<unsigned long>(eventId),
          static_cast<unsigned long>(millis()));

      if (hasCardEventId && eventId == lastCardEventId) {
        Serial.printf("ACK|CARD_EVENT|%lu\r\n",
                      static_cast<unsigned long>(eventId));
        Serial.printf(
            "DISPLAY CARD_EVENT DUP id=%lu t=%lu\n",
            static_cast<unsigned long>(eventId),
            static_cast<unsigned long>(millis()));
        return;
      }

      hasCardEventId = true;
      lastCardEventId = eventId;
      state.cardId = cardId;
      if (depositPhase == DepositPhase::InProgress &&
          state.cardId == activeCardId) {
        finishDepositSession();
      } else {
        beginDepositSession();
      }
      Serial.printf("ACK|CARD_EVENT|%lu\r\n",
                    static_cast<unsigned long>(eventId));
      lv_refr_now(lv_display_get_default());
      Serial.printf(
          "DISPLAY CARD_EVENT UI id=%lu t=%lu\n",
          static_cast<unsigned long>(eventId),
          static_cast<unsigned long>(millis()));
    }
    return;
  }

  if (command.equalsIgnoreCase("CARD") && first > 0) {
    const int second = line.indexOf('|', first + 1);
    const int cardEnd = second > first ? second : line.length();
    if (cardEnd > first + 1) {
      state.cardId =
          formatCardId(line.substring(first + 1, cardEnd));
    }
    if (second > first && second + 1 < line.length()) {
      const int points = line.substring(second + 1).toInt();
      if (points >= 0) {
        state.totalPoints = points;
      }
    }
    const uint32_t now = millis();
    if (state.cardId == lastFallbackCardId &&
        now - lastFallbackCardMs < CARD_FALLBACK_COOLDOWN_MS) {
      return;
    }
    lastFallbackCardId = state.cardId;
    lastFallbackCardMs = now;

    if (depositPhase == DepositPhase::InProgress &&
        state.cardId == activeCardId) {
      finishDepositSession();
    } else {
      beginDepositSession();
    }
    return;
  }

  if (command.equalsIgnoreCase("MEASURE") && first > 0) {
    const int second = line.indexOf('|', first + 1);
    const int third = second >= 0 ? line.indexOf('|', second + 1) : -1;
    if (second > first && third > second) {
      const String category = line.substring(first + 1, second);
      state.category = category.equalsIgnoreCase("plastic") ? "塑料瓶" : "纸类";
      state.weightKg =
          line.substring(second + 1, third).toFloat();
      state.bottles = line.substring(third + 1).toInt();
      updateTransactionPoints();
      state.statusTitle = "谢谢您做出的贡献";
      state.statusMain =
          String("投放完成，（") + state.transactionPoints +
          "）积分已到账";
      depositPhase = DepositPhase::Completed;
      setStepIndicator(2);
      updateDashboard();
    }
    return;
  }

  if (command.equalsIgnoreCase("SENSOR") && first > 0) {
    const int second = line.indexOf('|', first + 1);
    if (second > first) {
      const String weightText = line.substring(first + 1, second);
      if (!weightText.equalsIgnoreCase("--")) {
        absoluteWeightKg = weightText.toFloat();
      }
      if (second + 1 < line.length()) {
        absoluteBottleCount = line.substring(second + 1).toInt();
      }
      if (depositPhase != DepositPhase::InProgress) {
        if (!waitingBaselineValid) {
          waitingBaselineKg = absoluteWeightKg;
          waitingBaselineValid = true;
        } else if (absoluteWeightKg < waitingBaselineKg - 0.02f) {
          waitingBaselineKg = absoluteWeightKg;
        }
        return;
      }

      state.weightKg = absoluteWeightKg - sessionWeightBaselineKg;
      if (state.weightKg < 0.0f) {
        state.weightKg = 0.0f;
      }
      state.bottles =
          absoluteBottleCount - sessionBottleBaseline;
      if (state.bottles < 0) {
        state.bottles = 0;
      }
      updateTransactionPoints();
      static uint32_t lastSensorUiMs = 0;
      const uint32_t now = millis();
      if (lastSensorUiMs == 0 ||
          now - lastSensorUiMs >= 100) {
        updateSensorDashboard();
        lastSensorUiMs = now;
      }
    }
    return;
  }

  if (command.equalsIgnoreCase("STUDENT_INFO") && first > 0) {
    const int second = line.indexOf('|', first + 1);
    const int third = second > first ? line.indexOf('|', second + 1) : -1;
    if (second > first && third > second) {
      state.cardId =
          formatCardId(line.substring(first + 1, second));
      const int totalPoints =
          line.substring(second + 1, third).toInt();
      String studentName = line.substring(third + 1);
      studentName.trim();
      if (studentName.length() > 0) {
        state.studentName = studentName;
      }
      if (totalPoints >= 0) {
        state.totalPoints = totalPoints;
        lv_label_set_text_fmt(pointsLabel, "%d", state.totalPoints);
      }
      updateSensorDashboard();
      lv_refr_now(lv_display_get_default());
    }
    return;
  }

  if (command.equalsIgnoreCase("SETTLE_RESULT") && first > 0) {
    const int second = line.indexOf('|', first + 1);
    const int third = second > first ? line.indexOf('|', second + 1) : -1;
    if (second > first && third > second) {
      state.cardId =
          formatCardId(line.substring(first + 1, second));
      const int totalPoints =
          line.substring(second + 1, third).toInt();
      const bool confirmed = line.substring(third + 1).toInt() == 1;
      if (confirmed && totalPoints >= 0) {
        state.totalPoints = totalPoints;
        lv_label_set_text_fmt(pointsLabel, "%d", state.totalPoints);
        state.statusTitle = "谢谢您做出的贡献";
        state.statusMain =
            String("投放完成，（") + state.transactionPoints +
            "）积分已到账";
        depositPhase = DepositPhase::Completed;
        setStepIndicator(2);
        startReturnCountdown();
      } else {
        state.statusTitle = "结算上传失败";
        state.statusMain = "本次积分未确认，请检查网络";
        depositPhase = DepositPhase::Completed;
        setStepIndicator(2);
        startReturnCountdown();
      }
      updateSensorDashboard();
      lv_refr_now(lv_display_get_default());
    }
    return;
  }

  if (command.equalsIgnoreCase("STUDENT_POINTS") && first > 0) {
    const int second = line.indexOf('|', first + 1);
    if (second > first) {
      state.cardId =
          formatCardId(line.substring(first + 1, second));
      const int totalPoints =
          line.substring(second + 1).toInt();
      if (totalPoints >= 0) {
        state.totalPoints = totalPoints;
        lv_label_set_text_fmt(pointsLabel, "%d", state.totalPoints);
      }
    }
    return;
  }

  if (command.equalsIgnoreCase("POINTS") && first > 0) {
    state.totalPoints = line.substring(first + 1).toInt();
    updateDashboard();
    return;
  }
  printHelp();
}

void pollSerial() {
  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      handleCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 96) {
      serialLine += character;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  smartdisplay_init();
  lv_display_set_rotation(lv_display_get_default(),
                          LV_DISPLAY_ROTATION_180);
  smartdisplay_lcd_set_backlight(0.78f);
  createDashboard();
  createWaitingScreen();
  loadResetState();

  Serial.println();
  Serial.println("Campus Recycling reference dashboard ready");
  printHelp();
}

void loop() {
  static uint32_t lastTick = millis();
  const uint32_t now = millis();

  lv_tick_inc(now - lastTick);
  lastTick = now;

  if (returnCountdownActive) {
    const uint32_t elapsed = now - returnCountdownStartedMs;
    if (elapsed >= 5000) {
      loadResetState();
    } else {
      const int remaining =
          5 - static_cast<int>(elapsed / 1000);
      if (remaining != lastCountdownSeconds) {
        lastCountdownSeconds = remaining;
        lv_label_set_text_fmt(countdownLabel, "%d 秒", remaining);
      }
    }
  }

  pollSerial();
  const uint32_t afterSerialMs = millis();
  if (depositPhase == DepositPhase::InProgress) {
    const uint32_t depositElapsedMs =
        afterSerialMs - depositStartedMs;
    if (depositElapsedMs >= DEPOSIT_AUTO_SETTLE_TIMEOUT_MS) {
      Serial.println("DEPOSIT AUTO-SETTLE: 60 second timeout");
      finishDepositSession();
    } else {
      const int remainingSeconds = static_cast<int>(
          (DEPOSIT_AUTO_SETTLE_TIMEOUT_MS - depositElapsedMs + 999UL) /
          1000UL);
      if (remainingSeconds != lastCountdownSeconds) {
        showCountdownSeconds(remainingSeconds);
      }
    }
  } else if (depositPhase == DepositPhase::Uploading &&
             afterSerialMs - settlementStartedMs >=
                 SETTLEMENT_CONFIRM_TIMEOUT_MS) {
    state.statusTitle = "服务器未确认";
    state.statusMain = "本次积分未到账，请刷卡";
    depositPhase = DepositPhase::Completed;
    startReturnCountdown();
    updateSensorDashboard();
  }
  lv_timer_handler();
  delay(5);
}
