#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Minimal assert-based test framework, no external dependencies.
namespace sparkle_tests
{
  struct TestCase
  {
    std::string name;
    std::function<void()> fn;
  };

  inline std::vector<TestCase>& Registry()
  {
    static std::vector<TestCase> tests;
    return tests;
  }

  struct Registrar
  {
    Registrar(std::string name, std::function<void()> fn)
    {
      Registry().push_back({ std::move(name), std::move(fn) });
    }
  };

  inline int gFailures = 0;
  inline const char* gCurrentTest = "";
}

#define TEST(name) \
  static void name(); \
  static sparkle_tests::Registrar name##_registrar(#name, name); \
  static void name()

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("  FAIL: %s (%s:%d) - %s\n", sparkle_tests::gCurrentTest, __FILE__, __LINE__, #cond); \
      sparkle_tests::gFailures++; \
    } \
  } while (0)
