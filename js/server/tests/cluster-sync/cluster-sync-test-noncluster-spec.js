/* global describe, it, before, beforeEach, afterEach */

// //////////////////////////////////////////////////////////////////////////////
// / @brief JavaScript cluster functionality
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2017 ArangoDB GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     http://www.apache.org/licenses/LICENSE-2.0
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is ArangoDB GmbH, Cologne, Germany
// /
// / @author Andreas Streichardt
// //////////////////////////////////////////////////////////////////////////////

const db = require('internal').db;
const cluster = require('@arangodb/cluster');
const expect = require('chai').expect;
const ArangoCollection = require('@arangodb/arango-collection').ArangoCollection;

describe('Cluster sync', function() {
  before(function() {
    require('@arangodb/sync-replication-debug').setup();
  });

  describe('Databaseplan to local', function() {
    beforeEach(function() {
      db._databases().forEach(database => {
        if (database !== '_system') {
          db._dropDatabase(database);
        }
      });
    });
    it('should create a planned database', function() {
      let plan = {
        "Databases": {
          "test": {
            "id": 1,
            "name": "test"
          }
        }
      };
      let errors = cluster.executePlanForDatabases(plan.Databases);
      let databases = db._databases();
      expect(databases).to.have.lengthOf(2);
      expect(databases).to.contain('test');
      expect(errors).to.be.empty;
    });
    it('should leave everything in place if a planned database already exists', function() {
      let plan = {
        Databases: {
          "test": {
            "id": 1,
            "name": "test"
          }
        }
      };
      db._createDatabase('test');
      let errors = cluster.executePlanForDatabases(plan.Databases);
      let databases = db._databases();
      expect(databases).to.have.lengthOf(2);
      expect(databases).to.contain('test');
      expect(errors).to.be.empty;
    });
    it('should delete a database if it is not used anymore', function() {
      db._createDatabase('peng');
      let plan = {
        Databases: {
        }
      };
      cluster.executePlanForDatabases(plan.Databases);
      let databases = db._databases();
      expect(databases).to.have.lengthOf(1);
      expect(databases).to.contain('_system');
    });
  });
  describe('Collection plan to local', function() {
    let numSystemCollections;
    before(function() {
      require('@arangodb/sync-replication-debug').setup();
    });

    beforeEach(function() {
      db._databases().forEach(database => {
        if (database !== '_system') {
          db._dropDatabase(database);
        }
      });
      db._createDatabase('test');
      db._useDatabase('test');
      numSystemCollections = db._collections().length;
    });
    afterEach(function() {
      db._useDatabase('_system');
    });
    it('should create and load a collection if it does not exist', function() {
      let plan = {
        Collections: {
          test: {
            "100001": {
              "deleted": false,
              "doCompact": true,
              "id": "100001",
              "indexBuckets": 8,
              "indexes": [
                {
                  "fields": [
                    "_key"
                  ],
                  "id": "0",
                  "sparse": false,
                  "type": "primary",
                  "unique": true
                }
              ],
              "isSystem": false,
              "isVolatile": false,
              "journalSize": 1048576,
              "keyOptions": {
                "allowUserKeys": true,
                "type": "traditional"
              },
              "name": "test",
              "numberOfShards": 1,
              "replicationFactor": 2,
              "shardKeys": [
                "_key"
              ],
              "shards": {
                "s100001": [
                  "repltest",
                ]
              },
              "status": 3,
              "type": 2,
              "waitForSync": false
            }
          }
        }
      };
      cluster.executePlanForCollections(plan.Collections);
      db._useDatabase('test');
      let collections = db._collections();
      expect(collections.map(collection => collection.name())).to.contain('s100001');
      expect(db._collection('s100001').status()).to.equal(ArangoCollection.STATUS_LOADED);
    });
    it('should create a collection if it does not exist (unloaded case)', function() {
      let plan = {
        Collections: {
          test: {
            "100001": {
              "deleted": false,
              "doCompact": true,
              "id": "100001",
              "indexBuckets": 8,
              "indexes": [
                {
                  "fields": [
                    "_key"
                  ],
                  "id": "0",
                  "sparse": false,
                  "type": "primary",
                  "unique": true
                }
              ],
              "isSystem": false,
              "isVolatile": false,
              "journalSize": 1048576,
              "keyOptions": {
                "allowUserKeys": true,
                "type": "traditional"
              },
              "name": "test",
              "numberOfShards": 1,
              "replicationFactor": 2,
              "shardKeys": [
                "_key"
              ],
              "shards": {
                "s100001": [
                  "repltest",
                ]
              },
              "status": 2,
              "type": 2,
              "waitForSync": false
            }
          }
        }
      };
      cluster.executePlanForCollections(plan.Collections);
      db._useDatabase('test');
      let collections = db._collections();
      expect(collections.map(collection => collection.name())).to.contain('s100001');
      expect(db._collection('s100001').status()).to.equal(ArangoCollection.STATUS_UNLOADED);
    });
    it('should unload an existing collection', function() {
      db._create('s100001');
      expect(db._collection('s100001').status()).to.equal(ArangoCollection.STATUS_LOADED);
      let plan = {
        test: {
          "100001": {
            "deleted": false,
            "doCompact": true,
            "id": "100001",
            "indexBuckets": 8,
            "indexes": [
              {
                "fields": [
                  "_key"
                ],
                "id": "0",
                "sparse": false,
                "type": "primary",
                "unique": true
              }
            ],
            "isSystem": false,
            "isVolatile": false,
            "journalSize": 1048576,
            "keyOptions": {
              "allowUserKeys": true,
              "type": "traditional"
            },
            "name": "test",
            "numberOfShards": 1,
            "replicationFactor": 2,
            "shardKeys": [
              "_key"
            ],
            "shards": {
              "s100001": [
                  "repltest",
              ]
            },
            "status": 2,
            "type": 2,
            "waitForSync": false
          }
        }
      };
      cluster.executePlanForCollections(plan);
      db._useDatabase('test');
      expect(db._collection('s100001').status()).to.equal(ArangoCollection.STATUS_UNLOADED);
    });
    it('should delete a stale collection', function() {
      db._create('s100001');
      let plan = {
        Collections: {
          test: {
          }
        }
      };
      cluster.executePlanForCollections(plan.Collections);
      db._useDatabase('test');
      let collections = db._collections();
      expect(collections).to.have.lengthOf(numSystemCollections);
    });
    it('should ignore a collection for which it is not responsible', function() {
      let plan = {
        Collections: {
          test: {
            "100001": {
              "deleted": false,
              "doCompact": true,
              "id": "100001",
              "indexBuckets": 8,
              "indexes": [
                {
                  "fields": [
                    "_key"
                  ],
                  "id": "0",
                  "sparse": false,
                  "type": "primary",
                  "unique": true
                }
              ],
              "isSystem": false,
              "isVolatile": false,
              "journalSize": 1048576,
              "keyOptions": {
                "allowUserKeys": true,
                "type": "traditional"
              },
              "name": "test",
              "numberOfShards": 1,
              "replicationFactor": 2,
              "shardKeys": [
                "_key"
              ],
              "shards": {
                "s100001": [
                  "swag",
                ]
              },
              "status": 3,
              "type": 2,
              "waitForSync": false
            }
          }
        }
      };
      cluster.executePlanForCollections(plan.Collections);
      db._useDatabase('test');
      let collections = db._collections();
      expect(collections).to.have.lengthOf(numSystemCollections);
    });
    it('should delete a collection for which it lost responsibility', function() {
      db._create('s100001');
      let plan = {
        Collections: {
          test: {
            "100001": {
              "deleted": false,
              "doCompact": true,
              "id": "100001",
              "indexBuckets": 8,
              "indexes": [
                {
                  "fields": [
                    "_key"
                  ],
                  "id": "0",
                  "sparse": false,
                  "type": "primary",
                  "unique": true
                }
              ],
              "isSystem": false,
              "isVolatile": false,
              "journalSize": 1048576,
              "keyOptions": {
                "allowUserKeys": true,
                "type": "traditional"
              },
              "name": "test",
              "numberOfShards": 1,
              "replicationFactor": 2,
              "shardKeys": [
                "_key"
              ],
              "shards": {
                "s100001": [
                  "debug-follower", // this is a different server than we are
                ]
              },
              "status": 2,
              "type": 2,
              "waitForSync": false
            }
          }
        }
      };
      cluster.executePlanForCollections(plan.Collections);
      db._useDatabase('test');
      let collections = db._collections();
      expect(collections).to.have.lengthOf(numSystemCollections);
    });
    it('should create an additional index if instructed to do so', function() {
      db._create('s100001');
      let plan = {
        Collections: {
          test: {
            "100001": {
              "deleted": false,
              "doCompact": true,
              "id": "100001",
              "indexBuckets": 8,
              "indexes": [
                {
                  "fields": [
                    "_key"
                  ],
                  "id": "0",
                  "sparse": false,
                  "type": "primary",
                  "unique": true
                },
                  {
                    "error": false,
                    "errorMessage": "",
                    "errorNum": 0,
                    "fields": [
                      "user"
                    ],
                    "id": "100005",
                    "sparse": true,
                    "type": "hash",
                    "unique": true
                  }
              ],
              "isSystem": false,
              "isVolatile": false,
              "journalSize": 1048576,
              "keyOptions": {
                "allowUserKeys": true,
                "type": "traditional"
              },
              "name": "test",
              "numberOfShards": 1,
              "replicationFactor": 2,
              "shardKeys": [
                "_key"
              ],
              "shards": {
                "s100001": [
                  "repltest",
                ]
              },
              "status": 2,
              "type": 2,
              "waitForSync": false
            }
          }
        }
      };
      cluster.executePlanForCollections(plan.Collections);
      db._useDatabase('test');
      let indexes = db._collection('s100001').getIndexes();
      expect(indexes).to.have.lengthOf(2);
    });
    it('should remove an additional index if instructed to do so', function() {
      db._create('s100001');
      db._collection('s100001').ensureIndex({ type: "hash", fields: [ "name" ] });
      let plan = {
        Databases: {
          "_system": {
            "id": 1,
            "name": "_system"
          },
          "test": {
            "id": 2,
            "name": "test"
          }
        },
        Collections: {
          test: {
            "100001": {
              "deleted": false,
              "doCompact": true,
              "id": "100001",
              "indexBuckets": 8,
              "indexes": [
                {
                  "fields": [
                    "_key"
                  ],
                  "id": "0",
                  "sparse": false,
                  "type": "primary",
                  "unique": true
                }
              ],
              "isSystem": false,
              "isVolatile": false,
              "journalSize": 1048576,
              "keyOptions": {
                "allowUserKeys": true,
                "type": "traditional"
              },
              "name": "test",
              "numberOfShards": 1,
              "replicationFactor": 2,
              "shardKeys": [
                "_key"
              ],
              "shards": {
                "s100001": [
                  "repltest",
                ]
              },
              "status": 2,
              "type": 2,
              "waitForSync": false
            }
          }
        }
      };
      cluster.executePlanForCollections(plan.Collections);
      db._useDatabase('test');
      let indexes = db._collection('s100001').getIndexes();
      expect(indexes).to.have.lengthOf(1);
    });
    it('should report an error when collection creation failed', function() {
      let plan = {
        Collections: {
          test: {
            "100001": {
              "deleted": false,
              "doCompact": true,
              "id": "100001",
              "indexBuckets": 8,
              "indexes": [
                {
                  "fields": [
                    "_key"
                  ],
                  "id": "0",
                  "sparse": false,
                  "type": "primary",
                  "unique": true
                }
              ],
              "isSystem": false,
              "isVolatile": false,
              "journalSize": 1048576,
              "keyOptions": {
                "allowUserKeys": true,
                "type": "traditional"
              },
              "name": "test",
              "numberOfShards": 1,
              "replicationFactor": 2,
              "shardKeys": [
                "_key"
              ],
              "shards": {
                "Möter": [
                  "repltest",
                ]
              },
              "status": 2,
              "type": 2,
              "waitForSync": false
            }
          }
        }
      };
      let errors = cluster.executePlanForCollections(plan.Collections);
      expect(errors).to.be.an('object');
      expect(errors).to.have.property('Möter');
    });
    it('should be leading a collection when ordered to be leader', function() {
      let plan = {
        test: {
          "100001": {
            "deleted": false,
            "doCompact": true,
            "id": "100001",
            "indexBuckets": 8,
            "indexes": [
              {
                "fields": [
                  "_key"
                ],
                "id": "0",
                "sparse": false,
                "type": "primary",
                "unique": true
              }
            ],
            "isSystem": false,
            "isVolatile": false,
            "journalSize": 1048576,
            "keyOptions": {
              "allowUserKeys": true,
              "type": "traditional"
            },
            "name": "test",
            "numberOfShards": 1,
            "replicationFactor": 2,
            "shardKeys": [
              "_key"
            ],
            "shards": {
              "s100001": [
                  "repltest",
              ]
            },
            "status": 3,
            "type": 2,
            "waitForSync": false
          }
        }
      };
      let errors = cluster.executePlanForCollections(plan);
      db._useDatabase('test');
      expect(db._collection('s100001').isLeader()).to.equal(true);
    });
    it('should be following a leader when ordered to be follower', function() {
      let plan = {
        test: {
          "100001": {
            "deleted": false,
            "doCompact": true,
            "id": "100001",
            "indexBuckets": 8,
            "indexes": [
              {
                "fields": [
                  "_key"
                ],
                "id": "0",
                "sparse": false,
                "type": "primary",
                "unique": true
              }
            ],
            "isSystem": false,
            "isVolatile": false,
            "journalSize": 1048576,
            "keyOptions": {
              "allowUserKeys": true,
              "type": "traditional"
            },
            "name": "test",
            "numberOfShards": 1,
            "replicationFactor": 2,
            "shardKeys": [
              "_key"
            ],
            "shards": {
              "s100001": [
                "the leader-leader",
                  "repltest",
              ]
            },
            "status": 2,
            "type": 2,
            "waitForSync": false
          }
        }
      };
      let errors = cluster.executePlanForCollections(plan);
      db._useDatabase('test');
      expect(db._collection('s100001').isLeader()).to.equal(false);
    });
    it('should be able to switch from leader to follower', function() {
      let plan = {
        test: {
          "100001": {
            "deleted": false,
            "doCompact": true,
            "id": "100001",
            "indexBuckets": 8,
            "indexes": [
              {
                "fields": [
                  "_key"
                ],
                "id": "0",
                "sparse": false,
                "type": "primary",
                "unique": true
              }
            ],
            "isSystem": false,
            "isVolatile": false,
            "journalSize": 1048576,
            "keyOptions": {
              "allowUserKeys": true,
              "type": "traditional"
            },
            "name": "test",
            "numberOfShards": 1,
            "replicationFactor": 2,
            "shardKeys": [
              "_key"
            ],
            "shards": {
              "s100001": [
                  "repltest",
              ]
            },
            "status": 2,
            "type": 2,
            "waitForSync": false
          }
        }
      };
      let errors = cluster.executePlanForCollections(plan);
      plan.test['100001'].shards['s100001'].unshift('der-hund');
      cluster.executePlanForCollections(plan);
      db._useDatabase('test');
      expect(db._collection('s100001').isLeader()).to.equal(false);
    });
    it('should be able to switch from follower to leader', function() {
      let plan = {
        test: {
          "100001": {
            "deleted": false,
            "doCompact": true,
            "id": "100001",
            "indexBuckets": 8,
            "indexes": [
              {
                "fields": [
                  "_key"
                ],
                "id": "0",
                "sparse": false,
                "type": "primary",
                "unique": true
              }
            ],
            "isSystem": false,
            "isVolatile": false,
            "journalSize": 1048576,
            "keyOptions": {
              "allowUserKeys": true,
              "type": "traditional"
            },
            "name": "test",
            "numberOfShards": 1,
            "replicationFactor": 2,
            "shardKeys": [
              "_key"
            ],
            "shards": {
              "s100001": [
                "old-leader",
                  "repltest",
              ]
            },
            "status": 2,
            "type": 2,
            "waitForSync": false
          }
        }
      };
      let errors = cluster.executePlanForCollections(plan);
      plan.test['100001'].shards['s100001'] = ["repltest"];
      cluster.executePlanForCollections(plan);
      db._useDatabase('test');
      expect(db._collection('s100001').isLeader()).to.equal(true);
    });
  });
  describe('Update current', function() {
    beforeEach(function() {
      db._databases().forEach(database => {
        if (database !== '_system') {
          db._dropDatabase(database);
        }
      });
    });
    it('should report a new database', function() {
      db._createDatabase('testi');
      let current = {
        _system: {
          repltest: {
            id: 1,
            name: '_system',
          },
        }
      };
      let result = cluster.updateCurrentForDatabases({}, current);
      expect(result).to.have.property('/arango/Current/Databases/testi/repltest');
      expect(result['/arango/Current/Databases/testi/repltest']).to.have.property('op', 'set');
      expect(result['/arango/Current/Databases/testi/repltest']).to.have.deep.property('new.name', 'testi');
    });
    it('should not do anything if there is nothing to do', function() {
      let current = {
        _system: {
          repltest: {
            id: 1,
            name: '_system',
          },
        }
      };
      let result = cluster.updateCurrentForDatabases({}, current);

      expect(Object.keys(result)).to.have.lengthOf(0);
    });
  });
});
