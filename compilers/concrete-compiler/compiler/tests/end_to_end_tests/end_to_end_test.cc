#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <regex>
#include <type_traits>

#include "concretelang/Common/Values.h"
#include "concretelang/Support/CompilationFeedback.h"
#include "concretelang/TestLib/TestCircuit.h"
#include "end_to_end_fixture/EndToEndFixture.h"
#include "end_to_end_jit_test.h"
#include "tests_tools/GtestEnvironment.h"
#include "tests_tools/assert.h"
#include "tests_tools/keySetCache.h"

using concretelang::testlib::createTempFolderIn;
using concretelang::testlib::deleteFolder;
using concretelang::testlib::getSystemTempFolderPath;
using concretelang::testlib::TestCircuit;
using concretelang::values::Value;

/// @brief EndToEndTest is a template that allows testing for one program for a
/// TestDescription.
class EndToEndTest : public ::testing::Test {
public:
  explicit EndToEndTest(std::string program, TestDescription desc,
                        std::optional<TestErrorRate> errorRate,
                        mlir::concretelang::CompilationOptions options)
      : program(program), desc(desc), errorRate(errorRate),
        testCircuit(std::nullopt), options(options) {
    if (errorRate.has_value()) {
      options.optimizerConfig.global_p_error = errorRate->global_p_error;
      options.optimizerConfig.p_error = errorRate->global_p_error;
    }
    artifactFolder = createTempFolderIn(getSystemTempFolderPath());
  };

  void SetUp() override {
    /* Compile the program */
    std::shared_ptr<mlir::concretelang::CompilationContext> ccx =
        mlir::concretelang::CompilationContext::createShared();
    mlir::concretelang::CompilerEngine ce{ccx};
    ce.setCompilationOptions(options);
    auto expectCompilationResult = ce.compile({program}, artifactFolder);
    ASSERT_EXPECTED_SUCCESS(expectCompilationResult);
    auto compiled = expectCompilationResult.get();

    /* Retrieve the keyset */
    auto keyset =
        getTestKeySetCachePtr()
            ->getKeyset(compiled.getProgramInfo().asReader().getKeyset(), 0, 0)
            .value();

    /* Create the test circuit */
    testCircuit =
        TestCircuit::create(
            keyset, compiled.getProgramInfo().asReader(),
            compiled.getSharedLibraryPath(compiled.getOutputDirPath()), 0, 0,
            false)
            .value();

    /* Create the public argument */
    args = std::vector<Value>();
    for (auto &input : desc.inputs) {
      args.push_back(input.getValue());
    }
  }

  void TearDown() override { deleteFolder(artifactFolder); }

  void TestBody() override {
    if (!errorRate.has_value()) {
      testOnce();
    } else {
      testErrorRate();
    }
  }

  void testOnce() {
    // We execute the circuit.
    auto maybeRes = (*testCircuit).call(args);
    ASSERT_OUTCOME_HAS_VALUE(maybeRes);
    auto result = maybeRes.value();

    /* Check result */
    for (size_t i = 0; i < desc.outputs.size(); i++) {
      ASSERT_LLVM_ERROR(checkResult(desc.outputs[i], result[i]));
    }
  }

  void testErrorRate() {
    auto nbError = 0;
    for (size_t i = 0; i < errorRate->nb_repetition; i++) {
      // We execute the circuit.
      auto maybeRes = (*testCircuit).call(args);
      ASSERT_OUTCOME_HAS_VALUE(maybeRes);
      auto result = maybeRes.value();

      /* Check result */
      // For now we support just one result
      assert(desc.outputs.size() == 1);
      auto err = checkResult(desc.outputs[0], result[0]);
      if (err) {
        nbError++;
        DISCARD_LLVM_ERROR(err);
      }
    }
    double threshold = errorRate->too_high_error_count_threshold();
    std::cout << "n_rep " << errorRate->nb_repetition << " p_error "
              << errorRate->global_p_error << " maximum_errors " << threshold
              << "\n";
    ASSERT_LE(nbError, threshold) << "Empirical error rate is too high";
  }

private:
  std::string program;
  std::string artifactFolder;
  TestDescription desc;
  std::optional<TestErrorRate> errorRate;
  std::optional<TestCircuit> testCircuit;
  mlir::concretelang::CompilationOptions options;
  std::vector<Value> args;
};

std::string getTestName(EndToEndDesc desc,
                        mlir::concretelang::CompilationOptions options,
                        int testNum) {
  std::ostringstream os;
  os << getOptionsName(options) << "." << desc.description << "." << testNum;
  return std::regex_replace(os.str(), std::regex("-"), "");
}

void registerEndToEnd(std::string suiteName, std::string testName,
                      std::string valueName, std::string program,
                      TestDescription test,
                      std::optional<TestErrorRate> errorRate,
                      mlir::concretelang::CompilationOptions options) {
  // TODO: Get file and line from yaml
  auto file = __FILE__;
  auto line = __LINE__;
  ::testing::RegisterTest(
      suiteName.c_str(), testName.c_str(), nullptr, valueName.c_str(), file,
      line, [=]() -> EndToEndTest * {
        return new EndToEndTest(program, test, errorRate, options);
      });
}

void registerEndToEnd(std::string suiteName, EndToEndDesc desc,
                      mlir::concretelang::CompilationOptions options) {
  if (desc.v0Constraint.has_value()) {
    options.v0FHEConstraints = desc.v0Constraint;
  }
  options.optimizerConfig.encoding = desc.encoding;
  if (desc.p_error.has_value()) {
    options.optimizerConfig.p_error = *desc.p_error;
    options.optimizerConfig.global_p_error = NAN;
  }
  auto i = 0;
  for (auto test : desc.tests) {
    auto valueName = std::to_string(i);
    auto testName = getTestName(desc, options, i);
    if (desc.test_error_rates.empty()) {
      registerEndToEnd(suiteName, testName, valueName, desc.program, test,
                       std::nullopt, options);
    } else {
      auto j = 0;
      for (auto rate : desc.test_error_rates) {
        auto rateName = testName + "_rate" + std::to_string(j);
        registerEndToEnd(suiteName, rateName, valueName, desc.program, test,
                         rate, options);
        j++;
      }
    }
    i++;
  }
}

/// @brief Register a suite of end to end test
/// @param suiteName The name of the suite.
/// @param descriptions A vector of description of tests to register .
/// @param options The compilation options.
void registerEndToEndSuite(std::string suiteName,
                           std::vector<EndToEndDesc> descriptions,
                           mlir::concretelang::CompilationOptions options) {
  for (auto desc : descriptions) {
    registerEndToEnd(suiteName, desc, options);
  }
}

namespace path = llvm::sys::path;

int main(int argc, char **argv) {

  // Parse google test options, update argc and argv by removing gtest options
  ::testing::InitGoogleTest(&argc, argv);

  // parse end to end test compiler options

  auto options = parseEndToEndCommandLine(argc, argv);

  auto compilationOptions = std::get<0>(options);
  auto descriptionFiles = std::get<1>(options);

  for (auto descFile : descriptionFiles) {
    auto suiteName = path::stem(descFile.path).str() + ".library";
    registerEndToEndSuite(suiteName, descFile.descriptions, compilationOptions);
  }
  return RUN_ALL_TESTS();
}
