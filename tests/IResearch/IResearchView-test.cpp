//////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 EMC Corporation
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
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"
#include "common.h"
#include "StorageEngineMock.h"
#include "ExpressionContextMock.h"

#include "analysis/token_attributes.hpp"
#include "search/scorers.hpp"
#include "utils/locale_utils.hpp"
#include "utils/log.hpp"
#include "utils/utf8_path.hpp"

#include "ApplicationFeatures/JemallocFeature.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/AstNode.h"
#include "Aql/Function.h"
#include "Aql/SortCondition.h"
#include "Basics/ArangoGlobalContext.h"
#include "Basics/files.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/ApplicationServerHelper.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchLinkMeta.h"
#include "IResearch/IResearchMMFilesLink.h"
#include "IResearch/IResearchView.h"
#include "IResearch/SystemDatabaseFeature.h"
#include "Logger/Logger.h"
#include "Logger/LogTopic.h"
#include "Random/RandomFeature.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/TraverserEngineRegistryFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/FlushFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FeatureCacheFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/UserTransaction.h"
#include "Utils/OperationOptions.h"
#include "velocypack/Iterator.h"
#include "velocypack/Parser.h"
#include "V8Server/V8DealerFeature.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ManagedDocumentResult.h"

NS_LOCAL

struct DocIdScorer: public irs::sort {
  DECLARE_SORT_TYPE() { static irs::sort::type_id type("test_doc_id"); return type; }
  static ptr make(const irs::string_ref&) { PTR_NAMED(DocIdScorer, ptr); return ptr; }
  DocIdScorer(): irs::sort(DocIdScorer::type()) { }
  virtual sort::prepared::ptr prepare() const override { PTR_NAMED(Prepared, ptr); return ptr; }

  struct Prepared: public irs::sort::prepared_base<uint64_t> {
    virtual void add(score_t& dst, const score_t& src) const override { dst = src; }
    virtual irs::flags const& features() const override { return irs::flags::empty_instance(); }
    virtual bool less(const score_t& lhs, const score_t& rhs) const override { return lhs < rhs; }
    virtual irs::sort::collector::ptr prepare_collector() const override { return nullptr; }
    virtual void prepare_score(score_t& score) const override { }
    virtual irs::sort::scorer::ptr prepare_scorer(
      irs::sub_reader const& segment,
      irs::term_reader const& field,
      irs::attribute_store const& query_attrs,
      irs::attribute_view const& doc_attrs
    ) const override {
      return irs::sort::scorer::make<Scorer>(doc_attrs.get<irs::document>());
    }
  };

  struct Scorer: public irs::sort::scorer {
    irs::attribute_view::ref<irs::document>::type const& _doc;
    Scorer(irs::attribute_view::ref<irs::document>::type const& doc): _doc(doc) { }
    virtual void score(irs::byte_type* score_buf) override {
      reinterpret_cast<uint64_t&>(*score_buf) = _doc.get()->value;
    }
  };
};

REGISTER_SCORER_TEXT(DocIdScorer, DocIdScorer::make);

// vocbase shutodown() must be exlicitly called or dropped collections are not deallocated
struct VocbaseWrapper {
  TRI_vocbase_t instance;
  template<typename... Args>
  VocbaseWrapper(Args&&... args): instance(std::forward<Args>(args)...) {}
  ~VocbaseWrapper() { instance.shutdown(); }
  TRI_vocbase_t* operator->() { return &instance; }
  TRI_vocbase_t& operator*() { return instance; }
};

NS_END

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

struct IResearchViewSetup {
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;
  std::unique_ptr<TRI_vocbase_t> system;
  std::vector<std::pair<arangodb::application_features::ApplicationFeature*, bool>> features;
  std::string testFilesystemPath;

  IResearchViewSetup(): server(nullptr, nullptr) {
    arangodb::EngineSelectorFeature::ENGINE = &engine;

    arangodb::tests::init();

    // suppress INFO {authentication} Authentication is turned on (system only), authentication for unix sockets is turned on
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::WARN);

    // setup required application features
    features.emplace_back(new arangodb::V8DealerFeature(&server), false);
    features.emplace_back(new arangodb::ViewTypesFeature(&server), true);
    features.emplace_back(new arangodb::QueryRegistryFeature(&server), false);
    arangodb::application_features::ApplicationServer::server->addFeature(features.back().first);
    system = irs::memory::make_unique<TRI_vocbase_t>(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 0, TRI_VOC_SYSTEM_DATABASE);
    features.emplace_back(new arangodb::FeatureCacheFeature(&server), true);
    features.emplace_back(new arangodb::RandomFeature(&server), false); // required by AuthenticationFeature
    features.emplace_back(new arangodb::AuthenticationFeature(&server), true);
    features.emplace_back(new arangodb::DatabaseFeature(&server), false);
    features.emplace_back(new arangodb::DatabasePathFeature(&server), false);
    features.emplace_back(new arangodb::JemallocFeature(&server), false); // required for DatabasePathFeature
    features.emplace_back(new arangodb::TraverserEngineRegistryFeature(&server), false); // must be before AqlFeature
    features.emplace_back(new arangodb::AqlFeature(&server), true);
    features.emplace_back(new arangodb::aql::AqlFunctionFeature(&server), true); // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::iresearch::IResearchAnalyzerFeature(&server), true);
    features.emplace_back(new arangodb::iresearch::IResearchFeature(&server), true);
    features.emplace_back(new arangodb::iresearch::SystemDatabaseFeature(&server, system.get()), false); // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::FlushFeature(&server), false); // do not start the thread

    for (auto& f : features) {
      arangodb::application_features::ApplicationServer::server->addFeature(f.first);
    }

    for (auto& f : features) {
      f.first->prepare();
    }

    for (auto& f : features) {
      if (f.second) {
        f.first->start();
      }
    }

    PhysicalViewMock::persistPropertiesResult = TRI_ERROR_NO_ERROR;
    TransactionStateMock::abortTransactionCount = 0;
    TransactionStateMock::beginTransactionCount = 0;
    TransactionStateMock::commitTransactionCount = 0;
    testFilesystemPath = (
      (irs::utf8_path()/=
      TRI_GetTempPath())/=
      (std::string("arangodb_tests.") + std::to_string(TRI_microtime()))
    ).utf8();
    auto* dbPathFeature = arangodb::application_features::ApplicationServer::getFeature<arangodb::DatabasePathFeature>("DatabasePath");
    const_cast<std::string&>(dbPathFeature->directory()) = testFilesystemPath;

    long systemError;
    std::string systemErrorStr;
    TRI_CreateDirectory(testFilesystemPath.c_str(), systemError, systemErrorStr);

    // suppress log messages since tests check error conditions
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::IResearchFeature::IRESEARCH.name(), arangodb::LogLevel::FATAL);
    irs::logger::output_le(iresearch::logger::IRL_FATAL, stderr);
  }

  ~IResearchViewSetup() {
    system.reset(); // destroy before reseting the 'ENGINE'
    TRI_RemoveDirectory(testFilesystemPath.c_str());
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::IResearchFeature::IRESEARCH.name(), arangodb::LogLevel::DEFAULT);
    arangodb::application_features::ApplicationServer::server = nullptr;
    arangodb::EngineSelectorFeature::ENGINE = nullptr;

    // destroy application features
    for (auto& f : features) {
      if (f.second) {
        f.first->stop();
      }
    }

    for (auto& f : features) {
      f.first->unprepare();
    }

    arangodb::FeatureCacheFeature::reset();
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::DEFAULT);
  }
};

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief setup
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("IResearchViewTest", "[iresearch][iresearch-view]") {
  IResearchViewSetup s;
  UNUSED(s);


SECTION("test_defaults") {
  auto namedJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\" }");
  auto json = arangodb::velocypack::Parser::fromJson("{}");

  // existing view definition
  {
    auto view = arangodb::iresearch::IResearchView::make(nullptr, json->slice(), false);
    CHECK((true == !view));
  }

  // existing view definition with LogicalView (for persistence)
  {
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto view = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    expectedMeta._dataPath = std::string("-") + std::to_string(logicalView.id());

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, true);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((6U == slice.length()));
    CHECK((!slice.hasKey("links"))); // for persistence so no links
    CHECK((meta.init(slice, error, logicalView) && expectedMeta == meta));
  }

  // existing view definition with LogicalView
  {
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto view = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    expectedMeta._dataPath = std::string("-") + std::to_string(logicalView.id());

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((slice.hasKey("links")));
    CHECK((meta.init(slice, error, logicalView) && expectedMeta == meta));
  }

  // new view definition
  {
    auto view = arangodb::iresearch::IResearchView::make(nullptr, json->slice(), true);
    CHECK((true == !view));
  }

  // new view definition with LogicalView (for persistence)
  {
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto view = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), true);
    CHECK((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    expectedMeta._dataPath = std::string("-") + std::to_string(logicalView.id());

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, true);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((6U == slice.length()));
    CHECK((!slice.hasKey("links"))); // for persistence so no links
    CHECK((meta.init(slice, error, logicalView) && expectedMeta == meta));
  }

  // new view definition with LogicalView
  {
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto view = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), true);
    CHECK((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    expectedMeta._dataPath = std::string("-") + std::to_string(logicalView.id());

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((meta.init(slice, error, logicalView) && expectedMeta == meta));

    auto tmpSlice = slice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // new view definition with links (not supported for link creation)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\", \"id\": 100 }");
    auto viewJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"iresearch\", \"id\": 101, \"properties\": { \"links\": { \"testCollection\": {} } } }");

    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());CHECK((nullptr != logicalCollection));
    CHECK((true == !vocbase.lookupView("testView")));
    CHECK((true == logicalCollection->getIndexes().empty()));
    auto logicalView = vocbase.createView(viewJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto* view = logicalView->getImplementation();
    REQUIRE((false == !view));
    auto* viewImpl = dynamic_cast<arangodb::iresearch::IResearchView*>(view);
    REQUIRE((nullptr != viewImpl));
    CHECK((0 == viewImpl->linkCount()));
    CHECK((true == logicalCollection->getIndexes().empty()));
  }
}

SECTION("test_drop") {
  std::string dataPath = ((irs::utf8_path()/=s.testFilesystemPath)/=std::string("deleteme")).utf8();
  auto json = arangodb::velocypack::Parser::fromJson("{ \
    \"name\": \"testView\", \
    \"type\": \"iresearch\", \
    \"properties\": { \
      \"dataPath\": \"" + arangodb::basics::StringUtils::replace(dataPath, "\\", "/") + "\" \
    } \
  }");

  CHECK((false == TRI_IsDirectory(dataPath.c_str())));

  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
  auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
  CHECK((nullptr != logicalCollection));
  CHECK((true == !vocbase.lookupView("testView")));
  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((false == TRI_IsDirectory(dataPath.c_str()))); // createView(...) will call open()
  auto logicalView = vocbase.createView(json->slice(), 0);
  REQUIRE((false == !logicalView));
  auto view = logicalView->getImplementation();
  REQUIRE((false == !view));

  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((false == !vocbase.lookupView("testView")));
  CHECK((true == TRI_IsDirectory(dataPath.c_str())));
  CHECK((TRI_ERROR_NO_ERROR == vocbase.dropView("testView")));
  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((true == !vocbase.lookupView("testView")));
  CHECK((false == TRI_IsDirectory(dataPath.c_str())));
}

SECTION("test_drop_with_link") {
  std::string dataPath = ((irs::utf8_path()/=s.testFilesystemPath)/=std::string("deleteme")).utf8();
  auto json = arangodb::velocypack::Parser::fromJson("{ \
    \"name\": \"testView\", \
    \"type\": \"iresearch\", \
    \"properties\": { \
      \"dataPath\": \"" + arangodb::basics::StringUtils::replace(dataPath, "\\", "/") + "\" \
    } \
  }");

  CHECK((false == TRI_IsDirectory(dataPath.c_str())));

  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
  auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
  CHECK((nullptr != logicalCollection));
  CHECK((true == !vocbase.lookupView("testView")));
  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((false == TRI_IsDirectory(dataPath.c_str()))); // createView(...) will call open()
  auto logicalView = vocbase.createView(json->slice(), 0);
  REQUIRE((false == !logicalView));
  auto view = logicalView->getImplementation();
  REQUIRE((false == !view));

  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((false == !vocbase.lookupView("testView")));
  CHECK((true == TRI_IsDirectory(dataPath.c_str())));


  auto links = arangodb::velocypack::Parser::fromJson("{ \
    \"links\": { \"testCollection\": {} } \
  }");

  arangodb::Result res = logicalView->updateProperties(links->slice(), true, false);
  CHECK(true == res.ok());
  CHECK((false == logicalCollection->getIndexes().empty()));

  CHECK((TRI_ERROR_NO_ERROR == vocbase.dropView("testView")));
  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((true == !vocbase.lookupView("testView")));
  CHECK((false == TRI_IsDirectory(dataPath.c_str())));
}

SECTION("test_drop_cid") {
  static std::vector<std::string> const EMPTY;

  // cid not in list of fully indexed (view definition not updated, not persisted)
  {
    auto json = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\" }");
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    arangodb::LogicalView logicalView(nullptr, json->slice());
    auto viewImpl = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !viewImpl));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(viewImpl.get());
    CHECK((nullptr != view));

    // fill with test data
    {
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
      CHECK((trx.begin().ok()));
      view->insert(trx, 42, arangodb::LocalDocumentId(0), doc->slice(), meta);
      CHECK((trx.commit().ok()));
      view->sync();
    }

    // query
    {
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
      CHECK((trx.begin().ok()));
      CHECK(1 == view->snapshot(trx).live_docs_count());
    }

    // drop cid 42
    {
      bool persisted = false;
      auto before = PhysicalViewMock::before;
      auto restore = irs::make_finally([&before]()->void { PhysicalViewMock::before = before; });
      PhysicalViewMock::before = [&persisted]()->void { persisted = true; };

      view->drop(42);
      CHECK((!persisted));
      view->sync();
    }

    // query
    {
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
      CHECK((trx.begin().ok()));
      CHECK(0 == view->snapshot(trx).live_docs_count());
    }
  }

  // cid in list of fully indexed (view definition updated+persisted)
  {
    auto json = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"collections\": [ 42 ] }");
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    arangodb::LogicalView logicalView(nullptr, json->slice());
    auto viewImpl = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !viewImpl));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(viewImpl.get());
    CHECK((nullptr != view));

    // fill with test data
    {
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
      CHECK((trx.begin().ok()));
      view->insert(trx, 42, arangodb::LocalDocumentId(0), doc->slice(), meta);
      CHECK((trx.commit().ok()));
      view->sync();
    }

    // query
    {
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
      CHECK((trx.begin().ok()));
      CHECK(1 == view->snapshot(trx).live_docs_count());
    }

    // drop cid 42
    {
      bool persisted = false;
      auto before = PhysicalViewMock::before;
      auto restore = irs::make_finally([&before]()->void { PhysicalViewMock::before = before; });
      PhysicalViewMock::before = [&persisted]()->void { persisted = true; };

      view->drop(42);
      CHECK((persisted));
      view->sync();
    }

    // query
    {
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
      CHECK((trx.begin().ok()));
      CHECK(0 == view->snapshot(trx).live_docs_count());
    }
  }
}

SECTION("test_insert") {
  static std::vector<std::string> const EMPTY;
  auto json = arangodb::velocypack::Parser::fromJson("{}");
  auto namedJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\" }");
  arangodb::aql::AstNode noop(arangodb::aql::AstNodeType::NODE_TYPE_FILTER);
  arangodb::aql::AstNode noopChild(true, arangodb::aql::AstNodeValueType::VALUE_TYPE_BOOL); // all

  noop.addMember(&noopChild);

  // in recovery (removes cid+rid before insert)
  {
    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto viewImpl = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !viewImpl));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(viewImpl.get());
    CHECK((nullptr != view));
    view->open();

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta))); // 2nd time
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
      CHECK((view->sync()));
    }

    arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
    CHECK((trx.begin().ok()));
    auto reader = view->snapshot(trx);
    CHECK(2 == reader.live_docs_count());
  }

  // in recovery batch (removes cid+rid before insert)
  {
    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto viewImpl = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !viewImpl));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(viewImpl.get());
    CHECK((nullptr != view));
    view->open();

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
      std::vector<std::pair<arangodb::LocalDocumentId, arangodb::velocypack::Slice>> batch = {
        { arangodb::LocalDocumentId(1), docJson->slice() },
        { arangodb::LocalDocumentId(2), docJson->slice() },
      };

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
      CHECK((view->sync()));
    }

    arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
    CHECK((trx.begin().ok()));
    auto reader = view->snapshot(trx);
    CHECK((2 == reader.docs_count()));
  }

  // not in recovery
  {
    StorageEngineMock::inRecoveryResult = false;
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto viewImpl = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !viewImpl));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(viewImpl.get());
    CHECK((nullptr != view));

    // validate cid count
    {
      std::unordered_set<TRI_voc_cid_t> actual;
      CHECK((0 == view->linkCount()));
      CHECK((view->appendKnownCollections(actual)));
      CHECK((actual.empty()));
    }

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta))); // 2nd time
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
      CHECK((view->sync()));
    }

    arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
    CHECK((trx.begin().ok()));
    auto reader = view->snapshot(trx);
    CHECK((4 == reader.docs_count()));

    // validate cid count
    {
      std::unordered_set<TRI_voc_cid_t> expected = { 1 };
      std::unordered_set<TRI_voc_cid_t> actual;
      CHECK((0 == view->linkCount()));
      CHECK((view->appendKnownCollections(actual)));

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }
  }

  // not in recovery (with waitForSync)
  {
    StorageEngineMock::inRecoveryResult = false;
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto viewImpl = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !viewImpl));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(viewImpl.get());
    CHECK((nullptr != view));

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::Options options;
      options.waitForSync = true;
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, options);

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta))); // 2nd time
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
    }

    arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
    CHECK((trx.begin().ok()));
    auto reader = view->snapshot(trx);
    CHECK((4 == reader.docs_count()));
  }

  // not in recovery batch
  {
    StorageEngineMock::inRecoveryResult = false;
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto viewImpl = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !viewImpl));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(viewImpl.get());
    CHECK((nullptr != view));

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
      std::vector<std::pair<arangodb::LocalDocumentId, arangodb::velocypack::Slice>> batch = {
        { arangodb::LocalDocumentId(1), docJson->slice() },
        { arangodb::LocalDocumentId(2), docJson->slice() },
      };

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
      CHECK((view->sync()));
    }

    arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
    CHECK((trx.begin().ok()));
    auto reader = view->snapshot(trx);
    CHECK((4 == reader.docs_count()));
  }

  // not in recovery batch (waitForSync)
  {
    StorageEngineMock::inRecoveryResult = false;
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto viewImpl = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !viewImpl));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(viewImpl.get());
    CHECK((nullptr != view));

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::Options options;
      options.waitForSync = true;
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, options);
      std::vector<std::pair<arangodb::LocalDocumentId, arangodb::velocypack::Slice>> batch = {
        { arangodb::LocalDocumentId(1), docJson->slice() },
        { arangodb::LocalDocumentId(2), docJson->slice() },
      };

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
    }

    arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
    CHECK((trx.begin().ok()));
    auto reader = view->snapshot(trx);
    CHECK((4 == reader.docs_count()));
  }
}

SECTION("test_move_datapath") {
  std::string createDataPath = ((irs::utf8_path()/=s.testFilesystemPath)/=std::string("deleteme0")).utf8();
  std::string updateDataPath = ((irs::utf8_path()/=s.testFilesystemPath)/=std::string("deleteme1")).utf8();
  auto namedJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\" }");
  auto createJson = arangodb::velocypack::Parser::fromJson("{ \
    \"name\": \"testView\", \
    \"type\": \"iresearch\", \
    \"properties\": { \
      \"dataPath\": \"" + arangodb::basics::StringUtils::replace(createDataPath, "\\", "/") + "\" \
    } \
  }");
  auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
    \"dataPath\": \"" + arangodb::basics::StringUtils::replace(updateDataPath, "\\", "/") + "\" \
  }");

  CHECK((false == TRI_IsDirectory(createDataPath.c_str())));
  CHECK((false == TRI_IsDirectory(updateDataPath.c_str())));

  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  CHECK((false == TRI_IsDirectory(createDataPath.c_str()))); // createView(...) will call open()
  auto logicalView = vocbase.createView(createJson->slice(), 0);
  REQUIRE((false == !logicalView));
  auto view = logicalView->getImplementation();
  REQUIRE((false == !view));

  CHECK((true == TRI_IsDirectory(createDataPath.c_str())));
  CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));
  CHECK((false == TRI_IsDirectory(createDataPath.c_str())));
  CHECK((true == TRI_IsDirectory(updateDataPath.c_str())));
}

SECTION("test_open") {
  // absolute data path
  {
    std::string dataPath = ((irs::utf8_path()/=s.testFilesystemPath)/=std::string("deleteme")).utf8();
    auto namedJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\" }");
    auto json = arangodb::velocypack::Parser::fromJson("{ \
      \"dataPath\": \"" + arangodb::basics::StringUtils::replace(dataPath, "\\", "/") + "\" \
    }");

    CHECK((false == TRI_IsDirectory(dataPath.c_str())));
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto view = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);

    REQUIRE((false == !view));
    CHECK((false == TRI_IsDirectory(dataPath.c_str())));
    view->open();
    CHECK((true == TRI_IsDirectory(dataPath.c_str())));
  }

  auto* dbPathFeature = arangodb::application_features::ApplicationServer::getFeature<arangodb::DatabasePathFeature>("DatabasePath");
  auto origDirectory = dbPathFeature->directory();
  auto restoreDirectory = irs::make_finally([dbPathFeature, &origDirectory]()->void{
    const_cast<std::string&>(dbPathFeature->directory()) = origDirectory;
  });

  // relative data path
  {
    arangodb::options::ProgramOptions options("", "", "", nullptr);

    options.addPositional((irs::utf8_path()/=s.testFilesystemPath).utf8());
    dbPathFeature->validateOptions(std::shared_ptr<decltype(options)>(&options, [](decltype(options)*){})); // set data directory

    std::string dataPath = (((irs::utf8_path()/=s.testFilesystemPath)/=std::string("databases"))/=std::string("deleteme")).utf8();
    auto namedJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\" }");
    auto json = arangodb::velocypack::Parser::fromJson("{ \
      \"dataPath\": \"deleteme\" \
    }");

    CHECK((false == TRI_IsDirectory(dataPath.c_str())));
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto view = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !view));
    CHECK((false == TRI_IsDirectory(dataPath.c_str())));
    view->open();
    CHECK((true == TRI_IsDirectory(dataPath.c_str())));
  }

  // default data path
  {
    arangodb::options::ProgramOptions options("", "", "", nullptr);

    options.addPositional((irs::utf8_path()/=s.testFilesystemPath).utf8());
    dbPathFeature->validateOptions(std::shared_ptr<decltype(options)>(&options, [](decltype(options)*){})); // set data directory

    std::string dataPath = (((irs::utf8_path()/=s.testFilesystemPath)/=std::string("databases"))/=std::string("testType-123")).utf8();
    auto namedJson = arangodb::velocypack::Parser::fromJson("{ \"id\": 123, \"name\": \"testView\", \"type\": \"testType\" }");
    auto json = arangodb::velocypack::Parser::fromJson("{}");

    CHECK((false == TRI_IsDirectory(dataPath.c_str())));
    arangodb::LogicalView logicalView(nullptr, namedJson->slice());
    auto view = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), false);
    CHECK((false == !view));
    CHECK((false == TRI_IsDirectory(dataPath.c_str())));
    view->open();
    CHECK((true == TRI_IsDirectory(dataPath.c_str())));
  }
}

SECTION("test_query") {
  auto createJson = arangodb::velocypack::Parser::fromJson("{ \
    \"name\": \"testView\", \
    \"type\": \"iresearch\" \
  }");
  static std::vector<std::string> const EMPTY;
  arangodb::aql::AstNode noop(arangodb::aql::AstNodeType::NODE_TYPE_FILTER);
  arangodb::aql::AstNode noopChild(true, arangodb::aql::AstNodeValueType::VALUE_TYPE_BOOL); // all

  noop.addMember(&noopChild);

  // no filter/order provided, means "RETURN *"
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    REQUIRE((false == !view));

    arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
    CHECK((trx.begin().ok()));
    CHECK(0 == view->snapshot(trx).docs_count());
  }

  // ordered iterator
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    CHECK((false == !logicalView));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    CHECK((false == !view));

    // fill with test data
    {
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
      CHECK((trx.begin().ok()));

      for (size_t i = 0; i < 12; ++i) {
        view->insert(trx, 1, arangodb::LocalDocumentId(i), doc->slice(), meta);
      }

      CHECK((trx.commit().ok()));
      view->sync();
    }

    arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
    CHECK((trx.begin().ok()));
    CHECK(12 == view->snapshot(trx).docs_count());
  }

  // snapshot isolation
  {
    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": { \"includeAllFields\" : true } } \
    }");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");

    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    std::vector<std::string> collections{ logicalCollection->name() };
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    CHECK((false == !logicalView));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    CHECK((false == !view));
    arangodb::Result res = logicalView->updateProperties(links->slice(), true, false);
    CHECK(true == res.ok());
    CHECK((false == logicalCollection->getIndexes().empty()));

    // fill with test data
    {
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, collections, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));

      arangodb::ManagedDocumentResult inserted;
      TRI_voc_tick_t tick;
      arangodb::OperationOptions options;
      for (size_t i = 1; i <= 12; ++i) {
        auto doc = arangodb::velocypack::Parser::fromJson(std::string("{ \"key\": ") + std::to_string(i) + " }");
        logicalCollection->insert(&trx, doc->slice(), inserted, options, tick, false);
      }

      CHECK((trx.commit().ok()));
      view->sync();
    }

    arangodb::transaction::UserTransaction readTrx(
      arangodb::transaction::StandaloneContext::Create(&vocbase), collections, EMPTY, EMPTY, arangodb::transaction::Options()
    );
    CHECK((readTrx.begin().ok()));
    auto reader = view->snapshot(readTrx);
    CHECK(12 == reader.docs_count());

    // add more data
    {
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, collections, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));

      arangodb::ManagedDocumentResult inserted;
      TRI_voc_tick_t tick;
      arangodb::OperationOptions options;
      for (size_t i = 13; i <= 24; ++i) {
        auto doc = arangodb::velocypack::Parser::fromJson(std::string("{ \"key\": ") + std::to_string(i) + " }");
        logicalCollection->insert(&trx, doc->slice(), inserted, options, tick, false);
      }

      CHECK(trx.commit().ok());
      CHECK(view->sync());
    }

    // old reader sees same data as before
    CHECK(12 == reader.docs_count());
    // new reader sees new data
    CHECK(24 == view->snapshot(readTrx).docs_count());
  }

  // query while running FlushThread
  {
    auto dataPath = arangodb::basics::StringUtils::replace(((irs::utf8_path()/=s.testFilesystemPath)/=std::string("deleteme")).utf8(), "\\", "/");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"iresearch\", \"properties\": { \"dataPath\": \"" + dataPath + "\" } }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": { \"includeAllFields\": true } } }");
    auto* feature = arangodb::iresearch::getFeature<arangodb::FlushFeature>("Flush");
    REQUIRE(feature);
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto logicalView = vocbase.createView(viewCreateJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    REQUIRE((false == !view));
    arangodb::Result res = logicalView->updateProperties(viewUpdateJson->slice(), true, false);
    REQUIRE(true == res.ok());

    // start flush thread
    auto flush = std::make_shared<std::atomic<bool>>(true);
    std::thread flushThread([feature, flush]()->void{
      while (flush->load()) {
        feature->executeCallbacks();
      }
    });
    auto flushStop = irs::make_finally([flush, &flushThread]()->void{
      flush->store(false);
      flushThread.join();
    });

    static std::vector<std::string> const EMPTY;
    arangodb::transaction::Options options;

    options.waitForSync = true;

    arangodb::aql::Variable variable("testVariable", 0);

    // test insert + query
    for (size_t i = 1; i < 200; ++i) {
      // insert
      {
        auto doc = arangodb::velocypack::Parser::fromJson(std::string("{ \"seq\": ") + std::to_string(i) + " }");
        arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, options);

        CHECK((trx.begin().ok()));
        CHECK((trx.insert(logicalCollection->name(), doc->slice(), arangodb::OperationOptions()).ok()));
        CHECK((trx.commit().ok()));
      }

      // query
      {
        arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());
        CHECK(i == view->snapshot(trx).docs_count());
      }
    }
  }
}

SECTION("test_register_link") {
  bool persisted = false;
  auto before = PhysicalViewMock::before;
  auto restore = irs::make_finally([&before]()->void { PhysicalViewMock::before = before; });
  PhysicalViewMock::before = [&persisted]()->void { persisted = true; };

  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\", \"id\": 100 }");
  auto viewJson0 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"iresearch\", \"id\": 101 }");
  auto viewJson1 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"iresearch\", \"id\": 101, \"properties\": { \"collections\": [ 100 ] } }");
  auto linkJson  = arangodb::velocypack::Parser::fromJson("{ \"view\": 101 }");

  // new link in recovery
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto logicalView = vocbase.createView(viewJson0->slice(), 0);
    REQUIRE((false == !logicalView));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    REQUIRE((false == !view));

    CHECK((0 == view->linkCount()));

    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    persisted = false;
    auto link = arangodb::iresearch::IResearchMMFilesLink::make(1, logicalCollection, linkJson->slice());
    CHECK((false == persisted));
    CHECK((false == !link));
    CHECK((1 == view->linkCount()));
  }

  // new link
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto logicalView = vocbase.createView(viewJson0->slice(), 0);
    REQUIRE((false == !logicalView));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    REQUIRE((false == !view));

    CHECK((0 == view->linkCount()));

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 123 };
      std::unordered_set<TRI_voc_cid_t> actual = { 123 };
      CHECK((view->appendKnownCollections(actual)));

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }

    persisted = false;
    auto link = arangodb::iresearch::IResearchMMFilesLink::make(1, logicalCollection, linkJson->slice());
    CHECK((true == persisted));
    CHECK((false == !link));
    CHECK((1 == view->linkCount()));

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100, 123 };
      std::unordered_set<TRI_voc_cid_t> actual = { 123 };
      CHECK((view->appendKnownCollections(actual)));

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }
  }

  // known link
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto logicalView = vocbase.createView(viewJson1->slice(), 0);
    REQUIRE((false == !logicalView));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    REQUIRE((false == !view));

    CHECK((1 == view->linkCount()));

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100, 123 };
      std::unordered_set<TRI_voc_cid_t> actual = { 123 };
      CHECK((view->appendKnownCollections(actual)));

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }

    persisted = false;
    auto link0 = arangodb::iresearch::IResearchMMFilesLink::make(1, logicalCollection, linkJson->slice());
    CHECK((false == persisted));
    CHECK((false == !link0));
    CHECK((1 == view->linkCount()));

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100, 123 };
      std::unordered_set<TRI_voc_cid_t> actual = { 123 };
      CHECK((view->appendKnownCollections(actual)));

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }

    persisted = false;
    auto link1 = arangodb::iresearch::IResearchMMFilesLink::make(1, logicalCollection, linkJson->slice());
    CHECK((false == persisted));
    CHECK((true == !link1));
    CHECK((1 == view->linkCount()));

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100, 123 };
      std::unordered_set<TRI_voc_cid_t> actual = { 123 };
      CHECK((view->appendKnownCollections(actual)));

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }
  }
}

SECTION("test_unregister_link") {
  bool persisted = false;
  auto before = PhysicalViewMock::before;
  auto restore = irs::make_finally([&before]()->void { PhysicalViewMock::before = before; });
  PhysicalViewMock::before = [&persisted]()->void { persisted = true; };

  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\", \"id\": 100 }");
  auto viewJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"iresearch\", \"id\": 101, \"properties\": { } }");

  // link removed before view (in recovery)
  {
    VocbaseWrapper vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase->createCollection(collectionJson->slice());
    auto logicalView = vocbase->createView(viewJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    REQUIRE((false == !view));

    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": {} } \
    }");

    arangodb::Result res = logicalView->updateProperties(links->slice(), true, false);
    CHECK(true == res.ok());
    CHECK((false == logicalCollection->getIndexes().empty()));

    CHECK((1 == view->linkCount()));

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100 };
      std::unordered_set<TRI_voc_cid_t> actual = { };
      CHECK((view->appendKnownCollections(actual)));

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }

    CHECK((nullptr != vocbase->lookupCollection("testCollection")));

    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    persisted = false;
    CHECK((TRI_ERROR_NO_ERROR == vocbase->dropCollection(logicalCollection, true, -1)));
    CHECK((false == persisted));
    CHECK((nullptr == vocbase->lookupCollection("testCollection")));
    CHECK((0 == view->linkCount()));

    {
      std::unordered_set<TRI_voc_cid_t> actual = { };
      CHECK((view->appendKnownCollections(actual)));
      CHECK((actual.empty()));
    }

    CHECK((false == !vocbase->lookupView("testView")));
    CHECK((TRI_ERROR_NO_ERROR == vocbase->dropView("testView")));
    CHECK((true == !vocbase->lookupView("testView")));
  }

  // link removed before view
  {
    VocbaseWrapper vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase->createCollection(collectionJson->slice());
    auto logicalView = vocbase->createView(viewJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    REQUIRE((false == !view));

    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": {} } \
    }");

    arangodb::Result res = logicalView->updateProperties(links->slice(), true, false);
    CHECK(true == res.ok());
    CHECK((false == logicalCollection->getIndexes().empty()));

    CHECK((1 == view->linkCount()));

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100 };
      std::unordered_set<TRI_voc_cid_t> actual = { };
      CHECK((view->appendKnownCollections(actual)));

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }

    CHECK((nullptr != vocbase->lookupCollection("testCollection")));
    persisted = false;
    CHECK((TRI_ERROR_NO_ERROR == vocbase->dropCollection(logicalCollection, true, -1)));
    CHECK((true == persisted));
    CHECK((nullptr == vocbase->lookupCollection("testCollection")));
    CHECK((0 == view->linkCount()));

    {
      std::unordered_set<TRI_voc_cid_t> actual = { };
      CHECK((view->appendKnownCollections(actual)));
      CHECK((actual.empty()));
    }

    CHECK((false == !vocbase->lookupView("testView")));
    CHECK((TRI_ERROR_NO_ERROR == vocbase->dropView("testView")));
    CHECK((true == !vocbase->lookupView("testView")));
  }

  // view removed before link
  {
    VocbaseWrapper vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase->createCollection(collectionJson->slice());
    auto logicalView = vocbase->createView(viewJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto* view = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
    REQUIRE((false == !view));

    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": {} } \
    }");

    arangodb::Result res = logicalView->updateProperties(links->slice(), true, false);
    CHECK(true == res.ok());
    CHECK((false == logicalCollection->getIndexes().empty()));

    CHECK((1 == view->linkCount()));
    CHECK((false == !vocbase->lookupView("testView")));
    CHECK((TRI_ERROR_NO_ERROR == vocbase->dropView("testView")));
    CHECK((true == !vocbase->lookupView("testView")));
    CHECK((nullptr != vocbase->lookupCollection("testCollection")));
    CHECK((TRI_ERROR_NO_ERROR == vocbase->dropCollection(logicalCollection, true, -1)));
    CHECK((nullptr == vocbase->lookupCollection("testCollection")));
  }

  // view deallocated before link removed
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());

    {
      auto createJson = arangodb::velocypack::Parser::fromJson("{}");
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": {} } }");
      auto logicalView = vocbase.createView(viewJson->slice(), 0);
      REQUIRE((false == !logicalView));
      auto* viewImpl = dynamic_cast<arangodb::iresearch::IResearchView*>(logicalView->getImplementation());
      REQUIRE((nullptr != viewImpl));
      CHECK((viewImpl->updateProperties(updateJson->slice(), true, false).ok()));
      CHECK((false == logicalCollection->getIndexes().empty()));
      CHECK((1 == viewImpl->linkCount()));

      auto factory = [](arangodb::LogicalView*, arangodb::velocypack::Slice const&, bool isNew)->std::unique_ptr<arangodb::ViewImplementation>{ return nullptr; };
      logicalView->spawnImplementation(factory, createJson->slice(), true); // ensure destructor for ViewImplementation is called
      CHECK((false == logicalCollection->getIndexes().empty()));
    }

    // create a new view with same ID to validate links
    {
      auto json = arangodb::velocypack::Parser::fromJson("{}");
      arangodb::LogicalView logicalView(&vocbase, viewJson->slice());
      auto view = arangodb::iresearch::IResearchView::make(&logicalView, json->slice(), true);
      REQUIRE((false == !view));
      auto* viewImpl = dynamic_cast<arangodb::iresearch::IResearchView*>(view.get());
      REQUIRE((nullptr != viewImpl));
      CHECK((0 == viewImpl->linkCount()));

      for (auto& index: logicalCollection->getIndexes()) {
        auto* link = dynamic_cast<arangodb::iresearch::IResearchLink*>(index.get());
        REQUIRE((*link != *viewImpl)); // check that link is unregistred from view
      }
    }
  }
}

SECTION("test_update_overwrite") {
  auto createJson = arangodb::velocypack::Parser::fromJson("{ \
    \"name\": \"testView\", \
    \"type\": \"iresearch\" \
  }");

  // modify meta params
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    // initial update (overwrite)
    {
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
        \"locale\": \"en\", \
        \"threadsMaxIdle\": 10, \
        \"threadsMaxTotal\": 20 \
      }");

      expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());
      expectedMeta._locale = irs::locale_utils::locale("en", true);
      expectedMeta._threadsMaxIdle = 10;
      expectedMeta._threadsMaxTotal = 20;
      CHECK((view->updateProperties(updateJson->slice(), false, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      CHECK((7U == slice.length()));
      CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

      auto tmpSlice = slice.get("links");
      CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
    }

    // subsequent update (overwrite)
    {
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
        \"locale\": \"ru\" \
      }");

      expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());
      expectedMeta._locale = irs::locale_utils::locale("ru", true);
      CHECK((view->updateProperties(updateJson->slice(), false, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      CHECK((7U == slice.length()));
      CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

      auto tmpSlice = slice.get("links");
      CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
    }
  }

  // overwrite links
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson0 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection0\" }");
    auto collectionJson1 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection1\" }");
    auto* logicalCollection0 = vocbase.createCollection(collectionJson0->slice());
    REQUIRE((nullptr != logicalCollection0));
    auto* logicalCollection1 = vocbase.createCollection(collectionJson1->slice());
    REQUIRE((nullptr != logicalCollection1));
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));
    CHECK((true == logicalCollection0->getIndexes().empty()));
    CHECK((true == logicalCollection1->getIndexes().empty()));

    // initial creation
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection0\": {} } }");
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      std::unordered_map<std::string, arangodb::iresearch::IResearchLinkMeta> expectedLinkMeta;

      expectedMeta._collections.insert(logicalCollection0->cid());
      expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());
      expectedLinkMeta["testCollection0"]; // use defaults
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      CHECK((7U == slice.length()));
      CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

      auto tmpSlice = slice.get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

      for (arangodb::velocypack::ObjectIterator itr(tmpSlice); itr.valid(); ++itr) {
        arangodb::iresearch::IResearchLinkMeta linkMeta;
        auto key = itr.key();
        auto value = itr.value();
        CHECK((true == key.isString()));

        auto expectedItr = expectedLinkMeta.find(key.copyString());
        CHECK((
          true == value.isObject()
          && expectedItr != expectedLinkMeta.end()
          && linkMeta.init(value, error)
          && expectedItr->second == linkMeta
        ));
        expectedLinkMeta.erase(expectedItr);
      }

      CHECK((true == expectedLinkMeta.empty()));
      CHECK((false == logicalCollection0->getIndexes().empty()));
      CHECK((true == logicalCollection1->getIndexes().empty()));
    }

    // update overwrite links
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection1\": {} } }");
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      std::unordered_map<std::string, arangodb::iresearch::IResearchLinkMeta> expectedLinkMeta;

      expectedMeta._collections.insert(logicalCollection1->cid());
      expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());
      expectedLinkMeta["testCollection1"]; // use defaults
      CHECK((view->updateProperties(updateJson->slice(), false, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      CHECK((7U == slice.length()));
      CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

      auto tmpSlice = slice.get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

      for (arangodb::velocypack::ObjectIterator itr(tmpSlice); itr.valid(); ++itr) {
        arangodb::iresearch::IResearchLinkMeta linkMeta;
        auto key = itr.key();
        auto value = itr.value();
        CHECK((true == key.isString()));

        auto expectedItr = expectedLinkMeta.find(key.copyString());
        CHECK((
          true == value.isObject()
          && expectedItr != expectedLinkMeta.end()
          && linkMeta.init(value, error)
          && expectedItr->second == linkMeta
        ));
        expectedLinkMeta.erase(expectedItr);
      }

      CHECK((true == expectedLinkMeta.empty()));
      CHECK((true == logicalCollection0->getIndexes().empty()));
      CHECK((false == logicalCollection1->getIndexes().empty()));
    }
  }
}

SECTION("test_update_partial") {
  auto createJson = arangodb::velocypack::Parser::fromJson("{ \
    \"name\": \"testView\", \
    \"type\": \"iresearch\" \
  }");
  bool persisted = false;
  auto before = PhysicalViewMock::before;
  auto restore = irs::make_finally([&before]()->void { PhysicalViewMock::before = before; });
  PhysicalViewMock::before = [&persisted]()->void { persisted = true; };

  // modify meta params
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"locale\": \"en\", \
      \"threadsMaxIdle\": 10, \
      \"threadsMaxTotal\": 20 \
    }");

    expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());
    expectedMeta._locale = irs::locale_utils::locale("en", true);
    expectedMeta._threadsMaxIdle = 10;
    expectedMeta._threadsMaxTotal = 20;
    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

    auto tmpSlice = slice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // test rollback on meta modification failure
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    std::string dataPath = ((irs::utf8_path()/=s.testFilesystemPath)/=std::string("deleteme")).utf8();
    auto res = TRI_CreateDatafile(dataPath, 1); // create a file where the data path directory should be
    arangodb::iresearch::IResearchViewMeta expectedMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson(std::string() + "{ \
      \"dataPath\": \"" + arangodb::basics::StringUtils::replace(dataPath, "\\", "/") + "\", \
      \"locale\": \"en\", \
      \"threadsMaxIdle\": 10, \
      \"threadsMaxTotal\": 20 \
    }");

    expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());
    CHECK((TRI_ERROR_BAD_PARAMETER == view->updateProperties(updateJson->slice(), true, false).errorNumber()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

    auto tmpSlice = slice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // test rollback on persist failure
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"locale\": \"en\", \
      \"threadsMaxIdle\": 10, \
      \"threadsMaxTotal\": 20 \
    }");

    expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());

    PhysicalViewMock::persistPropertiesResult = TRI_ERROR_INTERNAL; // test fail
    CHECK((TRI_ERROR_INTERNAL == view->updateProperties(updateJson->slice(), true, false).errorNumber()));
    PhysicalViewMock::persistPropertiesResult = TRI_ERROR_NO_ERROR; // revert to valid

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

    auto tmpSlice = slice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // add a new link (in recovery)
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    auto updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": { \"testCollection\": {} } }"
    );

    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    persisted = false;
    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));
    CHECK((false == persisted));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    CHECK((
      true == slice.hasKey("links")
      && slice.get("links").isObject()
      && 1 == slice.get("links").length()
    ));
  }

  // add a new link
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    std::unordered_map<std::string, arangodb::iresearch::IResearchLinkMeta> expectedLinkMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": {} \
      }}");

    expectedMeta._collections.insert(logicalCollection->cid());
    expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());
    expectedLinkMeta["testCollection"]; // use defaults
    persisted = false;
    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));
    CHECK((true == persisted));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

    auto tmpSlice = slice.get("links");
    CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

    for (arangodb::velocypack::ObjectIterator itr(tmpSlice); itr.valid(); ++itr) {
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      auto key = itr.key();
      auto value = itr.value();
      CHECK((true == key.isString()));

      auto expectedItr = expectedLinkMeta.find(key.copyString());
      CHECK((
        true == value.isObject()
        && expectedItr != expectedLinkMeta.end()
        && linkMeta.init(value, error)
        && expectedItr->second == linkMeta
      ));
      expectedLinkMeta.erase(expectedItr);
    }

    CHECK((true == expectedLinkMeta.empty()));
  }

  // add a new link to a collection with documents
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    {
      static std::vector<std::string> const EMPTY;
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"abc\": \"def\" }");
      arangodb::transaction::UserTransaction trx(arangodb::transaction::StandaloneContext::Create(&vocbase), EMPTY, EMPTY, EMPTY, arangodb::transaction::Options());

      CHECK((trx.begin().ok()));
      CHECK((trx.insert(logicalCollection->name(), doc->slice(), arangodb::OperationOptions()).ok()));
      CHECK((trx.commit().ok()));
    }

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    std::unordered_map<std::string, arangodb::iresearch::IResearchLinkMeta> expectedLinkMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": {} \
      }}");

    expectedMeta._collections.insert(logicalCollection->cid());
    expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());
    expectedLinkMeta["testCollection"]; // use defaults
    persisted = false;
    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));
    CHECK((true == persisted));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

    auto tmpSlice = slice.get("links");
    CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

    for (arangodb::velocypack::ObjectIterator itr(tmpSlice); itr.valid(); ++itr) {
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      auto key = itr.key();
      auto value = itr.value();
      CHECK((true == key.isString()));

      auto expectedItr = expectedLinkMeta.find(key.copyString());
      CHECK((
        true == value.isObject()
        && expectedItr != expectedLinkMeta.end()
        && linkMeta.init(value, error)
        && expectedItr->second == linkMeta
      ));
      expectedLinkMeta.erase(expectedItr);
    }

    CHECK((true == expectedLinkMeta.empty()));
  }

  // add new link to non-existant collection
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());

    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": {} \
      }}");

    CHECK((TRI_ERROR_BAD_PARAMETER == view->updateProperties(updateJson->slice(), true, false).errorNumber()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

    auto tmpSlice = slice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // remove link (in recovery)
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": {} } }"
      );
      persisted = false;
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));
      CHECK((true == persisted));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      CHECK((
        true == slice.hasKey("links")
        && slice.get("links").isObject()
        && 1 == slice.get("links").length()
      ));
    }

    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": null } }"
      );

      auto before = StorageEngineMock::inRecoveryResult;
      StorageEngineMock::inRecoveryResult = true;
      auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
      persisted = false;
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));
      CHECK((false == persisted));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      CHECK((
        true == slice.hasKey("links")
        && slice.get("links").isObject()
        && 0 == slice.get("links").length()
      ));
    }
  }

  // remove link
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    expectedMeta._collections.insert(logicalCollection->cid());
    expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());

    {
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
        \"links\": { \
          \"testCollection\": {} \
      }}");

      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      CHECK((7U == slice.length()));
      CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

      auto tmpSlice = slice.get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
    }

    {
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
        \"links\": { \
          \"testCollection\": null \
      }}");

      expectedMeta._collections.clear();
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      CHECK((7U == slice.length()));
      CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

      auto tmpSlice = slice.get("links");
      CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
    }
  }

  // remove link from non-existant collection
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());

    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": null \
      }}");

    CHECK((TRI_ERROR_BAD_PARAMETER == view->updateProperties(updateJson->slice(), true, false).errorNumber()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

    auto tmpSlice = slice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // remove non-existant link
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    expectedMeta._dataPath = std::string("iresearch-") + std::to_string(logicalView->id());

    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": null \
    }}");

    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->getPropertiesVPack(builder, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK((7U == slice.length()));
    CHECK((meta.init(slice, error, *logicalView) && expectedMeta == meta));

    auto tmpSlice = slice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // remove + add link to same collection (reindex)
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto logicalView = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !logicalView));
    auto view = logicalView->getImplementation();
    REQUIRE((false == !view));

    // initial add of link
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": {} } }"
      );
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      auto tmpSlice = slice.get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
    }

    // add + remove
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": null, \"testCollection\": {} } }"
      );
      std::unordered_set<TRI_idx_iid_t> initial;

      for (auto& idx: logicalCollection->getIndexes()) {
        initial.emplace(idx->id());
      }

      CHECK((!initial.empty()));
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));
      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->getPropertiesVPack(builder, false);
      builder.close();

      auto slice = builder.slice();
      auto tmpSlice = slice.get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

      std::unordered_set<TRI_idx_iid_t> actual;

      for (auto& index: logicalCollection->getIndexes()) {
        actual.emplace(index->id());
      }

      CHECK((initial != actual)); // a reindexing took place (link recreated)
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generate tests
////////////////////////////////////////////////////////////////////////////////

}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------