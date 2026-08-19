#include <atlib/papertrading/AppendableIntervals.h>

#include <gtest/gtest.h>

// get_values_for_key_range() expands a sparse list of changes back into one value
// per key, and every test below is stated as the whole expanded vector rather than
// as a spot check of a few positions. That is deliberate: the two defects this
// file was written for -- a segment that discarded the segments before it, and a
// segment that ran past the end of the requested range -- both produced a vector
// of the wrong *length*, which an assertion about v[i] at a handful of chosen i
// can miss entirely.

namespace
{

using Intervals = AppendableIntervals<size_t, double>;

// A symbol first held partway through a backtest: nothing is recorded for the days
// before the first change, and the value for those days comes from before_begin.
// The days that follow must still be there after it -- which is the case that the
// callers in PortfolioHistory hit on any position not opened on day one.
TEST(AppendableIntervals, LeadingRangeBeforeTheFirstChangeIsKept)
{
    Intervals a(0.0);
    for (size_t i = 5; i < 10; ++i) {
        a.insert_at_end(i, 7.0);
    }
    EXPECT_EQ(a.get_values_for_key_range(0, 10), (vector<double>{0, 0, 0, 0, 0, 7, 7, 7, 7, 7}));
}

// The last change before `end` extends to `end` and no further. The change at 100
// is outside the range asked for and must not decide how many values come back,
// which is the one thing the interval's own key length says nothing about.
TEST(AppendableIntervals, RangeEndingBeforeTheNextChangeIsNotOverrun)
{
    Intervals a(0.0);
    a.insert_at_end(0, 1.0);
    a.insert_at_end(100, 2.0);
    EXPECT_EQ(a.get_values_for_key_range(0, 10), (vector<double>{1, 1, 1, 1, 1, 1, 1, 1, 1, 1}));
}

// Several changes inside one range, which is what makes the segments accumulate
// rather than replace each other, plus the sub-ranges that start and end at every
// interesting position relative to them: inside a segment, on a boundary, before
// the first change and after the last.
TEST(AppendableIntervals, ChangesInsideTheRangeEachBecomeASegment)
{
    Intervals a(-1.0);
    a.insert_at_end(2, 1.0);
    a.insert_at_end(4, 2.0);
    a.insert_at_end(7, 3.0);

    EXPECT_EQ(a.get_values_for_key_range(0, 9), (vector<double>{-1, -1, 1, 1, 2, 2, 2, 3, 3}));
    EXPECT_EQ(a.get_values_for_key_range(3, 6), (vector<double>{1, 2, 2}));
    EXPECT_EQ(a.get_values_for_key_range(4, 7), (vector<double>{2, 2, 2}));
    EXPECT_EQ(a.get_values_for_key_range(0, 2), (vector<double>{-1, -1}));
    EXPECT_EQ(a.get_values_for_key_range(8, 10), (vector<double>{3, 3}));
    EXPECT_EQ(a.get_values_for_key_range(4, 4), (vector<double>{}));
}

// A value equal to the one already in force records nothing, so an interval that
// never departs from before_begin holds no changes at all. That empty state is a
// legitimate one to read from -- a portfolio that starts and stays fully invested
// leaves its cash there -- and answering it is not the same as having nothing to
// say.
TEST(AppendableIntervals, IntervalThatNeverChangesReadsAsBeforeBegin)
{
    Intervals a(5.0);
    a.insert_at_end(0, 5.0);
    a.insert_at_end(1, 5.0);
    EXPECT_EQ(a.get_values_for_key_range(0, 3), (vector<double>{5, 5, 5}));
}

// The append-only precondition and the direction of the range are the caller's to
// get right, and both are CHECKed rather than reported. Pinned because a key that
// goes backwards would otherwise be recorded and read back as an interval whose
// segments overlap.
TEST(AppendableIntervalsDeathTest, KeyGoingBackwardsIsAnError)
{
    Intervals a(0.0);
    a.insert_at_end(5, 1.0);
    EXPECT_DEATH(a.insert_at_end(4, 2.0), "");
    EXPECT_DEATH((void)a.get_values_for_key_range(6, 3), "");
}

constexpr double k_before_begin = -99.0;
void test(const vector<pair<int, double>>& inserts, const vector<pair<int, double>>& expected)
{
    AppendableIntervals<int, double> a(k_before_begin);
    for (auto [k, v] : inserts) {
        a.insert_at_end(k, v);
    }
    for (auto it = a.begin(), jt = expected.begin();; ++it, ++jt) {
        if (it == a.end()) {
            if (jt == expected.end()) {
                break;
            } else {
                FAIL();
            }
        } else {
            if (jt == expected.end()) {
                FAIL();
            } else {
                EXPECT_EQ(it->first, jt->first);
                EXPECT_EQ(it->second, jt->second);
            }
        }
    }
}

TEST(AppendableIntervals, insert_at_end)
{
    test({}, {});
    test(
      {
        {1, k_before_begin}
    },
      {}
    );
    test(
      {
        {1, 1.0}
    },
      {{1, 1.0}}
    );

    test(
      {
        {1, k_before_begin},
        {2, 2.0           }
    },
      {{2, 2.0}}
    );
    test(
      {
        {1, k_before_begin},
        {2, k_before_begin}
    },
      {}
    );
    test(
      {
        {1, 1.0},
        {2, 2.0}
    },
      {{1, 1.0}, {2, 2.0}}
    );
    test(
      {
        {1, 1.0},
        {2, 1.0}
    },
      {{1, 1.0}}
    );
    test(
      {
        {1, 1.0           },
        {2, k_before_begin}
    },
      {{1, 1.0}, {2, k_before_begin}}
    );
}
void test3(const AppendableIntervals<int, double>& a, const vector<pair<int, double>>& inserts, int b, int e)
{
    vector<double> expected(sucast(e - b), k_before_begin);
    for (auto [k, v] : inserts) {
        std::fill(expected.begin() + std::clamp(k - b, 0, iicast<int>(expected.size())), expected.end(), v);
    }
    EXPECT_EQ(a.get_values_for_key_range(b, e), expected);
}
void test2(const vector<pair<int, double>>& inserts)
{
    AppendableIntervals<int, double> a(k_before_begin);
    for (auto [k, v] : inserts) {
        a.insert_at_end(k, v);
    }
    for (int b = 0; b < 6; ++b) {
        for (int e = b; e < 6; ++e) {
            test3(a, inserts, b, e);
        }
    }
}

TEST(AppendableIntervals, get_values_for_key_range)
{
    test2({});
    test2({
      {2, 2.0}
    });
    test2({
      {2, 2.0},
      {3, 3.0}
    });
    test2({
      {2, 2.0},
      {4, 3.0}
    });
}
} // namespace
