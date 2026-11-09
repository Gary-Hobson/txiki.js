/*
 * txiki.js JavaScript inspector, distributed under the MIT license.
 * CDP behavior follows G-Yong/QuickJS-Debugger; transport uses libwebsockets.
 */
#include "inspector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <libwebsockets.h>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr size_t max_message = 16 * 1024 * 1024;
constexpr size_t write_chunk = 16 * 1024;
constexpr uint64_t poll_interval_ms = 20;

std::string js_string(JSContext *ctx, JSValueConst value) {
    size_t size = 0;
    const char *str = JS_ToCStringLen(ctx, &size, value);
    if (!str) {
        return {};
    }
    std::string result(str, size);
    JS_FreeCString(ctx, str);
    return result;
}

std::string atom_string(JSContext *ctx, JSAtom atom) {
    if (atom == JS_ATOM_NULL) {
        return {};
    }
    JSValue value = JS_AtomToString(ctx, atom);
    auto result = js_string(ctx, value);
    JS_FreeValue(ctx, value);
    return result;
}

class Json {
  public:
    Json(JSContext *ctx, JSValue value) : ctx(ctx), value(value) {
    }
    Json(const Json &other) : ctx(other.ctx), value(JS_DupValue(ctx, other.value)) {
    }
    ~Json() {
        JS_FreeValue(ctx, value);
    }

    Json get(const char *key) const {
        return Json(ctx, JS_IsObject(value) ? JS_GetPropertyStr(ctx, value, key) : JS_UNDEFINED);
    }
    std::string str() const {
        return JS_IsString(value) ? js_string(ctx, value) : std::string();
    }
    bool boolean() const {
        return JS_IsBool(value) && JS_ToBool(ctx, value);
    }
    int integer(int fallback = -1) const {
        double n;
        if (!JS_IsNumber(value) || JS_ToFloat64(ctx, &n, value) || !(n >= 0 && n <= INT32_MAX && std::floor(n) == n)) {
            return fallback;
        }
        return static_cast<int>(n);
    }
    void set(const char *key, const Json &item) const {
        JS_DefinePropertyValueStr(ctx, value, key, JS_DupValue(ctx, item.value), JS_PROP_C_W_E);
    }
    std::string stringify() const {
        Json encoded(ctx, JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED));
        return encoded.str();
    }

    JSContext *ctx;
    JSValue value;
};

std::string script_url(std::string name) {
#ifdef _WIN32
    std::replace(name.begin(), name.end(), '\\', '/');
#endif
    if (name.empty() || name.front() == '<' || name.find("://") != std::string::npos) {
        return name;
    }

    // File URLs are compared with the URLs sent by Chrome and VS Code.
    std::string url = name.front() == '/' ? "file://" : "file:///";
    for (unsigned char ch : name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            std::strchr("/:-._~", ch)) {
            url += static_cast<char>(ch);
        } else {
            const char *hex = "0123456789ABCDEF";
            url += '%';
            url += hex[ch >> 4];
            url += hex[ch & 15];
        }
    }
    return url;
}

std::string source_url(const char *filename) {
    if (filename[0] == '<' || std::strstr(filename, "://") || filename[0] == '/'
#ifdef _WIN32
        || filename[0] == '\\' || (filename[0] && filename[1] == ':')
#endif
    ) {
        return script_url(filename);
    }
    uv_fs_t request;
    const auto result = uv_fs_realpath(nullptr, &request, filename, nullptr);
    auto url = script_url(result == 0 ? static_cast<const char *>(request.ptr) : filename);
    uv_fs_req_cleanup(&request);
    return url;
}

using Position = std::pair<int, int>;

struct Script {
    std::string id;
    std::string url;
    std::string source;
    bool module;
    bool parsed;
    std::set<Position> locations;
};

struct Breakpoint {
    std::string url;
    std::regex pattern;
    int line;
    int column;
    bool regex;

    bool matches(const Script &script) const {
        return regex ? std::regex_search(script.url, pattern) : script.url == url;
    }

    std::set<Position>::const_iterator resolve(const Script &script) const {
        auto pos = script.locations.lower_bound({ line, column });
        return pos != script.locations.end() && pos->first == line && matches(script) ? pos : script.locations.end();
    }
};

enum class Step { none, into, over, out };

}  // namespace

// Both the inspector loop and all calls into QuickJS run on the runtime thread.
// A paused interpreter pumps only the inspector's sockets.
struct TJSInspector {
    explicit TJSInspector(JSContext *ctx, bool wait) : debuggee(ctx), waiting(wait), break_on_entry(wait) {
    }
    ~TJSInspector();

    bool start(uv_loop_t *loop, int port);
    bool service(bool block);
    void disconnect();
    void send(const Json &message);
    void event(const char *method, const Json &params);
    void handle_message(const std::string &message);
    void announce(const Script &script);
    void register_source(const char *name, const char *source, size_t length, bool module, JSValueConst bytecode);
    const Script *script_by_id(const std::string &id) const;
    void trace(JSAtom filename, int line, int column, int flags);
    void clear_objects();

    Json object(std::initializer_list<std::pair<const char *, Json>> items = {}) const {
        Json result(protocol, JS_NewObjectProto(protocol, JS_NULL));
        for (const auto &item : items) {
            result.set(item.first, item.second);
        }
        return result;
    }
    Json array(const std::vector<Json> &items = {}) const {
        Json result(protocol, JS_NewArray(protocol));
        for (uint32_t i = 0; i < items.size(); i++) {
            JS_SetPropertyUint32(protocol, result.value, i, JS_DupValue(protocol, items[i].value));
        }
        return result;
    }
    Json str(std::string_view value) const {
        return Json(protocol, JS_NewStringLen(protocol, value.data(), value.size()));
    }
    Json number(int value) const {
        return Json(protocol, JS_NewInt32(protocol, value));
    }
    Json boolean(bool value) const {
        return Json(protocol, JS_NewBool(protocol, value));
    }
    Json location(const Script &script, int line, int column = 0) const {
        return object(
            { { "scriptId", str(script.id) }, { "lineNumber", number(line) }, { "columnNumber", number(column) } });
    }
    Json frames();
    Json scope(int level, bool closure);
    Json properties(const std::string &id, bool &found);
    Json remote_value(JSValueConst value);
    Json evaluation(const Json &params, int level);
    std::string store_object(JSValueConst value);
    int frame_level(const std::string &id) const;

    static int callback(lws *wsi, lws_callback_reasons reason, void *, void *in, size_t len);
    int socket_event(lws *wsi, lws_callback_reasons reason, void *in, size_t len);
    bool allowed_host(lws *wsi) const;
    bool allowed_upgrade(lws *wsi) const;
    int discovery(lws *wsi, std::string_view path);

    JSContext *debuggee;
    JSRuntime *protocol_rt = nullptr;
    JSContext *protocol = nullptr;
    lws_context *server = nullptr;
    uv_timer_t *timer = nullptr;
    lws *client = nullptr;
    std::string host;
    std::string incoming;
    std::deque<std::string> requests;
    std::deque<std::string> outgoing;
    size_t queued_bytes = 0;
    size_t write_offset = 0;

    bool waiting;
    bool break_on_entry;
    bool enabled = false;
    bool breakpoints_active = true;
    bool pause_requested = false;
    bool skip_breakpoint = false;
    bool paused = false;
    bool evaluating = false;
    Step step = Step::none;
    int pause_depth = 0;
    int pause_line = 0;
    int pause_column = 0;
    std::string pause_file;
    uint64_t pause_id = 0;
    uint64_t next_poll = 0;
    uint64_t next_script = 0;
    uint64_t next_breakpoint = 0;
    uint64_t next_object = 0;
    std::map<std::string, Script> scripts;
    std::map<std::string, Breakpoint> breakpoints;
    std::map<std::string, JSValue> objects;
    std::map<std::string, std::pair<int, bool>> scopes;
    std::map<std::string, int> frame_levels;
};

TJSInspector::~TJSInspector() {
    JS_SetDebugTraceHandler(debuggee, nullptr, nullptr);
    JS_SetDebugSourceHandler(debuggee, nullptr, nullptr);
    if (timer) {
        uv_close(reinterpret_cast<uv_handle_t *>(timer),
                 [](uv_handle_t *handle) { delete reinterpret_cast<uv_timer_t *>(handle); });
    }
    if (server) {
        if (!outgoing.empty()) {
            lws_cancel_service(server);
            lws_service(server, 0);
        }
        lws_context_destroy(server);
    }
    clear_objects();
    if (protocol) {
        JS_FreeContext(protocol);
    }
    if (protocol_rt) {
        JS_FreeRuntime(protocol_rt);
    }
}

bool TJSInspector::start(uv_loop_t *loop, int port) {
    protocol_rt = JS_NewRuntime();
    if (!protocol_rt || !(protocol = JS_NewContext(protocol_rt))) {
        return false;
    }

    static const lws_protocols protocols[] = { { "tjs-inspector", callback, 0, write_chunk, 0, nullptr, write_chunk },
                                               {} };
    lws_context_creation_info info{};
    info.port = port;
    info.iface = "127.0.0.1";
    info.vhost_name = "tjs-inspector";
    info.protocols = protocols;
    info.user = this;
    info.options = LWS_SERVER_OPTION_DISABLE_IPV6 | LWS_SERVER_OPTION_VALIDATE_UTF8;
    server = lws_create_context(&info);
    if (!server) {
        return false;
    }

    auto *vhost = lws_get_vhost_by_name(server, info.vhost_name);
    if (!vhost) {
        return false;
    }
    host = "127.0.0.1:" + std::to_string(lws_get_vhost_listen_port(vhost));

    auto handle = std::make_unique<uv_timer_t>();
    if (uv_timer_init(loop, handle.get())) {
        return false;
    }
    timer = handle.release();
    timer->data = this;
    if (uv_timer_start(
            timer,
            [](uv_timer_t *timer) { static_cast<TJSInspector *>(timer->data)->service(false); },
            0,
            poll_interval_ms)) {
        return false;
    }
    uv_unref(reinterpret_cast<uv_handle_t *>(timer));

    std::fprintf(stderr, "Debugger listening on ws://%s/debug\n", host.c_str());
    if (waiting) {
        std::fprintf(stderr, "Waiting for debugger to connect...\n");
    }
    return true;
}

bool TJSInspector::service(bool block) {
    if (!block) {
        lws_cancel_service(server);
    }
    if (lws_service(server, 0) < 0) {
        disconnect();
        return false;
    }
    while (!requests.empty()) {
        auto message = std::move(requests.front());
        requests.pop_front();
        handle_message(message);
    }
    return true;
}

void TJSInspector::disconnect() {
    client = nullptr;
    enabled = false;
    paused = false;
    step = Step::none;
    breakpoints_active = true;
    pause_requested = false;
    skip_breakpoint = false;
    break_on_entry = waiting;
    breakpoints.clear();
    requests.clear();
    outgoing.clear();
    incoming.clear();
    queued_bytes = 0;
    write_offset = 0;
    clear_objects();
}

void TJSInspector::send(const Json &message) {
    if (!client) {
        return;
    }
    auto encoded = message.stringify();
    if (encoded.empty()) {
        JS_FreeValue(protocol, JS_GetException(protocol));
        return;
    }
    if (encoded.size() > max_message - queued_bytes) {
        lws_set_timeout(client, PENDING_TIMEOUT_KILLED_BY_SSL_INFO, LWS_TO_KILL_ASYNC);
        return;
    }
    queued_bytes += encoded.size();
    outgoing.push_back(std::move(encoded));
    lws_callback_on_writable(client);
}

void TJSInspector::event(const char *method, const Json &params) {
    send(object({ { "method", str(method) }, { "params", params } }));
}

int TJSInspector::callback(lws *wsi, lws_callback_reasons reason, void *, void *in, size_t len) {
    auto *self = wsi ? static_cast<TJSInspector *>(lws_context_user(lws_get_context(wsi))) : nullptr;
    return self ? self->socket_event(wsi, reason, in, len) : 0;
}

bool TJSInspector::allowed_host(lws *wsi) const {
    char header[128];
    if (lws_hdr_copy(wsi, header, sizeof(header), WSI_TOKEN_HOST) <= 0) {
        return false;
    }
    // VS Code's JavaScript debugger uses a bare localhost Host for discovery.
    return std::strcmp(header, "localhost") == 0 || host == header ||
           "localhost" + host.substr(host.find(':')) == header;
}

bool TJSInspector::allowed_upgrade(lws *wsi) const {
    char path[32];
    if (!allowed_host(wsi) || lws_hdr_copy(wsi, path, sizeof(path), WSI_TOKEN_GET_URI) <= 0 ||
        std::strcmp(path, "/debug") != 0) {
        return false;
    }

    char origin[128];
    const int n = lws_hdr_copy(wsi, origin, sizeof(origin), WSI_TOKEN_ORIGIN);
    return n == 0 || (n > 0 && (std::strcmp(origin, "devtools://devtools") == 0 ||
                                std::strcmp(origin, "chrome-devtools://devtools") == 0));
}

int TJSInspector::socket_event(lws *wsi, lws_callback_reasons reason, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_HTTP:
            return discovery(wsi, std::string_view(static_cast<const char *>(in), len));
        case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
            return client || !allowed_upgrade(wsi);
        case LWS_CALLBACK_ESTABLISHED:
            client = wsi;
            return 0;
        case LWS_CALLBACK_CLOSED:
            if (client == wsi) {
                disconnect();
            }
            return 0;
        case LWS_CALLBACK_RECEIVE:
            if (lws_frame_is_binary(wsi) || len > max_message - incoming.size()) {
                return -1;
            }
            incoming.append(static_cast<const char *>(in), len);
            if (lws_is_final_fragment(wsi) && !lws_remaining_packet_payload(wsi)) {
                if (requests.size() >= 64) {
                    return -1;
                }
                requests.push_back(std::move(incoming));
                incoming.clear();
            }
            return 0;
        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (outgoing.empty()) {
                return 0;
            }
            const auto &message = outgoing.front();
            const size_t length = std::min(write_chunk, message.size() - write_offset);
            const bool last = length == message.size() - write_offset;
            unsigned char buffer[LWS_PRE + write_chunk];
            std::memcpy(buffer + LWS_PRE, message.data() + write_offset, length);
            auto flags = lws_write_ws_flags(LWS_WRITE_TEXT, write_offset == 0, last);
            if (lws_write(wsi, buffer + LWS_PRE, length, static_cast<lws_write_protocol>(flags)) < 0) {
                return -1;
            }
            write_offset += length;
            if (last) {
                queued_bytes -= message.size();
                outgoing.pop_front();
                write_offset = 0;
            }
            if (!outgoing.empty()) {
                lws_callback_on_writable(wsi);
            }
            return 0;
        }
        default:
            return 0;
    }
}

int TJSInspector::discovery(lws *wsi, std::string_view path) {
    if (!allowed_host(wsi)) {
        lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, nullptr);
        return -1;
    }

    std::string body;
    if (path == "/json/version") {
        body = object({ { "Browser", str("txiki.js") }, { "Protocol-Version", str("1.3") } }).stringify();
    } else if (path == "/json" || path == "/json/list") {
        const std::string endpoint = host + "/debug";
        body = array({ object({
                         { "id", str("txiki") },
                         { "type", str("node") },
                         { "title", str("txiki.js") },
                         { "url", str(scripts.empty() ? "" : scripts.begin()->second.url) },
                         { "webSocketDebuggerUrl", str("ws://" + endpoint) },
                         { "devtoolsFrontendUrl",
                           str("devtools://devtools/bundled/js_app.html?v8only=true&ws=" + endpoint) },
                     }) })
                   .stringify();
    } else {
        lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, nullptr);
        return -1;
    }

    unsigned char buffer[LWS_PRE + 1024];
    unsigned char *p = buffer + LWS_PRE;
    if (lws_add_http_common_headers(wsi,
                                    HTTP_STATUS_OK,
                                    "application/json",
                                    body.size(),
                                    &p,
                                    buffer + sizeof(buffer)) ||
        lws_finalize_write_http_header(wsi, buffer + LWS_PRE, &p, buffer + sizeof(buffer))) {
        return -1;
    }
    std::vector<unsigned char> payload(LWS_PRE + body.size());
    std::memcpy(payload.data() + LWS_PRE, body.data(), body.size());
    if (lws_write(wsi, payload.data() + LWS_PRE, body.size(), LWS_WRITE_HTTP_FINAL) < 0) {
        return -1;
    }
    return lws_http_transaction_completed(wsi) ? -1 : 0;
}

void TJSInspector::announce(const Script &script) {
    const int end_line = static_cast<int>(std::count(script.source.begin(), script.source.end(), '\n'));
    auto last_newline = script.source.rfind('\n');
    const int end_column =
        static_cast<int>(script.source.size() - (last_newline == std::string::npos ? 0 : last_newline + 1));
    event(script.parsed ? "Debugger.scriptParsed" : "Debugger.scriptFailedToParse",
          object({ { "scriptId", str(script.id) },
                   { "url", str(script.url) },
                   { "startLine", number(0) },
                   { "startColumn", number(0) },
                   { "endLine", number(end_line) },
                   { "endColumn", number(end_column) },
                   { "executionContextId", number(1) },
                   { "hash", str("") },
                   { "isModule", boolean(script.module) } }));
}

const Script *TJSInspector::script_by_id(const std::string &id) const {
    for (const auto &entry : scripts) {
        if (entry.second.id == id) {
            return &entry.second;
        }
    }
    return nullptr;
}

void TJSInspector::register_source(const char *name,
                                   const char *source,
                                   size_t length,
                                   bool module,
                                   JSValueConst bytecode) {
    if (evaluating || (name[0] == '<' && std::strcmp(name, "<evalScript>") != 0) ||
        std::strncmp(name, "tjs:", 4) == 0) {
        return;
    }

    // Internal modules (including the CLI bootstrap) come from embedded bytecode.
    const bool parsed = !JS_IsUndefined(bytecode);
    auto existing = scripts.find(name);
    if (existing != scripts.end() && existing->second.parsed == parsed &&
        existing->second.source == std::string_view(source, length)) {
        return;
    }
    Script script{ std::to_string(++next_script), source_url(name), std::string(source, length), module, parsed, {} };
    JS_ForEachDebugLocation(
        debuggee,
        bytecode,
        [](int line, int column, void *opaque) {
            static_cast<std::set<Position> *>(opaque)->emplace(line - 1, std::max(0, column - 1));
        },
        &script.locations);
    auto &registered = scripts.insert_or_assign(name, std::move(script)).first->second;
    if (enabled) {
        announce(registered);
        for (const auto &[id, bp] : breakpoints) {
            const auto pos = bp.resolve(registered);
            if (pos != registered.locations.end()) {
                event("Debugger.breakpointResolved",
                      object({ { "breakpointId", str(id) },
                               { "location", location(registered, pos->first, pos->second) } }));
            }
        }
    }
}

void TJSInspector::clear_objects() {
    for (const auto &entry : objects) {
        JS_FreeValue(debuggee, entry.second);
    }
    objects.clear();
    scopes.clear();
    frame_levels.clear();
}

std::string TJSInspector::store_object(JSValueConst value) {
    if (objects.size() >= 4096) {
        return {};
    }
    auto id = "object:" + std::to_string(++next_object);
    objects.emplace(id, JS_DupValue(debuggee, value));
    return id;
}

Json TJSInspector::remote_value(JSValueConst value) {
    if (JS_IsNull(value)) {
        return object({ { "type", str("object") }, { "subtype", str("null") }, { "value", Json(protocol, JS_NULL) } });
    }
    if (JS_IsBool(value)) {
        return object({ { "type", str("boolean") }, { "value", boolean(JS_ToBool(debuggee, value)) } });
    }
    if (JS_IsNumber(value)) {
        double n;
        JS_ToFloat64(debuggee, &n, value);
        if (!std::isfinite(n) || (n == 0 && std::signbit(n))) {
            const char *text = std::isnan(n) ? "NaN" : n == 0 ? "-0" : n < 0 ? "-Infinity" : "Infinity";
            return object(
                { { "type", str("number") }, { "unserializableValue", str(text) }, { "description", str(text) } });
        }
        return object({ { "type", str("number") }, { "value", Json(protocol, JS_NewFloat64(protocol, n)) } });
    }
    if (JS_IsString(value)) {
        return object({ { "type", str("string") }, { "value", str(js_string(debuggee, value)) } });
    }
    if (JS_IsBigInt(value)) {
        auto text = js_string(debuggee, value) + "n";
        return object(
            { { "type", str("bigint") }, { "unserializableValue", str(text) }, { "description", str(text) } });
    }
    if (JS_IsObject(value)) {
        const bool fn = JS_IsFunction(debuggee, value);
        const bool arr = JS_IsArray(value);
        auto result = object({ { "type", str(fn ? "function" : "object") },
                               { "description",
                                 str(fn    ? "Function"
                                     : arr ? "Array"
                                           : "Object") } });
        if (arr) {
            result.set("subtype", str("array"));
        }
        auto id = store_object(value);
        if (!id.empty()) {
            result.set("objectId", str(id));
        }
        return result;
    }
    if (JS_IsSymbol(value)) {
        return object({ { "type", str("symbol") }, { "description", str("Symbol") } });
    }
    return object({ { "type", str("undefined") } });
}

Json TJSInspector::scope(int level, bool closure) {
    const auto id =
        "scope:" + std::to_string(pause_id) + ":" + std::to_string(level) + (closure ? ":closure" : ":local");
    scopes.emplace(id, std::make_pair(level, closure));
    return object({ { "type", str(closure ? "closure" : "local") },
                    { "object", object({ { "type", str("object") }, { "objectId", str(id) } }) } });
}

Json TJSInspector::frames() {
    std::vector<Json> result;
    for (int level = 0, depth = JS_GetStackDepth(debuggee); level < depth; level++) {
        JSAtom filename, function;
        int line, column;
        if (!JS_GetDebugFrame(debuggee, level, &filename, &function, &line, &column)) {
            continue;
        }
        auto script = scripts.find(atom_string(debuggee, filename));
        if (script == scripts.end()) {
            continue;
        }
        const auto id = std::to_string(pause_id) + ":" + std::to_string(level);
        frame_levels.emplace(id, level);
        result.push_back(object({
            { "callFrameId", str(id) },
            { "functionName", str(atom_string(debuggee, function)) },
            { "location", location(script->second, std::max(0, line - 1), std::max(0, column - 1)) },
            { "url", str(script->second.url) },
            { "scopeChain", array({ scope(level, false), scope(level, true) }) },
            { "this", object({ { "type", str("undefined") } }) },
        }));
    }
    return array(result);
}

Json TJSInspector::properties(const std::string &id, bool &found) {
    std::vector<Json> result;
    auto scope_info = scopes.find(id);
    auto object_info = objects.find(id);
    found = scope_info != scopes.end() || object_info != objects.end();
    if (scope_info != scopes.end()) {
        const auto [level, closure] = scope_info->second;
        JSDebugLocalVar *vars = nullptr;
        int count = 0;
        if (JS_GetLocalVariablesAtLevel(debuggee, level, &vars, &count) == 0) {
            for (int i = 0; i < count; i++) {
                if (vars[i].is_closure == closure) {
                    result.push_back(object({ { "name", str(vars[i].name) },
                                              { "value", remote_value(vars[i].value) },
                                              { "isOwn", boolean(true) },
                                              { "enumerable", boolean(true) } }));
                }
            }
            JS_FreeLocalVariables(debuggee, vars, count);
        }
    } else if (object_info != objects.end() && !JS_IsProxy(object_info->second)) {
        JSValueConst value = object_info->second;
        JSPropertyEnum *props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(debuggee, &props, &count, value, JS_GPN_STRING_MASK) == 0) {
            for (uint32_t i = 0; i < std::min(count, uint32_t{ 1000 }); i++) {
                JSPropertyDescriptor desc;
                if (JS_GetOwnProperty(debuggee, &desc, value, props[i].atom) != 1) {
                    continue;
                }
                auto prop = object({ { "name", str(atom_string(debuggee, props[i].atom)) },
                                     { "isOwn", boolean(true) },
                                     { "enumerable", boolean(desc.flags & JS_PROP_ENUMERABLE) },
                                     { "configurable", boolean(desc.flags & JS_PROP_CONFIGURABLE) } });
                if ((desc.flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
                    prop.set("get", remote_value(desc.getter));
                    prop.set("set", remote_value(desc.setter));
                } else {
                    prop.set("value", remote_value(desc.value));
                    prop.set("writable", boolean(desc.flags & JS_PROP_WRITABLE));
                }
                result.push_back(prop);
                JS_FreeValue(debuggee, desc.value);
                JS_FreeValue(debuggee, desc.getter);
                JS_FreeValue(debuggee, desc.setter);
            }
            JS_FreePropertyEnum(debuggee, props, count);
        }
    }
    if (JS_HasException(debuggee)) {
        JS_FreeValue(debuggee, JS_GetException(debuggee));
    }
    return object({ { "result", array(result) } });
}

int TJSInspector::frame_level(const std::string &id) const {
    auto it = frame_levels.find(id);
    return paused && it != frame_levels.end() ? it->second : -1;
}

Json TJSInspector::evaluation(const Json &params, int level) {
    const auto expression = params.get("expression").str();
    evaluating = true;
    Json value(debuggee,
               level < 0 ? JS_Eval(debuggee, expression.c_str(), expression.size(), "<inspector>", JS_EVAL_TYPE_GLOBAL)
                         : JS_EvalInStackFrame(debuggee, level, expression.c_str(), expression.size(), "<inspector>"));
    if (JS_IsException(value.value)) {
        Json exception(debuggee, JS_GetException(debuggee));
        auto remote = remote_value(exception.value);
        evaluating = false;
        return object({ { "result", remote },
                        { "exceptionDetails",
                          object({ { "text", str("Uncaught") },
                                   { "lineNumber", number(0) },
                                   { "columnNumber", number(0) },
                                   { "exceptionId", number(1) },
                                   { "exception", remote } }) } });
    }
    auto result = remote_value(value.value);
    if (JS_HasException(debuggee)) {
        JS_FreeValue(debuggee, JS_GetException(debuggee));
    }
    evaluating = false;
    return object({ { "result", result } });
}

void TJSInspector::trace(JSAtom filename, int line, int column, int flags) {
    if (evaluating || JS_HasException(debuggee)) {
        return;
    }
    const uint64_t now = uv_hrtime();
    if (now >= next_poll) {
        next_poll = now + poll_interval_ms * 1000000;
        service(false);
    }
    if (!enabled || !client || line <= 0) {
        return;
    }
    const auto file = atom_string(debuggee, filename);
    const auto it = scripts.find(file);
    if (it == scripts.end()) {
        return;
    }

    const int depth = JS_GetStackDepth(debuggee);
    const bool moved = file != pause_file || line != pause_line || column != pause_column || depth != pause_depth;
    if (moved) {
        skip_breakpoint = false;
    }
    const char *reason = nullptr;
    std::vector<Json> hits;
    if (breakpoints_active && !skip_breakpoint) {
        for (const auto &[id, bp] : breakpoints) {
            auto pos = bp.resolve(it->second);
            if (pos != it->second.locations.end() && *pos == Position{ line - 1, std::max(0, column - 1) }) {
                hits.push_back(str(id));
            }
        }
    }
    if (break_on_entry) {
        reason = "Break on start";
        break_on_entry = false;
    } else if (pause_requested) {
        reason = "other";
    } else if (flags & JS_DEBUG_TRACE_DEBUGGER_STMT) {
        reason = "debugCommand";
    } else if (!hits.empty()) {
        reason = "other";
    } else if (step == Step::into && moved) {
        reason = "step";
    } else if (step == Step::over && moved && depth <= pause_depth) {
        reason = "step";
    } else if (step == Step::out && depth < pause_depth) {
        reason = "step";
    }
    if (!reason) {
        return;
    }

    step = Step::none;
    pause_requested = false;
    skip_breakpoint = true;
    pause_depth = depth;
    pause_line = line;
    pause_column = column;
    pause_file = file;
    ++pause_id;
    clear_objects();
    paused = true;
    event("Debugger.paused",
          object({ { "reason", str(reason) }, { "callFrames", frames() }, { "hitBreakpoints", array(hits) } }));
    while (paused && client && service(true)) {
    }
    clear_objects();
    if (client) {
        service(false);
    }
}

void TJSInspector::handle_message(const std::string &message) {
    const Json request(protocol, JS_ParseJSON(protocol, message.c_str(), message.size(), "<cdp>"));
    if (JS_IsException(request.value)) {
        JS_FreeValue(protocol, JS_GetException(protocol));
        return;
    }
    const auto method = request.get("method").str();
    const int id = request.get("id").integer();
    if (id < 0 || method.empty()) {
        return;
    }

    const auto params = request.get("params");
    const auto reply = [&](const Json &result) { send(object({ { "id", number(id) }, { "result", result } })); };
    const auto error = [&](int code, const char *message) {
        send(object(
            { { "id", number(id) }, { "error", object({ { "code", number(code) }, { "message", str(message) } }) } }));
    };
    if (method == "Runtime.enable") {
        reply(object());
        event("Runtime.executionContextCreated",
              object({ { "context",
                         object({ { "id", number(1) },
                                  { "origin", str("") },
                                  { "name", str("txiki.js") },
                                  { "auxData", object({ { "isDefault", boolean(true) } }) } }) } }));
    } else if (method == "Debugger.enable") {
        enabled = true;
        reply(object({ { "debuggerId", str("txiki-inspector") } }));
        for (const auto &entry : scripts) {
            announce(entry.second);
        }
    } else if (method == "Runtime.runIfWaitingForDebugger") {
        waiting = false;
        reply(object());
    } else if (method == "Debugger.disable") {
        enabled = false;
        paused = false;
        step = Step::none;
        breakpoints.clear();
        reply(object());
    } else if (method == "Debugger.resume" || method == "Debugger.stepInto" || method == "Debugger.stepOver" ||
               method == "Debugger.stepOut") {
        if (!paused) {
            return error(-32000, "The debugger is not paused");
        }
        step = method == "Debugger.stepInto"   ? Step::into
               : method == "Debugger.stepOver" ? Step::over
               : method == "Debugger.stepOut"  ? Step::out
                                               : Step::none;
        paused = false;
        reply(object());
        event("Debugger.resumed", object());
    } else if (method == "Debugger.pause") {
        pause_requested = !paused;
        reply(object());
    } else if (method == "Debugger.setBreakpointByUrl" || method == "Debugger.setBreakpoint") {
        const bool by_url = method == "Debugger.setBreakpointByUrl";
        const auto point = by_url ? params : params.get("location");
        const int line = point.get("lineNumber").integer();
        const int column = point.get("columnNumber").integer(0);
        auto url = params.get("url").str();
        auto regex = params.get("urlRegex").str();
        const auto *script = by_url ? nullptr : script_by_id(point.get("scriptId").str());
        if (!by_url && script) {
            url = script->url;
        }
        if (line < 0 || (url.empty() == regex.empty()) || regex.size() > 1024) {
            return error(-32602, "Expected a location, or a line number and a URL or URL regex");
        }
        Breakpoint bp{ script_url(std::move(url)), {}, line, column, !regex.empty() };
        if (bp.regex) {
            try {
                bp.pattern = std::regex(regex, std::regex::ECMAScript);
            } catch (const std::regex_error &) {
                return error(-32602, "Invalid breakpoint URL regex");
            }
        }
        std::vector<Json> locations;
        for (const auto &entry : scripts) {
            const auto pos = bp.resolve(entry.second);
            if (pos != entry.second.locations.end()) {
                locations.push_back(location(entry.second, pos->first, pos->second));
            }
        }
        if (!by_url && locations.empty()) {
            return error(-32000, "No breakpoint location on this line");
        }
        const auto bp_id = std::to_string(++next_breakpoint);
        breakpoints.emplace(bp_id, std::move(bp));
        if (by_url) {
            reply(object({ { "breakpointId", str(bp_id) }, { "locations", array(locations) } }));
        } else {
            reply(object({ { "breakpointId", str(bp_id) }, { "actualLocation", locations.front() } }));
        }
    } else if (method == "Debugger.getPossibleBreakpoints") {
        const auto start = params.get("start");
        const auto end = params.get("end");
        const auto *script = script_by_id(start.get("scriptId").str());
        const int first_line = start.get("lineNumber").integer();
        if (!script || first_line < 0) {
            return error(-32602, "Expected a start location in a known script");
        }
        const Position first{ first_line, start.get("columnNumber").integer(0) };
        const Position last{ end.get("lineNumber").integer(INT32_MAX), end.get("columnNumber").integer(0) };
        std::vector<Json> locations;
        for (auto pos = script->locations.lower_bound(first); pos != script->locations.end() && *pos < last; ++pos) {
            locations.push_back(location(*script, pos->first, pos->second));
        }
        reply(object({ { "locations", array(locations) } }));
    } else if (method == "Debugger.removeBreakpoint") {
        breakpoints.erase(params.get("breakpointId").str());
        reply(object());
    } else if (method == "Debugger.setBreakpointsActive") {
        breakpoints_active = params.get("active").boolean();
        reply(object());
    } else if (method == "Debugger.getScriptSource") {
        const auto *script = script_by_id(params.get("scriptId").str());
        if (!script) {
            return error(-32000, "Unknown script");
        }
        reply(object({ { "scriptSource", str(script->source) } }));
    } else if (method == "Debugger.evaluateOnCallFrame" || method == "Runtime.evaluate") {
        const int level = method == "Runtime.evaluate" ? -1 : frame_level(params.get("callFrameId").str());
        if (method == "Debugger.evaluateOnCallFrame" && level < 0) {
            return error(-32000, "Unknown call frame");
        }
        if (JS_HasException(debuggee)) {
            return error(-32000, "The runtime is handling an exception");
        }
        reply(evaluation(params, level));
    } else if (method == "Runtime.getProperties") {
        if (JS_HasException(debuggee)) {
            return error(-32000, "The runtime is handling an exception");
        }
        bool found;
        auto result = properties(params.get("objectId").str(), found);
        if (!found) {
            return error(-32000, "Unknown object or scope");
        }
        reply(result);
    } else if (method == "Runtime.releaseObject") {
        auto it = objects.find(params.get("objectId").str());
        if (it != objects.end()) {
            JS_FreeValue(debuggee, it->second);
            objects.erase(it);
        }
        reply(object());
    } else if (method == "Debugger.setVariableValue") {
        const int level = frame_level(params.get("callFrameId").str());
        const int scope_number = params.get("scopeNumber").integer();
        const auto name = params.get("variableName").str();
        const auto arg = params.get("newValue");
        const auto value = arg.get("value");
        if (level < 0 || scope_number < 0 || scope_number > 1 || name.empty() || !JS_IsObject(arg.value) ||
            JS_HasException(debuggee)) {
            return error(-32602, "Expected a paused frame, scope and variable value");
        }
        const auto unserializable = arg.get("unserializableValue").str();
        const auto object_id = arg.get("objectId").str();
        JSValue replacement = JS_UNDEFINED;
        if (!object_id.empty()) {
            auto it = objects.find(object_id);
            if (it == objects.end()) {
                return error(-32000, "Unknown object");
            }
            replacement = JS_DupValue(debuggee, it->second);
        } else if (!unserializable.empty()) {
            if (unserializable == "NaN") {
                replacement = JS_NewFloat64(debuggee, NAN);
            } else if (unserializable == "Infinity" || unserializable == "-Infinity") {
                replacement = JS_NewFloat64(debuggee, unserializable.front() == '-' ? -INFINITY : INFINITY);
            } else if (unserializable == "-0") {
                replacement = JS_NewFloat64(debuggee, -0.0);
            } else {
                return error(-32602, "Unsupported unserializable value");
            }
        } else if (!JS_IsUndefined(value.value)) {
            const auto encoded = value.stringify();
            replacement = JS_ParseJSON(debuggee, encoded.c_str(), encoded.size(), "<cdp>");
        }
        if (JS_IsException(replacement)) {
            JS_FreeValue(debuggee, JS_GetException(debuggee));
            return error(-32602, "Invalid variable value");
        }
        if (JS_SetVariableAtLevel(debuggee, level, name.c_str(), replacement, scope_number == 1) != 0) {
            if (JS_HasException(debuggee)) {
                JS_FreeValue(debuggee, JS_GetException(debuggee));
            }
            return error(-32000, "Variable is missing, uninitialized or read-only");
        }
        reply(object());
    } else if (method == "Runtime.disable" || method == "Profiler.enable" || method == "Profiler.disable" ||
               (method == "Debugger.setPauseOnExceptions" && params.get("state").str() == "none")) {
        reply(object());
    } else {
        error(-32601, "Method not supported by txiki.js");
    }
}

TJSInspector *tjs__inspector_start(JSContext *ctx, uv_loop_t *loop, int port, bool wait) {
    if (port < 0 || port > 65535) {
        JS_ThrowRangeError(ctx, "Invalid inspector port");
        return nullptr;
    }

    auto inspector = std::make_unique<TJSInspector>(ctx, wait);
    if (!inspector->start(loop, port)) {
        JS_ThrowInternalError(ctx, "Could not start inspector on 127.0.0.1:%d", port);
        return nullptr;
    }

    while (inspector->waiting) {
        if (!inspector->service(true)) {
            JS_ThrowInternalError(ctx, "Inspector stopped while waiting for a debugger");
            return nullptr;
        }
    }
    inspector->service(false);

    JS_SetDebugSourceHandler(
        ctx,
        [](JSContext *,
           const char *name,
           const char *source,
           size_t length,
           bool module,
           JSValueConst bytecode,
           void *opaque) {
            static_cast<TJSInspector *>(opaque)->register_source(name, source, length, module, bytecode);
        },
        inspector.get());
    JS_SetDebugTraceHandler(
        ctx,
        [](JSContext *, JSAtom filename, JSAtom, int line, int column, int flags, void *opaque) {
            static_cast<TJSInspector *>(opaque)->trace(filename, line, column, flags);
            return 0;
        },
        inspector.get());
    return inspector.release();
}

void tjs__inspector_stop(TJSInspector *inspector) {
    delete inspector;
}
