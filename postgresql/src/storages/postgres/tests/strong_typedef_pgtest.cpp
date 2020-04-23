#include <storages/postgres/detail/connection.hpp>
#include <storages/postgres/io/strong_typedef.hpp>
#include <storages/postgres/tests/util_pgtest.hpp>

namespace pg = storages::postgres;
namespace io = pg::io;

namespace static_test {

/*! [Strong typedef] */
using StringTypedef = utils::StrongTypedef<struct TestTypedef, std::string>;
// This strong typedef will be nullable
using OptStringTypedef =
    utils::StrongTypedef<struct TestTypedef, std::optional<std::string>>;
using IntTypedef = utils::StrongTypedef<struct TestTypedef, pg::Integer>;
/*! [Strong typedef] */

struct UserType {
  StringTypedef s;
  IntTypedef i;
};

using UserTypedef = utils::StrongTypedef<struct TestTypedef, UserType>;

enum class MappedEnum { kOne, kTwo };

/*! [Enum typedef] */
enum class EnumStrongTypedef : int {};
/*! [Enum typedef] */
enum class UnusableEnumTypedef : unsigned int {};

}  // namespace static_test

namespace storages::postgres::io {

template <>
struct CppToUserPg<static_test::UserType> {
  // dummy name, not actually used in test
  static constexpr DBTypeName postgres_name = "schema.name";
};

template <>
struct CppToUserPg<static_test::MappedEnum>
    : EnumMappingBase<static_test::MappedEnum> {
  // dummy name, not actually used in test
  static constexpr DBTypeName postgres_name = "schema.name";
  static constexpr EnumeratorList enumerators{{EnumType::kOne, "one"},
                                              {EnumType::kTwo, "two"}};
};

}  // namespace storages::postgres::io

namespace static_test {

using namespace io::traits;

// Strong typedef to system pg type
static_assert(kIsMappedToPg<StringTypedef>,
              "Strong typedef must map to underlying type mapping");
static_assert(
    std::is_same<io::CppToPg<StringTypedef>::Mapping,
                 io::CppToSystemPg<std::string>>::value,
    "Strong typedef must have the same mapping as the underlying type");
static_assert(kHasParser<StringTypedef>,
              "Strong typedef to a defined type must have a parser");
static_assert(kHasFormatter<StringTypedef>,
              "Strong typedef to a defined type must have a formatter");
static_assert(!kIsNullable<StringTypedef>,
              "Strong typedef must derive nullability from underlying type");

static_assert(kIsMappedToPg<OptStringTypedef>,
              "Strong typedef must map to underlying type mapping");
static_assert(
    std::is_same<io::CppToPg<OptStringTypedef>::Mapping,
                 io::CppToSystemPg<std::string>>::value,
    "Strong typedef must have the same mapping as the underlying type");
static_assert(kHasParser<OptStringTypedef>,
              "Strong typedef to a defined type must have a parser");
static_assert(kHasFormatter<OptStringTypedef>,
              "Strong typedef to a defined type must have a formatter");
static_assert(kIsNullable<OptStringTypedef>,
              "Strong typedef must derive nullability from underlying type");

static_assert(kIsMappedToPg<IntTypedef>,
              "Strong typedef must map to underlying type mapping");
static_assert(
    std::is_same<io::CppToPg<IntTypedef>::Mapping,
                 io::CppToSystemPg<pg::Integer>>::value,
    "Strong typedef must have the same mapping as the underlying type");
static_assert(kHasParser<IntTypedef>,
              "Strong typedef to a defined type must have a parser");
static_assert(kHasFormatter<IntTypedef>,
              "Strong typedef to a defined type must have a formatter");
static_assert(!kIsNullable<IntTypedef>,
              "Strong typedef must derive nullability from underlying type");

static_assert(kIsMappedToPg<UserTypedef>,
              "Strong typedef must map to underlying type mapping");
static_assert(
    std::is_same<io::CppToPg<UserTypedef>::Mapping,
                 io::CppToUserPg<UserType>>::value,
    "Strong typedef must have the same mapping as the underlying type");
static_assert(kHasParser<UserTypedef>,
              "Strong typedef to a defined type must have a parser");
static_assert(kHasFormatter<UserTypedef>,
              "Strong typedef to a defined type must have a formatter");
static_assert(!kIsNullable<UserTypedef>,
              "Strong typedef must derive nullability from underlying type");

// Check mapping calculation doesn't break hand-mapped types
static_assert(kIsMappedToPg<pg::TimePointTz>);
static_assert(std::is_same<io::CppToPg<pg::TimePointTz>::Mapping,
                           io::CppToSystemPg<pg::TimePointTz>>::value);
static_assert(std::is_same<IO<pg::TimePointTz>::ParserType,
                           io::BufferParser<pg::TimePointTz>>::value);
static_assert(std::is_same<IO<pg::TimePointTz>::FormatterType,
                           io::BufferFormatter<pg::TimePointTz>>::value);

static_assert(!CanUseEnumAsStrongTypedef<std::string>(), "not an enum");
static_assert(!CanUseEnumAsStrongTypedef<int>(), "not an enum");

// enum with unsigned underlying type cannot be used as strong typedef with
// postgres
static_assert(
    !CanUseEnumAsStrongTypedef<UnusableEnumTypedef>(),
    "Enumeration with unsigned underlying type cannot be used with postgres");

static_assert(!CanUseEnumAsStrongTypedef<MappedEnum>(),
              "Mapped enum cannot be used as a strong typedef");

static_assert(CanUseEnumAsStrongTypedef<EnumStrongTypedef>(),
              "Enum with signed underlying type and no mapping can be used as "
              "a strong typedef");
static_assert(kIsMappedToPg<EnumStrongTypedef>,
              "A valid enum strong typedef must have a parser");
static_assert(kHasParser<EnumStrongTypedef>,
              "A valid enum strong typedef must have a parser");
static_assert(kHasFormatter<EnumStrongTypedef>,
              "A valid enum strong typedef must have a formatter");
static_assert(!kIsNullable<EnumStrongTypedef>,
              "Strong typedef must derive nullability from underlying type");

}  // namespace static_test

namespace {

POSTGRE_TEST_P(StringStrongTypedef) {
  ASSERT_TRUE(conn.get()) << "Expected non-empty connection pointer";
  pg::ResultSet res{nullptr};

  static_test::StringTypedef str{"test"};
  EXPECT_NO_THROW(res = conn->Execute("select $1", str));
  EXPECT_EQ(str, res[0][0].As<static_test::StringTypedef>());
  // Row interface
  EXPECT_EQ(str, res[0].As<static_test::StringTypedef>());
  // Single row interface
  EXPECT_EQ(str, res.AsSingleRow<static_test::StringTypedef>());
  // As container interface
  EXPECT_EQ(str, res.AsContainer<std::vector<static_test::StringTypedef>>()[0]);
}

POSTGRE_TEST_P(IntStrongTypedef) {
  ASSERT_TRUE(conn.get()) << "Expected non-empty connection pointer";
  pg::ResultSet res{nullptr};

  static_test::IntTypedef i{42};
  EXPECT_NO_THROW(res = conn->Execute("select $1", i));
  EXPECT_EQ(i, res[0][0].As<static_test::IntTypedef>());
  // Row interface
  EXPECT_EQ(i, res[0].As<static_test::IntTypedef>());
  // Single row interface
  EXPECT_EQ(i, res.AsSingleRow<static_test::IntTypedef>());
  // As container interface
  EXPECT_EQ(i, res.AsContainer<std::vector<static_test::IntTypedef>>()[0]);
}

POSTGRE_TEST_P(IntEnumStrongTypedef) {
  ASSERT_TRUE(conn.get()) << "Expected non-empty connection pointer";
  pg::ResultSet res{nullptr};

  static_test::EnumStrongTypedef i{42};
  EXPECT_NO_THROW(res = conn->Execute("select $1", i));
  EXPECT_EQ(i, res[0][0].As<static_test::EnumStrongTypedef>());
  // Row interface
  EXPECT_EQ(i, res[0].As<static_test::EnumStrongTypedef>());
  // Single row interface
  EXPECT_EQ(i, res.AsSingleRow<static_test::EnumStrongTypedef>());
  // As container interface
  EXPECT_EQ(i,
            res.AsContainer<std::vector<static_test::EnumStrongTypedef>>()[0]);
}

}  // namespace
