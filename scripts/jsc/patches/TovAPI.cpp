/* TovAPI.cpp: see TovAPI.h. */

#include "config.h"
#include "TovAPI.h"

#include "APICast.h"
#include "BytecodeCacheError.h"
#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "Completion.h"
#include "DeferredWorkTimer.h"
#include "InitializeThreading.h"
#include "IntlCache.h"
#include "JSCInlines.h"
#include "FunctionCodeBlock.h"
#include "LLIntThunks.h"
#include "ScriptExecutableInlines.h"
#include "VMEntryScopeInlines.h"
#include "VMInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSTypedArrays.h"
#include "TopExceptionScope.h"
#include "Options.h"
#include "SourceProvider.h"
#include <unistd.h>
#include <wtf/FileHandle.h>
#include <wtf/RunLoop.h>
#include <wtf/SIMDUTF.h>
#include <wtf/TimeZone.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringImpl.h>
#include <unicode/utf16.h>

using namespace JSC;

namespace {

/* The engine's lock for one of these functions: while the program owns its sticky lock (it
 * nearly always does), only counted; otherwise JSLockHolder's. */
class TovLockScope {
public:
    explicit TovLockScope(VM& vm)
        : m_lock(vm.apiLock())
    {
        if (m_lock.stickyHeldByCurrentThread()) [[likely]] {
            m_lock.stickyEnter();
            m_counted = true;
        } else
            m_holder.emplace(vm);
    }
    ~TovLockScope()
    {
        if (m_counted)
            m_lock.stickyLeave();
    }

private:
    JSLock& m_lock;
    bool m_counted { false };
    std::optional<JSLockHolder> m_holder;
};

/* A script's text as the caller gave it (not copied when it's ASCII), and its bytecode if it has
 * some cached. */
class TovSourceProvider final : public SourceProvider {
public:
    static Ref<TovSourceProvider> create(const char* source, size_t len, const char* url)
    {
        return adoptRef(*new TovSourceProvider(text(source, len), String::fromUTF8(url)));
    }

    unsigned hash() const final { return m_source.hash(); }
    StringView source() const final { return m_source; }
    RefPtr<CachedBytecode> cachedBytecode() const final { return m_cachedBytecode; }
    void setCachedBytecode(Ref<CachedBytecode>&& bytecode) { m_cachedBytecode = WTF::move(bytecode); }

private:
    TovSourceProvider(String&& source, String&& url)
        : SourceProvider(SourceOrigin { URL({ }, url) }, String(url), String(), SourceTaintedOrigin::Untainted, TextPosition(), SourceProviderSourceType::Program)
        , m_source(WTF::move(source))
    {
    }

    static String text(const char* source, size_t len)
    {
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        std::span<const Latin1Character> bytes { reinterpret_cast<const Latin1Character*>(source), len };
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        if (charactersAreAllASCII(bytes))
            return StringImpl::createWithoutCopying(bytes);
        return String::fromUTF8(std::span<const char8_t> { reinterpret_cast<const char8_t*>(bytes.data()), bytes.size() });
    }

    String m_source;
    RefPtr<CachedBytecode> m_cachedBytecode;
};

}

JSValueRef TVEvaluateScript(JSContextRef ctx, const char* source, size_t len, const char* url, int cacheFd, bool* usedCache, JSValueRef* exception)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    JSLockHolder locker(vm);
    Ref provider = TovSourceProvider::create(source, len, url);
    if (usedCache)
        *usedCache = false;
    if (cacheFd >= 0) {
        auto handle = FileSystem::FileHandle::adopt(dup(cacheFd));
        if (auto mapped = handle.map(FileSystem::MappedFileMode::Private)) {
            Ref bytecode = CachedBytecode::create(WTF::move(*mapped));
            SourceCodeKey key = sourceCodeKeyForSerializedProgram(vm, SourceCode(provider.copyRef()));
            if (isCachedBytecodeStillValid(vm, bytecode.copyRef(), key, SourceCodeType::ProgramType)) {
                provider->setCachedBytecode(WTF::move(bytecode));
                if (usedCache)
                    *usedCache = true;
            }
        }
    }
    NakedPtr<Exception> evaluationException;
    JSValue result = profiledEvaluate(globalObject, ProfilingReason::API, SourceCode(WTF::move(provider)), JSValue(), evaluationException);
    if (evaluationException) {
        if (exception)
            *exception = toRef(globalObject, evaluationException->value());
        return nullptr;
    }
    return toRef(globalObject, result ? result : jsUndefined());
}

bool TVWriteBytecode(JSContextGroupRef group, const char* source, size_t len, const char* url, int fd)
{
    VM& vm = *toJS(group);
    JSLockHolder locker(vm);
    auto handle = FileSystem::FileHandle::adopt(dup(fd));
    if (!handle)
        return false;
    BytecodeCacheError error;
    RefPtr written = generateProgramBytecode(vm, SourceCode(TovSourceProvider::create(source, len, url)), handle, error);
    return written && !error.isValid();
}

// (libpas's, exported though not in the headers JavaScriptCore sees)
extern "C" {
extern bool pas_physical_page_sharing_pool_balancing_enabled;
extern bool pas_physical_page_sharing_pool_balancing_enabled_for_utility;
}

void TVInitialize()
{
    // The engine's allocator takes the free pages it holds before new ones from the system, as
    // WebKit's "mini mode" has it (without that mode's faster scavenger rounds, which cost a
    // server working in bursts ~40% more CPU a request, or its allocator and collector changes,
    // which cost speed): Cap's media server peaks at ~94 MB, not ~140, and holds ~2 MB less.
    pas_physical_page_sharing_pool_balancing_enabled = true;
    pas_physical_page_sharing_pool_balancing_enabled_for_utility = true;
    JSC::initialize();
#if !PLATFORM(COCOA)
    // (WebKit's ports make the UI thread's run loop the main one. Without one, the first full
    // collection on Linux crashed: its timer asks the memory pressure handler, made then, which
    // puts a timer of its own on the main run loop.)
    RunLoop::initializeMain();
#endif
}

double TVRunLoopSecondsUntilWork()
{
    Seconds until = RunLoop::currentSingleton().secondsUntilWork();
    return until.isInfinity() ? -1 : until.seconds();
}

void TVRunLoopCycle()
{
    RunLoop::cycle();
}

void TVRunLoopSetWakeUp(void (*wake)(void))
{
    RunLoop::setWakeUpCallback([wake] {
        wake();
    });
}

bool TVHasImminentWork(JSContextRef ctx)
{
    VM& vm = toJS(ctx)->vm();
    JSLockHolder locker(vm);
    return vm.deferredWorkTimer->hasImminentlyScheduledWork();
}

JSObjectRef TVMakeUint8Array(JSContextRef ctx, size_t len, void** bytes)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    JSLockHolder locker(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Structure* structure = globalObject->typedArrayStructure(TypeUint8, false);
    JSUint8Array* array;
    if (len <= JSArrayBufferView::fastSizeLimit)
        array = JSUint8Array::createUninitialized(globalObject, structure, len);
    else {
        // (a big one with its ArrayBuffer from the start: asked for later, as a DataView over it
        // asks, the engine would make one then and count the bytes towards collections again)
        RefPtr<ArrayBuffer> buffer = ArrayBuffer::tryCreateUninitialized(len, 1);
        if (!buffer)
            return nullptr;
        array = JSUint8Array::create(globalObject, structure, WTF::move(buffer), 0, len);
    }
    if (scope.exception() || !array) {
        scope.clearException();
        return nullptr;
    }
    *bytes = array->vector();
    return toRef(array);
}

bool TVSetOptions(const char* options)
{
    JSC::initialize();
    return Options::setOptions(options);
}

void TVTimeZoneDidChange(JSContextRef ctx)
{
    WTF::timeZoneDidChange();
    if (!ctx)
        return;
    VM& vm = toJS(ctx)->vm();
    JSLockHolder locker(vm);
    vm.intlCache().clearForTimeZoneChange();
    vm.dateCache.clearForTimeZoneChange();
}

void TVSetStickyLock(JSContextRef ctx)
{
    toJS(ctx)->vm().apiLock().setSticky(true);
}

void TVReleaseStickyLock(JSContextRef ctx)
{
    toJS(ctx)->vm().apiLock().releaseStickyLock();
}

void* TVPropertyName(JSContextRef ctx, const char* utf8)
{
    VM& vm = toJS(ctx)->vm();
    JSLockHolder locker(vm);
    Identifier name = Identifier::fromString(vm, String::fromUTF8(utf8));
    RefPtr<UniquedStringImpl> impl = name.impl();
    return impl.leakRef();
}

JSValueRef TVGetProperty(JSContextRef ctx, JSValueRef value, void* name, JSValueRef* exception)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    TovLockScope locker(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue base = toJS(globalObject, value);
    JSValue result = base.get(globalObject, PropertyName(static_cast<UniquedStringImpl*>(name)));
    if (Exception* thrown = scope.exception()) [[unlikely]] {
        if (exception)
            *exception = toRef(globalObject, thrown->value());
        scope.clearException();
        return nullptr;
    }
    return toRef(globalObject, result);
}

JSValueRef TVGetPropertyCached(JSContextRef ctx, JSValueRef value, void* name, TVPropertyCache* cache, JSValueRef* exception)
{
#if CPU(ADDRESS64)
    static_assert(JSObject::butterflyOffset() == 8 && !JSCell::structureIDOffset());
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    TovLockScope locker(vm);
    JSValue base = toJS(globalObject, value);
    if (JSObject* object = base.getObject()) {
        Structure* structure = object->structure();
        PropertyName property(static_cast<UniquedStringImpl*>(name));
        unsigned attributes;
        PropertyOffset offset;
        if (!structure->isDictionary() && !structure->typeInfo().overridesGetOwnPropertySlot()
            && isValidOffset(offset = structure->get(vm, property, attributes))
            && !(attributes & static_cast<unsigned>(PropertyAttribute::AccessorOrCustomAccessorOrValue))) {
            const char* slot = std::bit_cast<const char*>(object->locationForOffset(offset));
            int32_t delta = isInlineOffset(offset)
                ? static_cast<int32_t>(slot - std::bit_cast<const char*>(object))
                : static_cast<int32_t>(slot - std::bit_cast<const char*>(object->butterfly()));
            if (cache->structure != structure) {
                gcProtect(structure);
                if (cache->structure)
                    gcUnprotect(static_cast<Structure*>(cache->structure));
                cache->structure = structure;
            }
            cache->shape = structure->id().bits();
            cache->offset = delta;
            return toRef(globalObject, object->getDirect(offset));
        }
    }
#else
    UNUSED_PARAM(cache);
#endif
    return TVGetProperty(ctx, value, name, exception);
}

#if (CPU(ARM64) || CPU(X86_64)) && CPU(ADDRESS64) && !ENABLE(C_LOOP)
/* A call of a JavaScript function (not a host one) with up to six arguments and no more
 * parameters than that: into its machine code directly, the arguments in registers, as the
 * engine's own repeated calls (CachedCall::callWithArguments) go; no argument buffer, call frame
 * built in memory, nor count taken on the code. Empty when it can't (or the callee throws). */
static JSValue tovCallDirect(VM& vm, JSFunction* fn, JSValue thisValue, size_t argumentCount, const JSValueRef arguments[], bool& handled)
{
    handled = false;
    FunctionExecutable* executable = fn->jsExecutable();
    JSScope* functionScope = fn->scope();
    JSGlobalObject* realm = functionScope->realm();
    VMEntryScope entryScope(vm, realm);
    if (!vm.isSafeToRecurseSoft() || vm.disallowVMEntryCount) [[unlikely]]
        return { };
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    CodeBlock* codeBlock = nullptr;
    {
        DeferTraps deferTraps(vm); // (not jettisoned before it runs)
        executable->prepareForExecution<FunctionExecutable>(vm, fn, functionScope, CodeSpecializationKind::CodeForCall, codeBlock);
        if (throwScope.exception()) [[unlikely]] {
            handled = true;
            return { };
        }
    }
    if (codeBlock->numParameters() > static_cast<unsigned>(argumentCount + 1))
        return { };
    codeBlock->m_shouldAlwaysBeInlined = false;
    void* entry = executable->generatedJITCodeAddressForCall();
    handled = true;
    auto a = [&](size_t i) { return toJS(realm, arguments[i]); };
    auto clobberizeValidator = makeScopeExit([&] {
        vm.didEnterVM = true;
    });
    throwScope.release();
    switch (argumentCount) {
    case 0: return JSValue::decode(vmEntryToJavaScriptWith0Arguments(entry, &vm, codeBlock, fn, thisValue, nullptr));
    case 1: return JSValue::decode(vmEntryToJavaScriptWith1Arguments(entry, &vm, codeBlock, fn, thisValue, nullptr, a(0)));
    case 2: return JSValue::decode(vmEntryToJavaScriptWith2Arguments(entry, &vm, codeBlock, fn, thisValue, nullptr, a(0), a(1)));
    case 3: return JSValue::decode(vmEntryToJavaScriptWith3Arguments(entry, &vm, codeBlock, fn, thisValue, nullptr, a(0), a(1), a(2)));
    case 4: return JSValue::decode(vmEntryToJavaScriptWith4Arguments(entry, &vm, codeBlock, fn, thisValue, nullptr, a(0), a(1), a(2), a(3)));
    case 5: return JSValue::decode(vmEntryToJavaScriptWith5Arguments(entry, &vm, codeBlock, fn, thisValue, nullptr, a(0), a(1), a(2), a(3), a(4)));
    default: return JSValue::decode(vmEntryToJavaScriptWith6Arguments(entry, &vm, codeBlock, fn, thisValue, nullptr, a(0), a(1), a(2), a(3), a(4), a(5)));
    }
}
#endif

JSValueRef TVCall(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    TovLockScope locker(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* callee = toJS(function);
#if (CPU(ARM64) || CPU(X86_64)) && CPU(ADDRESS64) && !ENABLE(C_LOOP)
    if (argumentCount <= 6) {
        if (auto* fn = dynamicDowncast<JSFunction>(callee); fn && !fn->isHostFunction()) {
            bool handled;
            JSValue thisValue = thisObject ? JSValue(toJS(thisObject)) : JSValue(globalObject->globalThis());
            JSValue result = tovCallDirect(vm, fn, thisValue, argumentCount, arguments, handled);
            if (handled) {
                if (Exception* thrown = scope.exception()) [[unlikely]] {
                    if (exception)
                        *exception = toRef(globalObject, thrown->value());
                    scope.clearException();
                    return nullptr;
                }
                return toRef(globalObject, result);
            }
        }
    }
#endif
    auto callData = JSC::getCallData(callee);
    if (callData.type == CallData::Type::None) [[unlikely]]
        return JSObjectCallAsFunction(ctx, function, thisObject, argumentCount, arguments, exception);
    MarkedArgumentBuffer args;
    for (size_t i = 0; i < argumentCount; i++)
        args.append(toJS(globalObject, arguments[i]));
    JSValue result = JSC::call(globalObject, callee, callData, thisObject ? JSValue(toJS(thisObject)) : JSValue(globalObject->globalThis()), args);
    if (Exception* thrown = scope.exception()) [[unlikely]] {
        if (exception)
            *exception = toRef(globalObject, thrown->value());
        scope.clearException();
        return nullptr;
    }
    return toRef(globalObject, result);
}

void TVHeapStatistics(JSContextRef ctx, double out[3])
{
    VM& vm = toJS(ctx)->vm();
    JSLockHolder locker(vm);
    out[0] = vm.heap.size();
    out[1] = vm.heap.capacity();
    out[2] = vm.heap.extraMemorySize();
}

void TVReleaseMemory(JSContextRef ctx, bool dropCode)
{
    VM& vm = toJS(ctx)->vm();
    JSLockHolder locker(vm);
    if (dropCode)
        vm.deleteAllCode(DeleteAllCodeIfNotCollecting);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    // (the blocks that collection emptied are freed now, not as the incremental sweeper gets to
    // them: until then the allocator has nothing to give back)
    vm.heap.sweepSynchronously();
    WTF::releaseFastMallocFreeMemory();
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
JSValueRef TVMakeString(JSContextRef ctx, const char* utf8, size_t len)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    TovLockScope locker(vm);
    if (!len)
        return toRef(globalObject, jsEmptyString(vm));
    std::span<const Latin1Character> bytes { reinterpret_cast<const Latin1Character*>(utf8), len };
    String text = charactersAreAllASCII(bytes) ? String(bytes) : String::fromUTF8ReplacingInvalidSequences(bytes);
    return toRef(globalObject, jsString(vm, WTF::move(text)));
}

// (UTF-8 written straight from the string's characters, counted first: no copy between)
template<typename CharacterType>
static size_t tovUTF8Length(std::span<const CharacterType> chars)
{
    size_t len = chars.size();
    for (size_t i = 0; i < chars.size(); i++) {
        char16_t c = chars[i];
        if (c < 0x80)
            continue;
        if (c < 0x800)
            len += 1;
        else if (U16_IS_LEAD(c) && i + 1 < chars.size() && U16_IS_TRAIL(chars[i + 1])) {
            len += 2;
            i++;
        } else
            len += 2;
    }
    return len;
}

template<typename CharacterType>
static void tovWriteUTF8(std::span<const CharacterType> chars, char* out)
{
    for (size_t i = 0; i < chars.size(); i++) {
        char32_t c = chars[i];
        if (c < 0x80) {
            *out++ = static_cast<char>(c);
            continue;
        }
        if (c < 0x800) {
            *out++ = static_cast<char>(0xc0 | (c >> 6));
            *out++ = static_cast<char>(0x80 | (c & 0x3f));
            continue;
        }
        if (U16_IS_SURROGATE(c)) {
            if (U16_IS_LEAD(c) && i + 1 < chars.size() && U16_IS_TRAIL(chars[i + 1])) {
                c = U16_GET_SUPPLEMENTARY(c, chars[i + 1]);
                i++;
                *out++ = static_cast<char>(0xf0 | (c >> 18));
                *out++ = static_cast<char>(0x80 | ((c >> 12) & 0x3f));
                *out++ = static_cast<char>(0x80 | ((c >> 6) & 0x3f));
                *out++ = static_cast<char>(0x80 | (c & 0x3f));
                continue;
            }
            c = 0xfffd;
        }
        *out++ = static_cast<char>(0xe0 | (c >> 12));
        *out++ = static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        *out++ = static_cast<char>(0x80 | (c & 0x3f));
    }
}

size_t TVGetStringUTF8(JSContextRef ctx, JSValueRef value, char* buf, size_t cap)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    TovLockScope locker(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue v = toJS(globalObject, value);
    if (!v.isString())
        return SIZE_MAX;
    // (a rope is resolved, as reading it anywhere would)
    auto text = asString(v)->value(globalObject);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return SIZE_MAX;
    }
    const String& s = text;
    // (simdutf's, as for any of the engine's conversions; but for lone surrogates, which it
    // doesn't write as U+FFFD)
    if (s.is8Bit()) {
        auto chars = s.span8();
        if (charactersAreAllASCII(chars)) {
            if (buf && chars.size() <= cap)
                memcpy(buf, chars.data(), chars.size());
            return chars.size();
        }
        auto latin1 = reinterpret_cast<const char*>(chars.data());
        size_t len = simdutf::utf8_length_from_latin1(latin1, chars.size());
        if (buf && len <= cap)
            simdutf::convert_latin1_to_utf8(latin1, chars.size(), buf);
        return len;
    }
    auto chars = s.span16();
    if (simdutf::validate_utf16(chars.data(), chars.size())) {
        size_t len = simdutf::utf8_length_from_utf16(chars.data(), chars.size());
        if (buf && len <= cap)
            simdutf::convert_valid_utf16_to_utf8(chars.data(), chars.size(), buf);
        return len;
    }
    size_t len = tovUTF8Length(chars);
    if (buf && len <= cap)
        tovWriteUTF8(chars, buf);
    return len;
}

JSValueRef TVMakeLatin1String(JSContextRef ctx, const char* bytes, size_t len)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    TovLockScope locker(vm);
    if (!len)
        return toRef(globalObject, jsEmptyString(vm));
    return toRef(globalObject, jsString(vm, String(std::span<const Latin1Character> { reinterpret_cast<const Latin1Character*>(bytes), len })));
}

size_t TVGetStringLatin1(JSContextRef ctx, JSValueRef value, char* buf, size_t cap)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    TovLockScope locker(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue v = toJS(globalObject, value);
    if (!v.isString())
        return SIZE_MAX;
    auto text = asString(v)->value(globalObject);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return SIZE_MAX;
    }
    const String& s = text;
    size_t len = s.length();
    if (buf && len <= cap) {
        if (s.is8Bit())
            memcpy(buf, s.span8().data(), len);
        else {
            auto chars = s.span16();
            for (size_t i = 0; i < len; i++)
                buf[i] = static_cast<char>(chars[i]);
        }
    }
    return len;
}
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
