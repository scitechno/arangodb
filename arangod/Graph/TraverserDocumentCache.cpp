////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017-2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "TraverserDocumentCache.h"

#include "Basics/StringRef.h"
#include "Basics/VelocyPackHelper.h"

#include "Aql/AqlValue.h"

#include "Cache/Common.h"
#include "Cache/Cache.h"
#include "Cache/CacheManagerFeature.h"
#include "Cache/Finding.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::graph;

TraverserDocumentCache::TraverserDocumentCache(transaction::Methods* trx)
    : TraverserCache(trx), _cache(nullptr) {
  auto cacheManager = CacheManagerFeature::MANAGER;
  TRI_ASSERT(cacheManager != nullptr);
  _cache = cacheManager->createCache(cache::CacheType::Plain);
}

TraverserDocumentCache::~TraverserDocumentCache() {
  if (_cache != nullptr) {
    auto cacheManager = CacheManagerFeature::MANAGER;
    cacheManager->destroyCache(_cache);
  }
}

// @brief Only for internal use, Cache::Finding prevents
// the cache from removing this specific object. Should not be retained
// for a longer period of time.
// DO NOT give it to a caller.
cache::Finding TraverserDocumentCache::lookup(StringRef idString) {
  TRI_ASSERT(_cache != nullptr);
  VPackValueLength keySize = idString.length();
  void const* key = idString.data();
  // uint32_t keySize = static_cast<uint32_t>(idString.byteSize());
  return _cache->find(key, (uint32_t)keySize);
}

VPackSlice TraverserDocumentCache::lookupAndCache(StringRef id) {
  VPackSlice result = lookupInCollection(id);
  if (_cache != nullptr) {
    void const* key = id.begin();
    auto keySize = static_cast<uint32_t>(id.length());

    void const* resVal = result.begin();
    uint64_t resValSize = static_cast<uint64_t>(result.byteSize());
    std::unique_ptr<cache::CachedValue> value(
        cache::CachedValue::construct(key, keySize, resVal, resValSize));

    if (value) {
      bool success = _cache->insert(value.get());
      if (!success) {
        LOG_TOPIC(DEBUG, Logger::GRAPHS) << "Insert failed";
      } else {
        // Cache is responsible.
        // If this failed, well we do not store it and read it again next time.
        value.release();
      }
    }
  }
  return result;
}

void TraverserDocumentCache::insertIntoResult(StringRef idString,
                                              VPackBuilder& builder) {
  if (_cache != nullptr) {
    auto finding = lookup(idString);
    if (finding.found()) {
      auto val = finding.value();
      VPackSlice slice(val->value());
      // finding makes sure that slice contant stays valid.
      builder.add(slice);
      return;
    }
  }
  // Not in cache. Fetch and insert.
  builder.add(lookupAndCache(idString));
}

aql::AqlValue TraverserDocumentCache::fetchAqlResult(StringRef idString) {
  if (_cache != nullptr) {
    auto finding = lookup(idString);
    if (finding.found()) {
      auto val = finding.value();
      VPackSlice slice(val->value());
      // finding makes sure that slice contant stays valid.
      return aql::AqlValue(slice);
    }
  }
  // Not in cache. Fetch and insert.
  return aql::AqlValue(lookupAndCache(idString));
}

void TraverserDocumentCache::insertDocument(
    StringRef idString, arangodb::velocypack::Slice const& document) {
  ++_insertedDocuments;
  if (_cache != nullptr) {
    auto finding = lookup(idString);
    if (!finding.found()) {
      void const* key = idString.data();
      auto keySize = static_cast<uint32_t>(idString.length());

      void const* resVal = document.begin();
      uint64_t resValSize = static_cast<uint64_t>(document.byteSize());
      std::unique_ptr<cache::CachedValue> value(
          cache::CachedValue::construct(key, keySize, resVal, resValSize));

      if (value) {
        bool success = _cache->insert(value.get());
        if (!success) {
          LOG_TOPIC(DEBUG, Logger::GRAPHS) << "Insert document into cache failed";
        } else {
          // Cache is responsible.
          // If this failed, well we do not store it and read it again next time.
          value.release();
        }
      }
    }
  }
}

bool TraverserDocumentCache::validateFilter(
    StringRef idString, std::function<bool(VPackSlice const&)> filterFunc) {
  if (_cache != nullptr) {
    auto finding = lookup(idString);
    if (finding.found()) {
      auto val = finding.value();
      VPackSlice slice(val->value());
      // finding makes sure that slice contant stays valid.
      return filterFunc(slice);
    }
  }
  // Not in cache. Fetch and insert.
  VPackSlice slice = lookupAndCache(idString);
  return filterFunc(slice);
}