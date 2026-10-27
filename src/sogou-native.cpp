// SPDX-License-Identifier: MIT
// Fcitx5 candidate UI and CPIS engine bridge. See docs/cloud.md for network behavior.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <curl/curl.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/event.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <functional>
#include <gio/gio.h>
#include <opencc/opencc.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace fcitx {
class Word : public CandidateWord {
    std::function<void(InputContext *)> fn;

  public:
    Word(Text t, std::function<void(InputContext *)> f)
        : CandidateWord(std::move(t)), fn(std::move(f)) {}
    void select(InputContext *ic) const override { fn(ic); }
};
class SogouNative : public InputMethodEngine {
    GDBusConnection *bus = nullptr;
    bool modeReady = false, needsReplay = false;
    using Clock = std::chrono::steady_clock;
    Clock::time_point retryAfter{}, deadline{};
    EventLoop *loop = nullptr;
    std::unique_ptr<EventSourceTime> recovery;
    unsigned long operation = 0;
    const char *operationName = "startup";
    Clock::time_point operationStart{};
    bool operationFailed = false;
    static double elapsed(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    void beginOperation(const char *name = "candidate") {
        ++operation;
        operationName = name;
        operationFailed = false;
        operationStart = Clock::now();
        deadline = operationStart + std::chrono::milliseconds(180);
    }
    // Log only slow/failed operations; never log key values or composition text.
    struct OperationLog {
        SogouNative *self;
        ~OperationLog() {
            double ms = elapsed(self->operationStart);
            if (ms >= 30 || self->operationFailed)
                fprintf(stderr, "sogou-native: event=operation op=%lu kind=%s elapsed_ms=%.2f failed=%d replay_pending=%d\n",
                        self->operation, self->operationName, ms, self->operationFailed, self->needsReplay);
        }
    };

    // Main-thread-only conversion: preserve engine indices, simplify display and commits.
    opencc_t simplified = opencc_open("t2s.json");
    std::string toSimplified(const std::string &text) {
        if (text.empty() || simplified == reinterpret_cast<opencc_t>(-1))
            return text;
        char *converted = opencc_convert_utf8(simplified, text.data(), text.size());
        if (!converted)
            return text;
        std::string result(converted);
        opencc_convert_utf8_free(converted);
        return result;
    }
    void commitSimplified(InputContext *ic, const std::string &text) {
        ic->commitString(toSimplified(text));
    }
    std::string uid, raw, composition;
    std::vector<std::string> words;
    int selected = 0, page = 0;
    std::vector<int> localIndices;
    EventDispatcher dispatcher;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false, pending = false;
    std::atomic<unsigned long> generation{0};
    std::string query;
    std::function<void(std::vector<std::string>)> deliver;
    // Generations invalidate queued replies; only the Fcitx thread touches InputContext.
    void invalidate() {
        ++generation;
        std::lock_guard<std::mutex> lock(mutex);
        pending = false;
        wake.notify_one();
    }
    static size_t receive(char *data, size_t size, size_t count, void *out) {
        auto &body = *static_cast<std::string *>(out);
        size_t n = size * count;
        if (body.size() + n > 65536)
            return 0;
        body.append(data, n);
        return n;
    }
    std::vector<std::string> fetch(const std::string &q, CURL *curl, CURLM *multi,
                                   unsigned long ticket) {
        std::vector<std::string> result;
        if (q.empty() || q.size() > 100 ||
            q.find_first_not_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos)
            return result;
        std::string payload;
        payload += char(q.size() + 10);
        payload.append("\0\5\0\0\0\0\1", 7);
        payload += char(q.size());
        payload += q;
        unsigned char checksum = 0;
        for (unsigned char c : payload)
            checksum ^= c;
        payload += char(checksum);
        if (!curl || !multi)
            return result;
        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL,
                         "https://shouji.sogou.com/web_ime/"
                         "mobile.php?durtot=0&h=000000000000000&r=store_mf_wandoujia&v=3.7");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(payload.size()));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1200L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2500L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_multi_add_handle(multi, curl);
        int running = 0;
        CURLMcode mc = curl_multi_perform(multi, &running);
        while (mc == CURLM_OK && running && generation.load() == ticket) {
            curl_multi_poll(multi, nullptr, 0, 25, nullptr);
            mc = curl_multi_perform(multi, &running);
        }
        CURLcode status = CURLE_ABORTED_BY_CALLBACK;
        int remaining = 0;
        while (auto *message = curl_multi_info_read(multi, &remaining))
            if (message->msg == CURLMSG_DONE)
                status = message->data.result;
        long http = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        curl_multi_remove_handle(multi, curl);
        if (status != CURLE_OK || http != 200 || body.size() < 22)
            return result;
        auto u16 = [&](size_t pos) -> size_t {
            if (pos + 2 > body.size())
                throw std::runtime_error("truncated cloud reply");
            return (unsigned char)body[pos] | ((unsigned char)body[pos + 1] << 8);
        };
        try {
            size_t count = u16(18), pos = 20;
            if (count > 32)
                return {};
            for (size_t i = 0; i < count; i++) {
                size_t n = u16(pos);
                pos += 2;
                if (n % 2 || !n || pos + n > body.size())
                    return {};
                GError *error = nullptr;
                gsize written = 0;
                char *word =
                    g_convert(body.data() + pos, n, "UTF-8", "UTF-16LE", nullptr, &written, &error);
                if (!word) {
                    if (error)
                        g_error_free(error);
                    return {};
                }
                std::string text(word, written);
                g_free(word);
                if (text.find('\0') != std::string::npos)
                    return {};
                result.push_back(text);
                pos += n;
                n = u16(pos);
                pos += 2 + n;
                if (pos > body.size())
                    return {};
                n = u16(pos);
                pos += 2 + n + 1;
                if (pos > body.size())
                    return {};
            }
        } catch (...) {
            return {};
        }
        return result;
    }
    // Keep curl handles on one worker to reuse connections without blocking keystrokes.
    void run() {
        CURL *curl = curl_easy_init();
        CURLM *multi = curl_multi_init();
        std::map<std::string, std::vector<std::string>> cache;
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            wake.wait(lock, [&] { return stopping || pending; });
            if (stopping)
                break;
            auto ticket = generation.load();
            if (!cache.count(query) && wake.wait_for(lock, std::chrono::milliseconds(60), [&] {
                    return stopping || generation.load() != ticket;
                }))
                continue;
            auto q = query;
            auto callback = deliver;
            pending = false;
            lock.unlock();
            auto found = cache.find(q);
            auto result = found != cache.end() ? found->second : fetch(q, curl, multi, ticket);
            if (!result.empty()) {
                if (cache.size() >= 256)
                    cache.clear();
                cache[q] = result;
            }
            if (generation.load() == ticket && !result.empty())
                dispatcher.schedule([this, ticket, callback, result = std::move(result)]() mutable {
                    if (generation.load() == ticket)
                        callback(std::move(result));
                });
            lock.lock();
        }
        if (curl)
            curl_easy_cleanup(curl);
        if (multi)
            curl_multi_cleanup(multi);
    }
    // Weak context reference is dereferenced only in the event-loop callback.
    void requestCloud(InputContext *ic) {
        if (raw.empty() || getenv("SOGOU_NATIVE_OFFLINE"))
            return;
        auto ref = ic->watch();
        auto expected = raw;
        std::lock_guard<std::mutex> lock(mutex);
        query = raw;
        deliver = [this, ref, expected](std::vector<std::string> cloud) {
            auto *context = ref.get();
            if (!context || !context->hasFocus() || raw != expected)
                return;
            auto locals = words;
            for (auto &word : locals) word = toSimplified(word);
            words.clear();
            localIndices.clear();
            for (auto &word : cloud) {
                word = toSimplified(word);
                if (words.size() == 2)
                    break;
                if (std::find(words.begin(), words.end(), word) == words.end()) {
                    words.push_back(word);
                    localIndices.push_back(-1);
                }
            }
            for (size_t i = 0; i < locals.size() && words.size() < 5; i++)
                if (std::find(words.begin(), words.end(), locals[i]) == words.end()) {
                    words.push_back(locals[i]);
                    localIndices.push_back(i);
                }
            selected = 0;
            show(context);
        };
        pending = true;
        wake.notify_one();
    }

    GVariant *call(const char *method, GVariant *args) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (Clock::now() < retryAfter || remaining <= 0) {
            operationFailed = true;
            // Do not emit one cooldown line per key; the original failure is logged.
            if (Clock::now() >= retryAfter)
                fprintf(stderr, "sogou-native: event=budget_exhausted op=%lu method=%s elapsed_ms=%.2f\n", operation, method, elapsed(operationStart));
            if (args) { g_variant_ref_sink(args); g_variant_unref(args); }
            throw std::runtime_error("engine cooling down; composition retained");
        }
        auto started = Clock::now();
        GError *err = nullptr;
        if (!bus)
            bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
        if (!bus) {
            operationFailed = true;
            fprintf(stderr, "sogou-native: event=bus_connection_failed op=%lu elapsed_ms=%.2f domain=%s code=%d\n",
                    operation, elapsed(started), err ? g_quark_to_string(err->domain) : "unknown", err ? err->code : -1);
            if (args) { g_variant_ref_sink(args); g_variant_unref(args); }
            std::string msg = err ? err->message : "no bus";
            if (err)
                g_error_free(err);
            throw std::runtime_error(msg);
        }
        auto *r = g_dbus_connection_call_sync(bus, "com.cpis.engine", "/com/cpis/engine",
                                              "com.cpis.engine", method, args, nullptr,
                                              G_DBUS_CALL_FLAGS_NO_AUTO_START, int(remaining), nullptr, &err);
        if (!r) {
            operationFailed = true;
            const char *reason = "dbus_error";
            if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
                g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY) ||
                g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_TIMEOUT)) reason = "engine_reply_timeout";
            else if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
                     g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)) reason = "engine_service_unavailable";
            else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CLOSED) ||
                     g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_DISCONNECTED)) reason = "bus_disconnected";
            char *remote = err ? g_dbus_error_get_remote_error(err) : nullptr;
            fprintf(stderr, "sogou-native: event=rpc_failed op=%lu method=%s reason=%s elapsed_ms=%.2f budget_ms=%lld domain=%s code=%d remote=%s cooldown_ms=1000\n",
                    operation, method, reason, elapsed(started), static_cast<long long>(remaining),
                    err ? g_quark_to_string(err->domain) : "unknown", err ? err->code : -1, remote ? remote : "none");
            g_free(remote);
            modeReady = false;
            retryAfter = Clock::now() + std::chrono::milliseconds(1000);
            std::string msg = err ? err->message : "call failed";
            if (err)
                g_error_free(err);

            throw std::runtime_error(msg);
        }
        if (elapsed(started) >= 30)
            fprintf(stderr, "sogou-native: event=rpc_slow op=%lu method=%s elapsed_ms=%.2f budget_ms=%lld\n",
                    operation, method, elapsed(started), static_cast<long long>(remaining));
        return r;
    }
    void simple(const char *method) {
        auto *r = call(method, g_variant_new("(s)", uid.c_str()));
        g_variant_unref(r);
    }
    void ensureMode() {
        if (!modeReady) {
            auto *r = call("SetMode",
                           g_variant_new("(sss)", uid.c_str(), "kb_zh_cn_pc_pinyin_26key", "zh_cn"));
            g_variant_unref(r);
            modeReady = true;
        }
    }
    void prepare() {
        ensureMode();
        simple("Clear");
    }
    void clear() {
        page = 0;
        invalidate();
        if (recovery) recovery->setEnabled(false);
        raw.clear();
        composition.clear();
        words.clear();
        localIndices.clear();
        needsReplay = false;
        selected = 0;
        simple("Clear");
    }
    void replay(InputContext *ic) {
        prepare();
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("ai"));
        for (char c : raw) g_variant_builder_add(&b, "i", int(c));
        auto *r = call("PushChars", g_variant_new("(s@ai)", uid.c_str(), g_variant_builder_end(&b)));
        g_variant_unref(r);
        update(ic);
    }
    void show(InputContext *ic) {
        auto &p = ic->inputPanel();
        p.reset();
        p.setPreedit(Text(toSimplified(composition)));
        auto list = std::make_unique<CommonCandidateList>();
        list->setPageSize(5);
        list->setLayoutHint(CandidateLayoutHint::Horizontal);
        for (size_t i = 0; i < words.size(); i++)
            list->append<Word>(Text(std::to_string(i + 1) + " " + toSimplified(words[i]) +
                                    (localIndices.size() > i && localIndices[i] < 0 ? " ☁" : "")),
                               [this, i](InputContext *c) {
                                   beginOperation("candidate");
                                   OperationLog trace{this};
                                   try {
                                       select(c, i);
                                   } catch (...) {
                                       fallback(c);
                                   }
                               });
        if (!words.empty()) {
            list->setGlobalCursorIndex(selected);
            p.setCandidateList(std::move(list));
        }
        ic->updatePreedit();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    }
    void update(InputContext *ic, bool cloud = true) {
        invalidate();
        auto *r = call("AcquireResult", g_variant_new("(s)", uid.c_str()));
        GVariant *a = g_variant_get_child_value(r, 0);
        words.clear();
        for (gsize i = 0; i < g_variant_n_children(a); i++) {
            auto *v = g_variant_get_child_value(a, i);
            words.emplace_back(g_variant_get_string(v, nullptr));
            g_variant_unref(v);
        }
        g_variant_unref(a);
        a = g_variant_get_child_value(r, 2);
        composition = g_variant_get_string(a, nullptr);
        g_variant_unref(a);
        a = g_variant_get_child_value(r, 3);
        std::string commit = g_variant_get_string(a, nullptr);
        g_variant_unref(a);
        g_variant_unref(r);
        if (!commit.empty())
            commitSimplified(ic, commit);
        if (composition.empty())
            raw.clear();
        else {
            raw = composition;
            raw.erase(std::remove(raw.begin(), raw.end(), '\''), raw.end());
        }
        localIndices.clear();
        for (size_t i = 0; i < words.size(); i++)
            localIndices.push_back(i);
        selected = 0;
        needsReplay = false;
        if (recovery) recovery->setEnabled(false);
        show(ic);
        if (cloud)
            requestCloud(ic);
    }
    void turnPage(InputContext *ic, bool forward, bool edge = false) {
        invalidate();
        if (!forward && page == 0)
            return;
        auto before = words;
        auto indices = localIndices;
        int cursor = selected;
        simple(forward ? "PageDown" : "PageUp");
        update(ic, false);
        if (words == before) {
            selected = cursor;
            localIndices = indices;
        } else {
            page += forward ? 1 : -1;
            selected = (!forward && edge && !words.empty()) ? int(words.size()) - 1 : 0;
        }
        show(ic);
    }
    void select(InputContext *ic, int i) {
        invalidate();
        if (i < 0 || size_t(i) >= localIndices.size())
            return;
        if (localIndices[i] < 0) {
            auto word = words[i];
            clear();
            commitSimplified(ic, word);
            show(ic);
            return;
        }
        // CPIS SelectCandidate uses an absolute index, not the visible page slot.
        i = page * 5 + localIndices[i];
        auto *r = call("SelectCandidate", g_variant_new("(sii)", uid.c_str(), 0, i));
        g_variant_unref(r);
        page = 0;
        update(ic);
    }
    void fallback(InputContext *ic, bool schedule = true) {
        invalidate();
        // A timeout is not a request to commit English. Keep the entire spelling.
        needsReplay = true;
        composition = raw;
        words.clear();
        localIndices.clear();
        page = selected = 0;
        show(ic);
        if (schedule && !raw.empty() && (!recovery || !recovery->isEnabled())) {
            auto ref = ic->watch();
            recovery = loop->addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1100000, 0,
                [this, ref](EventSourceTime *, uint64_t) {
                    auto *context = ref.get();
                    if (context && context->hasFocus() && needsReplay && !raw.empty()) {
                        beginOperation("recovery");
                        OperationLog trace{this};
                        fprintf(stderr, "sogou-native: event=recovery_start op=%lu\n", operation);
                        try {
                            replay(context);
                            fprintf(stderr, "sogou-native: event=recovery_ok op=%lu elapsed_ms=%.2f\n", operation, elapsed(operationStart));
                        }
                        catch (...) {
                            operationFailed = true;
                            fprintf(stderr, "sogou-native: event=recovery_failed op=%lu\n", operation);
                            fallback(context, false);
                        }
                    }
                    return true;
                });
            recovery->setOneShot();
        }
    }

  public:
    SogouNative(AddonManager *manager) {
        fprintf(stderr, "sogou-native: event=diagnostics_enabled version=1 slow_threshold_ms=30 operation_budget_ms=180 input_text_logged=false\n");
        loop = manager->eventLoop();
        dispatcher.attach(manager->eventLoop());
        worker = std::thread([this] { run(); });
        uid = std::string(g_get_user_name()) +
              "#im.module=fcitx5|im.name=sogou-ime-ng-fcitx5-kylin-desktop";
    }
    ~SogouNative() {
        recovery.reset();
        invalidate();
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            wake.notify_one();
        }
        worker.join();
        if (simplified != reinterpret_cast<opencc_t>(-1)) opencc_close(simplified);
        dispatcher.detach();
        if (bus)
            g_object_unref(bus);
    }
    void activate(const InputMethodEntry &, InputContextEvent &e) override {
        beginOperation("activate");
        OperationLog trace{this};
        try {
            ensureMode();
            clear();
            show(e.inputContext());
        } catch (...) {
            fallback(e.inputContext());
        }
    }
    void deactivate(const InputMethodEntry &, InputContextEvent &e) override {
        beginOperation("deactivate");
        OperationLog trace{this};
        if (!raw.empty())
            commitSimplified(e.inputContext(), raw);
        try {
            clear();
        } catch (...) {
            raw.clear();
            composition.clear();
            words.clear();
            selected = 0;
        }
        show(e.inputContext());
    }
    void reset(const InputMethodEntry &, InputContextEvent &e) override {
        beginOperation("reset");
        OperationLog trace{this};
        try {
            clear();
        } catch (...) {
            raw.clear();
            composition.clear();
            words.clear();
            selected = 0;
        }
        show(e.inputContext());
    }
    void keyEvent(const InputMethodEntry &, KeyEvent &e) override {
        if (e.isRelease())
            return;
        beginOperation("key");
        OperationLog trace{this};
        auto *ic = e.inputContext();
        auto k = e.key().sym();
        if (k == FcitxKey_Shift_L || k == FcitxKey_Shift_R)
            return;
        const bool punctuation = (k >= 33 && k <= 47) || (k >= 58 && k <= 64) ||
                                 (k >= 91 && k <= 96) || (k >= 123 && k <= 126);
        if (e.key().states().test(KeyState::Ctrl) || e.key().states().test(KeyState::Alt) ||
            e.key().states().test(KeyState::Super)) {
            if (!raw.empty()) {
                commitSimplified(ic, raw);
                try {
                    clear();
                } catch (...) {
                }
                show(ic);
            }
            return;
        }
        try {
            if ((k >= 'a' && k <= 'z') || (k == '\'' && !raw.empty())) {
                page = 0;
                bool fresh = raw.empty();
                raw += char(k);
                if (needsReplay) {
                    replay(ic);
                    e.filterAndAccept();
                    return;
                }
                if (fresh)
                    prepare();
                GVariantBuilder b;
                g_variant_builder_init(&b, G_VARIANT_TYPE("ai"));
                g_variant_builder_add(&b, "i", int(k));
                auto *r = call("PushChars",
                               g_variant_new("(s@ai)", uid.c_str(), g_variant_builder_end(&b)));
                g_variant_unref(r);
                update(ic);
            } else if (raw.empty()) {
                if (punctuation) {
                    ic->commitString(std::string(1, char(k)));
                    e.filterAndAccept();
                }
                return;
            } else if (k == FcitxKey_Escape) {
                clear();
                show(ic);
            } else if (k == FcitxKey_BackSpace) {
                raw.pop_back();
                std::string replay = raw;
                // Keep the edited spelling even if resetting the engine times out.
                prepare();
                raw = replay;
                if (!raw.empty()) {
                    GVariantBuilder b;
                    g_variant_builder_init(&b, G_VARIANT_TYPE("ai"));
                    for (char c : raw)
                        g_variant_builder_add(&b, "i", int(c));
                    auto *r = call("PushChars",
                                   g_variant_new("(s@ai)", uid.c_str(), g_variant_builder_end(&b)));
                    g_variant_unref(r);
                }
                update(ic);
            } else if (needsReplay && (k == FcitxKey_space || (k >= '1' && k <= '5'))) {
                replay(ic);
                // Recovery only: display candidates before accepting a selection.
            } else if (k == FcitxKey_space && !words.empty())
                select(ic, selected);
            else if (k >= '1' && k <= '5' && size_t(k - '1') < words.size())
                select(ic, k - '1');
            else if (k == FcitxKey_Page_Down || k == '.') {
                turnPage(ic, true);
            } else if (k == FcitxKey_Page_Up || k == ',') {
                turnPage(ic, false);
            } else if ((k == FcitxKey_Left || k == FcitxKey_Right || k == FcitxKey_Up ||
                        k == FcitxKey_Down) &&
                       !words.empty()) {
                invalidate();
                bool forward = k == FcitxKey_Right || k == FcitxKey_Down;
                int next = selected + (forward ? 1 : -1);
                if (next < 0 || next >= int(words.size()))
                    turnPage(ic, forward, true);
                else {
                    selected = next;
                    show(ic);
                }
            } else if (k == FcitxKey_Return) {
                commitSimplified(ic, raw);
                clear();
                show(ic);
            } else {
                if (!words.empty())
                    select(ic, selected);
                else {
                    commitSimplified(ic, raw);
                    clear();
                    show(ic);
                }
                if (punctuation) {
                    ic->commitString(std::string(1, char(k)));
                    e.filterAndAccept();
                }
                return;
            }
            e.filterAndAccept();
        } catch (const std::exception &ex) {
            fallback(ic);
            e.filterAndAccept();
        }
    }
};
class Factory : public AddonFactory {
    AddonInstance *create(AddonManager *m) override { return new SogouNative(m); }
};
} // namespace fcitx
FCITX_ADDON_FACTORY(fcitx::Factory);
