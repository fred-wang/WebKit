/*
 * Copyright (C) 2014-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "NetworkCache.h"

#include "AsyncRevalidation.h"
#include "Logging.h"
#include "NetworkCacheSpeculativeLoad.h"
#include "NetworkCacheSpeculativeLoadManager.h"
#include "NetworkCacheStorage.h"
#include "NetworkProcess.h"
#include "NetworkSession.h"
#include "WebsiteDataType.h"
#include <WebCore/CacheValidation.h>
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/HTTPStatusCodes.h>
#include <WebCore/LowPowerModeNotifier.h>
#include <WebCore/NetworkStorageSession.h>
#include <WebCore/RegistrableDomain.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/ThermalMitigationNotifier.h>
#include <wtf/FileSystem.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/SuperFastHash.h>

#if PLATFORM(COCOA)
#include <notify.h>
#include <wtf/darwin/DispatchExtras.h>
#endif

#include <WebCore/ExceptionOr.h>
#include <WebCore/URLPattern.h>
#include <WebCore/URLPatternOptions.h>

namespace WebKit {
namespace NetworkCache {

using namespace FileSystem;

WTF_MAKE_TZONE_ALLOCATED_IMPL(Cache::RetrieveInfo);

static const AtomString& resourceType()
{
    ASSERT(WTF::RunLoop::isMain());
    static NeverDestroyed<const AtomString> resource("Resource"_s);
    return resource;
}

static const AtomString& compressionDictionaryType()
{
    static MainRunLoopNeverDestroyed<const AtomString> compressionDictionary("CompressionDictionary"_s);
    return compressionDictionary;
}

class CompressionDictionaryHash {
public:
    CompressionDictionaryHash() = default;
    explicit CompressionDictionaryHash(const std::array<uint8_t, Entry::CompressionDictionaryData::hashSize>& hash)
        : m_hash(hash)
        , m_state(State::Value)
    {
    }
    explicit CompressionDictionaryHash(WTF::HashTableEmptyValueType)
        : m_state(State::Empty)
    {
    }
    explicit CompressionDictionaryHash(WTF::HashTableDeletedValueType)
        : m_state(State::Deleted)
    {
    }

    const std::array<uint8_t, Entry::CompressionDictionaryData::hashSize>& hash() const { return m_hash; }
    bool isEmptyValue() const { return m_state == State::Empty; }
    bool isDeletedValue() const { return m_state == State::Deleted; }
    bool operator==(const CompressionDictionaryHash&) const = default;

private:
    enum class State : uint8_t { Empty, Deleted, Value };

    std::array<uint8_t, Entry::CompressionDictionaryData::hashSize> m_hash { };
    State m_state { State::Empty };
};

struct CompressionDictionaryHashHash {
    static unsigned hash(const CompressionDictionaryHash& hash)
    {
        return SuperFastHash::computeHash(std::span<const uint8_t> { hash.hash() });
    }
    static bool equal(const CompressionDictionaryHash& a, const CompressionDictionaryHash& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = false;
};

struct CompressionDictionaryHashTraits : WTF::GenericHashTraits<CompressionDictionaryHash> {
    static constexpr bool emptyValueIsZero = false;
    static constexpr bool hasIsEmptyValueFunction = true;

    static CompressionDictionaryHash emptyValue() { return CompressionDictionaryHash { WTF::HashTableEmptyValue }; }
    static bool isEmptyValue(const CompressionDictionaryHash& hash) { return hash.isEmptyValue(); }
    static void constructDeletedValue(CompressionDictionaryHash& hash) { new (NotNull, std::addressof(hash)) CompressionDictionaryHash { WTF::HashTableDeletedValue }; }
    static bool isDeletedValue(const CompressionDictionaryHash& hash) { return hash.isDeletedValue(); }
};

struct CompressionDictionaryKeyHash : WTF::NetworkCacheKeyHash {
    static bool equal(const Key& a, const Key& b) { return a.hash() == b.hash(); }
};

static String normalizedCompressionDictionaryPartition(const String& partition)
{
    return partition.isNull() ? emptyString() : partition;
}

class CompressionDictionaryCache {
    WTF_MAKE_TZONE_ALLOCATED(CompressionDictionaryCache);
public:
    using ReadyHandler = CompletionHandler<void()>;

    bool isInitialized(const String& partition) const { return m_initializedPartitions.contains(normalizedCompressionDictionaryPartition(partition)); }

    bool addPendingHandler(const String& partition, ReadyHandler&& handler)
    {
        auto result = m_pendingHandlers.add(normalizedCompressionDictionaryPartition(partition), Vector<ReadyHandler> { });
        result.iterator->value.append(WTF::move(handler));
        return result.isNewEntry;
    }

    Vector<ReadyHandler> takePendingHandlers(const String& partition)
    {
        return m_pendingHandlers.take(normalizedCompressionDictionaryPartition(partition));
    }

    uint64_t generation() const { return m_generation; }

    void markInitialized(const String& partition)
    {
        m_initializedPartitions.add(normalizedCompressionDictionaryPartition(partition));
    }

    void add(const Entry& entry, RefPtr<WebCore::FragmentedSharedBuffer>&& bufferToCacheInMemory = { })
    {
        auto& data = entry.compressionDictionaryData();
        auto baseURL = entry.response().url().string();
        auto result = WebCore::URLPattern::createWithoutRegExpSupport(data.match, WTF::move(baseURL), { });
        if (result.hasException()) {
            remove(entry.key());
            return;
        }

        auto now = WallTime::now();
        auto freshnessLifetime = WebCore::computeFreshnessLifetimeForHTTPFamily(entry.response(), entry.timeStamp());
        auto currentAge = WebCore::computeCurrentAge(entry.response(), entry.timeStamp());
        if (currentAge > freshnessLifetime) {
            remove(entry.key());
            return;
        }
        auto expirationTime = now + freshnessLifetime - currentAge;

        auto partition = normalizedCompressionDictionaryPartition(entry.key().partition());
        auto& entries = m_entries.ensure(partition, [] {
            return DictionaryMap { };
        }).iterator->value;
        if (auto existing = entries.find(entry.key()); existing != entries.end()) {
            if (existing->value->timeStamp >= entry.timeStamp())
                return;
            removeHashReference(partition, existing->value->hash, entry.key());
        }

        entries.set(entry.key(), makeUnique<Dictionary>(Dictionary {
            result.releaseReturnValue(),
            WebCore::SecurityOriginData::fromURLWithoutStrictOpaqueness(entry.response().url()),
            data.id,
            data.hash,
            data.matchDest,
            data.match.length(),
            entry.timeStamp(),
            expirationTime
        }));

        m_keysByHash.ensure(partition, [] {
            return HashMapByHash { };
        }).iterator->value.set(CompressionDictionaryHash { data.hash }, DictionaryStorageData { entry.key(), WTF::move(bufferToCacheInMemory) });
    }

    void remove(const Key& key)
    {
        auto partition = normalizedCompressionDictionaryPartition(key.partition());
        auto partitionIterator = m_entries.find(partition);
        if (partitionIterator == m_entries.end())
            return;
        auto entryIterator = partitionIterator->value.find(key);
        if (entryIterator == partitionIterator->value.end())
            return;

        auto hash = entryIterator->value->hash;
        partitionIterator->value.remove(entryIterator);
        removeHashReference(partition, hash, key);
        if (partitionIterator->value.isEmpty())
            m_entries.remove(partitionIterator);
    }

    void clear()
    {
        m_entries.clear();
        m_keysByHash.clear();
        m_initializedPartitions.clear();
        ++m_generation;
    }

    std::optional<Cache::CompressionDictionaryMatch> bestMatch(const String& partition, const URL& url, WebCore::FetchOptionsDestination destination) const
    {
        auto iterator = m_entries.find(normalizedCompressionDictionaryPartition(partition));
        if (iterator == m_entries.end())
            return std::nullopt;

        auto now = WallTime::now();
        const Dictionary* bestMatch = nullptr;
        for (auto& dictionary : iterator->value.values()) {
            auto& entry = *dictionary;
            if (entry.expirationTime < now)
                continue;
            if (!entry.matchDest.isEmpty() && !entry.matchDest.contains(destination))
                continue;
            if (!entry.pattern->testWithoutRegExp(url))
                continue;

            if (bestMatch) {
                bool isBetterMatch = false;
                if (entry.matchDest.isEmpty() != bestMatch->matchDest.isEmpty())
                    isBetterMatch = !entry.matchDest.isEmpty();
                else if (entry.matchLength != bestMatch->matchLength)
                    isBetterMatch = entry.matchLength > bestMatch->matchLength;
                else
                    isBetterMatch = entry.timeStamp > bestMatch->timeStamp;
                if (!isBetterMatch)
                    continue;
            }
            bestMatch = &entry;
        }

        if (!bestMatch)
            return std::nullopt;
        return Cache::CompressionDictionaryMatch { bestMatch->hash, bestMatch->id };
    }

    std::optional<std::pair<Key, RefPtr<WebCore::FragmentedSharedBuffer>>> dataForHash(const String& partition, const std::array<uint8_t, Entry::CompressionDictionaryData::hashSize>& hash) const
    {
        auto partitionIterator = m_keysByHash.find(normalizedCompressionDictionaryPartition(partition));
        if (partitionIterator == m_keysByHash.end())
            return std::nullopt;
        auto hashIterator = partitionIterator->value.find(CompressionDictionaryHash { hash });
        if (hashIterator == partitionIterator->value.end())
            return std::nullopt;
        return std::pair { hashIterator->value.key, hashIterator->value.buffer };
    }

    Vector<Key> keysForOrigin(const String& partition, const std::optional<WebCore::SecurityOriginData>& origin) const
    {
        Vector<Key> result;
        auto iterator = m_entries.find(normalizedCompressionDictionaryPartition(partition));
        if (iterator == m_entries.end())
            return result;
        for (auto& [key, dictionary] : iterator->value) {
            if (!origin || dictionary->origin == *origin)
                result.append(key);
        }
        return result;
    }
private:
    struct Dictionary {
        WTF_MAKE_STRUCT_TZONE_ALLOCATED(Dictionary);
        Ref<WebCore::URLPattern> pattern;
        WebCore::SecurityOriginData origin;
        String id;
        std::array<uint8_t, Entry::CompressionDictionaryData::hashSize> hash;
        Vector<WebCore::FetchOptionsDestination> matchDest;
        size_t matchLength;
        WallTime timeStamp;
        WallTime expirationTime;
    };

    struct DictionaryStorageData {
        Key key;
        RefPtr<WebCore::FragmentedSharedBuffer> buffer;
    };

    using DictionaryMap = HashMap<Key, std::unique_ptr<Dictionary>, CompressionDictionaryKeyHash>;
    using HashMapByHash = HashMap<CompressionDictionaryHash, DictionaryStorageData, CompressionDictionaryHashHash, CompressionDictionaryHashTraits>;

    void removeHashReference(const String& partition, const std::array<uint8_t, Entry::CompressionDictionaryData::hashSize>& hash, const Key& key)
    {
        auto normalizedPartition = normalizedCompressionDictionaryPartition(partition);
        auto partitionIterator = m_keysByHash.find(normalizedPartition);
        if (partitionIterator == m_keysByHash.end())
            return;
        auto hashIterator = partitionIterator->value.find(CompressionDictionaryHash { hash });
        if (hashIterator == partitionIterator->value.end() || hashIterator->value.key.hash() != key.hash())
            return;
        auto buffer = hashIterator->value.buffer;
        partitionIterator->value.remove(hashIterator);

        if (auto entriesIterator = m_entries.find(normalizedPartition); entriesIterator != m_entries.end()) {
            for (auto& [candidateKey, dictionary] : entriesIterator->value) {
                if (candidateKey.hash() != key.hash() && dictionary->hash == hash) {
                    partitionIterator->value.set(CompressionDictionaryHash { hash }, DictionaryStorageData { candidateKey, WTF::move(buffer) });
                    return;
                }
            }
        }
        if (partitionIterator->value.isEmpty())
            m_keysByHash.remove(partitionIterator);
    }

    HashMap<String, DictionaryMap> m_entries;
    HashMap<String, HashMapByHash> m_keysByHash;
    HashSet<String> m_initializedPartitions;
    HashMap<String, Vector<ReadyHandler>> m_pendingHandlers;
    uint64_t m_generation { 0 };
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(CompressionDictionaryCache);
WTF_MAKE_TZONE_ALLOCATED_IMPL(CompressionDictionaryCache::Dictionary);

static size_t computeCapacity(CacheModel cacheModel, const String& cachePath)
{
    if (auto diskFreeSize = FileSystem::volumeFreeSpace(cachePath)) {
        // As a fudge factor, use 1000 instead of 1024, in case the reported byte
        // count doesn't align exactly to a megabyte boundary.
        *diskFreeSize /= KB * 1000;
        return calculateURLCacheDiskCapacity(cacheModel, *diskFreeSize);
    }
    return 0;
}

RefPtr<Cache> Cache::open(NetworkProcess& networkProcess, const String& cachePath, OptionSet<CacheOption> options, PAL::SessionID sessionID)
{
    if (!FileSystem::makeAllDirectories(cachePath))
        return nullptr;

    auto cacheModel = networkProcess.cacheModel();
    auto capacity = computeCapacity(cacheModel, cachePath);

    // Cache a small number of recently used memory mapped main resource blobs to speed up hot loads of
    // recently visited websites.
    size_t mainResourceBlobMemoryCacheFileLimit = cacheModel == CacheModel::PrimaryWebBrowser ? 32 : 0;

    auto storage = Storage::open(cachePath, options.contains(CacheOption::TestingMode) ? Storage::Mode::AvoidRandomness : Storage::Mode::Normal, capacity, mainResourceBlobMemoryCacheFileLimit);

    LOG(NetworkCache, "(NetworkProcess) opened cache storage, success %d", !!storage);

    if (!storage)
        return nullptr;

    return adoptRef(*new Cache(networkProcess, cachePath, storage.releaseNonNull(), options, sessionID));
}

#if PLATFORM(GTK) || PLATFORM(WPE)
static void dumpFileChanged(Cache* cache)
{
    cache->dumpContentsToFile();
}
#endif

Cache::Cache(NetworkProcess& networkProcess, const String& storageDirectory, Ref<Storage>&& storage, OptionSet<CacheOption> options, PAL::SessionID sessionID)
    : m_storage(WTF::move(storage))
    , m_compressionDictionaryCache(makeUnique<CompressionDictionaryCache>())
    , m_networkProcess(networkProcess)
    , m_sessionID(sessionID)
    , m_storageDirectory(storageDirectory)
{
    if (options.contains(CacheOption::SpeculativeRevalidation)) {
        m_lowPowerModeNotifier = makeUnique<WebCore::LowPowerModeNotifier>([weakThis = WeakPtr { *this }](bool) {
            if (RefPtr protectedThis = weakThis)
                protectedThis->updateSpeculativeLoadManagerEnabledState();
        });
        m_thermalMitigationNotifier = WebCore::ThermalMitigationNotifier::create([weakThis = WeakPtr { *this }](bool) {
            if (RefPtr protectedThis = weakThis)
                protectedThis->updateSpeculativeLoadManagerEnabledState();
        });
        if (shouldUseSpeculativeLoadManager())
            m_speculativeLoadManager = makeUnique<SpeculativeLoadManager>(*this, protect(m_storage));
    }

    if (options.contains(CacheOption::RegisterNotify)) {
#if PLATFORM(COCOA)
        // Triggers with "notifyutil -p com.apple.WebKit.Cache.dump".
        int token;
        notify_register_dispatch("com.apple.WebKit.Cache.dump", &token, mainDispatchQueueSingleton(), ^(int) {
            dumpContentsToFile();
        });
#endif
#if PLATFORM(GTK) || PLATFORM(WPE)
        // Triggers with "touch $cachePath/dump".
        CString dumpFilePath = fileSystemRepresentation(pathByAppendingComponent(m_storage->basePathIsolatedCopy(), "dump"_s));
        GRefPtr<GFile> dumpFile = adoptGRef(g_file_new_for_path(dumpFilePath.data()));
        GFileMonitor* monitor = g_file_monitor_file(dumpFile.get(), G_FILE_MONITOR_NONE, nullptr, nullptr);
        g_signal_connect_swapped(monitor, "changed", G_CALLBACK(dumpFileChanged), this);
#endif
    }
}

Cache::~Cache() = default;

size_t Cache::capacity() const
{
    return m_storage->capacity();
}

void Cache::updateCapacity()
{
    auto newCapacity = computeCapacity(m_networkProcess->cacheModel(), m_storage->basePathIsolatedCopy());
    m_storage->setCapacity(newCapacity);
}

Key Cache::makeCacheKey(const String& type, const WebCore::ResourceRequest& request)
{
    // FIXME: This implements minimal Range header disk cache support. We don't parse
    // ranges so only the same exact range request will be served from the cache.
    String range = request.httpHeaderField(WebCore::HTTPHeaderName::Range);
    return { request.cachePartition(), type, range, request.url().stringWithoutFragmentIdentifier(), m_storage->salt() };
}

static bool NODELETE cachePolicyAllowsExpired(WebCore::ResourceRequestCachePolicy policy)
{
    switch (policy) {
    case WebCore::ResourceRequestCachePolicy::ReturnCacheDataElseLoad:
    case WebCore::ResourceRequestCachePolicy::ReturnCacheDataDontLoad:
        return true;
    case WebCore::ResourceRequestCachePolicy::UseProtocolCachePolicy:
    case WebCore::ResourceRequestCachePolicy::ReloadIgnoringCacheData:
    case WebCore::ResourceRequestCachePolicy::RefreshAnyCacheData:
        return false;
    case WebCore::ResourceRequestCachePolicy::DoNotUseAnyCache:
        ASSERT_NOT_REACHED();
        return false;
    }
    return false;
}

static UseDecision responseNeedsRevalidation(NetworkSession& networkSession, const WebCore::ResourceResponse& response, WallTime timestamp, const WebCore::CacheControlDirectives& requestDirectives)
{
    if (response.cacheControlContainsNoCache())
        return UseDecision::Validate;

    auto age = WebCore::computeCurrentAge(response, timestamp);
    auto lifetime = WebCore::computeFreshnessLifetimeForHTTPFamily(response, timestamp);

    // Request max-age=0 (like no-cache) always forces revalidation.
    if (requestDirectives.maxAge && requestDirectives.maxAge.value() == 0_ms)
        return UseDecision::Validate;

    if (age <= lifetime) {
        if (requestDirectives.maxAge && age > requestDirectives.maxAge.value())
            return UseDecision::Validate;

        if (requestDirectives.minFresh && age + requestDirectives.minFresh.value() > lifetime)
            return UseDecision::Validate;
    }

    auto maxStale = requestDirectives.maxStale;
    auto maximumStaleness = maxStale ? maxStale.value() : 0_ms;
    bool hasExpired = age - lifetime > maximumStaleness;
    if (hasExpired && !maxStale && networkSession.isStaleWhileRevalidateEnabled()) {
        auto responseMaxStaleness = response.cacheControlStaleWhileRevalidate();
        maximumStaleness += responseMaxStaleness ? responseMaxStaleness.value() : 0_ms;
        bool inResponseStaleness = age - lifetime < maximumStaleness;
        if (inResponseStaleness)
            return UseDecision::AsyncRevalidate;
    }

    if (hasExpired) {
#ifndef LOG_DISABLED
        LOG(NetworkCache, "(NetworkProcess) needsRevalidation hasExpired age=%f lifetime=%f max-staleness=%f", age, lifetime, maximumStaleness);
#endif
        return UseDecision::Validate;
    }

    return UseDecision::Use;
}

static UseDecision responseNeedsRevalidation(NetworkSession& networkSession, const WebCore::ResourceResponse& response, const WebCore::ResourceRequest& request, WallTime timestamp)
{
    auto requestDirectives = WebCore::parseCacheControlDirectives(request.httpHeaderFields());
    if (requestDirectives.noCache)
        return UseDecision::Validate;
    // A request carrying no-store must not be satisfied from cache.
    if (requestDirectives.noStore)
        return UseDecision::Validate;

    return responseNeedsRevalidation(networkSession, response, timestamp, requestDirectives);
}

static UseDecision makeUseDecision(NetworkProcess& networkProcess, PAL::SessionID sessionID, const Entry& entry, const WebCore::ResourceRequest& request)
{
    // The request is conditional so we force revalidation from the network. We merely check the disk cache
    // so we can update the cache entry.
    if (request.isConditional() && !entry.redirectRequest())
        return UseDecision::Validate;

    if (!WebCore::verifyVaryingRequestHeaders(protect(networkProcess.storageSession(sessionID)), entry.varyingRequestHeaders(), request))
        return UseDecision::NoDueToVaryingHeaderMismatch;

    // We never revalidate in the case of a history navigation.
    if (cachePolicyAllowsExpired(request.cachePolicy()))
        return UseDecision::Use;

    // We could have cached a redirect without a fragment and now may have
    // a fragment in the URL.
    if (request.url().hasFragmentIdentifier() && entry.redirectRequest())
        return UseDecision::NoDueToRequestContainingFragments;

    auto decision = responseNeedsRevalidation(*protect(networkProcess.networkSession(sessionID)), entry.response(), request, entry.timeStamp());
    if (decision != UseDecision::Validate)
        return decision;

    if (!entry.response().hasCacheValidatorFields())
        return UseDecision::NoDueToMissingValidatorFields;

    return entry.redirectRequest() ? UseDecision::NoDueToExpiredRedirect : UseDecision::Validate;
}

static RetrieveDecision makeRetrieveDecision(const WebCore::ResourceRequest& request)
{
    ASSERT(request.cachePolicy() != WebCore::ResourceRequestCachePolicy::DoNotUseAnyCache);

    // FIXME: Support HEAD requests.
    if (request.httpMethod() != "GET"_s)
        return RetrieveDecision::NoDueToHTTPMethod;
    if (request.cachePolicy() == WebCore::ResourceRequestCachePolicy::ReloadIgnoringCacheData && !request.isConditional())
        return RetrieveDecision::NoDueToReloadIgnoringCache;

    return RetrieveDecision::Yes;
}

static bool isMediaMIMEType(const String& type)
{
    return startsWithLettersIgnoringASCIICase(type, "video/"_s) || startsWithLettersIgnoringASCIICase(type, "audio/"_s);
}

static StoreDecision makeStoreDecision(const WebCore::ResourceRequest& originalRequest, const WebCore::ResourceResponse& response, size_t bodySize)
{
    if (!originalRequest.url().protocolIsInHTTPFamily() || !response.isInHTTPFamily())
        return StoreDecision::NoDueToProtocol;

    if (originalRequest.httpMethod() != "GET"_s)
        return StoreDecision::NoDueToHTTPMethod;

    auto requestDirectives = WebCore::parseCacheControlDirectives(originalRequest.httpHeaderFields());
    if (requestDirectives.noStore)
        return StoreDecision::NoDueToNoStoreRequest;

    if (response.cacheControlContainsNoStore())
        return StoreDecision::NoDueToNoStoreResponse;

    if (response.httpStatusCode() == httpStatus304NotModified)
        return StoreDecision::NoDueToHTTPStatusCode;

    if (!WebCore::isStatusCodeCacheableByDefault(response.httpStatusCode())) {
        // http://tools.ietf.org/html/rfc7234#section-4.3.2
        bool hasExpirationHeaders = response.expires() || response.cacheControlMaxAge();
        if (!hasExpirationHeaders && !response.cacheControlContainsPublic())
            return StoreDecision::NoDueToMissingExpirationHeaders;
    }

    // FIXME: We are not correctly computing the redirected request URL in case original request
    // has a fragment identifier and response location URL does not have one. Let's not store it for now.
    if ((response.isRedirection() || response.isRedirected()) && originalRequest.url().hasFragmentIdentifier())
        return StoreDecision::NoDueToRequestContainingFragments;

    bool isMainResource = originalRequest.requester() == WebCore::ResourceRequestRequester::Main;
    bool storeUnconditionallyForHistoryNavigation = isMainResource || originalRequest.priority() == WebCore::ResourceLoadPriority::VeryHigh;
    if (!storeUnconditionallyForHistoryNavigation) {
        auto now = WallTime::now();
        Seconds allowedStale { 0_ms };
        if (auto value = response.cacheControlStaleWhileRevalidate())
            allowedStale = value.value();
        bool hasNonZeroLifetime = !response.cacheControlContainsNoCache() && (WebCore::computeFreshnessLifetimeForHTTPFamily(response, now) > 0_ms || allowedStale > 0_ms);
        bool possiblyReusable = response.hasCacheValidatorFields() || hasNonZeroLifetime;
        if (!possiblyReusable)
            return StoreDecision::NoDueToUnlikelyToReuse;
    }

    // Media loaded via XHR is likely being used for MSE streaming (YouTube and Netflix for example).
    // Streaming media fills the cache quickly and is unlikely to be reused.
    // FIXME: We should introduce a separate media cache partition that doesn't affect other resources.
    // FIXME: We should also make sure make the MSE paths are copy-free so we can use mapped buffers from disk effectively.
    auto requester = originalRequest.requester();
    bool isDefinitelyStreamingMedia = requester == WebCore::ResourceRequestRequester::Media;
    bool isLikelyStreamingMedia = requester == WebCore::ResourceRequestRequester::XHR && isMediaMIMEType(response.mimeType());
    if (isLikelyStreamingMedia || isDefinitelyStreamingMedia)
        return StoreDecision::NoDueToStreamingMedia;

    return StoreDecision::Yes;
}

bool Cache::shouldUseSpeculativeLoadManager() const
{
    CheckedPtr lowPowerModeNotifier = m_lowPowerModeNotifier.get();
    bool isLowPowerModeEnabled = lowPowerModeNotifier && lowPowerModeNotifier->isLowPowerModeEnabled();
    bool isThermalMitigationEnabled = m_thermalMitigationNotifier && m_thermalMitigationNotifier->isThermalMitigationEnabled();
    return !isLowPowerModeEnabled && !isThermalMitigationEnabled;
}

void Cache::updateSpeculativeLoadManagerEnabledState()
{
    ASSERT(WTF::RunLoop::isMain());

    bool shouldEnable = shouldUseSpeculativeLoadManager();
    if (!shouldEnable && m_speculativeLoadManager) {
        m_speculativeLoadManager = nullptr;
        RELEASE_LOG(NetworkCacheSpeculativePreloading, "%p - Cache::updateSpeculativeLoadManagerEnabledState: disabling speculative loads due to low power mode or thermal change", this);
    } else if (shouldEnable && !m_speculativeLoadManager) {
        m_speculativeLoadManager = makeUnique<SpeculativeLoadManager>(*this, protect(m_storage));
        RELEASE_LOG(NetworkCacheSpeculativePreloading, "%p - Cache::updateSpeculativeLoadManagerEnabledState: enabling speculative loads due to low power mode or thermal change", this);
    }
}

static bool inline canRequestUseSpeculativeRevalidation(const WebCore::ResourceRequest& request)
{
    if (request.isConditional())
        return false;

    if (request.requester() == WebCore::ResourceRequestRequester::XHR || request.requester() == WebCore::ResourceRequestRequester::Fetch)
        return false;

    switch (request.cachePolicy()) {
    case WebCore::ResourceRequestCachePolicy::ReturnCacheDataElseLoad:
    case WebCore::ResourceRequestCachePolicy::ReturnCacheDataDontLoad:
    case WebCore::ResourceRequestCachePolicy::ReloadIgnoringCacheData:
        return false;
    case WebCore::ResourceRequestCachePolicy::UseProtocolCachePolicy:
    case WebCore::ResourceRequestCachePolicy::RefreshAnyCacheData:
        return true;
    case WebCore::ResourceRequestCachePolicy::DoNotUseAnyCache:
        ASSERT_NOT_REACHED();
        return false;
    }
    return false;
}

void Cache::startAsyncRevalidationIfNeeded(const WebCore::ResourceRequest& request, const NetworkCache::Key& key, std::unique_ptr<Entry>&& entry, const GlobalFrameID& frameID, std::optional<NavigatingToAppBoundDomain> isNavigatingToAppBoundDomain, bool allowPrivacyProxy, OptionSet<WebCore::AdvancedPrivacyProtections> advancedPrivacyProtections)
{
    m_pendingAsyncRevalidations.ensure(key, [&] {
        auto addResult = m_pendingAsyncRevalidationByPage.ensure(frameID, [] {
            return WeakHashSet<AsyncRevalidation>();
        });
        Ref revalidation = AsyncRevalidation::create(*this, frameID, request, WTF::move(entry), isNavigatingToAppBoundDomain, allowPrivacyProxy, advancedPrivacyProtections, [weakThis = WeakPtr { *this }, key](auto result) {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis)
                return;
            ASSERT(protectedThis->m_pendingAsyncRevalidations.contains(key));
            protectedThis->m_pendingAsyncRevalidations.remove(key);
            LOG(NetworkCache, "(NetworkProcess) revalidation completed for '%s' with result %d", key.identifier().utf8().data(), static_cast<int>(result));
        });
        addResult.iterator->value.add(revalidation.get());
        return revalidation;
    });
}

void Cache::browsingContextRemoved(WebPageProxyIdentifier webPageProxyID, WebCore::PageIdentifier webPageID, WebCore::FrameIdentifier webFrameID)
{
    auto loaders = m_pendingAsyncRevalidationByPage.take({ webPageProxyID, webPageID, webFrameID });
    for (Ref loader : loaders)
        loader->cancel();
}

void Cache::retrieve(const WebCore::ResourceRequest& request, std::optional<GlobalFrameID> frameID, std::optional<NavigatingToAppBoundDomain> isNavigatingToAppBoundDomain, bool allowPrivacyProxy, OptionSet<WebCore::AdvancedPrivacyProtections> advancedPrivacyProtections, RetrieveCompletionHandler&& completionHandler)
{
    ASSERT(request.url().protocolIsInHTTPFamily());

    LOG(NetworkCache, "(NetworkProcess) retrieving %s priority %d", request.url().stringWithoutFragmentIdentifier().ascii().data(), static_cast<int>(request.priority()));

    Key storageKey = makeCacheKey(resourceType(), request);
    auto priority = static_cast<unsigned>(request.priority());

    RetrieveInfo info;
    info.url = request.url();
    info.startTime = MonotonicTime::now();
    info.priority = priority;

    CheckedPtr speculativeLoadManager = m_speculativeLoadManager.get();
    bool canUseSpeculativeRevalidation = frameID && speculativeLoadManager && canRequestUseSpeculativeRevalidation(request);
    if (canUseSpeculativeRevalidation)
        speculativeLoadManager->registerLoad(*frameID, request, storageKey, isNavigatingToAppBoundDomain, allowPrivacyProxy, advancedPrivacyProtections);

    auto retrieveDecision = makeRetrieveDecision(request);
    info.retrieveDecision = retrieveDecision;
    if (retrieveDecision != RetrieveDecision::Yes) {
        completeRetrieve(WTF::move(completionHandler), nullptr, info);
        return;
    }

    info.speculativeLoadDecision = SpeculativeLoadDecision::NoDueToCannotUse;
    if (canUseSpeculativeRevalidation && speculativeLoadManager->canRetrieve(storageKey, request, *frameID)) {
        speculativeLoadManager->retrieve(storageKey, [networkProcess = Ref { networkProcess() }, request, completionHandler = WTF::move(completionHandler), info = crossThreadCopy(WTF::move(info)), sessionID = m_sessionID](std::unique_ptr<Entry> entry) mutable {
            if (entry && WebCore::verifyVaryingRequestHeaders(protect(networkProcess->storageSession(sessionID)), entry->varyingRequestHeaders(), request)) {
                info.speculativeLoadDecision = SpeculativeLoadDecision::Yes;
                completeRetrieve(WTF::move(completionHandler), WTF::move(entry), info);
            } else {
                info.speculativeLoadDecision = SpeculativeLoadDecision::NoDueToVaryingHeaderMismatch;
                completeRetrieve(WTF::move(completionHandler), nullptr, info);
            }
        });
        return;
    }

    m_storage->retrieve(storageKey, priority, [this, protectedThis = Ref { *this }, request, completionHandler = WTF::move(completionHandler), info = crossThreadCopy(WTF::move(info)), storageKey, networkProcess = Ref { networkProcess() }, sessionID = m_sessionID, frameID, isNavigatingToAppBoundDomain, allowPrivacyProxy, advancedPrivacyProtections](auto record, auto timings) mutable {
        info.storageTimings = timings;

        if (record.isNull()) {
            LOG(NetworkCache, "(NetworkProcess) not found in storage");
            completeRetrieve(WTF::move(completionHandler), nullptr, info);
            return false;
        }

        ASSERT(record.key == storageKey);

        auto entry = Entry::decodeStorageRecord(record);

        // FIXME: This is a workaround for rdar://181130091, which we can drop after a release.
        if (entry && entry->response().httpStatusCode() == httpStatus304NotModified) {
            LOG(NetworkCache, "(NetworkProcess) discarding poisoned 304 entry from disk cache (rdar://181130091)");
            completeRetrieve(WTF::move(completionHandler), nullptr, info);
            return false;
        }

        auto useDecision = entry ? makeUseDecision(networkProcess, sessionID, *entry, request) : UseDecision::NoDueToDecodeFailure;
        info.useDecision = useDecision;

        switch (useDecision) {
        case UseDecision::AsyncRevalidate: {
            auto entryCopy = makeUnique<Entry>(*entry);
            entryCopy->setNeedsValidation(true);
            startAsyncRevalidationIfNeeded(request, storageKey, WTF::move(entryCopy), *frameID, isNavigatingToAppBoundDomain, allowPrivacyProxy, advancedPrivacyProtections);
            [[fallthrough]];
        }
        case UseDecision::Use:
            break;
        case UseDecision::Validate:
            entry->setNeedsValidation(true);
            break;
        default:
            entry = nullptr;
        };

#if !LOG_DISABLED
        auto elapsed = MonotonicTime::now() - info.startTime;
        LOG(NetworkCache, "(NetworkProcess) retrieve complete useDecision=%d priority=%d time=%" PRIi64 "ms", static_cast<int>(useDecision), static_cast<int>(request.priority()), elapsed.millisecondsAs<int64_t>());
#endif
        completeRetrieve(WTF::move(completionHandler), WTF::move(entry), info);

        return useDecision != UseDecision::NoDueToDecodeFailure;
    });
}

void Cache::completeRetrieve(RetrieveCompletionHandler&& handler, std::unique_ptr<Entry> entry, RetrieveInfo& info)
{
    info.completionTime = MonotonicTime::now();

#if ENABLE(NETWORK_CACHE_SIGNPOSTS)
    if (WTFSignpostsEnabled()) [[unlikely]] {
        auto retrieveDecision = info.retrieveDecision ? static_cast<int>(*info.retrieveDecision) : -1;
        auto speculativeLoadDecision = info.speculativeLoadDecision ? static_cast<int>(*info.speculativeLoadDecision) : -1;
        auto useDecision = info.useDecision ? static_cast<int>(*info.useDecision) : -1;

        if (entry) {
            WTFBeginSignpostAlwaysWithTimeDelta(&info, NetworkCacheHit, info.startTime - info.completionTime, "Network cache hit for %" PRIVATE_LOG_STRING " retrieveDecision: %d speculativeLoadDecision: %d useDecision: %d", info.url.string().ascii().data(), retrieveDecision, speculativeLoadDecision, useDecision);
            WTFEndSignpostAlways(&info, NetworkCacheHit);
        } else {
            WTFBeginSignpostAlwaysWithTimeDelta(&info, NetworkCacheMiss, info.startTime - info.completionTime, "Network cache miss for %" PRIVATE_LOG_STRING " retrieveDecision: %d speculativeLoadDecision: %d useDecision: %d", info.url.string().ascii().data(), retrieveDecision, speculativeLoadDecision, useDecision);
            WTFEndSignpostAlways(&info, NetworkCacheMiss);
        }
    }
#endif

    handler(WTF::move(entry), info);
}
    
std::unique_ptr<Entry> Cache::makeEntry(const WebCore::ResourceRequest& request, const WebCore::ResourceResponse& response, PrivateRelayed privateRelayed, RefPtr<WebCore::FragmentedSharedBuffer>&& responseData)
{
    return makeUnique<Entry>(makeCacheKey(resourceType(), request), response, privateRelayed, WTF::move(responseData), WebCore::collectVaryingRequestHeaders(protect(m_networkProcess->storageSession(m_sessionID)), request, response));
}

std::unique_ptr<Entry> Cache::makeRedirectEntry(const WebCore::ResourceRequest& request, const WebCore::ResourceResponse& response, const WebCore::ResourceRequest& redirectRequest)
{
    auto cachedRedirectRequest = redirectRequest;
    cachedRedirectRequest.clearHTTPAuthorization();
    return makeUnique<Entry>(makeCacheKey(resourceType(), request), response, WTF::move(cachedRedirectRequest), WebCore::collectVaryingRequestHeaders(protect(m_networkProcess->storageSession(m_sessionID)), request, response));
}

std::unique_ptr<Entry> Cache::makeCompressionDictionaryEntry(const WebCore::ResourceRequest& request, const WebCore::ResourceResponse& response, RefPtr<WebCore::FragmentedSharedBuffer>&& responseData, const Entry::CompressionDictionaryData& compressionDictionaryData)
{
    return makeUnique<Entry>(makeCacheKey(compressionDictionaryType(), request), response, WTF::move(responseData), WebCore::collectVaryingRequestHeaders(protect(m_networkProcess->storageSession(m_sessionID)), request, response), compressionDictionaryData);
}

std::unique_ptr<Entry> Cache::store(const WebCore::ResourceRequest& request, const WebCore::ResourceResponse& response, PrivateRelayed privateRelayed, RefPtr<WebCore::FragmentedSharedBuffer>&& responseData, Function<void(MappedBody&&)>&& completionHandler)
{
    ASSERT(responseData);

    LOG(NetworkCache, "(NetworkProcess) storing %s, partition %s", request.url().stringWithoutFragmentIdentifier().latin1().data(), makeCacheKey(resourceType(), request).partition().latin1().data());

    StoreDecision storeDecision = makeStoreDecision(request, response, responseData ? responseData->size() : 0);
    if (storeDecision != StoreDecision::Yes) {
        LOG(NetworkCache, "(NetworkProcess) didn't store, storeDecision=%d", static_cast<int>(storeDecision));
        auto key = makeCacheKey(resourceType(), request);

        auto isSuccessfulRevalidation = response.httpStatusCode() == httpStatus304NotModified;
        if (!isSuccessfulRevalidation) {
            // Make sure we don't keep a stale entry in the cache.
            remove(key);
        }

        return nullptr;
    }

    auto cacheEntry = makeEntry(request, response, privateRelayed, WTF::move(responseData));
    auto record = cacheEntry->encodeAsStorageRecord();
    bool storeBlobInMemoryCache = request.isTopSite();

    m_storage->store(record, [protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)](const Data& bodyData) mutable {
        MappedBody mappedBody;
#if ENABLE(SHAREABLE_RESOURCE)
        if (auto sharedMemory = bodyData.tryCreateSharedMemory()) {
            mappedBody.shareableResource = WebCore::ShareableResource::create(sharedMemory.releaseNonNull(), 0, bodyData.size());
            if (!mappedBody.shareableResource) {
                if (completionHandler)
                    completionHandler(WTF::move(mappedBody));
                return;
            }
            if (auto handle = Ref { *mappedBody.shareableResource }->createHandle())
                mappedBody.shareableResourceHandle = WTF::move(*handle);
        }
#endif
        if (completionHandler)
            completionHandler(WTF::move(mappedBody));
        LOG(NetworkCache, "(NetworkProcess) stored");
    }, storeBlobInMemoryCache);

    return cacheEntry;
}

std::unique_ptr<Entry> Cache::storeRedirect(const WebCore::ResourceRequest& request, const WebCore::ResourceResponse& response, const WebCore::ResourceRequest& redirectRequest, std::optional<Seconds> maxAgeCap)
{
    LOG(NetworkCache, "(NetworkProcess) storing redirect %s -> %s", request.url().string().latin1().data(), redirectRequest.url().string().latin1().data());

    StoreDecision storeDecision = makeStoreDecision(request, response, 0);
    if (storeDecision != StoreDecision::Yes) {
        LOG(NetworkCache, "(NetworkProcess) didn't store redirect, storeDecision=%d", static_cast<int>(storeDecision));
        return nullptr;
    }

    auto cacheEntry = makeRedirectEntry(request, response, redirectRequest);

    if (maxAgeCap) {
        LOG(NetworkCache, "(NetworkProcess) capping max age for redirect %s -> %s", request.url().string().latin1().data(), redirectRequest.url().string().latin1().data());
        cacheEntry->capMaxAge(maxAgeCap.value());
    }

    auto record = cacheEntry->encodeAsStorageRecord();

    m_storage->store(record, nullptr);
    
    return cacheEntry;
}

std::unique_ptr<Entry> Cache::storeCompressionDictionary(const WebCore::ResourceRequest& request, const WebCore::ResourceResponse& response, RefPtr<WebCore::FragmentedSharedBuffer>&& responseData, const Entry::CompressionDictionaryData& compressionDictionaryData, Function<void(MappedBody&&)>&& completionHandler)
{
    ASSERT(responseData);

    LOG(NetworkCache, "(NetworkProcess) storing %s as compression dictionary, partition %s", request.url().stringWithoutFragmentIdentifier().latin1().data(), makeCacheKey(compressionDictionaryType(), request).partition().latin1().data());

    StoreDecision storeDecision = makeStoreDecision(request, response, responseData ? responseData->size() : 0);
    if (storeDecision != StoreDecision::Yes) {
        LOG(NetworkCache, "(NetworkProcess) didn't store, storeDecision=%d", static_cast<int>(storeDecision));
        auto key = makeCacheKey(compressionDictionaryType(), request);

        auto isSuccessfulRevalidation = response.httpStatusCode() == httpStatus304NotModified;
        if (!isSuccessfulRevalidation) {
            // Make sure we don't keep a stale entry in the cache.
            remove(key);
        }

        return nullptr;
    }

    auto cacheEntry = makeCompressionDictionaryEntry(request, response, WTF::move(responseData), compressionDictionaryData);
    auto record = cacheEntry->encodeAsStorageRecord();
    bool storeBlobInMemoryCache = request.isTopSite();

    m_storage->store(record, [protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)](const Data&) mutable {
        MappedBody mappedBody;
        if (completionHandler)
            completionHandler(WTF::move(mappedBody));
        LOG(NetworkCache, "(NetworkProcess) stored");
    }, storeBlobInMemoryCache, [protectedThis = Ref { *this }, storeBlobInMemoryCache](const Storage::Record& record) {
        if (auto entry = Entry::decodeStorageRecord(record)) {
            RefPtr bufferToCacheInMemory = storeBlobInMemoryCache ? entry->buffer() : nullptr;
            protectedThis->m_compressionDictionaryCache->add(*entry, WTF::move(bufferToCacheInMemory));
        }
    });

    return cacheEntry;
}

void Cache::ensureCompressionDictionaryCache(const String& partition, CompletionHandler<void()>&& completionHandler)
{
    if (m_compressionDictionaryCache->isInitialized(partition)) {
        completionHandler();
        return;
    }
    if (!m_compressionDictionaryCache->addPendingHandler(partition, WTF::move(completionHandler)))
        return;

    auto generation = m_compressionDictionaryCache->generation();
    m_storage->traverse(compressionDictionaryType(), partition, { }, [protectedThis = Ref { *this }, partition, generation](const Storage::Record* record, const Storage::RecordInfo&) mutable {
        if (record && generation == protectedThis->m_compressionDictionaryCache->generation()) {
            if (auto entry = Entry::decodeStorageRecord(*record))
                protectedThis->m_compressionDictionaryCache->add(*entry);
            return;
        }
        if (record)
            return;

        auto handlers = protectedThis->m_compressionDictionaryCache->takePendingHandlers(partition);
        if (generation != protectedThis->m_compressionDictionaryCache->generation()) {
            for (auto& handler : handlers)
                protectedThis->ensureCompressionDictionaryCache(partition, WTF::move(handler));
            return;
        }

        protectedThis->m_compressionDictionaryCache->markInitialized(partition);
        for (auto& handler : handlers)
            handler();
    });
}

void Cache::removeCompressionDictionaries(const String& partition, std::optional<WebCore::SecurityOriginData> origin, CompletionHandler<void()>&& completionHandler)
{
    ensureCompressionDictionaryCache(partition, [protectedThis = Ref { *this }, partition, origin = WTF::move(origin), completionHandler = WTF::move(completionHandler)]() mutable {
        protectedThis->remove(protectedThis->m_compressionDictionaryCache->keysForOrigin(partition, origin), WTF::move(completionHandler));
    });
}

void Cache::retrieveCompressionDictionaryBestMatchHash(WebCore::ResourceRequest&& request, WebCore::FetchOptionsDestination destination, Function<void(WebCore::ResourceRequest&&, std::optional<CompressionDictionaryMatch>&&)>&& completionHandler)
{
    LOG(NetworkCache, "(NetworkProcess) retrieving best compression dictionary for %s", request.url().string().latin1().data());

    auto partition = request.cachePartition();
    ensureCompressionDictionaryCache(partition, [protectedThis = Ref { *this }, request = WTF::move(request), partition, destination, completionHandler = WTF::move(completionHandler)]() mutable {
        auto match = protectedThis->m_compressionDictionaryCache->bestMatch(partition, request.url(), destination);
        completionHandler(WTF::move(request), WTF::move(match));
    });
}

void Cache::retrieveCompressionDictionaryByHash(const String& partition, std::span<const uint8_t> hashSpan, Function<void(RefPtr<WebCore::SharedBuffer>&&)>&& completionHandler)
{
    std::array<uint8_t, Entry::CompressionDictionaryData::hashSize> hash;
    ASSERT(hashSpan.size() == hash.size());
    std::copy(hashSpan.begin(), hashSpan.end(), hash.begin());
    retrieveCompressionDictionaryByHash(partition, WTF::move(hash), WTF::move(completionHandler));
}

void Cache::retrieveCompressionDictionaryByHash(const String& partition, std::array<uint8_t, Entry::CompressionDictionaryData::hashSize>&& hash, Function<void(RefPtr<WebCore::SharedBuffer>&&)>&& completionHandler)
{
    ensureCompressionDictionaryCache(partition, [protectedThis = Ref { *this }, partition, hash = WTF::move(hash), completionHandler = WTF::move(completionHandler)]() mutable {
        auto data = protectedThis->m_compressionDictionaryCache->dataForHash(partition, hash);
        if (!data) {
            completionHandler(nullptr);
            return;
        }
        if (data->second) {
            completionHandler(data->second->makeContiguous().ptr());
            return;
        }
        auto key = WTF::move(data->first);
        protectedThis->m_storage->retrieve(key, 1, [protectedThis, partition, hash = WTF::move(hash), key, completionHandler = WTF::move(completionHandler)](Storage::Record&& fullRecord, const Storage::Timings&) mutable {
            if (!fullRecord.isNull()) {
                if (auto entry = Entry::decodeStorageRecord(fullRecord)) {
                    if (RefPtr buffer = entry->buffer()) {
                        completionHandler(buffer->makeContiguous().ptr());
                        return true;
                    }
                }
            }

            protectedThis->m_compressionDictionaryCache->remove(key);
            protectedThis->retrieveCompressionDictionaryByHash(partition, WTF::move(hash), WTF::move(completionHandler));
            return false;
        });
    });
}

std::unique_ptr<Entry> Cache::update(const WebCore::ResourceRequest& originalRequest, const Entry& existingEntry, const WebCore::ResourceResponse& validatingResponse, PrivateRelayed privateRelayed)
{
    LOG(NetworkCache, "(NetworkProcess) updating %s", originalRequest.url().string().latin1().data());

    WebCore::ResourceResponse response = existingEntry.response();
    WebCore::updateResponseHeadersAfterRevalidation(response, validatingResponse);
    response.setIPAddressSpace(validatingResponse.ipAddressSpace());

    auto updateEntry = makeUnique<Entry>(existingEntry.key(), response, privateRelayed, existingEntry.buffer(), WebCore::collectVaryingRequestHeaders(protect(m_networkProcess->storageSession(m_sessionID)), originalRequest, response));
    auto updateRecord = updateEntry->encodeAsStorageRecord();
    bool storeBlobInMemoryCache = originalRequest.isTopSite();

    m_storage->store(updateRecord, { }, storeBlobInMemoryCache);

    return updateEntry;
}

void Cache::remove(const Key& key)
{
    if (key.type() == compressionDictionaryType())
        m_compressionDictionaryCache->remove(key);
    m_storage->remove(key);
}

void Cache::remove(const WebCore::ResourceRequest& request)
{
    remove(makeCacheKey(resourceType(), request));
}

void Cache::remove(const Vector<Key>& keys, Function<void()>&& completionHandler)
{
    for (auto& key : keys) {
        if (key.type() == compressionDictionaryType())
            m_compressionDictionaryCache->remove(key);
    }
    m_storage->remove(keys, WTF::move(completionHandler));
}

void Cache::traverseRecords(const String& type, Function<void(const TraversalRecord*)>&& traverseHandler)
{
    // Protect against clients making excessive traversal requests.
    const unsigned maximumTraverseCount = 3;
    if (m_traverseCount >= maximumTraverseCount) {
        WTFLogAlways("Maximum parallel cache traverse count exceeded. Ignoring traversal request.");

        RunLoop::mainSingleton().dispatch([traverseHandler = WTF::move(traverseHandler)] () mutable {
            traverseHandler(nullptr);
        });
        return;
    }

    ++m_traverseCount;

    m_storage->traverse(type, { }, [this, protectedThis = Ref { *this }, traverseHandler = WTF::move(traverseHandler)] (const Storage::Record* record, const Storage::RecordInfo& recordInfo) mutable {
        if (!record) {
            --m_traverseCount;
            traverseHandler(nullptr);
            return;
        }

        TraversalRecord traversalRecord { *record, recordInfo };
        traverseHandler(&traversalRecord);
    });
}

void Cache::traverseRecords(const String& type, const String& partition, Function<void(const TraversalRecord*)>&& traverseHandler)
{
    m_storage->traverse(type, partition, { }, [traverseHandler = WTF::move(traverseHandler)] (const Storage::Record* record, const Storage::RecordInfo& recordInfo) mutable {
        if (!record) {
            traverseHandler(nullptr);
            return;
        }

        TraversalRecord traversalRecord { *record, recordInfo };
        traverseHandler(&traversalRecord);
    });
}

void Cache::traverseRecords(Vector<String>&& types, Function<void(const TraversalRecord*)>&& traverseHandler)
{
    if (types.isEmpty()) {
        traverseHandler(nullptr);
        return;
    }

    auto type = types.takeLast();
    traverseRecords(type, [this, protectedThis = Ref { *this }, types = WTF::move(types), traverseHandler = WTF::move(traverseHandler)] (const TraversalRecord* traversalRecord) mutable {
        if (traversalRecord) {
            traverseHandler(traversalRecord);
            return;
        }
        traverseRecords(WTF::move(types), WTF::move(traverseHandler));
    });
}

void Cache::traverseRecords(Vector<String>&& types, const String& partition, Function<void(const TraversalRecord*)>&& traverseHandler)
{
    if (types.isEmpty()) {
        traverseHandler(nullptr);
        return;
    }

    auto type = types.takeLast();
    traverseRecords(type, partition, [this, protectedThis = Ref { *this }, types = WTF::move(types), partition, traverseHandler = WTF::move(traverseHandler)] (const TraversalRecord* traversalRecord) mutable {
        if (traversalRecord) {
            traverseHandler(traversalRecord);
            return;
        }
        traverseRecords(WTF::move(types), partition, WTF::move(traverseHandler));
    });
}

String Cache::dumpFilePath() const
{
    return pathByAppendingComponent(m_storage->versionPath(), "dump.json"_s);
}

void Cache::dumpContentsToFile()
{
    auto fileHandle = openFile(dumpFilePath(), FileOpenMode::Truncate);
    if (!fileHandle)
        return;

    constexpr auto prologue = "{\n\"entries\": [\n"_span;
    fileHandle.write(byteCast<uint8_t>(prologue));

    struct Totals {
        unsigned count { 0 };
        double worth { 0 };
        size_t bodySize { 0 };
    };
    Totals totals;
    auto flags = { Storage::TraverseFlag::ComputeWorth, Storage::TraverseFlag::ShareCount };
    size_t capacity = m_storage->capacity();
    // TODO: Existing code does not dump info for SubResources type (SubresourcesEntry). Should we?
    String anyType;
    m_storage->traverse(WTF::move(anyType), flags, [fileHandle = WTF::move(fileHandle), totals, capacity](const Storage::Record* record, const Storage::RecordInfo& info) mutable {
        if (!record) {
            CString writeData = makeString(
                "{}\n"
                "],\n"
                "\"totals\": {\n"
                "\"capacity\": "_s, capacity, ",\n"
                "\"count\": "_s, totals.count, ",\n"
                "\"bodySize\": "_s, totals.bodySize, ",\n"
                "\"averageWorth\": "_s, totals.count ? totals.worth / totals.count : 0, "\n"
                "}\n}\n"_s
            ).utf8();
            fileHandle.write(byteCast<uint8_t>(writeData.span()));
            fileHandle = { };
            return;
        }
        if (record->key.type() != resourceType() && record->key.type() != compressionDictionaryType())
            return;
        auto entry = Entry::decodeStorageRecord(*record);
        if (!entry)
            return;
        ++totals.count;
        totals.worth += info.worth;
        totals.bodySize += info.bodySize;

        StringBuilder json;
        entry->asJSON(json, info);
        json.append(",\n"_s);
        fileHandle.write(byteCast<uint8_t>(json.toString().utf8().span()));
    });
}

void Cache::deleteDumpFile()
{
    WorkQueue::create("com.apple.WebKit.Cache.delete"_s)->dispatch([path = dumpFilePath().isolatedCopy()] {
        deleteFile(path);
    });
}

void Cache::clear(WallTime modifiedSince, Function<void()>&& completionHandler)
{
    LOG(NetworkCache, "(NetworkProcess) clearing cache");

    m_compressionDictionaryCache->clear();
    String anyType;
    m_storage->clear(WTF::move(anyType), modifiedSince, [protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)]() mutable {
        protectedThis->m_compressionDictionaryCache->clear();
        if (completionHandler)
            completionHandler();
    });

    deleteDumpFile();
}

void Cache::clear()
{
    clear(-WallTime::infinity(), nullptr);
}

String Cache::recordsPathIsolatedCopy() const
{
    return m_storage->recordsPathIsolatedCopy();
}

void Cache::fetchData(bool shouldComputeSize, CompletionHandler<void(Vector<WebsiteData::Entry>&&)>&& completionHandler)
{
    HashMap<WebCore::SecurityOriginData, uint64_t> originsAndSizes;
    traverseRecords(resourceType(), [shouldComputeSize, completionHandler = WTF::move(completionHandler), originsAndSizes = WTF::move(originsAndSizes)](auto* traversalRecord) mutable {
        if (traversalRecord) {
            auto url = Entry::decodeStorageRecordResponseURL(traversalRecord->record);
            if (!url)
                return;

            auto result = originsAndSizes.add({ url->protocol().toString(), url->host().toString(), url->port() }, 0);
            if (shouldComputeSize)
                result.iterator->value += traversalRecord->record.header.size() + traversalRecord->recordInfo.bodySize;
            return;
        }

        auto entries = WTF::map(originsAndSizes, [](auto& originAndSize) {
            return WebsiteData::Entry { originAndSize.key, WebsiteDataType::DiskCache, originAndSize.value };
        });
        completionHandler(WTF::move(entries));
    });
}

void Cache::fetchOriginAccessTimes(CompletionHandler<void(HashMap<WebCore::RegistrableDomain, WallTime>&&)>&& completionHandler)
{
    HashMap<WebCore::RegistrableDomain, WallTime> originAccessTimes;
    m_storage->traverse(resourceType(), { Storage::TraverseFlag::LastAccessedRecordPerPartition }, [completionHandler = WTF::move(completionHandler), originAccessTimes = WTF::move(originAccessTimes)](const Storage::Record* record, const Storage::RecordInfo& recordInfo) mutable {
        if (!record) {
            completionHandler(WTF::move(originAccessTimes));
            return;
        }

        auto& partition = record->key.partition();
        if (partition.isEmpty())
            return;

        auto domain = WebCore::RegistrableDomain::uncheckedCreateFromRegistrableDomainString(partition);
        originAccessTimes.set(WTF::move(domain), recordInfo.lastAccessTime);
    });
}

void Cache::deleteData(const Vector<WebCore::SecurityOriginData>& origins, CompletionHandler<void()>&& completionHandler)
{
    HashSet<WebCore::SecurityOriginData> originSet;
    for (auto& origin : origins)
        originSet.add(origin);

    Vector<NetworkCache::Key> keysToDelete;
    traverseRecords({ resourceType(), compressionDictionaryType() }, [this, protectedThis = Ref { *this }, originSet = WTF::move(originSet), completionHandler = WTF::move(completionHandler), keysToDelete = WTF::move(keysToDelete)](auto* traversalRecord) mutable {
        if (traversalRecord) {
            auto url = Entry::decodeStorageRecordResponseURL(traversalRecord->record);
            if (!url)
                return;

            if (originSet.contains(WebCore::SecurityOriginData::fromURLWithoutStrictOpaqueness(*url)))
                keysToDelete.append(traversalRecord->record.key);
            return;
        }

        remove(keysToDelete, WTF::move(completionHandler));
    });
}

void Cache::deleteDataForRegistrableDomains(const Vector<WebCore::RegistrableDomain>& domains, CompletionHandler<void(HashSet<WebCore::RegistrableDomain>&&)>&& completionHandler)
{
    HashSet<WebCore::RegistrableDomain> domainSet;
    for (auto& domain : domains)
        domainSet.add(domain);

    Vector<NetworkCache::Key> keysToDelete;
    HashSet<WebCore::RegistrableDomain> domainsDeleted;
    traverseRecords({ resourceType(), compressionDictionaryType() }, [this, protectedThis = Ref { *this }, domainSet = WTF::move(domainSet), completionHandler = WTF::move(completionHandler), keysToDelete = WTF::move(keysToDelete), domainsDeleted = WTF::move(domainsDeleted)](auto* traversalRecord) mutable {
        if (traversalRecord) {
            auto url = Entry::decodeStorageRecordResponseURL(traversalRecord->record);
            if (!url)
                return;

            auto domain = WebCore::RegistrableDomain { *url };
            if (domainSet.contains(domain)) {
                keysToDelete.append(traversalRecord->record.key);
                domainsDeleted.add(domain);
            }
            return;
        }

        protectedThis->remove(keysToDelete, [completionHandler = WTF::move(completionHandler), domainsDeleted = WTF::move(domainsDeleted)]() mutable {
            completionHandler(WTF::move(domainsDeleted));
        });
    });
}

} // namespace NetworkCache
} // namespace WebKit
