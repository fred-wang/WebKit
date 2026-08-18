/*
 * Copyright (C) 2015 Apple Inc. All rights reserved.
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

#include "Logging.h"
#include "NetworkCacheCoders.h"
#include "NetworkProcess.h"
#include <WebCore/JSFetchRequestDestination.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/SharedBuffer.h>
#include <pal/crypto/CryptoDigest.h>
#include <wtf/HexNumber.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/persistence/PersistentEncoder.h>
#include <wtf/text/StringBuilder.h>

namespace WebKit {
namespace NetworkCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Entry);

Entry::Entry(const Key& key, const WebCore::ResourceResponse& response, PrivateRelayed privateRelayed, RefPtr<WebCore::FragmentedSharedBuffer>&& buffer, const Vector<std::pair<String, String>>& varyingRequestHeaders)
    : m_key(key)
    , m_timeStamp(WallTime::now())
    , m_response(response)
    , m_varyingRequestHeaders(varyingRequestHeaders)
    , m_buffer(WTF::move(buffer))
    , m_privateRelayed(privateRelayed)
{
    ASSERT(m_key.type() == "Resource"_s);
}

Entry::Entry(const Key& key, const WebCore::ResourceResponse& response, const WebCore::ResourceRequest& redirectRequest, const Vector<std::pair<String, String>>& varyingRequestHeaders)
    : m_key(key)
    , m_timeStamp(WallTime::now())
    , m_response(response)
    , m_varyingRequestHeaders(varyingRequestHeaders)
{
    ASSERT(m_key.type() == "Resource"_s);

    m_redirectRequest.emplace();
    m_redirectRequest->setAsIsolatedCopy(redirectRequest);
    // Redirect body is not needed even if exists.
    m_redirectRequest->setHTTPBody(nullptr);
}

Entry::Entry(const Key& key, const WebCore::ResourceResponse& response, RefPtr<WebCore::FragmentedSharedBuffer>&& buffer, const Vector<std::pair<String, String>>& varyingRequestHeaders, const CompressionDictionaryData& compressionDictionaryData)
    : m_key(key)
    , m_timeStamp(WallTime::now())
    , m_response(response)
    , m_varyingRequestHeaders(varyingRequestHeaders)
    , m_buffer(WTF::move(buffer))
    , m_compressionDictionaryData(compressionDictionaryData)
{
    ASSERT(m_key.type() == "CompressionDictionary"_s);

    auto crypto = PAL::Crypto::CryptoDigest::create(PAL::Crypto::CryptoDigest::Algorithm::SHA_256);
    m_buffer->forEachSegment([&](auto segment) {
        crypto->addBytes(segment);
    });
    memcpySpan(std::span<uint8_t, CompressionDictionaryData::hashSize>(m_compressionDictionaryData->hash), crypto->computeHash().span());
}

Entry::Entry(const Entry& other)
    : m_key(other.m_key)
    , m_timeStamp(other.m_timeStamp)
    , m_response(other.m_response)
    , m_varyingRequestHeaders(other.m_varyingRequestHeaders)
    , m_redirectRequest(other.m_redirectRequest)
    , m_buffer(other.m_buffer)
    , m_sourceStorageRecord(other.m_sourceStorageRecord)
    , m_compressionDictionaryData(other.m_compressionDictionaryData)
{
}

Entry::Entry(const Storage::Record& storageEntry)
    : m_key(storageEntry.key)
    , m_timeStamp(storageEntry.timeStamp)
    , m_sourceStorageRecord(storageEntry)
{
    ASSERT(m_key.type() == "Resource"_s || m_key.type() == "CompressionDictionary"_s);
}

Storage::Record Entry::encodeAsStorageRecord() const
{
    WTF::Persistence::Encoder encoder;
    encoder << m_response;

    bool hasVaryingRequestHeaders = !m_varyingRequestHeaders.isEmpty();
    encoder << hasVaryingRequestHeaders;
    if (hasVaryingRequestHeaders)
        encoder << m_varyingRequestHeaders;

    uint8_t isRedirect = !!m_redirectRequest;
    uint8_t privateRelayed = m_privateRelayed == PrivateRelayed::Yes;
    encoder << static_cast<uint8_t>((isRedirect << 0) | (privateRelayed << 1));
    if (isRedirect)
        encoder << m_redirectRequest;

    encoder << m_maxAgeCap;
    
    encoder.encodeChecksum();

    if (m_key.type() == "CompressionDictionary"_s) {
        ASSERT(m_compressionDictionaryData);
        encoder << m_compressionDictionaryData->match;
        encoder << m_compressionDictionaryData->id;
        encoder << toHexString(m_compressionDictionaryData->hash);
        bool hasMatchDest = !m_compressionDictionaryData->matchDest.isEmpty();
        encoder << hasMatchDest;
        if (hasMatchDest) {
            encoder << m_compressionDictionaryData->matchDest.map([](auto& destination) {
                return WebCore::convertEnumerationToString(destination);
            });
        }
    }

    Data header(encoder.span());
    Data body;
    if (RefPtr buffer = m_buffer) {
        Ref contiguousBuffer = buffer->makeContiguous();
        m_buffer = contiguousBuffer.copyRef();
        body = { contiguousBuffer->span() };
    }

    return { m_key, m_timeStamp, header, body, { } };
}

std::unique_ptr<Entry> Entry::decodeStorageRecord(const Storage::Record& storageEntry)
{
    auto entry = makeUnique<Entry>(storageEntry);

    WTF::Persistence::Decoder decoder(storageEntry.header.span());
    std::optional<WebCore::ResourceResponse> response;
    decoder >> response;
    if (!response)
        return nullptr;
    entry->m_response = WTF::move(*response);
    entry->m_response.setSource(WebCore::ResourceResponse::Source::DiskCache);
    ASSERT(entry->m_response.isNull() || decodeStorageRecordResponseURL(storageEntry) == entry->m_response.url());

    std::optional<bool> hasVaryingRequestHeaders;
    decoder >> hasVaryingRequestHeaders;
    if (!hasVaryingRequestHeaders)
        return nullptr;

    if (*hasVaryingRequestHeaders) {
        std::optional<Vector<std::pair<String, String>>> varyingRequestHeaders;
        decoder >> varyingRequestHeaders;
        if (!varyingRequestHeaders)
            return nullptr;
        entry->m_varyingRequestHeaders = WTF::move(*varyingRequestHeaders);
    }

    std::optional<uint8_t> isRedirectAndPrivateRelayed;
    decoder >> isRedirectAndPrivateRelayed;
    if (!isRedirectAndPrivateRelayed)
        return nullptr;

    bool isRedirect = *isRedirectAndPrivateRelayed & 0x1;
    entry->m_privateRelayed = *isRedirectAndPrivateRelayed & 0x2 ? PrivateRelayed::Yes : PrivateRelayed::No;
    
    if (isRedirect) {
        entry->m_redirectRequest.emplace();
        std::optional<std::optional<WebCore::ResourceRequest>> resourceRequest;
        decoder >> resourceRequest;
        if (!resourceRequest)
            return nullptr;
        entry->m_redirectRequest = WTF::move(*resourceRequest);
    }

    std::optional<std::optional<Seconds>> maxAgeCap;
    decoder >> maxAgeCap;
    if (!maxAgeCap)
        return nullptr;
    entry->m_maxAgeCap = WTF::move(*maxAgeCap);

    if (!decoder.verifyChecksum()) {
        LOG(NetworkCache, "(NetworkProcess) checksum verification failure\n");
        return nullptr;
    }

    if (entry->m_key.type() == "CompressionDictionary"_s) {
        CompressionDictionaryData compressionDictionaryData;
        std::optional<String> match;
        decoder >> match;
        if (!match)
            return nullptr;
        compressionDictionaryData.match = WTF::move(*match);

        std::optional<String> id;
        decoder >> id;
        if (!id)
            return nullptr;
        compressionDictionaryData.id = WTF::move(*id);

        std::optional<String> hash;
        decoder >> hash;
        if (!hash || hash->length() != 2 * CompressionDictionaryData::hashSize)
            return nullptr;
        for (size_t i = 0; i < CompressionDictionaryData::hashSize; i++) {
            auto high = (*hash)[2 * i];
            auto low = (*hash)[2 * i + 1];
            if (!isASCIIHexDigit(high) || !isASCIIHexDigit(low))
                return nullptr;
            compressionDictionaryData.hash[i] = toASCIIHexValue(high, low);
        }

        std::optional<bool> hasMatchDest;
        decoder >> hasMatchDest;
        if (!hasMatchDest)
            return nullptr;
        if (*hasMatchDest) {
            std::optional<Vector<String>> matchDest;
            decoder >> matchDest;
            if (!matchDest)
                return nullptr;
            for (auto& item : *matchDest) {
                auto destination = WebCore::parseEnumerationFromString<WebCore::FetchRequestDestination>(item);
                if (!destination)
                    return nullptr;
                compressionDictionaryData.matchDest.append(*destination);
            }
        }
        entry->m_compressionDictionaryData = WTF::move(compressionDictionaryData);
    }

    return entry;
}

std::optional<URL> Entry::decodeStorageRecordResponseURL(const Storage::Record& storageEntry)
{
    WTF::Persistence::Decoder decoder(storageEntry.header.span());

    std::optional<bool> responseIsNull;
    decoder >> responseIsNull;
    if (!responseIsNull || *responseIsNull)
        return std::nullopt;

    std::optional<URL> url;
    decoder >> url;
    return url;
}

bool Entry::hasReachedPrevalentResourceAgeCap() const
{
    return m_maxAgeCap && WebCore::computeCurrentAge(response(), timeStamp()) > m_maxAgeCap;
}

void Entry::capMaxAge(const Seconds seconds)
{
    m_maxAgeCap = seconds;
}

void Entry::initializeBufferFromStorageRecord() const
{
#if ENABLE(SHAREABLE_RESOURCE)
    if (auto handle = shareableResourceHandle()) {
        m_buffer = WTF::move(*handle).tryWrapInSharedBuffer();
        if (m_buffer)
            return;
    }
#endif
    m_buffer = WebCore::SharedBuffer::create(m_sourceStorageRecord.body.span());
}

WebCore::FragmentedSharedBuffer* Entry::buffer() const
{
    if (!m_buffer)
        initializeBufferFromStorageRecord();

    return m_buffer.get();
}

#if ENABLE(SHAREABLE_RESOURCE)
std::optional<WebCore::ShareableResource::Handle> Entry::shareableResourceHandle() const
{
    if (RefPtr shareableResource = m_shareableResource)
        return shareableResource->createHandle();

    auto sharedMemory = m_sourceStorageRecord.body.tryCreateSharedMemory();
    if (!sharedMemory)
        return std::nullopt;

    RefPtr shareableResource = WebCore::ShareableResource::create(sharedMemory.releaseNonNull(), 0, m_sourceStorageRecord.body.size());
    m_shareableResource = shareableResource;
    if (shareableResource)
        return shareableResource->createHandle();
    return std::nullopt;
}
#endif

bool Entry::needsValidation() const
{
    return m_response.source() == WebCore::ResourceResponse::Source::DiskCacheAfterValidation;
}

void Entry::setNeedsValidation(bool value)
{
    m_response.setSource(value ? WebCore::ResourceResponse::Source::DiskCacheAfterValidation : WebCore::ResourceResponse::Source::DiskCache);
}

void Entry::asJSON(StringBuilder& json, const Storage::RecordInfo& info) const
{
    json.append("{\n"_s
        "\"type\": "_s);
    json.appendQuotedJSONString(m_key.type());
    json.append(",\n"_s
        "\"hash\": "_s);
    json.appendQuotedJSONString(m_key.hashAsString());
    json.append(",\n"_s
        "\"bodySize\": "_s, info.bodySize, ",\n"_s
        "\"worth\": "_s, info.worth, ",\n"_s
        "\"partition\": "_s);
    json.appendQuotedJSONString(m_key.partition());
    json.append(",\n"_s
        "\"timestamp\": "_s, m_timeStamp.secondsSinceEpoch().milliseconds(), ",\n"_s
        "\"URL\": "_s);
    json.appendQuotedJSONString(m_response.url().string());
    json.append(",\n"_s
        "\"bodyHash\": "_s);
    json.appendQuotedJSONString(info.bodyHash);
    json.append(",\n"_s
        "\"bodyShareCount\": "_s, info.bodyShareCount, ",\n"_s
        "\"headers\": {\n"_s);
    bool firstHeader = true;
    for (auto& header : m_response.httpHeaderFields()) {
        json.append(std::exchange(firstHeader, false) ? ""_s : ",\n"_s, "    "_s);
        json.appendQuotedJSONString(header.key);
        json.append(": "_s);
        json.appendQuotedJSONString(header.value);
    }
    if (auto data = m_compressionDictionaryData) {
        json.append("\n"_s
            "},\n"_s,
            "\"compressionDictionaryData\": {\n"_s,
            "\"match\": "_s);
        json.appendQuotedJSONString(data->match);
        json.append(",\n"_s
            "\"id\": "_s);
        json.appendQuotedJSONString(data->id);
        json.append(",\n"_s
            "\"hash\": "_s);
        json.appendQuotedJSONString(toHexString(data->hash));
        json.append(",\n"_s
            "\"matchDest\": [\n"_s);
        bool firstDestination = true;
        for (auto& destination : data->matchDest) {
            json.append(std::exchange(firstDestination, false) ? ""_s : ",\n"_s, "    "_s);
            json.appendQuotedJSONString(WebCore::convertEnumerationToString(destination));
        }
        json.append("\n"_s
            "]"_s);
    }
    json.append("\n"_s
        "}\n"_s
        "}"_s);
}

}
}
