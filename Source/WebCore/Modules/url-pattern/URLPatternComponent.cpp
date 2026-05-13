/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
 * Copyright (C) 2026 Igalia S.L.
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
#include "URLPatternComponent.h"

#include "ExceptionOr.h"
#include "ScriptExecutionContext.h"
#include "URLPatternCanonical.h"
#include "URLPatternParser.h"
#include "URLPatternResult.h"
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSObjectInlines.h>
#include <JavaScriptCore/RegExp.h>
#include <JavaScriptCore/StructureInlines.h>
#include <JavaScriptCore/VM.h>
#include <JavaScriptCore/YarrFlags.h>
#include <JavaScriptCore/YarrInterpreter.h>
#include <JavaScriptCore/YarrPattern.h>
#include <wtf/BumpPointerAllocator.h>
#include <wtf/text/MakeString.h>

namespace WebCore {
namespace URLPatternUtilities {

URLPatternComponent::URLPatternComponent() = default;

URLPatternComponent::~URLPatternComponent()
{
    if (m_vm) {
        JSC::JSLockHolder lock(*m_vm);
        m_jitRegExp.clear();
    }
}

URLPatternComponent::URLPatternComponent(URLPatternComponent&&) = default;
URLPatternComponent& URLPatternComponent::operator=(URLPatternComponent&&) = default;

URLPatternComponent::URLPatternComponent(String&& patternString, std::unique_ptr<CompiledPattern>&& compiled, JSC::Strong<JSC::RegExp>&& jitRegExp, RefPtr<JSC::VM>&& vm, Vector<String>&& groupNameList, bool hasRegexpGroupsFromPartsList)
    : m_patternString(WTF::move(patternString))
    , m_compiledPattern(WTF::move(compiled))
    , m_jitRegExp(WTF::move(jitRegExp))
    , m_vm(WTF::move(vm))
    , m_groupNameList(WTF::move(groupNameList))
    , m_hasRegexGroupsFromPartList(hasRegexpGroupsFromPartsList)
{
}

// https://urlpattern.spec.whatwg.org/#compile-a-component
ExceptionOr<URLPatternComponent> URLPatternComponent::compile(StringView input, EncodingCallbackType type, const URLPatternStringOptions& options, JSC::VM* vm)
{
    auto maybePartList = URLPatternParser::parse(input, options, type);
    if (maybePartList.hasException())
        return maybePartList.releaseException();
    Vector<Part> partList = maybePartList.releaseReturnValue();

    auto [regularExpressionString, nameList] = generateRegexAndNameList(partList, options);

    OptionSet<JSC::Yarr::Flags> flags = { JSC::Yarr::Flags::UnicodeSets };
    if (options.ignoreCase)
        flags.add(JSC::Yarr::Flags::IgnoreCase);

    JSC::Yarr::ErrorCode errorCode = JSC::Yarr::ErrorCode::NoError;
    JSC::Yarr::YarrPattern yarrPattern(regularExpressionString, flags, errorCode);
    if (JSC::Yarr::hasError(errorCode))
        return Exception { ExceptionCode::TypeError, makeString("Unable to create RegExp object regular expression from provided URLPattern string: "_s, JSC::Yarr::errorMessage(errorCode)) };

    auto allocator = makeUnique<WTF::BumpPointerAllocator>();
    auto bytecode = JSC::Yarr::byteCompile(yarrPattern, allocator.get(), errorCode, nullptr);
    if (JSC::Yarr::hasError(errorCode) || !bytecode)
        return Exception { ExceptionCode::TypeError, "Unable to compile RegExp bytecode from provided URLPattern string."_s };

    String patternString = generatePatternString(partList, options);
    bool hasRegexGroups = partList.containsIf([](auto& part) {
        return part.type == PartType::Regexp;
    });

    auto compiled = std::make_unique<CompiledPattern>(CompiledPattern { WTF::move(allocator), WTF::move(bytecode) });

    JSC::Strong<JSC::RegExp> jitRegExp;
    RefPtr<JSC::VM> ownedVM;
    if (vm) {
        JSC::JSLockHolder lock(*vm);
        JSC::RegExp* regExp = JSC::RegExp::create(*vm, regularExpressionString, flags);
        if (regExp->isValid()) {
            jitRegExp = JSC::Strong<JSC::RegExp>(*vm, regExp);
            ownedVM = vm;
        }
    }

    return URLPatternComponent { WTF::move(patternString), WTF::move(compiled), WTF::move(jitRegExp), WTF::move(ownedVM), WTF::move(nameList), hasRegexGroups };
}

// https://urlpattern.spec.whatwg.org/#protocol-component-matches-a-special-scheme
bool URLPatternComponent::matchSpecialSchemeProtocol() const
{
    static constexpr std::array specialSchemeList { "ftp"_s, "file"_s, "http"_s, "https"_s, "ws"_s, "wss"_s };
    return std::ranges::any_of(specialSchemeList, [this](const String& scheme) {
        return componentExec(scheme).has_value();
    });
}

std::optional<Vector<unsigned>> URLPatternComponent::componentExec(StringView comparedString, ScriptExecutionContext* context) const
{
    if (!m_compiledPattern)
        return std::nullopt;

    if (context && m_jitRegExp) {
        if (auto* globalObject = context->globalObject()) {
            auto* regExp = m_jitRegExp.get();
            unsigned outputSize = static_cast<unsigned>(regExp->offsetVectorSize());
            Vector<int> jitOutput(outputSize);
            std::ranges::fill(jitOutput, -1);
            int result = regExp->match(globalObject, comparedString, 0, jitOutput.mutableSpan());
            if (result == -1)
                return std::nullopt;
            Vector<unsigned> output(outputSize);
            for (unsigned i = 0; i < outputSize; ++i)
                output[i] = jitOutput[i] < 0 ? std::numeric_limits<unsigned>::max() : static_cast<unsigned>(jitOutput[i]);
            return output;
        }
    }

    // Interpreter path (context-free fallback).
    // m_offsetsSize accounts for (numSubpatterns+1)*2 capture offsets plus any
    // additional slots for duplicate named capture groups.
    unsigned outputSize = m_compiledPattern->bytecode->m_offsetsSize;
    Vector<unsigned> output(outputSize);
    std::ranges::fill(output, std::numeric_limits<unsigned>::max());

    unsigned result = JSC::Yarr::interpret(m_compiledPattern->bytecode.get(), comparedString, 0, output.begin());
    if (result == std::numeric_limits<unsigned>::max())
        return std::nullopt;

    return output;
}

// https://urlpattern.spec.whatwg.org/#create-a-component-match-result
URLPatternComponentResult URLPatternComponent::createComponentMatchResult(String&& input, const Vector<unsigned>& offsets) const
{
    URLPatternComponentResult::GroupsRecord groups;

    ASSERT(offsets.size() >= (m_groupNameList.size() + 1) * 2);
    for (unsigned index = 0; index < m_groupNameList.size(); ++index) {
        unsigned start = offsets[(index + 1) * 2];
        unsigned end = offsets[(index + 1) * 2 + 1];

        Variant<std::monostate, String> value;
        if (start != std::numeric_limits<unsigned>::max() && end != std::numeric_limits<unsigned>::max())
            value = StringView(input).substring(start, end - start).toString();

        groups.append(URLPatternComponentResult::NameMatchPair { m_groupNameList[index], WTF::move(value) });
    }

    return URLPatternComponentResult { !input.isEmpty() ? WTF::move(input) : emptyString(), WTF::move(groups) };
}

}
}
