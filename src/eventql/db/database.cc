/**
 * Copyright (c) 2016 DeepCortex GmbH <legal@eventql.io>
 * Authors:
 *   - Paul Asmuth <paul@eventql.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include <eventql/db/database.h>
#include "eventql/util/io/filerepository.h"
#include "eventql/util/io/fileutil.h"
#include "eventql/util/application.h"
#include "eventql/util/logging.h"
#include "eventql/util/random.h"
#include "eventql/util/assets.h"
#include "eventql/util/thread/eventloop.h"
#include "eventql/util/thread/threadpool.h"
#include "eventql/util/thread/FixedSizeThreadPool.h"
#include "eventql/util/wallclock.h"
#include "eventql/util/VFS.h"
#include "eventql/util/rpc/ServerGroup.h"
#include "eventql/util/rpc/RPC.h"
#include "eventql/util/rpc/RPCClient.h"
#include "eventql/util/cli/flagparser.h"
#include "eventql/util/json/json.h"
#include "eventql/util/json/jsonrpc.h"
#include "eventql/util/http/httprouter.h"
#include "eventql/util/http/httpserver.h"
#include "eventql/util/http/VFSFileServlet.h"
#include "eventql/util/io/FileLock.h"
#include "eventql/util/stats/statsdagent.h"
#include "eventql/io/sstable/SSTableServlet.h"
#include "eventql/util/mdb/MDB.h"
#include "eventql/util/mdb/MDBUtil.h"
#include "eventql/transport/http/api_servlet.h"
#include "eventql/db/table_config.pb.h"
#include "eventql/db/table_service.h"
#include "eventql/db/metadata_coordinator.h"
#include "eventql/db/metadata_replication.h"
#include "eventql/db/metadata_service.h"
#include "eventql/transport/http/rpc_servlet.h"
#include "eventql/db/replication_worker.h"
#include "eventql/db/tablet_index_cache.h"
#include "eventql/db/compaction_worker.h"
#include "eventql/db/garbage_collector.h"
#include "eventql/db/leader.h"
#include "eventql/db/database.h"
#include "eventql/transport/http/default_servlet.h"
#include "eventql/sql/defaults.h"
#include "eventql/sql/runtime/query_cache.h"
#include "eventql/config/config_directory.h"
#include "eventql/config/config_directory_zookeeper.h"
#include "eventql/config/config_directory_standalone.h"
#include "eventql/transport/http/status_servlet.h"
#include "eventql/server/sql/scheduler.h"
#include "eventql/server/sql/table_provider.h"
#include "eventql/server/listener.h"
#include "eventql/auth/client_auth.h"
#include "eventql/auth/client_auth_trust.h"
#include "eventql/auth/client_auth_legacy.h"
#include "eventql/auth/internal_auth.h"
#include "eventql/auth/internal_auth_trust.h"
#include <jsapi.h>
#include "eventql/mapreduce/mapreduce_preludejs.cc"
#include "eventql/eventql.h"
#include "eventql/db/file_tracker.h"

namespace js {
void DisableExtraThreads();
}

namespace eventql {

Database::Database(ProcessConfig* process_config) : cfg_(process_config) {}

ReturnCode Database::start() {
  /* data directory */
  auto server_datadir = cfg_->getString("server.datadir").get();
  if (!FileUtil::exists(server_datadir)) {
    return ReturnCode::error(
        "EIO",
        "data dir not found: %s",
        server_datadir.c_str());
  }

  auto server_name = cfg_->getString("server.name");
  auto trash_dir = FileUtil::joinPaths(server_datadir, "trash");
  auto cache_dir = FileUtil::joinPaths(server_datadir, "cache");
  String tsdb_dir;
  String metadata_dir;
  if (server_name.isEmpty()) {
    tsdb_dir = FileUtil::joinPaths(server_datadir, "data/__anonymous");
    metadata_dir = FileUtil::joinPaths(server_datadir, "metadata/__anonymous");
  } else {
    tsdb_dir = FileUtil::joinPaths(
        server_datadir,
        "data/" + server_name.get());
    metadata_dir = FileUtil::joinPaths(
        server_datadir,
        "metadata/" + server_name.get());
  }

  try {
    server_lock_.reset(new FileLock(FileUtil::joinPaths(tsdb_dir, "__lock")));
    server_lock_->lock();
  } catch (const std::exception& e) {
    shutdown();
    return ReturnCode::error("EIO", e.what());
  }

  try {
    if (!FileUtil::exists(tsdb_dir)) {
      FileUtil::mkdir_p(tsdb_dir);
    }

    if (!FileUtil::exists(metadata_dir)) {
      FileUtil::mkdir_p(metadata_dir);
    }

    if (!FileUtil::exists(trash_dir)) {
      FileUtil::mkdir(trash_dir);
    }

    if (!FileUtil::exists(cache_dir)) {
      FileUtil::mkdir(cache_dir);
    }
  } catch (const std::exception& e) {
    shutdown();
    return ReturnCode::error("EIO", e.what());
  }

  /* file tracker */
  file_tracker_.reset(new FileTracker(trash_dir));

  /* garbage collector */
  try {
    auto gc_mode = garbageCollectorModeFromString(
        cfg_->getString("server.gc_mode").get());

    garbage_collector_.reset(
        new GarbageCollector(
            gc_mode,
            file_tracker_.get(),
            tsdb_dir,
            trash_dir,
            cache_dir,
            cfg_->getInt("server.cachedir_maxsize").get(),
            cfg_->getInt("server.gc_interval").get()));
  } catch (const std::exception& e) {
    shutdown();
    return ReturnCode::error("ERUNTIME", e.what());
  }

  /* config dir */
  {
    auto rc = ConfigDirectoryFactory::getConfigDirectoryForServer(
        cfg_,
        &config_dir_);

    if (rc.isSuccess()) {
      rc = config_dir_->start();
    }

    if (!rc.isSuccess()) {
      shutdown();
      return ReturnCode::error(
          "ERUNTIME",
          "Can't connect to config backend: %s", rc.message().c_str());
    }
  }

  /* client auth */
  if (!cfg_->hasProperty("server.client_auth_backend")) {
    shutdown();
    return ReturnCode::error("EARG", "missing 'server.client_auth_backend'");
  }

  auto client_auth_opt = cfg_->getString("server.client_auth_backend");
  if (client_auth_opt.get() == "trust") {
    client_auth_.reset(new TrustClientAuth());
  } else if (client_auth_opt.get() == "legacy") {
    if (!cfg_->hasProperty("server.legacy_auth_secret")) {
      shutdown();
      return ReturnCode::error("EARG", "missing 'server.legacy_auth_secret'");
    }

    client_auth_.reset(
        new LegacyClientAuth(
            cfg_->getString("server.legacy_auth_secret").get()));
  } else {
    shutdown();
    return ReturnCode::error(
        "EARG",
        "invalid client auth backend: " + client_auth_opt.get());
  }

  /* internal auth */
  internal_auth_.reset(new TrustInternalAuth());

  /* metadata service */
  metadata_store_.reset(new MetadataStore(metadata_dir));
  metadata_service_.reset(
      new MetadataService(config_dir_.get(), metadata_store_.get()));

  /* server config */
  server_cfg_.reset(new ServerCfg());
  server_cfg_->db_path = tsdb_dir;
  server_cfg_->config_directory = config_dir_.get();
  server_cfg_->idx_cache = mkRef(new LSMTableIndexCache(tsdb_dir));
  server_cfg_->metadata_store = metadata_store_.get();
  server_cfg_->file_tracker = file_tracker_.get();

  /* core */
  partition_map_.reset(new PartitionMap(server_cfg_.get()));

  table_service_.reset(
      new TableService(config_dir_.get(), partition_map_.get()));

  replication_worker_.reset(
      new ReplicationWorker(partition_map_.get()));

  compaction_worker_.reset(
      new CompactionWorker(
          partition_map_.get(),
          cfg_->getInt("server.indexbuild_threads").get()));

  /* metadata replication */
  if (!server_name.isEmpty()) {
    metadata_replication_.reset(
        new MetadataReplication(
            config_dir_.get(),
            server_name.get(),
            metadata_store_.get()));
  }

  /* sql */
  {
    sql_query_cache_.reset(new csql::QueryCache(cache_dir));
    sql_symbols_ = mkRef(new csql::SymbolTable());
    csql::installDefaultSymbols(sql_symbols_.get());

    sql_ = mkRef(new csql::Runtime(
        thread::ThreadPoolOptions {
          .thread_name = Some(String("evqld-sql"))
        },
        sql_symbols_,
        new csql::QueryBuilder(
            new csql::ValueExpressionBuilder(sql_symbols_.get())),
        new csql::QueryPlanBuilder(
            csql::QueryPlanBuilderOptions{},
            sql_symbols_.get()),
        mkScoped(
            new Scheduler(
                partition_map_.get(),
                config_dir_.get(),
                internal_auth_.get()))));

    sql_->setCacheDir(cache_dir);
    sql_->symbols()->registerFunction("version", &evqlVersionExpr);
    sql_->setQueryCache(sql_query_cache_.get());
  }

  /* spidermonkey javascript runtime */
  JS_Init();
  js::DisableExtraThreads();

  /* more services */
  sql_service_.reset(
      new SQLService(
          sql_.get(),
          partition_map_.get(),
          config_dir_.get(),
          internal_auth_.get(),
          table_service_.get(),
          cache_dir));

  mapreduce_service_.reset(
      new MapReduceService(
          config_dir_.get(),
          internal_auth_.get(),
          table_service_.get(),
          partition_map_.get(),
          cache_dir));

  /* open tables */
  config_dir_->setTableConfigChangeCallback([this] (const TableDefinition& tbl) {
    Set<SHA1Hash> affected_partitions;
    try {
      partition_map_->configureTable(tbl, &affected_partitions);
    } catch (const std::exception& e) {
      logError(
          "evqld",
          "error while applying table config change to $0/$1",
          tbl.customer(),
          tbl.table_name());
    }

    for (const auto& partition_id : affected_partitions) {
      try {
        auto partition = partition_map_->findPartition(
            tbl.customer(),
            tbl.table_name(),
            partition_id);

        replication_worker_->enqueuePartition(partition.get(), 0);
      } catch (const std::exception& e) {
        logError(
            "evqld",
            "error while applying table config change to $0/$1/$2",
            tbl.customer(),
            tbl.table_name(),
            partition_id.toString());
      }
    }
  });

  config_dir_->listTables([this] (const TableDefinition& tbl) {
    partition_map_->configureTable(tbl);
  });

  /* start the database */
  replication_worker_->start();

  try {
    partition_map_->open();
  } catch (const std::exception& e) {
    shutdown();
    return ReturnCode::error("ERUNTIME", e.what());
  }

  garbage_collector_->startGCThread();

  if (metadata_replication_.get()) {
    metadata_replication_->start();
  }

  if (!cfg_->getBool("server.noleader")) {
    leader_.reset(
        new Leader(
            config_dir_.get(),
            cfg_->getInt("cluster.rebalance_interval").get()));

    leader_->startLeaderThread();
  }

  return ReturnCode::success();
}

void Database::shutdown() {
  if (leader_) {
    leader_->stopLeaderThread();
    leader_.reset(nullptr);
  }

  mapreduce_service_.reset(nullptr);
  //JS_ShutDown();

  sql_service_.reset(nullptr);
  sql_.reset(nullptr);
  sql_symbols_.reset(nullptr);
  sql_query_cache_.reset(nullptr);

  if (metadata_replication_) {
    metadata_replication_->stop();
    metadata_replication_.reset(nullptr);
  }

  compaction_worker_.reset(nullptr);

  if (replication_worker_) {
    replication_worker_->stop();
    replication_worker_.reset(nullptr);
  }

  table_service_.reset(nullptr);
  partition_map_.reset(nullptr);
  metadata_service_.reset(nullptr);
  metadata_store_.reset(nullptr);
  internal_auth_.reset(nullptr);
  client_auth_.reset(nullptr);

  if (config_dir_) {
    config_dir_->stop();
    config_dir_.reset(nullptr);
  }

  if (garbage_collector_) {
    garbage_collector_->stopGCThread();
    garbage_collector_.reset(nullptr);
  }

  file_tracker_.reset(nullptr);
  server_cfg_.reset(nullptr);
  server_lock_.reset(nullptr);
}

std::unique_ptr<Session> Database::createContext() {
  return std::unique_ptr<Session>(new Session());
}

void Database::startThread(
    Session* context,
    std::function<void()> entrypoint) {
  auto t = std::thread(entrypoint);
  t.detach();
}

} // namespace tdsb
