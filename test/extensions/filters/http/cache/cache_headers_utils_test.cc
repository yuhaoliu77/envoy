#include <chrono>
#include <string>
#include <vector>

#include "envoy/common/time.h"

#include "source/common/common/macros.h"
#include "source/common/common/utility.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/header_utility.h"
#include "source/extensions/filters/http/cache/cache_headers_utils.h"

#include "test/extensions/filters/http/cache/common.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace {

Protobuf::RepeatedPtrField<::envoy::type::matcher::v3::StringMatcher>
toStringMatchers(std::initializer_list<absl::string_view> allow_list) {
  Protobuf::RepeatedPtrField<::envoy::type::matcher::v3::StringMatcher> proto_allow_list;
  for (const auto& rule : allow_list) {
    ::envoy::type::matcher::v3::StringMatcher* matcher = proto_allow_list.Add();
    matcher->set_exact(std::string(rule));
  }

  return proto_allow_list;
}

struct TestRequestCacheControl : public RequestCacheControl {
  TestRequestCacheControl(bool must_validate, bool no_store, bool no_transform, bool only_if_cached,
                          OptionalDuration max_age, OptionalDuration min_fresh,
                          OptionalDuration max_stale) {
    must_validate_ = must_validate;
    no_store_ = no_store;
    no_transform_ = no_transform;
    only_if_cached_ = only_if_cached;
    max_age_ = max_age;
    min_fresh_ = min_fresh;
    max_stale_ = max_stale;
  }
};

struct RequestCacheControlTestCase {
  absl::string_view cache_control_header;
  TestRequestCacheControl request_cache_control;
};

class RequestCacheControlTest : public testing::TestWithParam<RequestCacheControlTestCase> {
public:
  static const std::vector<RequestCacheControlTestCase>& getTestCases() {
    // clang-format off
    CONSTRUCT_ON_FIRST_USE(std::vector<RequestCacheControlTestCase>,
        // Empty header
        {
          "",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, false, false, false, absl::nullopt, absl::nullopt, absl::nullopt}
        },
        // Valid cache-control headers
        {
          "max-age=3600, min-fresh=10, no-transform, only-if-cached, no-store",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, true, true, true, Seconds(3600), Seconds(10), absl::nullopt}
        },
        {
          "min-fresh=100, max-stale, no-cache",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {true, false, false, false, absl::nullopt, Seconds(100), SystemTime::duration::max()}
        },
        {
          "max-age=10, max-stale=50",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, false, false, false, Seconds(10), absl::nullopt, Seconds(50)}
        },
        // Quoted arguments are interpreted correctly
        {
          "max-age=\"3600\", min-fresh=\"10\", no-transform, only-if-cached, no-store",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, true, true, true, Seconds(3600), Seconds(10), absl::nullopt}
        },
        {
          "max-age=\"10\", max-stale=\"50\", only-if-cached",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, false, false, true, Seconds(10), absl::nullopt, Seconds(50)}
        },
        // Unknown directives are ignored
        {
          "max-age=10, max-stale=50, unknown-directive",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, false, false, false, Seconds(10), absl::nullopt, Seconds(50)}
        },
        {
          "max-age=10, max-stale=50, unknown-directive-with-arg=arg1",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, false, false, false, Seconds(10), absl::nullopt, Seconds(50)}
        },
        {
          "max-age=10, max-stale=50, unknown-directive-with-quoted-arg=\"arg1\"",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, false, false, false, Seconds(10), absl::nullopt, Seconds(50)}
        },
        {
          "max-age=10, max-stale=50, unknown-directive, unknown-directive-with-quoted-arg=\"arg1\"",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, false, false, false, Seconds(10), absl::nullopt, Seconds(50)}
        },
        // Invalid durations are ignored
        {
          "max-age=five, min-fresh=30, no-store",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, true, false, false, absl::nullopt, Seconds(30), absl::nullopt}
        },
        {
          "max-age=five, min-fresh=30s, max-stale=-2",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, false, false, false, absl::nullopt, absl::nullopt, absl::nullopt}
        },
        {
          "max-age=\"",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {false, false, false, false, absl::nullopt, absl::nullopt, absl::nullopt}
        },
        // Invalid parts of the header are ignored
        {
          "no-cache, ,,,fjfwioen3298, max-age=20, min-fresh=30=40",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {true, false, false, false, Seconds(20), absl::nullopt, absl::nullopt}
        },
        // If a directive argument contains a comma by mistake
        // the part before the comma will be interpreted as the argument
        // and the part after it will be ignored
        {
          "no-cache, max-age=10,0, no-store",
          // {must_validate_, no_store_, no_transform_, only_if_cached_, max_age_, min_fresh_, max_stale_}
          {true, true, false, false, Seconds(10), absl::nullopt, absl::nullopt}
        },
    );
    // clang-format on
  }
};

INSTANTIATE_TEST_SUITE_P(RequestCacheControlTest, RequestCacheControlTest,
                         testing::ValuesIn(RequestCacheControlTest::getTestCases()));

TEST_P(RequestCacheControlTest, RequestCacheControlTest) {
  const absl::string_view cache_control_header = GetParam().cache_control_header;
  const RequestCacheControl expected_request_cache_control = GetParam().request_cache_control;
  EXPECT_EQ(expected_request_cache_control, RequestCacheControl(cache_control_header));
}

struct TestResponseCacheControl : public ResponseCacheControl {
  TestResponseCacheControl(bool must_validate, bool no_store, bool no_transform, bool no_stale,
                           bool is_public, OptionalDuration max_age) {
    must_validate_ = must_validate;
    no_store_ = no_store;
    no_transform_ = no_transform;
    no_stale_ = no_stale;
    is_public_ = is_public;
    max_age_ = max_age;
  }
};

struct ResponseCacheControlTestCase {
  absl::string_view cache_control_header;
  TestResponseCacheControl response_cache_control;
};

class ResponseCacheControlTest : public testing::TestWithParam<ResponseCacheControlTestCase> {
public:
  static const std::vector<ResponseCacheControlTestCase>& getTestCases() {
    // clang-format off
    CONSTRUCT_ON_FIRST_USE(std::vector<ResponseCacheControlTestCase>,
        // Empty header
        {
          "",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, false, false, false, false, absl::nullopt}
        },
        // Valid cache-control headers
        {
          "s-maxage=1000, max-age=2000, proxy-revalidate, no-store",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, true, false, true, false, Seconds(1000)}
        },
        {
          "max-age=500, must-revalidate, no-cache, no-transform",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {true, false, true, true, false, Seconds(500)}
        },
        {
          "s-maxage=10, private=content-length, no-cache=content-encoding",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {true, true, false, false, false, Seconds(10)}
        },
        {
          "private",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, true, false, false, false, absl::nullopt}
        },
        {
          "public, max-age=0",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, false, false, false, true, Seconds(0)}
        },
        // Quoted arguments are interpreted correctly
        {
          "s-maxage=\"20\", max-age=\"10\", public",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, false, false, false, true, Seconds(20)}
        },
        {
          "max-age=\"50\", private",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, true, false, false, false, Seconds(50)}
        },
        {
          "s-maxage=\"0\"",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, false, false, false, false, Seconds(0)}
        },
        // Unknown directives are ignored
        {
          "private, no-cache, max-age=30, unknown-directive",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {true, true, false, false, false, Seconds(30)}
        },
        {
          "private, no-cache, max-age=30, unknown-directive-with-arg=arg",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {true, true, false, false, false, Seconds(30)}
        },
        {
          "private, no-cache, max-age=30, unknown-directive-with-quoted-arg=\"arg\"",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {true, true, false, false, false, Seconds(30)}
        },
        {
          "private, no-cache, max-age=30, unknown-directive, unknown-directive-with-quoted-arg=\"arg\"",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {true, true, false, false, false, Seconds(30)}
        },
        // Invalid durations are ignored
        {
          "max-age=five",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, false, false, false, false, absl::nullopt}
        },
        {
          "max-age=10s, private",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, true, false, false, false, absl::nullopt}
        },
        {
          "s-maxage=\"50s\", max-age=\"zero\", no-cache",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {true, false, false, false, false, absl::nullopt}
        },
        {
          "s-maxage=five, max-age=10, no-transform",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, false, true, false, false, Seconds(10)}
        },
        {
          "max-age=\"",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {false, false, false, false, false, absl::nullopt}
        },
        // Invalid parts of the header are ignored
        {
          "no-cache, ,,,fjfwioen3298, max-age=20",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {true, false, false, false, false, Seconds(20)}
        },
        // If a directive argument contains a comma by mistake
        // the part before the comma will be interpreted as the argument
        // and the part after it will be ignored
        {
          "no-cache, max-age=10,0, no-store",
          // {must_validate_, no_store_, no_transform_, no_stale_, is_public_, max_age_}
          {true, true, false, false, false, Seconds(10)}
        },
    );
    // clang-format on
  }
};

INSTANTIATE_TEST_SUITE_P(ResponseCacheControlTest, ResponseCacheControlTest,
                         testing::ValuesIn(ResponseCacheControlTest::getTestCases()));

TEST_P(ResponseCacheControlTest, ResponseCacheControlTest) {
  const absl::string_view cache_control_header = GetParam().cache_control_header;
  const ResponseCacheControl expected_response_cache_control = GetParam().response_cache_control;
  EXPECT_EQ(expected_response_cache_control, ResponseCacheControl(cache_control_header));
}

class HttpTimeTest : public testing::TestWithParam<std::string> {
public:
  static const std::vector<std::string>& getOkTestCases() {
    // clang-format off
    CONSTRUCT_ON_FIRST_USE(std::vector<std::string>,
        "Sun, 06 Nov 1994 08:49:37 GMT",  // IMF-fixdate.
        "Sunday, 06-Nov-94 08:49:37 GMT", // obsolete RFC 850 format.
        "Sun Nov  6 08:49:37 1994"        // ANSI C's asctime() format.
    );
    // clang-format on
  }
};

INSTANTIATE_TEST_SUITE_P(Ok, HttpTimeTest, testing::ValuesIn(HttpTimeTest::getOkTestCases()));

TEST_P(HttpTimeTest, OkFormats) {
  const Http::TestResponseHeaderMapImpl response_headers{{"date", GetParam()}};
  // Manually confirmed that 784111777 is 11/6/94, 8:46:37.
  EXPECT_EQ(784111777,
            SystemTime::clock::to_time_t(CacheHeadersUtils::httpTime(response_headers.Date())));
}

TEST(HttpTime, InvalidFormat) {
  const std::string invalid_format_date = "Sunday, 06-11-1994 08:49:37";
  const Http::TestResponseHeaderMapImpl response_headers{{"date", invalid_format_date}};
  EXPECT_EQ(CacheHeadersUtils::httpTime(response_headers.Date()), SystemTime());
}

TEST(HttpTime, Null) { EXPECT_EQ(CacheHeadersUtils::httpTime(nullptr), SystemTime()); }

struct CalculateAgeTestCase {
  std::string test_name;
  Http::TestResponseHeaderMapImpl response_headers;
  SystemTime response_time, now;
  Seconds expected_age;
};

class CalculateAgeTest : public testing::TestWithParam<CalculateAgeTestCase> {
public:
  static std::string durationToString(const SystemTime::duration& duration) {
    return std::to_string(duration.count());
  }
  static std::string formatTime(const SystemTime& time) { return formatter().fromTime(time); }
  static const DateFormatter& formatter() {
    CONSTRUCT_ON_FIRST_USE(DateFormatter, {"%a, %d %b %Y %H:%M:%S GMT"});
  }
  static const SystemTime& currentTime() {
    CONSTRUCT_ON_FIRST_USE(SystemTime, Event::SimulatedTimeSystem().systemTime());
  }
  static const std::vector<CalculateAgeTestCase>& getTestCases() {
    // clang-format off
    CONSTRUCT_ON_FIRST_USE(std::vector<CalculateAgeTestCase>,
        {
          "no_initial_age_all_times_equal",
          /*response_headers=*/{{"date", formatTime(currentTime())}},
          /*response_time=*/currentTime(),
          /*now=*/currentTime(),
          /*expected_age=*/Seconds(0)
        },
        {
          "initial_age_zero_all_times_equal",
          /*response_headers=*/{{"date", formatTime(currentTime())}, {"age", "0"}},
          /*response_time=*/currentTime(),
          /*now=*/currentTime(),
          /*expected_age=*/Seconds(0)
        },
        {
          "initial_age_non_zero_all_times_equal",
          /*response_headers=*/{{"date", formatTime(currentTime())}, {"age", "50"}},
          /*response_time=*/currentTime(),
          /*now=*/currentTime(),
          /*expected_age=*/Seconds(50)
        },
        {
          "date_after_response_time_no_initial_age",
          /*response_headers=*/{{"date", formatTime(currentTime() + Seconds(5))}},
          /*response_time=*/currentTime(),
          /*now=*/currentTime() + Seconds(10),
          /*expected_age=*/Seconds(10)
        },
        {
          "date_after_response_time_with_initial_age",
          /*response_headers=*/{{"date", formatTime(currentTime() + Seconds(10))}, {"age", "5"}},
          /*response_time=*/currentTime(),
          /*now=*/currentTime() + Seconds(10),
          /*expected_age=*/Seconds(15)
        },
        {
          "apparent_age_equals_initial_age",
          /*response_headers=*/{{"date", formatTime(currentTime())}, {"age", "1"}},
          /*response_time=*/currentTime() + Seconds(1),
          /*now=*/currentTime() + Seconds(5),
          /*expected_age=*/Seconds(5)
        },
        {
          "apparent_age_lower_than_initial_age",
          /*response_headers=*/{{"date", formatTime(currentTime())}, {"age", "3"}},
          /*response_time=*/currentTime() + Seconds(1),
          /*now=*/currentTime() + Seconds(5),
          /*expected_age=*/Seconds(7)
        },
        {
          "apparent_age_higher_than_initial_age",
          /*response_headers=*/{{"date", formatTime(currentTime())}, {"age", "1"}},
          /*response_time=*/currentTime() + Seconds(3),
          /*now=*/currentTime() + Seconds(5),
          /*expected_age=*/Seconds(5)
        },
    );
    // clang-format on
  }
};

INSTANTIATE_TEST_SUITE_P(CalculateAgeTest, CalculateAgeTest,
                         testing::ValuesIn(CalculateAgeTest::getTestCases()),
                         [](const auto& info) { return info.param.test_name; });

TEST_P(CalculateAgeTest, CalculateAgeTest) {
  const Seconds calculated_age = CacheHeadersUtils::calculateAge(
      GetParam().response_headers, GetParam().response_time, GetParam().now);
  const Seconds expected_age = GetParam().expected_age;
  EXPECT_EQ(calculated_age, expected_age)
      << "Expected age: " << durationToString(expected_age)
      << ", Calculated age: " << durationToString(calculated_age);
}

void testReadAndRemoveLeadingDigits(absl::string_view input, int64_t expected,
                                    absl::string_view remaining) {
  absl::string_view test_input(input);
  auto output = CacheHeadersUtils::readAndRemoveLeadingDigits(test_input);
  if (output) {
    EXPECT_EQ(output, static_cast<uint64_t>(expected)) << "input=" << input;
    EXPECT_EQ(test_input, remaining) << "input=" << input;
  } else {
    EXPECT_LT(expected, 0) << "input=" << input;
    EXPECT_EQ(test_input, remaining) << "input=" << input;
  }
}

TEST(ReadAndRemoveLeadingDigits, ComprehensiveTest) {
  testReadAndRemoveLeadingDigits("123", 123, "");
  testReadAndRemoveLeadingDigits("a123", -1, "a123");
  testReadAndRemoveLeadingDigits("9_", 9, "_");
  testReadAndRemoveLeadingDigits("11111111111xyz", 11111111111ll, "xyz");

  // Overflow case
  testReadAndRemoveLeadingDigits("1111111111111111111111111111111xyz", -1,
                                 "1111111111111111111111111111111xyz");

  // 2^64
  testReadAndRemoveLeadingDigits("18446744073709551616xyz", -1, "18446744073709551616xyz");
  // 2^64-1
  testReadAndRemoveLeadingDigits("18446744073709551615xyz", 18446744073709551615ull, "xyz");
  // (2^64-1)*10+9
  testReadAndRemoveLeadingDigits("184467440737095516159yz", -1, "184467440737095516159yz");
}

TEST(GetAllMatchingHeaderNames, EmptyRuleset) {
  Http::TestRequestHeaderMapImpl headers{{"accept", "image/*"}};
  std::vector<Matchers::StringMatcherPtr> ruleset;
  absl::flat_hash_set<absl::string_view> result;

  CacheHeadersUtils::getAllMatchingHeaderNames(headers, ruleset, result);

  EXPECT_TRUE(result.empty());
}

TEST(GetAllMatchingHeaderNames, EmptyHeaderMap) {
  Http::TestRequestHeaderMapImpl headers;
  std::vector<Matchers::StringMatcherPtr> ruleset;
  absl::flat_hash_set<absl::string_view> result;

  envoy::type::matcher::v3::StringMatcher matcher;
  matcher.set_exact("accept");
  ruleset.emplace_back(
      std::make_unique<Matchers::StringMatcherImpl<envoy::type::matcher::v3::StringMatcher>>(
          matcher));

  CacheHeadersUtils::getAllMatchingHeaderNames(headers, ruleset, result);

  EXPECT_TRUE(result.empty());
}

TEST(GetAllMatchingHeaderNames, SingleMatchSingleValue) {
  Http::TestRequestHeaderMapImpl headers{{"accept", "image/*"}, {"accept-language", "en-US"}};
  std::vector<Matchers::StringMatcherPtr> ruleset;
  absl::flat_hash_set<absl::string_view> result;

  envoy::type::matcher::v3::StringMatcher matcher;
  matcher.set_exact("accept");
  ruleset.emplace_back(
      std::make_unique<Matchers::StringMatcherImpl<envoy::type::matcher::v3::StringMatcher>>(
          matcher));

  CacheHeadersUtils::getAllMatchingHeaderNames(headers, ruleset, result);

  ASSERT_EQ(result.size(), 1);
  EXPECT_TRUE(result.contains("accept"));
}

TEST(GetAllMatchingHeaderNames, SingleMatchMultiValue) {
  Http::TestRequestHeaderMapImpl headers{{"accept", "image/*"}, {"accept", "text/html"}};
  std::vector<Matchers::StringMatcherPtr> ruleset;
  absl::flat_hash_set<absl::string_view> result;

  envoy::type::matcher::v3::StringMatcher matcher;
  matcher.set_exact("accept");
  ruleset.emplace_back(
      std::make_unique<Matchers::StringMatcherImpl<envoy::type::matcher::v3::StringMatcher>>(
          matcher));

  CacheHeadersUtils::getAllMatchingHeaderNames(headers, ruleset, result);

  ASSERT_EQ(result.size(), 1);
  EXPECT_TRUE(result.contains("accept"));
}

TEST(GetAllMatchingHeaderNames, MultipleMatches) {
  Http::TestRequestHeaderMapImpl headers{{"accept", "image/*"}, {"accept-language", "en-US"}};
  std::vector<Matchers::StringMatcherPtr> ruleset;
  absl::flat_hash_set<absl::string_view> result;

  envoy::type::matcher::v3::StringMatcher matcher;
  matcher.set_exact("accept");
  ruleset.emplace_back(
      std::make_unique<Matchers::StringMatcherImpl<envoy::type::matcher::v3::StringMatcher>>(
          matcher));
  matcher.set_exact("accept-language");
  ruleset.emplace_back(
      std::make_unique<Matchers::StringMatcherImpl<envoy::type::matcher::v3::StringMatcher>>(
          matcher));

  CacheHeadersUtils::getAllMatchingHeaderNames(headers, ruleset, result);

  ASSERT_EQ(result.size(), 2);
  EXPECT_TRUE(result.contains("accept"));
  EXPECT_TRUE(result.contains("accept-language"));
}

struct ParseCommaDelimitedHeaderTestCase {
  absl::string_view name;
  std::vector<absl::string_view> header_entries;
  std::vector<absl::string_view> expected_values;
};

std::string getParseCommaDelimitedHeaderTestName(
    const testing::TestParamInfo<ParseCommaDelimitedHeaderTestCase>& info) {
  return std::string(info.param.name);
}

std::vector<ParseCommaDelimitedHeaderTestCase> parseCommaDelimitedHeaderTestParams() {
  return {
      {
          "Null",
          {},
          {},
      },
      {
          "Empty",
          {},
          {},
      },
      {
          "SingleValue",
          {"accept"},
          {"accept"},
      },
      {
          "MultiValue",
          {"accept,accept-language"},
          {"accept", "accept-language"},
      },
      {
          "MultiValueLeadingSpace",
          {" accept,accept-language"},
          {"accept", "accept-language"},
      },
      {
          "MultiValueSpaceAfterValue",
          {"accept ,accept-language"},
          {"accept", "accept-language"},
      },
      {
          "MultiValueTrailingSpace",
          {"accept,accept-language "},
          {"accept", "accept-language"},
      },
      {
          "MultiValueLotsOfSpaces",
          {"  accept  ,  accept-language  "},
          {"accept", "accept-language"},
      },
      {
          "MultiEntry",
          {"accept", "accept-language"},
          {"accept", "accept-language"},
      },
      {
          "MultiEntryMultiValue",
          {"accept,accept-language", "foo,bar"},
          {"accept", "accept-language", "foo", "bar"},
      },
      {
          "MultiEntryMultiValueWithSpaces",
          {"accept,  accept-language  ", "foo  ,bar"},
          {"accept", "accept-language", "foo", "bar"},
      },
  };
}

class ParseCommaDelimitedHeaderTest
    : public testing::TestWithParam<ParseCommaDelimitedHeaderTestCase> {};

INSTANTIATE_TEST_SUITE_P(ParseCommaDelimitedHeaderTest, ParseCommaDelimitedHeaderTest,
                         testing::ValuesIn(parseCommaDelimitedHeaderTestParams()),
                         getParseCommaDelimitedHeaderTestName);

TEST_P(ParseCommaDelimitedHeaderTest, ParseCommaDelimitedHeader) {
  ParseCommaDelimitedHeaderTestCase test_case = GetParam();
  const Http::LowerCaseString header_name = Http::CustomHeaders::get().Vary;
  Http::TestResponseHeaderMapImpl headers;
  for (absl::string_view entry : test_case.header_entries) {
    headers.addCopy(header_name, entry);
  }
  std::vector<absl::string_view> result =
      CacheHeadersUtils::parseCommaDelimitedHeader(headers.get(header_name));
  std::vector<absl::string_view> expected(test_case.expected_values.begin(),
                                          test_case.expected_values.end());
  EXPECT_EQ(result, expected);
}

TEST(CreateVaryIdentifier, IsStableForAllowListOrder) {
  VaryHeader vary_allow_list1(toStringMatchers({"width", "accept", "accept-language"}));
  VaryHeader vary_allow_list2(toStringMatchers({"accept", "width", "accept-language"}));

  Http::TestRequestHeaderMapImpl request_headers{
      {"accept", "image/*"}, {"accept-language", "en-us"}, {"width", "640"}};

  absl::optional<std::string> vary_identifier1 = vary_allow_list1.createVaryIdentifier(
      {"accept", "accept-language", "width"}, request_headers);
  absl::optional<std::string> vary_identifier2 = vary_allow_list2.createVaryIdentifier(
      {"accept", "accept-language", "width"}, request_headers);

  ASSERT_TRUE(vary_identifier1.has_value());
  ASSERT_TRUE(vary_identifier2.has_value());
  EXPECT_EQ(vary_identifier1.value(), vary_identifier2.value());
}

TEST(GetVaryValues, noVary) {
  Http::TestResponseHeaderMapImpl headers;
  EXPECT_EQ(0, VaryHeader::getVaryValues(headers).size());
}

TEST(GetVaryValues, emptyVary) {
  Http::TestResponseHeaderMapImpl headers{{"vary", ""}};
  EXPECT_EQ(0, VaryHeader::getVaryValues(headers).size());
}

TEST(GetVaryValues, singleVary) {
  Http::TestResponseHeaderMapImpl headers{{"vary", "accept"}};
  absl::btree_set<absl::string_view> result_set = VaryHeader::getVaryValues(headers);
  std::vector<absl::string_view> result(result_set.begin(), result_set.end());
  std::vector<absl::string_view> expected = {"accept"};
  EXPECT_EQ(expected, result);
}

TEST(GetVaryValues, multipleVaryHeaders) {
  Http::TestResponseHeaderMapImpl headers{{"vary", "accept"}, {"vary", "origin"}};
  absl::btree_set<absl::string_view> result_set = VaryHeader::getVaryValues(headers);
  std::vector<absl::string_view> result(result_set.begin(), result_set.end());
  std::vector<absl::string_view> expected = {"accept", "origin"};
  EXPECT_EQ(expected, result);
}

TEST(HasVary, Null) {
  Http::TestResponseHeaderMapImpl headers;
  EXPECT_FALSE(VaryHeader::hasVary(headers));
}

TEST(HasVary, Empty) {
  Http::TestResponseHeaderMapImpl headers{{"vary", ""}};
  EXPECT_FALSE(VaryHeader::hasVary(headers));
}

TEST(HasVary, NotEmpty) {
  Http::TestResponseHeaderMapImpl headers{{"vary", "accept"}};
  EXPECT_TRUE(VaryHeader::hasVary(headers));
}

TEST(CreateVaryIdentifier, EmptyVaryEntry) {
  Http::TestRequestHeaderMapImpl request_headers{{"accept", "image/*"}};
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(vary_allow_list.createVaryIdentifier({}, request_headers), "vary-id\n");
}

TEST(CreateVaryIdentifier, SingleHeaderExists) {
  Http::TestRequestHeaderMapImpl request_headers{{"accept", "image/*"}};
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(vary_allow_list.createVaryIdentifier({"accept"}, request_headers), "vary-id\naccept\r"
                                                                               "image/*\n");
}

TEST(CreateVaryIdentifier, SingleHeaderMissing) {
  Http::TestRequestHeaderMapImpl request_headers;
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(vary_allow_list.createVaryIdentifier({"accept"}, request_headers),
            "vary-id\naccept\r\n");
}

TEST(CreateVaryIdentifier, MultipleHeadersAllExist) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"accept", "image/*"}, {"accept-language", "en-us"}, {"width", "640"}};
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(
      vary_allow_list.createVaryIdentifier({"accept", "accept-language", "width"}, request_headers),
      "vary-id\naccept\r"
      "image/*\naccept-language\r"
      "en-us\nwidth\r640\n");
}

TEST(CreateVaryIdentifier, MultipleHeadersSomeExist) {
  Http::TestResponseHeaderMapImpl response_headers{{"vary", "accept, accept-language, width"}};
  Http::TestRequestHeaderMapImpl request_headers{{"accept", "image/*"}, {"width", "640"}};
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(
      vary_allow_list.createVaryIdentifier({"accept", "accept-language", "width"}, request_headers),
      "vary-id\naccept\r"
      "image/*\naccept-language\r\nwidth\r640\n");
}

TEST(CreateVaryIdentifier, ExtraRequestHeaders) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"accept", "image/*"}, {"heigth", "1280"}, {"width", "640"}};
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(vary_allow_list.createVaryIdentifier({"accept", "width"}, request_headers),
            "vary-id\naccept\r"
            "image/*\nwidth\r640\n");
}

TEST(CreateVaryIdentifier, MultipleHeadersNoneExist) {
  Http::TestRequestHeaderMapImpl request_headers;
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(
      vary_allow_list.createVaryIdentifier({"accept", "accept-language", "width"}, request_headers),
      "vary-id\naccept\r\naccept-language\r\nwidth\r\n");
}

TEST(CreateVaryIdentifier, DifferentHeadersSameValue) {
  // Two requests with the same value for different headers must have different
  // vary-ids.
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  Http::TestRequestHeaderMapImpl request_headers1{{"accept", "foo"}};
  absl::optional<std::string> vary_identifier1 =
      vary_allow_list.createVaryIdentifier({"accept", "accept-language"}, request_headers1);

  Http::TestRequestHeaderMapImpl request_headers2{{"accept-language", "foo"}};
  absl::optional<std::string> vary_identifier2 = vary_allow_list.createVaryIdentifier(
      {"accept", "accept-language", "width"}, request_headers2);

  ASSERT_TRUE(vary_identifier1.has_value());
  ASSERT_TRUE(vary_identifier2.has_value());
  EXPECT_NE(vary_identifier1.value(), vary_identifier2.value());
}

TEST(CreateVaryIdentifier, MultiValueSameHeader) {
  Http::TestRequestHeaderMapImpl request_headers{{"width", "foo"}, {"width", "bar"}};
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(vary_allow_list.createVaryIdentifier({"width"}, request_headers), "vary-id\nwidth\r"
                                                                              "foo\r"
                                                                              "bar\n");
}

TEST(CreateVaryIdentifier, DisallowedHeader) {
  Http::TestRequestHeaderMapImpl request_headers{{"width", "foo"}};
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(vary_allow_list.createVaryIdentifier({"disallowed"}, request_headers), absl::nullopt);
}

TEST(CreateVaryIdentifier, DisallowedHeaderWithAllowedHeader) {
  Http::TestRequestHeaderMapImpl request_headers{{"width", "foo"}};
  VaryHeader vary_allow_list(toStringMatchers({"accept", "accept-language", "width"}));

  EXPECT_EQ(vary_allow_list.createVaryIdentifier({"disallowed,width"}, request_headers),
            absl::nullopt);
}

envoy::extensions::filters::http::cache::v3alpha::CacheConfig getConfig() {
  // Allows {accept, accept-language, width} to be varied in the tests.
  envoy::extensions::filters::http::cache::v3alpha::CacheConfig config;

  const auto& add_accept = config.mutable_allowed_vary_headers()->Add();
  add_accept->set_exact("accept");

  const auto& add_accept_language = config.mutable_allowed_vary_headers()->Add();
  add_accept_language->set_exact("accept-language");

  const auto& add_width = config.mutable_allowed_vary_headers()->Add();
  add_width->set_exact("width");

  return config;
}

class VaryHeaderTest : public testing::Test {
protected:
  VaryHeaderTest() : vary_allow_list_(getConfig().allowed_vary_headers()) {}

  VaryHeader vary_allow_list_;
  Http::TestRequestHeaderMapImpl request_headers_;
  Http::TestResponseHeaderMapImpl response_headers_;
};

TEST_F(VaryHeaderTest, AllowsHeaderAccept) { EXPECT_TRUE(vary_allow_list_.allowsHeader("accept")); }

TEST_F(VaryHeaderTest, AllowsHeaderWrongHeader) {
  EXPECT_FALSE(vary_allow_list_.allowsHeader("wrong-header"));
}

TEST_F(VaryHeaderTest, AllowsHeaderEmpty) { EXPECT_FALSE(vary_allow_list_.allowsHeader("")); }

TEST_F(VaryHeaderTest, IsAllowedNull) {
  EXPECT_TRUE(vary_allow_list_.isAllowed(response_headers_));
}

TEST_F(VaryHeaderTest, IsAllowedEmpty) {
  response_headers_.addCopy("vary", "");
  EXPECT_TRUE(vary_allow_list_.isAllowed(response_headers_));
}

TEST_F(VaryHeaderTest, IsAllowedSingle) {
  response_headers_.addCopy("vary", "accept");
  EXPECT_TRUE(vary_allow_list_.isAllowed(response_headers_));
}

TEST_F(VaryHeaderTest, IsAllowedMultiple) {
  response_headers_.addCopy("vary", "accept");
  EXPECT_TRUE(vary_allow_list_.isAllowed(response_headers_));
}

TEST_F(VaryHeaderTest, NotIsAllowedStar) {
  // Should never be allowed, regardless of the allow_list.
  response_headers_.addCopy("vary", "*");
  EXPECT_FALSE(vary_allow_list_.isAllowed(response_headers_));
}

TEST_F(VaryHeaderTest, NotIsAllowedSingle) {
  response_headers_.addCopy("vary", "wrong-header");
  EXPECT_FALSE(vary_allow_list_.isAllowed(response_headers_));
}

TEST_F(VaryHeaderTest, NotIsAllowedMixed) {
  response_headers_.addCopy("vary", "accept, wrong-header");
  EXPECT_FALSE(vary_allow_list_.isAllowed(response_headers_));
}

} // namespace
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
