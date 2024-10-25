/*
 * Copyright (C) 2013 Samsung Electronics
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "IdentifierTypes.h"
#include <WebCore/TextChecking.h>
#include <wtf/Forward.h>
#include <wtf/WeakPtr.h>

namespace WebKit {

using SpellDocumentTag = int64_t;

class WebPageProxy;

class TextCheckerCompletion : public RefCounted<TextCheckerCompletion> {
public:
    static Ref<TextCheckerCompletion> create(TextCheckerRequestID, const WebCore::TextCheckingRequestData&, WebPageProxy&);

    const WebCore::TextCheckingRequestData& textCheckingRequestData() const;
    SpellDocumentTag spellDocumentTag();
    void didFinishCheckingText(const Vector<WebCore::TextCheckingResult>&) const;
    void didCancelCheckingText() const;

private:
    TextCheckerCompletion(TextCheckerRequestID, const WebCore::TextCheckingRequestData&, WebPageProxy&);

    Ref<WebPageProxy> protectedPage() const;

    const TextCheckerRequestID m_requestID;
    const WebCore::TextCheckingRequestData m_requestData;
    WeakRef<WebPageProxy> m_page;
};

} // namespace WebKit
