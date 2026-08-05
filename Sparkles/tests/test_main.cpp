#include "test_framework.h"

TEST(Sanity_TrivialPass)
{
  CHECK(1 + 1 == 2);
}

int main()
{
  for (auto& test : sparkle_tests::Registry())
  {
    sparkle_tests::gCurrentTest = test.name.c_str();
    test.fn();
  }

  const int total = static_cast<int>(sparkle_tests::Registry().size());
  const int failed = sparkle_tests::gFailures;
  std::printf("%d/%d tests passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
