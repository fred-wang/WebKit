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

#pragma once

#include <JavaScriptCore/Strong.h>
#include <memory>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WTF {
class BumpPointerAllocator;
}

namespace JSC {
class RegExp;
class VM;
namespace Yarr {
struct BytecodePattern;
} }

namespace WebCore {

class ScriptExecutionContext;
struct URLPatternComponentResult;
enum class EncodingCallbackType : uint8_t;
template<typename> class ExceptionOr;

namespace URLPatternUtilities {
struct URLPatternStringOptions;

class URLPatternComponent {
public:
    static ExceptionOr<URLPatternComponent> compile(StringView, EncodingCallbackType, const URLPatternStringOptions&, JSC::VM* = nullptr);
    const String& patternString() const LIFETIME_BOUND { return m_patternString; }
    bool hasRegexGroupsFromPartList() const { return m_hasRegexGroupsFromPartList; }
    bool matchSpecialSchemeProtocol() const;
    std::optional<Vector<unsigned>> componentExec(StringView, ScriptExecutionContext* = nullptr) const;
    URLPatternComponentResult createComponentMatchResult(String&& input, const Vector<unsigned>& offsets) const;
    URLPatternComponent();
    ~URLPatternComponent();
    URLPatternComponent(URLPatternComponent&&);
    URLPatternComponent& operator=(URLPatternComponent&&);

private:
    struct CompiledPattern {
        std::unique_ptr<WTF::BumpPointerAllocator> allocator;
        std::unique_ptr<JSC::Yarr::BytecodePattern> bytecode;
    };

    URLPatternComponent(String&&, std::unique_ptr<CompiledPattern>&&, JSC::Strong<JSC::RegExp>&&, RefPtr<JSC::VM>&&, Vector<String>&&, bool);

    String m_patternString;
    std::unique_ptr<CompiledPattern> m_compiledPattern;
    JSC::Strong<JSC::RegExp> m_jitRegExp;
    RefPtr<JSC::VM> m_vm;
    Vector<String> m_groupNameList;
    bool m_hasRegexGroupsFromPartList { false };
};

}
}
