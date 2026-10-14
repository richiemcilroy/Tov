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
#include "JSGenericTypedArrayViewInlines.h"
#include "JSTypedArrays.h"
#include "TopExceptionScope.h"
#include "Options.h"
#include "SourceProvider.h"
#include <unistd.h>
#include <wtf/FileHandle.h>
#include <wtf/RunLoop.h>
#include <wtf/TimeZone.h>
#include <wtf/text/StringImpl.h>

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

void TVInitialize()
{
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

JSValueRef TVCall(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    TovLockScope locker(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* callee = toJS(function);
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
