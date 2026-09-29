// Tables that must not compile. The Makefile builds this once per case with
// -DCASE=n and checks the compiler names the expected error.

#include "blat/Controls.h"

constexpr blat::Option kOptions[] = {{1, "One"}, {2, "Two"}};

#if CASE == 1  // expect: error_initial_value_is_not_an_option
constexpr blat::Control kBad[] = {blat::choice("a.b", kOptions).initial(3)};
#elif CASE == 2  // expect: error_duplicate_key
constexpr blat::Control kBad[] = {blat::toggle("a"), blat::toggle("a")};
#elif CASE == 3  // expect: error_writable_control_needs_write_level_1_or_more
constexpr blat::Control kBad[] = {blat::toggle("a").write(0)};
#elif CASE == 4  // expect: error_save_key_too_long
constexpr blat::Control kBad[] = {blat::toggle("a").saveAs("much_too_long_key")};
#elif CASE == 5  // expect: error_parent_must_come_first_and_be_a_group_or_action
constexpr blat::Control kBad[] = {blat::toggle("a").in("g"), blat::group("g", "G")};
#elif CASE == 6  // expect: error_well_known_key_has_wrong_type
constexpr blat::Control kBad[] = {blat::text("wifi.password")};
#elif CASE == 7  // expect: error_initial_value_out_of_range
constexpr blat::Control kBad[] = {blat::number("a").range(0, 10).initial(11)};
#elif CASE == 8  // expect: error_secrets_cannot_have_a_default_or_be_live
constexpr blat::Control kBad[] = {blat::secret("a").initial("hunter22")};
#elif CASE == 9  // expect: error_no_control_with_that_key
constexpr blat::Control kBad[] = {blat::toggle("a")};
constexpr blat::Id kMissing = blat::idOf(kBad, "b");
#endif

static_assert(blat::check(kBad));
