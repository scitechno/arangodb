////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Jan Steemann
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_CLUSTER_AGENCY_COMM_H
#define ARANGOD_CLUSTER_AGENCY_COMM_H 1

#include "Basics/Common.h"
#include "Basics/ReadWriteLock.h"
#include "Basics/json.h"
#include "Rest/HttpRequest.h"
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>
#include <type_traits>

#include <list>

namespace arangodb {
class Endpoint;

namespace httpclient {
class GeneralClientConnection;
}

namespace velocypack {
class Builder;
class Slice;
}

class AgencyComm;

struct AgencyEndpoint {

  //////////////////////////////////////////////////////////////////////////////
  /// @brief creates an agency endpoint
  //////////////////////////////////////////////////////////////////////////////

  AgencyEndpoint(Endpoint*,
                 arangodb::httpclient::GeneralClientConnection*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroys an agency endpoint
  //////////////////////////////////////////////////////////////////////////////

  ~AgencyEndpoint();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the endpoint
  //////////////////////////////////////////////////////////////////////////////

  Endpoint* _endpoint;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the connection
  //////////////////////////////////////////////////////////////////////////////

  arangodb::httpclient::GeneralClientConnection* _connection;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the endpoint is busy
  //////////////////////////////////////////////////////////////////////////////

  bool _busy;
};

struct AgencyConnectionOptions {
  double _connectTimeout;
  double _requestTimeout;
  double _lockTimeout;
  size_t _connectRetries;
};

struct AgencyCommResultEntry {
  uint64_t _index;
  std::shared_ptr<arangodb::velocypack::Builder> _vpack;
  bool _isDir;
};

enum class AgencyValueOperationType {
  SET,
  OBSERVE,
  UNOBSERVE,
  PUSH,
  PREPEND
};

enum class AgencySimpleOperationType {
  INCREMENT_OP,
  DECREMENT_OP,
  DELETE_OP,
  POP_OP,
  SHIFT_OP
};

struct AgencyOperationType {
  enum {VALUE, SIMPLE} type;
  union {
    AgencyValueOperationType value;
    AgencySimpleOperationType simple;
  };

  // mop: hmmm...explicit implementation...maybe use to_string?
  std::string toString() {
    switch(type) {
      case VALUE:
        switch(value) {
          case AgencyValueOperationType::SET:
            return "set";
          case AgencyValueOperationType::OBSERVE:
            return "observe";
          case AgencyValueOperationType::UNOBSERVE:
            return "unobserve";
          case AgencyValueOperationType::PUSH:
            return "push";
          case AgencyValueOperationType::PREPEND:
            return "prepend";
          default:
            return "unknown_operation_type";
        }
        break;
      case SIMPLE:
        switch(simple) {
          case AgencySimpleOperationType::INCREMENT_OP:
            return "increment";
          case AgencySimpleOperationType::DECREMENT_OP:
            return "decrement";
          case AgencySimpleOperationType::DELETE_OP:
            return "delete";
          case AgencySimpleOperationType::POP_OP:
            return "pop";
          case AgencySimpleOperationType::SHIFT_OP:
            return "shift";
          default:
            return "unknown_operation_type";
        }
      default:
        return "unknown_operation_type";
    }
  }
};

struct AgencyOperationPrecondition {
  AgencyOperationPrecondition() : type(NONE) {}
  AgencyOperationPrecondition(AgencyOperationPrecondition const& other) {
    type = other.type;
    switch(type) {
      case NONE:
        break;
      case EMPTY:
        empty = other.empty;
        break;
      case VALUE:
        value = other.value;
        break;
    }
  }

  enum {NONE, EMPTY, VALUE} type;
  union {
    bool empty;
    VPackSlice value;
  };
};

struct AgencyOperation {

  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructs an operation
  //////////////////////////////////////////////////////////////////////////////

  AgencyOperation(std::string const& key, AgencySimpleOperationType opType);
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructs an operation with value
  //////////////////////////////////////////////////////////////////////////////

  AgencyOperation(
      std::string const& key,
      AgencyValueOperationType opType,
      VPackSlice value
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns to full operation formatted as a vpack slice
  //////////////////////////////////////////////////////////////////////////////

  std::shared_ptr<arangodb::velocypack::Builder> toVelocyPack();
  uint32_t _ttl = 0;
  VPackSlice _oldValue;
  AgencyOperationPrecondition _precondition;
  
private:
  std::string const _key;
  AgencyOperationType _opType;
  VPackSlice _value;
};

struct AgencyTransaction {

  //////////////////////////////////////////////////////////////////////////////
  /// @brief vector of operations
  //////////////////////////////////////////////////////////////////////////////

  std::vector<AgencyOperation> operations;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief converts the transaction to json
  //////////////////////////////////////////////////////////////////////////////

  std::string toJson() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief shortcut to create a transaction with one operation
  //////////////////////////////////////////////////////////////////////////////
  explicit AgencyTransaction(AgencyOperation const& operation) {
    operations.push_back(operation);
  }
  
  explicit AgencyTransaction() {
  }
};

struct AgencyCommResult {

  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructs a communication result
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroys a communication result
  //////////////////////////////////////////////////////////////////////////////

  ~AgencyCommResult();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns whether the last request was successful
  //////////////////////////////////////////////////////////////////////////////

  inline bool successful() const {
    return (_statusCode >= 200 && _statusCode <= 299);
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief extract the connected flag from the result
  //////////////////////////////////////////////////////////////////////////////

  bool connected() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief extract the http code from the result
  //////////////////////////////////////////////////////////////////////////////

  int httpCode() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief extract the "index" attribute from the result
  //////////////////////////////////////////////////////////////////////////////

  uint64_t index() const { return _index; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief extract the error code from the result
  //////////////////////////////////////////////////////////////////////////////

  int errorCode() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief extract the error message from the result
  /// if there is no error, an empty string will be returned
  //////////////////////////////////////////////////////////////////////////////

  std::string errorMessage() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief extract the error details from the result
  /// if there is no error, an empty string will be returned
  //////////////////////////////////////////////////////////////////////////////

  std::string errorDetails() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return the location header (might be empty)
  //////////////////////////////////////////////////////////////////////////////

  std::string const location() const { return _location; }
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief return the body (might be empty)
  //////////////////////////////////////////////////////////////////////////////

  std::string const body() const { return _body; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief flush the internal result buffer
  //////////////////////////////////////////////////////////////////////////////

  void clear();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief recursively flatten the VelocyPack response into a map
  ///
  /// stripKeyPrefix is decoded, as is the _globalPrefix
  //////////////////////////////////////////////////////////////////////////////

  bool parseVelocyPackNode(arangodb::velocypack::Slice const&,
                           std::string const&, bool);

  //////////////////////////////////////////////////////////////////////////////
  /// parse an agency result
  /// note that stripKeyPrefix is a decoded, normal key!
  //////////////////////////////////////////////////////////////////////////////

  bool parse(std::string const&, bool);

 public:
  std::string _location;
  std::string _message;
  std::string _body;
  std::string _realBody;

  std::map<std::string, AgencyCommResultEntry> _values;
  uint64_t _index;
  int _statusCode;
  bool _connected;
};

class AgencyCommLocker {

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructs an agency comm locker
  ///
  /// The keys mentioned in this class are all not yet encoded.
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommLocker(std::string const&, std::string const&, double = 0.0, double = 0.0);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroys an agency comm locker
  //////////////////////////////////////////////////////////////////////////////

  ~AgencyCommLocker();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief return whether the locking was successful
  //////////////////////////////////////////////////////////////////////////////

  bool successful() const { return _isLocked; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief unlocks the lock
  //////////////////////////////////////////////////////////////////////////////

  void unlock();

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief update a lock version in the agency
  //////////////////////////////////////////////////////////////////////////////

  bool updateVersion(AgencyComm&);

 private:
  std::string const _key;
  std::string const _type;
  std::shared_ptr<arangodb::velocypack::Builder> _vpack;
  bool _isLocked;
};

class AgencyComm {
  friend struct AgencyCommResult;
  friend class AgencyCommLocker;

 public:

  //////////////////////////////////////////////////////////////////////////////
  /// @brief cleans up all connections
  //////////////////////////////////////////////////////////////////////////////

  static void cleanup();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief initialize agency comm channel
  //////////////////////////////////////////////////////////////////////////////

  static bool initialize();
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief disconnects all communication channels
  //////////////////////////////////////////////////////////////////////////////

  static void disconnect();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief adds an endpoint to the agents list
  //////////////////////////////////////////////////////////////////////////////

  static bool addEndpoint(std::string const&, bool = false);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief checks if an endpoint is present
  //////////////////////////////////////////////////////////////////////////////

  static bool hasEndpoint(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get a stringified version of the endpoints
  //////////////////////////////////////////////////////////////////////////////

  static std::vector<std::string> getEndpoints();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get a stringified version of the endpoints
  //////////////////////////////////////////////////////////////////////////////

  static std::string getEndpointsString();
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief get a stringified version of the endpoints (unique)
  //////////////////////////////////////////////////////////////////////////////

  static std::string getUniqueEndpointsString();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the global prefix for all operations
  //////////////////////////////////////////////////////////////////////////////

  static bool setPrefix(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the global prefix for all operations
  //////////////////////////////////////////////////////////////////////////////

  static std::string prefix();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief generate a timestamp
  //////////////////////////////////////////////////////////////////////////////

  static std::string generateStamp();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief creates a new agency endpoint
  //////////////////////////////////////////////////////////////////////////////

  static AgencyEndpoint* createAgencyEndpoint(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sends the current server state to the agency
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult sendServerState(double ttl);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief gets the backend version
  //////////////////////////////////////////////////////////////////////////////

  std::string getVersion();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief update a version number in the agency
  //////////////////////////////////////////////////////////////////////////////

  inline bool increaseVersion(std::string const& key) {
    AgencyCommResult result = increment(key);
    return result.successful();
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief creates a directory in the backend
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult createDirectory(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets a value in the back end as string
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult setValue(std::string const&,
                            std::string const&, double);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets a value in the back end
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult setValue(std::string const&,
                            arangodb::velocypack::Slice const&, double);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief checks whether a key exists
  //////////////////////////////////////////////////////////////////////////////

  bool exists(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief gets one or multiple values from the back end
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult getValues(std::string const&, bool);
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief increment a value
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult increment(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief removes one or multiple values from the back end
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult removeValues(std::string const&, bool);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief compares and swaps a single value in the backend
  /// the CAS condition is whether or not a previous value existed for the key
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult casValue(std::string const&,
                            arangodb::velocypack::Slice const&, bool, double,
                            double);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief compares and swaps a single value in the back end
  /// the CAS condition is whether or not the previous value for the key was
  /// identical to `oldValue`
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult casValue(std::string const&,
                            arangodb::velocypack::Slice const&,
                            arangodb::velocypack::Slice const&, double, double);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get unique id
  //////////////////////////////////////////////////////////////////////////////

  AgencyCommResult uniqid(std::string const&, uint64_t, double);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief registers a callback on a key
  //////////////////////////////////////////////////////////////////////////////
  bool registerCallback(std::string const& key, std::string const& endpoint);
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief unregisters a callback on a key
  //////////////////////////////////////////////////////////////////////////////
  bool unregisterCallback(std::string const& key, std::string const& endpoint);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief acquire a read lock
  //////////////////////////////////////////////////////////////////////////////

  bool lockRead(std::string const&, double, double);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief acquire a write lock
  //////////////////////////////////////////////////////////////////////////////

  bool lockWrite(std::string const&, double, double);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief release a read lock
  //////////////////////////////////////////////////////////////////////////////

  bool unlockRead(std::string const&, double);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief release a write lock
  //////////////////////////////////////////////////////////////////////////////

  bool unlockWrite(std::string const&, double);

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief acquire a lock
  //////////////////////////////////////////////////////////////////////////////

  bool lock(std::string const&, double, double,
            arangodb::velocypack::Slice const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief release a lock
  //////////////////////////////////////////////////////////////////////////////

  bool unlock(std::string const&, arangodb::velocypack::Slice const&, double);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief pop an endpoint from the queue
  //////////////////////////////////////////////////////////////////////////////

  AgencyEndpoint* popEndpoint(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief reinsert an endpoint into the queue
  //////////////////////////////////////////////////////////////////////////////

  void requeueEndpoint(AgencyEndpoint*, bool);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief construct a URL, without a key
  //////////////////////////////////////////////////////////////////////////////

  std::string buildUrl() const;
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief sends a write HTTP request to the agency, handling failover
  //////////////////////////////////////////////////////////////////////////////

  bool sendWithFailover(
      arangodb::GeneralRequest::RequestType,
      double,
      AgencyCommResult&,
      std::string const&,
      std::string const&,
      bool
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sends a write HTTP request to the agency, handling failover
  //////////////////////////////////////////////////////////////////////////////

  bool sendTransactionWithFailover(
      AgencyCommResult&,
      AgencyTransaction const&
  );
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief sends data to the URL
  //////////////////////////////////////////////////////////////////////////////

  bool send(arangodb::httpclient::GeneralClientConnection*,
            arangodb::GeneralRequest::RequestType, double,
            AgencyCommResult&, std::string const&, std::string const&);
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief tries to establish a communication channel
  //////////////////////////////////////////////////////////////////////////////

  static bool tryConnect();
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief will initialize agency if it is freshly started
  //////////////////////////////////////////////////////////////////////////////

  bool ensureStructureInitialized();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief will try to initialize a new agency
  //////////////////////////////////////////////////////////////////////////////

  bool tryInitializeStructure();
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief initialize key in etcd
  //////////////////////////////////////////////////////////////////////////////

  bool initFromVPackSlice(std::string key, arangodb::velocypack::Slice s);
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief checks if the agency is initialized
  //////////////////////////////////////////////////////////////////////////////

  bool hasInitializedStructure();

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief the static global URL prefix
  //////////////////////////////////////////////////////////////////////////////

  static std::string const AGENCY_URL_PREFIX;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the (variable) global prefix
  //////////////////////////////////////////////////////////////////////////////

  static std::string _globalPrefix;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief endpoints lock
  //////////////////////////////////////////////////////////////////////////////

  static arangodb::basics::ReadWriteLock _globalLock;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief all endpoints
  //////////////////////////////////////////////////////////////////////////////

  static std::list<AgencyEndpoint*> _globalEndpoints;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief global connection options
  //////////////////////////////////////////////////////////////////////////////

  static AgencyConnectionOptions _globalConnectionOptions;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief number of connections per endpoint
  //////////////////////////////////////////////////////////////////////////////

  static size_t const NumConnections = 3;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief initial sleep time
  //////////////////////////////////////////////////////////////////////////////

  static unsigned long const InitialSleepTime = 5000;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief maximum sleep time
  //////////////////////////////////////////////////////////////////////////////

  static unsigned long const MaxSleepTime = 50000;
};
}

#endif
