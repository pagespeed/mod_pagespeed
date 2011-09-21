// Copyright 2011 Google Inc.
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

#include "net/instaweb/apache/apache_resource_manager.h"

#include "httpd.h"
#include "net/instaweb/apache/apache_cache.h"
#include "net/instaweb/apache/apache_config.h"
#include "net/instaweb/apache/apache_rewrite_driver_factory.h"
#include "net/instaweb/apache/serf_url_async_fetcher.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_system.h"

namespace net_instaweb {

class Statistics;
class RewriteStats;
class HTTPCache;

ApacheResourceManager::ApacheResourceManager(
    ApacheRewriteDriverFactory* factory,
    server_rec* server,
    const StringPiece& version)
    : ResourceManager(factory),
      apache_factory_(factory),
      server_rec_(server),
      version_(version.data(), version.size()),
      hostname_identifier_(StrCat(server->server_hostname, ":",
                                  IntegerToString(server->port))),
      initialized_(false),
      config_(new ApacheConfig(hostname_identifier_)),
      subresource_fetcher_(NULL) {
  // In Apache, we default to using the "core filters". Note that this is not
  // the only place the default is applied --- for directories with .htaccess
  // files it is given in create_dir_config in mod_instaweb.cc
  config_->SetDefaultRewriteLevel(RewriteOptions::kCoreFilters);
}

ApacheResourceManager::~ApacheResourceManager() {
}

// Override the ResourceManager definition of RewriteOptions.  This is
// in the .cc file rather than the .h because we forward-declare
// ApacheConfig in the .h and so the compiler doesn't know it's a
// subclass.
RewriteOptions* ApacheResourceManager::options() {
  return config_.get();
}

bool ApacheResourceManager::InitFileCachePath() {
  if (file_system()->IsDir(config_->file_cache_path().c_str(),
                           message_handler()).is_true()) {
    return true;
  }
  bool ok = file_system()->RecursivelyMakeDir(config_->file_cache_path(),
                                              message_handler());
  if (ok) {
    apache_factory_->AddCreatedDirectory(config_->file_cache_path());
  }
  return ok;
}

void ApacheResourceManager::ChildInit() {
  DCHECK(!initialized_);
  if (!initialized_) {
    initialized_ = true;
    ApacheCache* cache = apache_factory_->GetCache(config_.get());
    set_http_cache(cache->http_cache());
    set_metadata_cache(cache->cache());
    set_lock_manager(cache->lock_manager());
    UrlPollableAsyncFetcher* fetcher = apache_factory_->GetFetcher(
        config_.get());
    set_url_async_fetcher(fetcher);
    if (!config_->slurping_enabled_read_only()) {
      subresource_fetcher_ = fetcher;
    }
    apache_factory_->InitResourceManager(this);
  }
}

bool ApacheResourceManager::PoolDestroyed() {
  ShutDownDrivers();
  return apache_factory_->PoolDestroyed(this);
}

}  // namespace net_instaweb
