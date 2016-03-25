////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "ApplicationFeatures/RandomFeature.h"

#include <iostream>

#include "Basics/RandomGenerator.h"
#include "Logger/Logger.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::options;

RandomFeature::RandomFeature(application_features::ApplicationServer* server)
    : ApplicationFeature(server, "Random"),
      _randomGenerator((uint32_t) RandomGenerator::RandomType::MERSENNE) {
  setOptional(false);
  requiresElevatedPrivileges(false);
}

void RandomFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  LOG_TOPIC(TRACE, Logger::STARTUP) << name() << "::collectOptions";

  options->addSection(Section("random", "Configure the random generator",
                              "random number options", false, false));

#ifdef _WIN32
  std::unordered_set<uint32_t> generators = {1, 5};
#else
  std::unordered_set<uint32_t> generators = {1, 2, 3, 4};
#endif

  options->addHiddenOption("--random.generator", "random number generator to use (1 = MERSENNE, 2 = RANDOM, "
                           "3 = URANDOM, 4 = COMBINED (not for Windows), 5 = WinCrypt (Windows only)",
                           new DiscreteValuesParameter<UInt32Parameter>(&_randomGenerator, generators));
}

void RandomFeature::start() {
  RandomGenerator::initialize((RandomGenerator::RandomType) _randomGenerator);
}
