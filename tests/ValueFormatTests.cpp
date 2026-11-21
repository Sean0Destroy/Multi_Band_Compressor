#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ParameterSchema.h"
#include "ui/ValueFormat.h"

using namespace tl;
using namespace tl::ui;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE ("Value text: units and precision", "[ui][format]")
{
    CHECK (fmt::toText (fmt::Unit::Hz, 20.0) == "20.0 Hz");
    CHECK (fmt::toText (fmt::Unit::Hz, 432.0) == "432 Hz");
    CHECK (fmt::toText (fmt::Unit::Hz, 1250.0) == "1.25 kHz");
    CHECK (fmt::toText (fmt::Unit::Hz, 12000.0) == "12.0 kHz");
    CHECK (fmt::toText (fmt::Unit::Ms, 0.05) == "0.05 ms");
    CHECK (fmt::toText (fmt::Unit::Ms, 10.0) == "10.0 ms");
    CHECK (fmt::toText (fmt::Unit::Ms, 150.0) == "150 ms");
    CHECK (fmt::toText (fmt::Unit::Ms, 1500.0) == "1.50 s");
    CHECK (fmt::toText (fmt::Unit::Db, -18.0) == "-18.0 dB");
    CHECK (fmt::toText (fmt::Unit::Db, -0.01) == "0.0 dB");     // no "-0.0"
    CHECK (fmt::toText (fmt::Unit::Ratio, 2.0) == "2.00:1");
    CHECK (fmt::toText (fmt::Unit::Ratio, 12.5) == "12.5:1");
    CHECK (fmt::toText (fmt::Unit::Ratio, 100.0) == "inf:1");
    CHECK (fmt::toText (fmt::Unit::Percent, 100.0) == "100 %");
    CHECK (fmt::toText (fmt::Unit::Fraction, 0.5) == "50 %");

    // Values that are not round numbers are rounded to the shown precision (no "430.772 Hz").
    CHECK (fmt::toText (fmt::Unit::Hz, 430.772) == "431 Hz");
    CHECK (fmt::toText (fmt::Unit::Hz, 99.96) == "100 Hz");
    CHECK (fmt::toText (fmt::Unit::Hz, 9996.0) == "10.0 kHz");
    CHECK (fmt::toText (fmt::Unit::Ms, 158.114) == "158 ms");
    CHECK (fmt::toText (fmt::Unit::Ms, 99.96) == "100 ms");
    CHECK (fmt::toText (fmt::Unit::Percent, 37.2549) == "37 %");
    CHECK (fmt::toText (fmt::Unit::Fraction, 0.372549) == "37 %");
    CHECK (fmt::toText (fmt::Unit::Db, -17.96) == "-18.0 dB");
    CHECK (fmt::toText (fmt::Unit::Ratio, 3.14159) == "3.14:1");
}

TEST_CASE ("Value text: hand-typed forms parse", "[ui][format]")
{
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, "1.2k", 0.0), WithinRel (1200.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, "1,2 kHz", 0.0), WithinRel (1200.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, " 440 hz ", 0.0), WithinRel (440.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, "440", 0.0), WithinRel (440.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Ms, "2 s", 0.0), WithinRel (2000.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Ms, "2 ms", 0.0), WithinRel (2.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Ms, "35", 0.0), WithinRel (35.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Db, "-12.5 dB", 0.0), WithinRel (-12.5, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Db, "+3", 0.0), WithinRel (3.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Ratio, "4:1", 0.0), WithinRel (4.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Ratio, "INF", 0.0), WithinRel (100.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Ratio, "inf:1", 0.0), WithinRel (100.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Percent, "75 %", 0.0), WithinRel (75.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Fraction, "25%", 0.0), WithinRel (0.25, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, "1e3", 0.0), WithinRel (1000.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, "2.5E2 Hz", 0.0), WithinRel (250.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Ms, "5e-1", 0.0), WithinRel (0.5, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, "1 000", 0.0), WithinRel (1000.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, "12 500 Hz", 0.0), WithinRel (12500.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, "3 k", 0.0), WithinRel (3000.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Db, "-6e", 0.0), WithinRel (-6.0, 1e-9));      // "e" without digits is not an exponent

    // Unreadable text keeps the current value.
    CHECK_THAT (fmt::fromText (fmt::Unit::Hz, "", 123.0), WithinRel (123.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Db, "abc", -6.0), WithinRel (-6.0, 1e-9));
    CHECK_THAT (fmt::fromText (fmt::Unit::Ms, "-", 7.0), WithinRel (7.0, 1e-9));
}

TEST_CASE ("Value text: text -> value -> text is idempotent over every float parameter range", "[ui][format]")
{
    for (auto& spec : schema::allParams())
    {
        if (spec.kind != schema::Kind::Float) continue;
        const auto unit = fmt::unitFor (spec);
        INFO (spec.id);
        CHECK (unit != fmt::Unit::None);   // every float parameter has a display unit

        const auto range = schema::makeRange (spec);
        for (int i = 0; i <= 200; ++i)
        {
            const double v = (double) range.convertFrom0to1 ((float) i / 200.0f);
            const auto text = fmt::toText (unit, v);
            const double back = fmt::fromText (unit, text, 1.0e9);
            INFO (text);
            CHECK (fmt::toText (unit, back) == text);
            // Display precision: the parsed value is within 0.6 % or half a display step
            // (whole percent for the percentage units, 0.1 or finer elsewhere).
            const double halfStep = unit == fmt::Unit::Percent ? 0.51 : unit == fmt::Unit::Fraction ? 0.0051 : 0.051;
            CHECK_THAT (back, WithinRel (v, 0.006) || WithinAbs (v, halfStep));

            // What is shown never carries more digits than the format promises.
            CHECK (text.length() <= 10);
        }
    }
}
