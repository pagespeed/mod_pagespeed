// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jmarantz@google.com (Joshua Marantz)
//         lsong@google.com (Libo Song)

#include "net/instaweb/apache/apache_rewrite_driver_factory.h"

#include <unistd.h>

#include <cstddef>
#include <cstdlib>

#include <algorithm>
#include <utility>

#include "apr_pools.h"
#include "httpd.h"
#include "ap_mpm.h"

#include "base/logging.h"
#include "net/instaweb/apache/apache_config.h"
#include "net/instaweb/apache/apache_message_handler.h"
#include "net/instaweb/apache/apache_server_context.h"
#include "net/instaweb/apache/apache_thread_system.h"
#include "net/instaweb/apache/apr_mem_cache.h"
#include "net/instaweb/apache/apr_timer.h"
#include "net/instaweb/apache/in_place_resource_recorder.h"
#include "net/instaweb/apache/mod_spdy_fetch_controller.h"
#include "net/instaweb/apache/serf_url_async_fetcher.h"
#include "net/instaweb/http/public/fake_url_async_fetcher.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/http_dump_url_fetcher.h"
#include "net/instaweb/http/public/http_dump_url_writer.h"
#include "net/instaweb/http/public/rate_controller.h"
#include "net/instaweb/http/public/rate_controlling_url_async_fetcher.h"
#include "net/instaweb/http/public/sync_fetcher_adapter.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "net/instaweb/http/public/url_fetcher.h"
#include "net/instaweb/http/public/write_through_http_cache.h"
#include "net/instaweb/rewriter/public/beacon_critical_images_finder.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/system/public/system_cache_path.h"
#include "net/instaweb/util/public/abstract_shared_mem.h"
#include "net/instaweb/util/public/async_cache.h"
#include "net/instaweb/util/public/cache_batcher.h"
#include "net/instaweb/util/public/cache_copy.h"
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/cache_stats.h"
#ifndef NDEBUG
#include "net/instaweb/util/public/checking_thread_system.h"
#endif
#include "net/instaweb/util/public/fallback_cache.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/md5_hasher.h"
#include "net/instaweb/util/public/null_shared_mem.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/pthread_shared_mem.h"
#include "net/instaweb/util/public/queued_worker_pool.h"
#include "net/instaweb/util/public/shared_circular_buffer.h"
#include "net/instaweb/util/public/shared_mem_referer_statistics.h"
#include "net/instaweb/util/public/shared_mem_statistics.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/slow_worker.h"
#include "net/instaweb/util/public/stdio_file_system.h"
#include "net/instaweb/util/public/stl_util.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/write_through_cache.h"

namespace net_instaweb {

namespace {

const size_t kRefererStatisticsNumberOfPages = 1024;
const size_t kRefererStatisticsAverageUrlLength = 64;
const char kShutdownCount[] = "child_shutdown_count";

}  // namespace

const char ApacheRewriteDriverFactory::kMemcached[] = "memcached";
const char ApacheRewriteDriverFactory::kShmCache[] = "shm_cache";
const char ApacheRewriteDriverFactory::kStaticAssetPrefix[] =
    "/mod_pagespeed_static/";

ApacheRewriteDriverFactory::ApacheRewriteDriverFactory(
    server_rec* server, const StringPiece& version)
    : RewriteDriverFactory(
#ifdef NDEBUG
        new ApacheThreadSystem
#else
        new CheckingThreadSystem(new ApacheThreadSystem)
#endif
                           ),
      server_rec_(server),
#ifdef PAGESPEED_SUPPORT_POSIX_SHARED_MEM
      shared_mem_runtime_(new PthreadSharedMem()),
#else
      shared_mem_runtime_(new NullSharedMem()),
#endif
      shared_circular_buffer_(NULL),
      version_(version.data(), version.size()),
      statistics_frozen_(false),
      is_root_process_(true),
      fetch_with_gzip_(false),
      track_original_content_length_(false),
      list_outstanding_urls_on_error_(false),
      shared_mem_referer_statistics_(NULL),
      hostname_identifier_(StrCat(server->server_hostname,
                                  ":",
                                  IntegerToString(server->port))),
      apache_message_handler_(new ApacheMessageHandler(
          server_rec_, version_, timer(), thread_system()->NewMutex())),
      apache_html_parse_message_handler_(new ApacheMessageHandler(
          server_rec_, version_, timer(), thread_system()->NewMutex())),
      use_per_vhost_statistics_(false),
      enable_property_cache_(true),
      inherit_vhost_config_(false),
      disable_loopback_routing_(false),
      install_crash_handler_(false),
      thread_counts_finalized_(false),
      num_rewrite_threads_(-1),
      num_expensive_rewrite_threads_(-1),
      message_buffer_size_(0),
      cache_hasher_(20) {
  apr_pool_create(&pool_, NULL);

  // Make sure the ownership of apache_message_handler_ and
  // apache_html_parse_message_handler_ is given to scoped pointer.
  // Otherwise may result in leak error in test.
  message_handler();
  html_parse_message_handler();
  InitializeDefaultOptions();

  // Note: this must run after mod_pagespeed_register_hooks has completed.
  // See http://httpd.apache.org/docs/2.4/developer/new_api_2_4.html and
  // search for ap_mpm_query.
  AutoDetectThreadCounts();
}

ApacheRewriteDriverFactory::~ApacheRewriteDriverFactory() {
  // Finish up any background tasks and stop accepting new ones. This ensures
  // that as soon as the first ApacheRewriteDriverFactory is shutdown we
  // no longer have to worry about outstanding jobs in the slow_worker_ trying
  // to access FileCache and similar objects we're about to blow away.
  if (!is_root_process_) {
    slow_worker_->ShutDown();
  }

  // We free all the resources before destroying the pool, because some of the
  // resource uses the sub-pool and will need that pool to be around to
  // clean up properly.
  ShutDown();

  apr_pool_destroy(pool_);

  // We still have registered a pool deleter here, right?  This seems risky...
  STLDeleteElements(&uninitialized_managers_);

  for (PathCacheMap::iterator p = path_cache_map_.begin(),
           e = path_cache_map_.end(); p != e; ++p) {
    SystemCachePath* cache = p->second;
    defer_cleanup(new Deleter<SystemCachePath>(cache));
  }

  for (MemcachedMap::iterator p = memcached_map_.begin(),
           e = memcached_map_.end(); p != e; ++p) {
    CacheInterface* memcached = p->second;
    defer_cleanup(new Deleter<CacheInterface>(memcached));
  }

  for (MetadataShmCacheMap::iterator p = metadata_shm_caches_.begin(),
           e = metadata_shm_caches_.end(); p != e; ++p) {
    MetadataShmCacheInfo* cache_info = p->second;
    defer_cleanup(new Deleter<MetadataShmCacheInfo>(cache_info));
  }

  shared_mem_statistics_.reset(NULL);
}

SystemCachePath* ApacheRewriteDriverFactory::GetCache(ApacheConfig* config) {
  const GoogleString& path = config->file_cache_path();
  std::pair<PathCacheMap::iterator, bool> result = path_cache_map_.insert(
      PathCacheMap::value_type(path, static_cast<SystemCachePath*>(NULL)));
  PathCacheMap::iterator iter = result.first;
  if (result.second) {
    iter->second = new SystemCachePath(path, config, this,
                                       shared_mem_runtime());
  }
  return iter->second;
}

AprMemCache* ApacheRewriteDriverFactory::NewAprMemCache(
    const GoogleString& spec) {
  int thread_limit;
  ap_mpm_query(AP_MPMQ_HARD_LIMIT_THREADS, &thread_limit);
  thread_limit += num_rewrite_threads() + num_expensive_rewrite_threads();
  return new AprMemCache(spec, thread_limit, &cache_hasher_, statistics(),
                         timer(), message_handler());
}

CacheInterface* ApacheRewriteDriverFactory::GetMemcached(
    ApacheConfig* config, CacheInterface* l2_cache) {
  CacheInterface* memcached = NULL;

  // Find a memcache that matches the current spec, or create a new one
  // if needed. Note that this means that two different VirtualHost's will
  // share a memcached if their specs are the same but will create their own
  // if the specs are different.
  if (!config->memcached_servers().empty()) {
    const GoogleString& server_spec = config->memcached_servers();
    std::pair<MemcachedMap::iterator, bool> result = memcached_map_.insert(
        MemcachedMap::value_type(server_spec, memcached));
    if (result.second) {
      AprMemCache* mem_cache = NewAprMemCache(server_spec);
      if (config->has_memcached_timeout_us()) {
        mem_cache->set_timeout_us(config->memcached_timeout_us());
      }
      memcache_servers_.push_back(mem_cache);

      int num_threads = config->memcached_threads();
      if (num_threads != 0) {
        if (memcached_pool_.get() == NULL) {
          // Note -- we will use the first value of ModPagespeedMemCacheThreads
          // that we see in a VirtualHost, ignoring later ones.
          memcached_pool_.reset(new QueuedWorkerPool(num_threads,
                                                     "memcached",
                                                     thread_system()));
        }
        AsyncCache* async_cache = new AsyncCache(mem_cache,
                                                 memcached_pool_.get());
        async_caches_.push_back(async_cache);
        memcached = async_cache;
      } else {
        memcached = mem_cache;
      }

      // Put the batcher above the stats so that the stats sees the MultiGets
      // and can show us the histogram of how they are sized.
#if CACHE_STATISTICS
      memcached = new CacheStats(kMemcached, memcached, timer(), statistics());
#endif
      CacheBatcher* batcher = new CacheBatcher(
          memcached, thread_system()->NewMutex(), statistics());
      if (num_threads != 0) {
        batcher->set_max_parallel_lookups(num_threads);
      }
      memcached = batcher;
      result.first->second = memcached;
    } else {
      memcached = result.first->second;
    }

    // Note that a distinct FallbackCache gets created for every VirtualHost
    // that employs memcached, even if the memcached and file-cache
    // specifications are identical.  This does no harm, because there
    // is no data in the cache object itself; just configuration.  Sharing
    // FallbackCache* objects would require making a map using the
    // memcache & file-cache specs as a key, so it's simpler to make a new
    // small FallbackCache object for each VirtualHost.
    memcached = new FallbackCache(memcached, l2_cache,
                                  AprMemCache::kValueSizeThreshold,
                                  message_handler());
  }
  return memcached;
}

GoogleString ApacheRewriteDriverFactory::CreateShmMetadataCache(
    const GoogleString& name, int64 size_kb) {
  MetadataShmCacheInfo* cache_info = NULL;
  std::pair<MetadataShmCacheMap::iterator, bool> result =
      metadata_shm_caches_.insert(
          MetadataShmCacheMap::value_type(name, cache_info));
  if (result.second) {
    int entries, blocks;
    int64 size_cap;
    const int kSectors = 128;
    MetadataShmCache::ComputeDimensions(
        size_kb, 2 /* block/entry ratio, based empirically off load tests */,
        kSectors, &entries, &blocks, &size_cap);

    // Make sure the size cap is not unusably low. In particular, with 2K
    // inlining thresholds, something like 3K is needed. (As of time of writing,
    // that required about 4.3MiB).
    if (size_cap < 3 * 1024) {
      metadata_shm_caches_.erase(result.first);
      return "Shared memory cache unusably small.";
    } else {
      cache_info = new MetadataShmCacheInfo;
      cache_info->cache_backend =
          new SharedMemCache<64>(
              shared_mem_runtime(),
              StrCat(name, "/metadata_cache"),
              timer(),
              hasher(),
              kSectors,
              entries,  /* entries per sector */
              blocks /* blocks per sector*/,
              message_handler());
      // We can't set cache_info->cache_to_use yet since statistics aren't ready
      // yet. It will happen in ::RootInit().
      result.first->second = cache_info;
      return GoogleString();
    }
  } else {
    return StrCat("Cache named ", name, " already exists.");
  }
}

CacheInterface* ApacheRewriteDriverFactory::GetShmMetadataCache(
    ApacheConfig* config) {
  const GoogleString& name = config->use_shared_mem_metadata_cache();
  if (name.empty()) {
    return NULL;
  }
  MetadataShmCacheMap::iterator i = metadata_shm_caches_.find(name);
  if (i != metadata_shm_caches_.end()) {
    return i->second->cache_to_use.get();
  }
  return NULL;
}

void ApacheRewriteDriverFactory::PrintShmMetadataCacheStats(GoogleString* out) {
  for (MetadataShmCacheMap::iterator p = metadata_shm_caches_.begin(),
           e = metadata_shm_caches_.end(); p != e; ++p) {
    MetadataShmCacheInfo* cache_info = p->second;
    if (cache_info->cache_backend != NULL) {
      StrAppend(out, "Shared memory metadata cache '", p->first,
                "' statistics:<br>");
      StrAppend(out, "<pre>", cache_info->cache_backend->DumpStats(),
                "</pre>");
    }
  }
}

CacheInterface* ApacheRewriteDriverFactory::GetFilesystemMetadataCache(
    ApacheConfig* config) {
  // Reuse the memcached server(s) for the filesystem metadata cache. We need
  // to search for our config's entry in the vector of servers (not the more
  // obvious map) because the map's entries are wrapped in an AsyncCache, and
  // the filesystem metadata cache requires a blocking cache (like memcached).
  // Note that if we have a server spec we *know* it's in the searched vector.
  DCHECK_EQ(config->memcached_servers().empty(), memcache_servers_.empty());
  const GoogleString& server_spec = config->memcached_servers();
  for (int i = 0, n = memcache_servers_.size(); i < n; ++i) {
    if (server_spec == memcache_servers_[i]->server_spec()) {
      return memcache_servers_[i];
    }
  }

  return NULL;
}

FileSystem* ApacheRewriteDriverFactory::DefaultFileSystem() {
  return new StdioFileSystem(timer());
}

Hasher* ApacheRewriteDriverFactory::NewHasher() {
  return new MD5Hasher();
}

Timer* ApacheRewriteDriverFactory::DefaultTimer() {
  return new AprTimer();
}

MessageHandler* ApacheRewriteDriverFactory::DefaultHtmlParseMessageHandler() {
  return apache_html_parse_message_handler_;
}

MessageHandler* ApacheRewriteDriverFactory::DefaultMessageHandler() {
  return apache_message_handler_;
}

void ApacheRewriteDriverFactory::SetupCaches(
    ServerContext* server_context) {
  // TODO(morlovich): Factor out this logic. Most of this has nothing to do with
  // Apache, and will probably be needed for nginx.
  ApacheConfig* config = ApacheConfig::DynamicCast(
      server_context->global_options());
  SystemCachePath* caches_for_path = GetCache(config);
  CacheInterface* lru_cache = caches_for_path->lru_cache();
  CacheInterface* file_cache = caches_for_path->file_cache();
  CacheInterface* shm_metadata_cache = GetShmMetadataCache(config);
  CacheInterface* memcached = GetMemcached(config, file_cache);

  if (memcached != NULL) {
    server_context->set_owned_cache(memcached);
    server_context->set_filesystem_metadata_cache(
        new CacheCopy(GetFilesystemMetadataCache(config)));
  }
  Statistics* stats = server_context->statistics();

  // Figure out our L1/L2 hierarchy for http cache.
  // TODO(jmarantz): consider moving ownership of the LRU cache into the
  // factory, rather than having one per vhost.
  //
  // Note that a user can disable the LRU cache by setting its byte-count
  // to 0.
  CacheInterface* http_l2 = (memcached != NULL) ? memcached : file_cache;
  int64 max_content_length = config->max_cacheable_response_content_length();
  HTTPCache* http_cache = NULL;
  if (lru_cache == NULL) {
    // No L1, and so backend is just the L2
    http_cache = new HTTPCache(http_l2, timer(), hasher(), stats);
  } else {
    // L1 is LRU, with the L2 as computed above.
    WriteThroughHTTPCache* write_through_http_cache = new WriteThroughHTTPCache(
        lru_cache, http_l2, timer(), hasher(), stats);
    write_through_http_cache->set_cache1_limit(config->lru_cache_byte_limit());
    http_cache = write_through_http_cache;
  }

  http_cache->set_max_cacheable_response_content_length(max_content_length);
  server_context->set_http_cache(http_cache);

  // And now the metadata cache. If we only have one level, it will be in
  // metadata_l2, with metadata_l1 set to NULL.
  CacheInterface* metadata_l1 = NULL;
  CacheInterface* metadata_l2 = NULL;
  size_t l1_size_limit = WriteThroughCache::kUnlimited;
  if (shm_metadata_cache != NULL) {
    // Do we have both a local SHM cache and a memcached-backed cache? In that
    // case, it makes sense to go L1/L2 with them. If not, just use the SHM
    // cache and ignore the per-process LRU as it's basically strictly worse.
    if (memcached != NULL) {
      metadata_l1 = shm_metadata_cache;
      metadata_l2 = memcached;
    } else {
      metadata_l2 = shm_metadata_cache;
    }
  } else {
    l1_size_limit = config->lru_cache_byte_limit();
    metadata_l1 = lru_cache;  // may be NULL
    metadata_l2 = http_l2;  // memcached or file.
  }

  if (metadata_l1 != NULL) {
    WriteThroughCache* write_through_cache = new WriteThroughCache(
        metadata_l1, metadata_l2);
    write_through_cache->set_cache1_limit(l1_size_limit);
    server_context->set_metadata_cache(write_through_cache);
  } else {
    server_context->set_metadata_cache(new CacheCopy(metadata_l2));
  }
  server_context->MakePropertyCaches(metadata_l2);

  server_context->set_enable_property_cache(enable_property_cache());
  PropertyCache* pcache = server_context->page_property_cache();
  if (pcache->GetCohort(BeaconCriticalImagesFinder::kBeaconCohort) == NULL) {
    pcache->AddCohort(BeaconCriticalImagesFinder::kBeaconCohort);
  }
  if (pcache->GetCohort(RewriteDriver::kDomCohort) == NULL) {
    pcache->AddCohort(RewriteDriver::kDomCohort);
  }
}

void ApacheRewriteDriverFactory::InitStaticAssetManager(
    StaticAssetManager* static_asset_manager) {
  static_asset_manager->set_library_url_prefix(kStaticAssetPrefix);
}

NamedLockManager* ApacheRewriteDriverFactory::DefaultLockManager() {
  LOG(DFATAL)
      << "In Apache locks are owned by SystemCachePath, not the factory";
  return NULL;
}

UrlFetcher* ApacheRewriteDriverFactory::DefaultUrlFetcher() {
  LOG(DFATAL) << "In Apache the fetchers are not global, but kept in a map.";
  return NULL;
}

UrlAsyncFetcher* ApacheRewriteDriverFactory::DefaultAsyncUrlFetcher() {
  LOG(DFATAL) << "In Apache the fetchers are not global, but kept in a map.";
  return NULL;
}

QueuedWorkerPool* ApacheRewriteDriverFactory::CreateWorkerPool(
    WorkerPoolCategory pool, StringPiece name) {
  switch (pool) {
    case kHtmlWorkers:
      // In practice this is 0, as we don't use HTML threads in Apache.
      return new QueuedWorkerPool(1, name, thread_system());
    case kRewriteWorkers:
      return new QueuedWorkerPool(num_rewrite_threads_, name, thread_system());
    case kLowPriorityRewriteWorkers:
      return new QueuedWorkerPool(num_expensive_rewrite_threads_,
                                  name,
                                  thread_system());
    default:
      return RewriteDriverFactory::CreateWorkerPool(pool, name);
  }
}

void ApacheRewriteDriverFactory::AutoDetectThreadCounts() {
  if (thread_counts_finalized_) {
    return;
  }

  // Detect whether we're using a threaded MPM.
  apr_status_t status;
  int result = 0, threads = 1;
  status = ap_mpm_query(AP_MPMQ_IS_THREADED, &result);
  if (status == APR_SUCCESS &&
      (result == AP_MPMQ_STATIC || result == AP_MPMQ_DYNAMIC)) {
    status = ap_mpm_query(AP_MPMQ_MAX_THREADS, &threads);
    if (status != APR_SUCCESS) {
      threads = 0;
    }
  }

  threads = std::max(1, threads);

  if (threads > 1) {
    // Apply defaults for threaded.
    max_mod_spdy_fetch_threads_ = 8;  // TODO(morlovich): Base on MPM's count?
    if (num_rewrite_threads_ <= 0) {
      num_rewrite_threads_ = 4;
    }
    if (num_expensive_rewrite_threads_ <= 0) {
      num_expensive_rewrite_threads_ = 4;
    }
    message_handler()->Message(
        kInfo, "Detected threaded MPM with up to %d threads."
        " Own threads: %d Rewrite, %d Expensive Rewrite.",
        threads, num_rewrite_threads_, num_expensive_rewrite_threads_);

  } else {
    // Apply defaults for non-threaded.

    // If using mod_spdy_fetcher we roughly want one thread for non-background,
    // fetches one for background ones.
    max_mod_spdy_fetch_threads_ = 2;
    if (num_rewrite_threads_ <= 0) {
      num_rewrite_threads_ = 1;
    }
    if (num_expensive_rewrite_threads_ <= 0) {
      num_expensive_rewrite_threads_ = 1;
    }
    message_handler()->Message(
        kInfo, "No threading detected in MPM."
        " Own threads: %d Rewrite, %d Expensive Rewrite.",
        num_rewrite_threads_, num_expensive_rewrite_threads_);
  }

  thread_counts_finalized_ = true;
}

UrlAsyncFetcher* ApacheRewriteDriverFactory::GetFetcher(ApacheConfig* config) {
  const GoogleString& proxy = config->fetcher_proxy();

  // Fetcher-key format: "[(R|W)slurp_directory][\nproxy]"
  GoogleString key;
  if (config->slurping_enabled()) {
    if (config->slurp_read_only()) {
      key = StrCat("R", config->slurp_directory());
    } else {
      key = StrCat("W", config->slurp_directory());
    }
  }
  if (!proxy.empty()) {
    StrAppend(&key, "\n", proxy);
  }

  std::pair<FetcherMap::iterator, bool> result = fetcher_map_.insert(
      std::make_pair(key, static_cast<UrlAsyncFetcher*>(NULL)));
  FetcherMap::iterator iter = result.first;
  if (result.second) {
    UrlAsyncFetcher* fetcher = NULL;
    if (config->slurping_enabled()) {
      if (config->slurp_read_only()) {
        HttpDumpUrlFetcher* dump_fetcher = new HttpDumpUrlFetcher(
            config->slurp_directory(), file_system(), timer());
        defer_cleanup(new Deleter<HttpDumpUrlFetcher>(dump_fetcher));
        fetcher = new FakeUrlAsyncFetcher(dump_fetcher);
      } else {
        SerfUrlAsyncFetcher* base_fetcher = GetSerfFetcher(config);

        UrlFetcher* sync_fetcher = new SyncFetcherAdapter(
            timer(), config->blocking_fetch_timeout_ms(), base_fetcher,
            thread_system());
        defer_cleanup(new Deleter<UrlFetcher>(sync_fetcher));
        HttpDumpUrlWriter* dump_writer = new HttpDumpUrlWriter(
            config->slurp_directory(), sync_fetcher, file_system(), timer());
        defer_cleanup(new Deleter<HttpDumpUrlWriter>(dump_writer));
        fetcher = new FakeUrlAsyncFetcher(dump_writer);
      }
    } else {
      SerfUrlAsyncFetcher* serf = GetSerfFetcher(config);
      fetcher = serf;
      if (config->rate_limit_background_fetches()) {
        // Unfortunately, we need stats for load-shedding.
        if (config->statistics_enabled()) {
          CHECK(thread_counts_finalized_);
          int multiplier = std::min(4, num_rewrite_threads_);
          defer_cleanup(new Deleter<SerfUrlAsyncFetcher>(serf));
          fetcher = new RateControllingUrlAsyncFetcher(
              serf,
              500 * multiplier /* max queue size */,
              multiplier /* requests/host */,
              500 * multiplier /* queued per host */,
              thread_system(),
              statistics());
        } else {
          message_handler()->Message(
              kError, "Can't enable fetch rate-limiting without statistics");
        }
      }
    }
    iter->second = fetcher;
  }
  return iter->second;
}

SerfUrlAsyncFetcher* ApacheRewriteDriverFactory::GetSerfFetcher(
    ApacheConfig* config) {
  // Since we don't do slurping a this level, our key is just the proxy setting.
  const GoogleString& proxy = config->fetcher_proxy();
  std::pair<SerfFetcherMap::iterator, bool> result = serf_fetcher_map_.insert(
      std::make_pair(proxy, static_cast<SerfUrlAsyncFetcher*>(NULL)));
  SerfFetcherMap::iterator iter = result.first;
  if (result.second) {
    SerfUrlAsyncFetcher* serf = new SerfUrlAsyncFetcher(
        proxy.c_str(),
        NULL,  // Do not use the Factory pool so we can control deletion.
        thread_system(), statistics(), timer(),
        config->blocking_fetch_timeout_ms(),
        message_handler());
    serf->set_list_outstanding_urls_on_error(list_outstanding_urls_on_error_);
    serf->set_fetch_with_gzip(fetch_with_gzip_);
    serf->set_track_original_content_length(track_original_content_length_);
    serf->SetHttpsOptions(https_options_);
    iter->second = serf;
  }
  return iter->second;
}

bool ApacheRewriteDriverFactory::SetHttpsOptions(StringPiece directive,
                                                 GoogleString* error_message) {
  directive.CopyToString(&https_options_);
  return SerfUrlAsyncFetcher::ValidateHttpsOptions(directive, error_message);
}

// TODO(jmarantz): make this per-vhost.
void ApacheRewriteDriverFactory::SharedCircularBufferInit(bool is_root) {
  // Set buffer size to 0 means turning it off
  if (shared_mem_runtime() != NULL && (message_buffer_size_ != 0)) {
    // TODO(jmarantz): it appears that filename_prefix() is not actually
    // established at the time of this construction, calling into question
    // whether we are naming our shared-memory segments correctly.
    shared_circular_buffer_.reset(new SharedCircularBuffer(
        shared_mem_runtime(),
        message_buffer_size_,
        filename_prefix().as_string(),
        hostname_identifier()));
    if (shared_circular_buffer_->InitSegment(is_root, message_handler())) {
      apache_message_handler_->set_buffer(shared_circular_buffer_.get());
      apache_html_parse_message_handler_->set_buffer(
          shared_circular_buffer_.get());
     }
  }
}

// Temporarily disable shared-mem-referrers stuff until we get the rest the
// one-factory-per-process change in.
#define ENABLE_REFERER_STATS 0

void ApacheRewriteDriverFactory::SharedMemRefererStatisticsInit(bool is_root) {
#if ENABLE_REFERER_STATS
  if (shared_mem_runtime_ != NULL && config_->collect_referer_statistics()) {
    // TODO(jmarantz): it appears that filename_prefix() is not actually
    // established at the time of this construction, calling into question
    // whether we are naming our shared-memory segments correctly.
    if (config_->hash_referer_statistics()) {
      // By making the hashes equal roughly to half the expected url length,
      // entries corresponding to referrals in the
      // shared_mem_referer_statistics_ map will be roughly the expected size
      //   The size of the hash might be capped, so we check for this and cap
      // expected average url length if necessary.
      //
      // hostname_identifier() is passed in as a suffix so that the shared
      // memory segments for different v-hosts have unique identifiers, keeping
      // the statistics separate.
      Hasher* hasher =
          new MD5Hasher(kRefererStatisticsAverageUrlLength / 2);
      size_t referer_statistics_average_expected_url_length_ =
          2 * hasher->HashSizeInChars();
      shared_mem_referer_statistics_.reset(new HashedRefererStatistics(
          kRefererStatisticsNumberOfPages,
          referer_statistics_average_expected_url_length_,
          shared_mem_runtime(),
          filename_prefix().as_string(),
          hostname_identifier(),
          hasher));
    } else {
      shared_mem_referer_statistics_.reset(new SharedMemRefererStatistics(
          kRefererStatisticsNumberOfPages,
          kRefererStatisticsAverageUrlLength,
          shared_mem_runtime(),
          filename_prefix().as_string(),
          hostname_identifier()));
    }
    if (!shared_mem_referer_statistics_->InitSegment(is_root,
                                                     message_handler())) {
      shared_mem_referer_statistics_.reset(NULL);
    }
  }
#endif
}

void ApacheRewriteDriverFactory::ParentOrChildInit() {
  if (install_crash_handler_) {
    ApacheMessageHandler::InstallCrashHandler(server_rec_);
  }
  SharedCircularBufferInit(is_root_process_);
  SharedMemRefererStatisticsInit(is_root_process_);
}

void ApacheRewriteDriverFactory::RootInit() {
  ParentOrChildInit();
  for (ApacheServerContextSet::iterator p = uninitialized_managers_.begin(),
           e = uninitialized_managers_.end(); p != e; ++p) {
    ApacheServerContext* resource_manager = *p;

    // Determine the set of caches needed based on the unique
    // file_cache_path()s in the manager configurations.  We ignore
    // the GetCache return value because our goal is just to populate
    // the map which we'll iterate on below.
    GetCache(resource_manager->config());
  }
  for (MetadataShmCacheMap::iterator p = metadata_shm_caches_.begin(),
           e = metadata_shm_caches_.end(); p != e; ++p) {
    MetadataShmCacheInfo* cache_info = p->second;
    if (cache_info->cache_backend->Initialize()) {
      cache_info->cache_to_use.reset(
          new CacheStats(kShmCache, cache_info->cache_backend, timer(),
                         statistics()));
    } else {
      message_handler()->Message(
          kWarning, "Unable to initialize shared memory cache: %s.",
          p->first.c_str());
      cache_info->cache_backend = NULL;
      cache_info->cache_to_use.reset(NULL);
    }
  }

  for (PathCacheMap::iterator p = path_cache_map_.begin(),
           e = path_cache_map_.end(); p != e; ++p) {
    SystemCachePath* cache = p->second;
    cache->RootInit();
  }
}

void ApacheRewriteDriverFactory::ChildInit() {
  is_root_process_ = false;
  ParentOrChildInit();
  // Reinitialize pid for child process.
  apache_message_handler_->SetPidString(static_cast<int64>(getpid()));
  apache_html_parse_message_handler_->SetPidString(
      static_cast<int64>(getpid()));
  slow_worker_.reset(new SlowWorker("slow_work_thread", thread_system()));
  if (shared_mem_statistics_.get() != NULL) {
    shared_mem_statistics_->Init(false, message_handler());
  }
  for (MetadataShmCacheMap::iterator p = metadata_shm_caches_.begin(),
           e = metadata_shm_caches_.end(); p != e; ++p) {
    MetadataShmCacheInfo* cache_info = p->second;
    if ((cache_info->cache_backend != NULL) &&
        !cache_info->cache_backend->Attach()) {
      message_handler()->Message(
          kWarning, "Unable to attach to shared memory cache: %s.",
          p->first.c_str());
      cache_info->cache_backend = NULL;
      cache_info->cache_to_use.reset(NULL);
    }
  }

  for (PathCacheMap::iterator p = path_cache_map_.begin(),
           e = path_cache_map_.end(); p != e; ++p) {
    SystemCachePath* cache = p->second;
    cache->ChildInit(slow_worker_.get());
  }
  for (ApacheServerContextSet::iterator p = uninitialized_managers_.begin(),
           e = uninitialized_managers_.end(); p != e; ++p) {
    ApacheServerContext* resource_manager = *p;
    resource_manager->ChildInit();
  }
  uninitialized_managers_.clear();

  for (int i = 0, n = memcache_servers_.size(); i < n; ++i) {
    AprMemCache* mem_cache = memcache_servers_[i];
    if (!mem_cache->Connect()) {
      message_handler()->Message(kError, "Memory cache failed");
      abort();  // TODO(jmarantz): is there a better way to exit?
    }
  }

  mod_spdy_fetch_controller_.reset(
      new ModSpdyFetchController(max_mod_spdy_fetch_threads_, thread_system(),
                                 statistics()));
}

void ApacheRewriteDriverFactory::DumpRefererStatistics(Writer* writer) {
#if ENABLE_REFERER_STATS
  // Note: Referer statistics are only displayed for within the same v-host
  MessageHandler* handler = message_handler();
  if (shared_mem_referer_statistics_ == NULL) {
    writer->Write("mod_pagespeed referer statistics either had an error or "
                  "are not enabled.", handler);
  } else {
    switch (config_->referer_statistics_output_level()) {
      case ApacheConfig::kFast:
        shared_mem_referer_statistics_->DumpFast(writer, handler);
        break;
      case ApacheConfig::kSimple:
        shared_mem_referer_statistics_->DumpSimple(writer, handler);
        break;
      case ApacheConfig::kOrganized:
        shared_mem_referer_statistics_->DumpOrganized(writer, handler);
        break;
    }
  }
#endif
}

void ApacheRewriteDriverFactory::StopCacheActivity() {
  RewriteDriverFactory::StopCacheActivity();

  // Iterate through the map of CacheInterface* objects constructed for
  // the memcached.  Note that these are not typically AprMemCache* objects,
  // but instead are a hierarchy of CacheStats*, CacheBatcher*, AsyncCache*,
  // and AprMemCache*, all of which must be stopped.
  for (MemcachedMap::iterator p = memcached_map_.begin(),
           e = memcached_map_.end(); p != e; ++p) {
    CacheInterface* cache = p->second;
    cache->ShutDown();
  }
}

void ApacheRewriteDriverFactory::ShutDown() {
  if (!is_root_process_) {
    Variable* child_shutdown_count = statistics()->GetVariable(kShutdownCount);
    child_shutdown_count->Add(1);
    message_handler()->Message(kInfo, "Shutting down mod_pagespeed child");
  }
  StopCacheActivity();

  // Next, we shutdown the fetchers before killing the workers in
  // RewriteDriverFactory::ShutDown; this is so any rewrite jobs in progress
  // can quickly wrap up.
  for (FetcherMap::iterator p = fetcher_map_.begin(), e = fetcher_map_.end();
       p != e; ++p) {
    UrlAsyncFetcher* fetcher = p->second;
    fetcher->ShutDown();
    defer_cleanup(new Deleter<UrlAsyncFetcher>(fetcher));
  }
  fetcher_map_.clear();

  RewriteDriverFactory::ShutDown();

  // Take down any memcached threads.  Note that this may block
  // waiting for any wedged operations to terminate, possibly
  // requiring kill -9 to restart Apache if memcached is permanently
  // hung.  In pracice, the patches made in
  // src/third_party/aprutil/apr_memcache2.c make that very unlikely.
  //
  // The alternative scenario of exiting with pending I/O will often
  // crash and always leak memory.  Note that if memcached crashes, as
  // opposed to hanging, it will probably not appear wedged.
  memcached_pool_.reset(NULL);

  // Reset SharedCircularBuffer to NULL, so that any shutdown warnings
  // (e.g. in ServerContext::ShutDownDrivers) don't reference
  // deleted objects as the base-class is deleted.
  apache_message_handler_->set_buffer(NULL);
  apache_html_parse_message_handler_->set_buffer(NULL);

  if (is_root_process_) {
    // Cleanup statistics.
    // TODO(morlovich): This looks dangerous with async.
    if (shared_mem_statistics_.get() != NULL) {
      shared_mem_statistics_->GlobalCleanup(message_handler());
    }
    // Cleanup SharedCircularBuffer.
    // Use GoogleMessageHandler instead of ApacheMessageHandler.
    // As we are cleaning SharedCircularBuffer, we do not want to write to its
    // buffer and passing ApacheMessageHandler here may cause infinite loop.
    GoogleMessageHandler handler;
    if (shared_circular_buffer_ != NULL) {
      shared_circular_buffer_->GlobalCleanup(&handler);
    }
  }
}

// Initializes global statistics object if needed, using factory to
// help with the settings if needed.
// Note: does not call set_statistics() on the factory.
Statistics* ApacheRewriteDriverFactory::MakeGlobalSharedMemStatistics(
    bool logging, int64 logging_interval_ms,
    const GoogleString& logging_file_base) {
  if (shared_mem_statistics_.get() == NULL) {
    shared_mem_statistics_.reset(AllocateAndInitSharedMemStatistics(
        "global", logging, logging_interval_ms, logging_file_base));
  }
  DCHECK(!statistics_frozen_);
  statistics_frozen_ = true;
  SetStatistics(shared_mem_statistics_.get());
  return shared_mem_statistics_.get();
}

SharedMemStatistics* ApacheRewriteDriverFactory::
    AllocateAndInitSharedMemStatistics(
        const StringPiece& name, const bool logging,
        const int64 logging_interval_ms,
        const GoogleString& logging_file_base) {
  // Note that we create the statistics object in the parent process, and
  // it stays around in the kids but gets reinitialized for them
  // inside ChildInit(), called from pagespeed_child_init.
  //
  // TODO(jmarantz): it appears that filename_prefix() is not actually
  // established at the time of this construction, calling into question
  // whether we are naming our shared-memory segments correctly.
  SharedMemStatistics* stats = new SharedMemStatistics(
      logging_interval_ms, StrCat(logging_file_base, name), logging,
      StrCat(filename_prefix(), name), shared_mem_runtime(), message_handler(),
      file_system(), timer());
  InitStats(stats);
  stats->Init(true, message_handler());
  return stats;
}

void ApacheRewriteDriverFactory::Initialize() {
  ApacheConfig::Initialize();
  RewriteDriverFactory::Initialize();
}

void ApacheRewriteDriverFactory::InitStats(Statistics* statistics) {
  // Init standard PSOL stats.
  RewriteDriverFactory::InitStats(statistics);

  // Init Apache-specific stats.
  ApacheServerContext::InitStats(statistics);
  AprMemCache::InitStats(statistics);
  InPlaceResourceRecorder::InitStats(statistics);
  RateController::InitStats(statistics);
  SerfUrlAsyncFetcher::InitStats(statistics);

  CacheStats::InitStats(SystemCachePath::kFileCache, statistics);
  CacheStats::InitStats(SystemCachePath::kLruCache, statistics);
  CacheStats::InitStats(kShmCache, statistics);
  CacheStats::InitStats(kMemcached, statistics);
  PropertyCache::InitCohortStats(BeaconCriticalImagesFinder::kBeaconCohort,
                                 statistics);
  PropertyCache::InitCohortStats(RewriteDriver::kDomCohort, statistics);

  statistics->AddVariable(kShutdownCount);
}

void ApacheRewriteDriverFactory::Terminate() {
  RewriteDriverFactory::Terminate();
  ApacheConfig::Terminate();
  PthreadSharedMem::Terminate();
}

ApacheServerContext* ApacheRewriteDriverFactory::MakeApacheServerContext(
    server_rec* server) {
  ApacheServerContext* rm = new ApacheServerContext(this, server, version_);
  uninitialized_managers_.insert(rm);
  return rm;
}

bool ApacheRewriteDriverFactory::PoolDestroyed(ApacheServerContext* rm) {
  if (uninitialized_managers_.erase(rm) == 1) {
    delete rm;
  }

  // Returns true if all the ServerContexts known by the factory and its
  // superclass are finished.  Then it's time to destroy the factory.  Note
  // that ApacheRewriteDriverFactory keeps track of ServerContexts that
  // are partially constructed.  RewriteDriverFactory keeps track of
  // ServerContexts that are already serving requests.  We need to clean
  // all of them out before we can terminate the driver.
  bool no_active_resource_managers = TerminateServerContext(rm);
  return (no_active_resource_managers && uninitialized_managers_.empty());
}

RewriteOptions* ApacheRewriteDriverFactory::NewRewriteOptions() {
  return new ApacheConfig(hostname_identifier_);
}

RewriteOptions* ApacheRewriteDriverFactory::NewRewriteOptionsForQuery() {
  return new ApacheConfig("query");
}

void ApacheRewriteDriverFactory::PrintMemCacheStats(GoogleString* out) {
  for (int i = 0, n = memcache_servers_.size(); i < n; ++i) {
    AprMemCache* mem_cache = memcache_servers_[i];
    if (!mem_cache->GetStatus(out)) {
      StrAppend(out, "\nError getting memcached server status for ",
                mem_cache->server_spec());
    }
  }
}

}  // namespace net_instaweb