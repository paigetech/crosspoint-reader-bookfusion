#include "BookFusionLibraryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>

#include "BookFusionBookListActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr BookFusionLibraryActivity::Category CATEGORIES[BookFusionLibraryActivity::CATEGORY_COUNT] = {
    {StrId::STR_BF_CURRENTLY_READING, "currently_reading", "last_read_at-desc"},
    {StrId::STR_BF_FAVORITES, "favorites", nullptr},
    {StrId::STR_BF_PLAN_TO_READ, "planned_to_read", nullptr},
    {StrId::STR_BF_COMPLETED, "completed", nullptr},
    {StrId::STR_BF_ALL_BOOKS, nullptr, nullptr},
};
}  // namespace

const BookFusionLibraryActivity::Category& BookFusionLibraryActivity::category(const int index) {
  return CATEGORIES[index];
}

BookFusionLibraryActivity::BookFusionLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("BookFusionLibrary", renderer, mappedInput) {
  for (int i = 0; i < CATEGORY_COUNT; i++) {
    rowItems_[i].label = I18N.get(CATEGORIES[i].nameId);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int BookFusionLibraryActivity::listCount() const { return CATEGORY_COUNT; }

const char* BookFusionLibraryActivity::headerTitle() const { return tr(STR_BF_BROWSE_LIBRARY); }

void BookFusionLibraryActivity::activateIndex(const int index) {
  app.clearTapFlash();
  startActivityForResult(std::make_unique<BookFusionBookListActivity>(renderer, mappedInput, index),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void BookFusionLibraryActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(CATEGORY_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}
