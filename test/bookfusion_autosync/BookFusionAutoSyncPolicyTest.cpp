#include <gtest/gtest.h>

#include "BookFusionAutoSyncPolicy.h"

using namespace BookFusionAutoSyncPolicy;

TEST(BookFusionAutoSyncPolicy, PushesWhenNothingPushedYet) { EXPECT_TRUE(needsPush(false, 0.0f, 12.0f)); }

TEST(BookFusionAutoSyncPolicy, SkipsWhenPositionAlreadyPushed) {
  EXPECT_FALSE(needsPush(true, 42.0f, 42.0f));
  EXPECT_FALSE(needsPush(true, 42.0f, 42.01f));
}

TEST(BookFusionAutoSyncPolicy, PushesAfterReadingFurther) { EXPECT_TRUE(needsPush(true, 42.0f, 43.0f)); }

TEST(BookFusionAutoSyncPolicy, PushesWhenServerHasNoPosition) {
  EXPECT_TRUE(shouldPush(false, 0.0f, 30.0f, false, 0.0f));
}

TEST(BookFusionAutoSyncPolicy, PushesWhenServerIsBehindOrEqual) {
  EXPECT_TRUE(shouldPush(true, 20.0f, 30.0f, false, 0.0f));
  EXPECT_TRUE(shouldPush(true, 30.0f, 30.0f, false, 0.0f));
}

TEST(BookFusionAutoSyncPolicy, LayoutNoiseIsNotAhead) {
  // A few tenths of a percent is a different page layout, not reading.
  EXPECT_TRUE(shouldPush(true, 30.4f, 30.0f, false, 0.0f));
  EXPECT_FALSE(remoteAhead(30.4f, 30.0f));
}

TEST(BookFusionAutoSyncPolicy, NeverOverwritesAnotherDeviceThatIsAhead) {
  EXPECT_FALSE(shouldPush(true, 60.0f, 40.0f, false, 0.0f));
  // This device pushed 50% earlier, then the phone moved on to 60%.
  EXPECT_FALSE(shouldPush(true, 60.0f, 40.0f, true, 50.0f));
}

TEST(BookFusionAutoSyncPolicy, OverwritesItsOwnEarlierPushWhenPagingBack) {
  // The server still holds this device's 60%; the reader went back to 40%.
  EXPECT_TRUE(shouldPush(true, 60.0f, 40.0f, true, 60.0f));
}

TEST(BookFusionAutoSyncPolicy, RemoteAheadNeedsMargin) {
  EXPECT_TRUE(remoteAhead(62.0f, 40.0f));
  EXPECT_TRUE(remoteAhead(40.6f, 40.0f));
  EXPECT_FALSE(remoteAhead(40.0f, 40.0f));
  EXPECT_FALSE(remoteAhead(39.0f, 40.0f));
}

TEST(BookFusionAutoSyncPolicy, ShortNapSkipsWakeCheck) {
  EXPECT_TRUE(isShortNap(1000, 1000 + 60));
  EXPECT_TRUE(isShortNap(1000, 1000 + SHORT_NAP_SECONDS - 1));
  EXPECT_FALSE(isShortNap(1000, 1000 + SHORT_NAP_SECONDS));
  EXPECT_FALSE(isShortNap(1000, 1000 + 8 * 3600));
}

TEST(BookFusionAutoSyncPolicy, UnknownOrResetClockChecks) {
  EXPECT_FALSE(isShortNap(0, 500));             // never recorded
  EXPECT_FALSE(isShortNap(1'790'000'000, 30));  // clock reset after power loss
}
