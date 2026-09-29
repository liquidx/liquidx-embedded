// Unit tests for declarations, Values and the schema encoder. No host needed:
//   make -C blat/firmware/test

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "blat.h"
#include "blat/Schema.h"

namespace {

int failures = 0;

#define EXPECT(cond)                                              \
  do {                                                            \
    if (!(cond)) {                                                \
      fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                 \
    }                                                             \
  } while (0)

constexpr blat::Option kRefresh[] = {{6, "6 pages"}, {12, "12 pages"}, {0, "Never"}};

constexpr blat::Control kControls[] = {
    blat::group("display", "Display"),
    blat::choice("display.refresh", kRefresh).in("display").label("Refresh").initial(12).saveAs("refresh").live(),
    blat::toggle("display.invert").in("display").initial(true).saveAs("invert"),
    blat::number("display.level").range(-10, 10, 2).initial(0).saveAs("level"),
    blat::text("device.name").initial("Box").length(1, 8).saveAs("name"),
    blat::text("device.note").initial("hi").saveAs("note"),
    blat::secret("wifi.password").length(8, 63).write(2).saveAs("pass"),
    blat::number("power.battery").range(0, 100).readOnly(),
    blat::action("system.restart").write(2),
};
static_assert(blat::check(kControls));

// Ids are looked up at compile time.
constexpr blat::Id kRefreshId = blat::idOf(kControls, "display.refresh");
constexpr blat::Id kInvertId = blat::idOf(kControls, "display.invert");
constexpr blat::Id kLevelId = blat::idOf(kControls, "display.level");
constexpr blat::Id kNameId = blat::idOf(kControls, "device.name");
constexpr blat::Id kNoteId = blat::idOf(kControls, "device.note");
constexpr blat::Id kPasswordId = blat::idOf(kControls, "wifi.password");
constexpr blat::Id kBatteryId = blat::idOf(kControls, "power.battery");
static_assert(kRefreshId == 2 && kBatteryId == 8);

// The declaration is a constant: defaults and limits are visible at compile time.
static_assert(kControls[1].initial_ == 12 && kControls[1].is(blat::flag::kLive));
static_assert(kControls[3].min_ == -10 && kControls[3].step_ == 2);

void testDefaultsAndSaving() {
  blat::MemoryStore store;
  {
    blat::Values v;
    EXPECT(v.begin(blat::table(kControls), &store));
    EXPECT(v.get(kRefreshId) == 12);
    EXPECT(v.get(kInvertId) == 1);
    EXPECT(strcmp(v.text(kNameId), "Box") == 0);
    EXPECT(!v.isSet(kPasswordId));

    EXPECT(v.set(kRefreshId, 6) == blat::Status::Ok);
    EXPECT(v.set(kRefreshId, 7) == blat::Status::InvalidValue);  // not an option
    EXPECT(v.set(kLevelId, 3) == blat::Status::InvalidValue);    // off-step
    EXPECT(v.set(kLevelId, 12) == blat::Status::InvalidValue);   // out of range
    EXPECT(v.set(kLevelId, -4) == blat::Status::Ok);
    EXPECT(v.setText(kNameId, "Too long a name") == blat::Status::InvalidValue);
    EXPECT(v.setText(kNameId, "Lamp") == blat::Status::Ok);
    EXPECT(v.setText(kNameId, "") == blat::Status::InvalidValue);  // length(1, 8)
    EXPECT(v.setTextFromHost(kPasswordId, "short", 5) == blat::Status::InvalidValue);
    EXPECT(v.setTextFromHost(kPasswordId, "long enough", 11) == blat::Status::Ok);
    EXPECT(v.setTextFromHost(kPasswordId, "", 0) == blat::Status::Ok);  // cleared
    EXPECT(!v.isSet(kPasswordId));
    EXPECT(v.setTextFromHost(kPasswordId, "long enough", 11) == blat::Status::Ok);
    EXPECT(v.isSet(kPasswordId));
    EXPECT(strcmp(v.text(kPasswordId), "") == 0);  // secrets don't read back
    EXPECT(v.set(kBatteryId, 80) == blat::Status::Ok);  // read-only to hosts, not firmware
  }
  {
    // A restart: saved values come back, unsaved ones start at their default.
    blat::Values v;
    EXPECT(v.begin(blat::table(kControls), &store));
    EXPECT(v.get(kRefreshId) == 6);
    EXPECT(v.get(kLevelId) == -4);
    EXPECT(strcmp(v.text(kNameId), "Lamp") == 0);
    EXPECT(v.isSet(kPasswordId));
    EXPECT(v.get(kBatteryId) == 0);

    // Clearing a text keeps it empty across a restart, rather than
    // bringing back its default.
    EXPECT(v.setText(kNoteId, "") == blat::Status::Ok);
    v.reset(kRefreshId);
    EXPECT(v.get(kRefreshId) == 12);
  }
  {
    blat::Values v;
    EXPECT(v.begin(blat::table(kControls), &store));
    EXPECT(strcmp(v.text(kNoteId), "") == 0);
    EXPECT(strcmp(v.text(kNameId), "Lamp") == 0);
    EXPECT(v.get(kRefreshId) == 12);
  }
}

void testSavedValueNoLongerValid() {
  // A firmware update removed an option: the saved value falls back to the default.
  blat::MemoryStore store;
  store.putInt("refresh", 99);
  store.putInt("level", 5);  // off-step
  blat::Values v;
  EXPECT(v.begin(blat::table(kControls), &store));
  EXPECT(v.get(kRefreshId) == 12);
  EXPECT(v.get(kLevelId) == 0);
}

void testChangeTracking() {
  blat::Values v;
  EXPECT(v.begin(blat::table(kControls), nullptr));
  EXPECT(!v.takeHostChanges());
  EXPECT(v.takeDeviceChange() == 0);

  v.set(kRefreshId, 6);  // from the firmware: news for hosts
  EXPECT(v.takeDeviceChange() == kRefreshId);
  EXPECT(v.takeDeviceChange() == 0);
  EXPECT(!v.takeHostChanges());

  v.setFromHost(kInvertId, 0);  // from a host: news for the firmware
  EXPECT(v.takeHostChanges());
  EXPECT(!v.takeHostChanges());
  EXPECT(v.takeDeviceChange() == 0);

  v.set(kRefreshId, 6);  // unchanged: nothing to report
  EXPECT(v.takeDeviceChange() == 0);
}

void testSchema() {
  const blat::Table t = blat::table(kControls);
  const size_t size = blat::encodeSchema(t, nullptr, 0);
  EXPECT(size > 0);
  uint8_t buf[1024];
  EXPECT(blat::encodeSchema(t, buf, sizeof(buf)) == size);
  EXPECT(blat::encodeSchema(t, buf, size - 1) == 0);  // too small
  EXPECT(buf[0] == blat::kVersion);
  EXPECT(blat::get16(buf + 1) == t.count);

  // Walk the records: ids in order, parents resolved, keys first.
  size_t at = 3;
  for (blat::Id id = 1; id <= t.count; id++) {
    const uint16_t recordLength = blat::get16(buf + at);
    const uint8_t* r = buf + at + 2;
    EXPECT(blat::get16(r) == id);
    EXPECT(r[2] == static_cast<uint8_t>(t[id].type_));
    EXPECT(blat::get16(r + 4) == blat::parentOf(t, id));
    EXPECT(r[8] == blat::attr::kKey);
    EXPECT(r[9] == strlen(t[id].key_) && memcmp(r + 10, t[id].key_, r[9]) == 0);
    at += 2 + recordLength;
  }
  EXPECT(at == size);
  EXPECT(blat::parentOf(t, kRefreshId) == 1);
  // Read-only controls say write level 0; others their declared level.
  const auto access = [&](blat::Id id) {
    size_t p = 3;
    for (blat::Id i = 1; i < id; i++) p += 2 + blat::get16(buf + p);
    return buf[p + 2 + 3];
  };
  EXPECT(access(kBatteryId) == 0x00);
  EXPECT(access(kPasswordId) == 0x20);
  EXPECT(access(kRefreshId) == 0x10);
}

}  // namespace

int main() {
  testDefaultsAndSaving();
  testSavedValueNoLongerValid();
  testChangeTracking();
  testSchema();
  if (failures > 0) {
    fprintf(stderr, "%d failed\n", failures);
    return 1;
  }
  printf("test_values: ok\n");
  return 0;
}
