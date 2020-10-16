#include <utils/datetime/date.hpp>

#include <sstream>

#include <formats/json/serialize.hpp>
#include <formats/json/string_builder.hpp>
#include <formats/json/value_builder.hpp>

#include <gtest/gtest.h>

TEST(Date, Basics) {
  const auto date = utils::datetime::Date(2048, 1, 11);

  EXPECT_EQ(utils::datetime::DateFromRFC3339String("2048-01-11"), date);

  EXPECT_EQ("2048-01-11", ToString(date));
}

TEST(Date, JSON) {
  const auto json_object =
      formats::json::FromString(R"json({"data" : "2048-01-11"})json");
  const auto date = json_object["data"].As<utils::datetime::Date>();

  EXPECT_EQ(utils::datetime::DateFromRFC3339String("2048-01-11"), date);

  formats::json::ValueBuilder vb;
  vb["new_data"] = date;
  const auto new_json_string = ToString(vb.ExtractValue());

  EXPECT_EQ(R"json({"new_data":"2048-01-11"})json", new_json_string);
}

TEST(Date, RequestedUsecase) {
  // string -> Parse -> Timepoint -> Serialize -> string

  const std::string original{R"json({"data":"2049-02-10"})json"};

  const auto date_json = formats::json::FromString(original);
  const auto date = date_json["data"].As<utils::datetime::Date>();
  EXPECT_EQ(utils::datetime::Date(2049, 2, 10), date);

  auto time_point = date.GetUnderlying();
  utils::datetime::Date new_date{time_point};

  formats::json::ValueBuilder vb;
  vb["data"] = new_date;
  const auto resulting_string = ToString(vb.ExtractValue());
  EXPECT_EQ(original, resulting_string);
}

TEST(Date, Streaming) {
  // string -> Parse -> Timepoint -> Serialize -> string
  const auto date = utils::datetime::Date(2000, 2, 12);

  formats::json::StringBuilder sw;
  {
    formats::json::StringBuilder::ObjectGuard guard{sw};
    sw.Key("field1");
    WriteToStream(date, sw);
  }
  EXPECT_EQ(sw.GetString(), "{\"field1\":\"2000-02-12\"}");

  std::ostringstream oss;
  oss << date;
  EXPECT_EQ(oss.str(), "2000-02-12");
}
