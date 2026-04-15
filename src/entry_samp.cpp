/*
 * PawnREST - SA-MP Legacy Plugin Entry Point
 * Version: 1.3.0
 */

#include "plugin.h"
#include "amx/amx.h"
#include "amx/amx2.h"

#include <crc32.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <mutex>
#include <string>
#include <vector>

// SA-MP plugin interface
extern void* pAMXFunctions;
typedef void (*logprintf_t)(const char* format, ...);
logprintf_t logprintf;

static void PawnRestLogSink(const char* message) {
    if (logprintf && message) {
        logprintf("%s", message);
    }
}

// Shared entry include (used by both SA-MP and open.mp builds)
#include "pawnrest/entry_shared.inl"

// AMX list for callbacks
static std::vector<AMX*> g_AmxList;
static std::mutex g_AmxListMutex;
static std::chrono::steady_clock::time_point g_LastDrainTime;

static std::vector<AMX*> GetLoadedAMXSnapshot() {
    std::lock_guard<std::mutex> lock(g_AmxListMutex);
    return g_AmxList;
}

// AMX string helpers
static inline bool ReadAmxString(AMX* amx, cell arg, std::string& out) {
    out.clear();
    cell* phys = nullptr;
    if (amx_GetAddr(amx, arg, &phys) != AMX_ERR_NONE || !phys) return false;
    int length = 0;
    if (amx_StrLen(phys, &length) != AMX_ERR_NONE) return false;
    if (length <= 0) return true;

    out.resize(static_cast<size_t>(length) + 1);
    if (amx_GetString(out.data(), phys, 0, static_cast<size_t>(length + 1)) != AMX_ERR_NONE) return false;
    out.resize(static_cast<size_t>(length));
    return true;
}

static inline bool WriteAmxString(AMX* amx, cell addr, const std::string& str, int maxLen) {
    cell* dest = nullptr;
    if (amx_GetAddr(amx, addr, &dest) != AMX_ERR_NONE || !dest) return false;
    return amx_SetString(dest, str.c_str(), 0, 0, maxLen) == AMX_ERR_NONE;
}

// -----------------------------------------------------------------------------
// Variadic JSON builders (also referenced by component code)
// -----------------------------------------------------------------------------

cell AMX_NATIVE_CALL REST_JsonObjectVariadic(AMX* amx, const cell* params) {
    int objectId = ImplNodeObject();
    if (objectId < 0) return -1;
    if (!params) return objectId;

    int argc = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
    if (argc == 0) return objectId;
    if ((argc % 2) != 0) {
        ImplNodeRelease(objectId);
        return -1;
    }

    std::vector<int> consumed;
    consumed.reserve(static_cast<size_t>(argc / 2));

    for (int i = 1; i <= argc; i += 2) {
        std::string key;
        if (!ReadAmxString(amx, params[i], key) || key.empty()) {
            for (int nodeId : consumed) ImplNodeRelease(nodeId);
            ImplNodeRelease(objectId);
            return -1;
        }

        int valueNodeId = static_cast<int>(params[i + 1]);
        consumed.push_back(valueNodeId);
        if (!ImplNodeSet(objectId, key, valueNodeId)) {
            for (int nodeId : consumed) ImplNodeRelease(nodeId);
            ImplNodeRelease(objectId);
            return -1;
        }
    }

    for (int nodeId : consumed) ImplNodeRelease(nodeId);
    return objectId;
}

cell AMX_NATIVE_CALL REST_JsonArrayVariadic(AMX* amx, const cell* params) {
    (void)amx;

    int arrayId = ImplNodeArray();
    if (arrayId < 0) return -1;
    if (!params) return arrayId;

    int argc = static_cast<int>(params[0] / static_cast<cell>(sizeof(cell)));
    if (argc == 0) return arrayId;

    std::vector<int> consumed;
    consumed.reserve(static_cast<size_t>(argc));

    for (int i = 1; i <= argc; ++i) {
        int valueNodeId = static_cast<int>(params[i]);
        consumed.push_back(valueNodeId);
        if (!ImplNodeArrayPush(arrayId, valueNodeId)) {
            for (int nodeId : consumed) ImplNodeRelease(nodeId);
            ImplNodeRelease(arrayId);
            return -1;
        }
    }

    for (int nodeId : consumed) ImplNodeRelease(nodeId);
    return arrayId;
}

// -----------------------------------------------------------------------------
// Natives
// -----------------------------------------------------------------------------

// Server control
static cell AMX_NATIVE_CALL n_REST_Start(AMX* amx, cell* params) {
    (void)amx;
    return ImplStart(static_cast<int>(params[1])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_StartTLS(AMX* amx, cell* params) {
    std::string certPath, keyPath;
    ReadAmxString(amx, params[2], certPath);
    ReadAmxString(amx, params[3], keyPath);
    return ImplStartTLS(static_cast<int>(params[1]), certPath, keyPath) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_Stop(AMX* amx, cell* params) {
    (void)amx;
    (void)params;
    return ImplStop() ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_IsRunning(AMX* amx, cell* params) {
    (void)amx;
    (void)params;
    return ImplIsRunning() ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetPort(AMX* amx, cell* params) {
    (void)amx;
    (void)params;
    return static_cast<cell>(ImplGetPort());
}

static cell AMX_NATIVE_CALL n_REST_IsTLSEnabled(AMX* amx, cell* params) {
    (void)amx;
    (void)params;
    return ImplIsTLSEnabled() ? 1 : 0;
}

// Incoming file routes
static cell AMX_NATIVE_CALL n_REST_RegisterRoute(AMX* amx, cell* params) {
    std::string endpoint, path, allowedExts;
    ReadAmxString(amx, params[1], endpoint);
    ReadAmxString(amx, params[2], path);
    ReadAmxString(amx, params[3], allowedExts);
    return static_cast<cell>(ImplRegisterUploadRoute(endpoint, path, allowedExts, static_cast<int>(params[4])));
}

static cell AMX_NATIVE_CALL n_REST_AddKey(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplAddRouteKey(static_cast<int>(params[1]), key) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_RemoveKey(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplRemoveRouteKey(static_cast<int>(params[1]), key) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_SetConflict(AMX* amx, cell* params) {
    (void)amx;
    return ImplSetConflictMode(static_cast<int>(params[1]), static_cast<int>(params[2])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_SetCorruptAction(AMX* amx, cell* params) {
    (void)amx;
    return ImplSetCorruptAction(static_cast<int>(params[1]), static_cast<int>(params[2])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_SetRequireCRC32(AMX* amx, cell* params) {
    (void)amx;
    return ImplSetRequireCRC32(static_cast<int>(params[1]), params[2] != 0) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_RemoveRoute(AMX* amx, cell* params) {
    (void)amx;
    return ImplRemoveUploadRoute(static_cast<int>(params[1])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_AllowList(AMX* amx, cell* params) {
    (void)amx;
    return ImplSetAllowList(static_cast<int>(params[1]), params[2] != 0) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_AllowDownload(AMX* amx, cell* params) {
    (void)amx;
    return ImplSetAllowDownload(static_cast<int>(params[1]), params[2] != 0) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_AllowDelete(AMX* amx, cell* params) {
    (void)amx;
    return ImplSetAllowDelete(static_cast<int>(params[1]), params[2] != 0) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_AllowInfo(AMX* amx, cell* params) {
    (void)amx;
    return ImplSetAllowInfo(static_cast<int>(params[1]), params[2] != 0) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetFileCount(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetFileCount(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_GetFileName(AMX* amx, cell* params) {
    std::string name = ImplGetFileName(static_cast<int>(params[1]), static_cast<int>(params[2]));
    return WriteAmxString(amx, params[3], name, static_cast<int>(params[4])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_DeleteFile(AMX* amx, cell* params) {
    std::string filename;
    ReadAmxString(amx, params[2], filename);
    return ImplDeleteFile(static_cast<int>(params[1]), filename) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetFileSize(AMX* amx, cell* params) {
    std::string filename;
    ReadAmxString(amx, params[2], filename);
    return static_cast<cell>(ImplGetFileSize(static_cast<int>(params[1]), filename));
}

// API routes
static cell AMX_NATIVE_CALL n_REST_Route(AMX* amx, cell* params) {
    int method = static_cast<int>(params[1]);
    std::string endpoint, callback;
    ReadAmxString(amx, params[2], endpoint);
    ReadAmxString(amx, params[3], callback);
    return static_cast<cell>(ImplRegisterRoute(method, endpoint, callback));
}

static cell AMX_NATIVE_CALL n_REST_RemoveAPIRoute(AMX* amx, cell* params) {
    (void)amx;
    return ImplRemoveApiRoute(static_cast<int>(params[1])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_SetRouteAuthKey(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplSetApiRouteAuth(static_cast<int>(params[1]), key) ? 1 : 0;
}

// Request data
static cell AMX_NATIVE_CALL n_REST_GetRequestIP(AMX* amx, cell* params) {
    std::string ip = ImplGetRequestIP(static_cast<int>(params[1]));
    return WriteAmxString(amx, params[2], ip, static_cast<int>(params[3])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetRequestMethod(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetRequestMethod(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_GetRequestPath(AMX* amx, cell* params) {
    std::string path = ImplGetRequestPath(static_cast<int>(params[1]));
    return WriteAmxString(amx, params[2], path, static_cast<int>(params[3])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetRequestBody(AMX* amx, cell* params) {
    std::string body = ImplGetRequestBody(static_cast<int>(params[1]));
    return WriteAmxString(amx, params[2], body, static_cast<int>(params[3])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetRequestBodyLength(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetRequestBodyLength(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_GetParam(AMX* amx, cell* params) {
    std::string name;
    ReadAmxString(amx, params[2], name);
    std::string value = ImplGetParam(static_cast<int>(params[1]), name);
    return WriteAmxString(amx, params[3], value, static_cast<int>(params[4])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetParamInt(AMX* amx, cell* params) {
    std::string name;
    ReadAmxString(amx, params[2], name);
    return static_cast<cell>(ImplGetParamInt(static_cast<int>(params[1]), name));
}

static cell AMX_NATIVE_CALL n_REST_GetQuery(AMX* amx, cell* params) {
    std::string name;
    ReadAmxString(amx, params[2], name);
    std::string value = ImplGetQuery(static_cast<int>(params[1]), name);
    return WriteAmxString(amx, params[3], value, static_cast<int>(params[4])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetQueryInt(AMX* amx, cell* params) {
    std::string name;
    ReadAmxString(amx, params[2], name);
    return static_cast<cell>(ImplGetQueryInt(static_cast<int>(params[1]), name, static_cast<int>(params[3])));
}

static cell AMX_NATIVE_CALL n_REST_GetHeader(AMX* amx, cell* params) {
    std::string name;
    ReadAmxString(amx, params[2], name);
    std::string value = ImplGetHeader(static_cast<int>(params[1]), name);
    return WriteAmxString(amx, params[3], value, static_cast<int>(params[4])) ? 1 : 0;
}

// Response
static cell AMX_NATIVE_CALL n_REST_Respond(AMX* amx, cell* params) {
    std::string body, contentType;
    ReadAmxString(amx, params[3], body);
    ReadAmxString(amx, params[4], contentType);
    return ImplRespond(static_cast<int>(params[1]), static_cast<int>(params[2]), body, contentType) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_RespondJSON(AMX* amx, cell* params) {
    std::string json;
    ReadAmxString(amx, params[3], json);
    return ImplRespondJSON(static_cast<int>(params[1]), static_cast<int>(params[2]), json) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_RespondNode(AMX* amx, cell* params) {
    (void)amx;
    return ImplRespondNode(static_cast<int>(params[1]), static_cast<int>(params[2]), static_cast<int>(params[3])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_RespondError(AMX* amx, cell* params) {
    std::string message;
    ReadAmxString(amx, params[3], message);
    return ImplRespondError(static_cast<int>(params[1]), static_cast<int>(params[2]), message) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_SetResponseHeader(AMX* amx, cell* params) {
    std::string name, value;
    ReadAmxString(amx, params[2], name);
    ReadAmxString(amx, params[3], value);
    return ImplSetResponseHeader(static_cast<int>(params[1]), name, value) ? 1 : 0;
}

// JSON nodes
static cell AMX_NATIVE_CALL n_REST_JsonParseNode(AMX* amx, cell* params) {
    std::string json;
    ReadAmxString(amx, params[1], json);
    return static_cast<cell>(ImplJsonParse(json));
}

static cell AMX_NATIVE_CALL n_REST_RequestJsonNode(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplRequestJsonNode(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_NodeType(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplNodeType(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_NodeStringify(AMX* amx, cell* params) {
    std::string json = ImplNodeStringify(static_cast<int>(params[1]));
    return WriteAmxString(amx, params[2], json, static_cast<int>(params[3])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeRelease(AMX* amx, cell* params) {
    (void)amx;
    return ImplNodeRelease(static_cast<int>(params[1])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_JsonObject(AMX* amx, cell* params) {
    return REST_JsonObjectVariadic(amx, params);
}

static cell AMX_NATIVE_CALL n_REST_JsonArray(AMX* amx, cell* params) {
    return REST_JsonArrayVariadic(amx, params);
}

// Backward-compatible non-variadic node constructors
static cell AMX_NATIVE_CALL n_REST_NodeObject(AMX* amx, cell* params) {
    (void)amx;
    (void)params;
    return static_cast<cell>(ImplNodeObject());
}

static cell AMX_NATIVE_CALL n_REST_NodeArray(AMX* amx, cell* params) {
    (void)amx;
    (void)params;
    return static_cast<cell>(ImplNodeArray());
}

static cell AMX_NATIVE_CALL n_REST_NodeString(AMX* amx, cell* params) {
    std::string value;
    ReadAmxString(amx, params[1], value);
    return static_cast<cell>(ImplNodeString(value));
}

static cell AMX_NATIVE_CALL n_REST_NodeInt(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplNodeInt(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_NodeFloat(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplNodeFloat(amx_ctof(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_NodeBool(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplNodeBool(params[1] != 0));
}

static cell AMX_NATIVE_CALL n_REST_NodeNull(AMX* amx, cell* params) {
    (void)amx;
    (void)params;
    return static_cast<cell>(ImplNodeNull());
}

static cell AMX_NATIVE_CALL n_REST_JsonAppend(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplJsonAppend(static_cast<int>(params[1]), static_cast<int>(params[2])));
}

static cell AMX_NATIVE_CALL n_REST_NodeSet(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplNodeSet(static_cast<int>(params[1]), key, static_cast<int>(params[3])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeSetString(AMX* amx, cell* params) {
    std::string key, value;
    ReadAmxString(amx, params[2], key);
    ReadAmxString(amx, params[3], value);
    return ImplNodeSetString(static_cast<int>(params[1]), key, value) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeSetInt(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplNodeSetInt(static_cast<int>(params[1]), key, static_cast<int>(params[3])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeSetFloat(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplNodeSetFloat(static_cast<int>(params[1]), key, amx_ctof(params[3])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeSetBool(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplNodeSetBool(static_cast<int>(params[1]), key, params[3] != 0) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeSetNull(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplNodeSetNull(static_cast<int>(params[1]), key) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeHas(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplNodeHas(static_cast<int>(params[1]), key) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeGet(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return static_cast<cell>(ImplNodeGet(static_cast<int>(params[1]), key));
}

static cell AMX_NATIVE_CALL n_REST_NodeGetString(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    std::string value = ImplNodeGetString(static_cast<int>(params[1]), key);
    return WriteAmxString(amx, params[3], value, static_cast<int>(params[4])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeGetInt(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return static_cast<cell>(ImplNodeGetInt(static_cast<int>(params[1]), key, static_cast<int>(params[3])));
}

static cell AMX_NATIVE_CALL n_REST_NodeGetFloat(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    float value = ImplNodeGetFloat(static_cast<int>(params[1]), key, amx_ctof(params[3]));
    return amx_ftoc(value);
}

static cell AMX_NATIVE_CALL n_REST_NodeGetBool(AMX* amx, cell* params) {
    std::string key;
    ReadAmxString(amx, params[2], key);
    return ImplNodeGetBool(static_cast<int>(params[1]), key, params[3] != 0) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeArrayLength(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplNodeArrayLength(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_NodeArrayGet(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplNodeArrayGet(static_cast<int>(params[1]), static_cast<int>(params[2])));
}

static cell AMX_NATIVE_CALL n_REST_NodeArrayPush(AMX* amx, cell* params) {
    (void)amx;
    return ImplNodeArrayPush(static_cast<int>(params[1]), static_cast<int>(params[2])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeArrayPushString(AMX* amx, cell* params) {
    std::string value;
    ReadAmxString(amx, params[2], value);
    return ImplNodeArrayPushString(static_cast<int>(params[1]), value) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeArrayPushInt(AMX* amx, cell* params) {
    (void)amx;
    return ImplNodeArrayPushInt(static_cast<int>(params[1]), static_cast<int>(params[2])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeArrayPushFloat(AMX* amx, cell* params) {
    (void)amx;
    return ImplNodeArrayPushFloat(static_cast<int>(params[1]), amx_ctof(params[2])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeArrayPushBool(AMX* amx, cell* params) {
    (void)amx;
    return ImplNodeArrayPushBool(static_cast<int>(params[1]), params[2] != 0) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_NodeArrayPushNull(AMX* amx, cell* params) {
    (void)amx;
    return ImplNodeArrayPushNull(static_cast<int>(params[1])) ? 1 : 0;
}

// Outgoing upload
static cell AMX_NATIVE_CALL n_REST_UploadFile(AMX* amx, cell* params) {
    std::string url, filepath, filename, authKey, customHeaders;
    ReadAmxString(amx, params[1], url);
    ReadAmxString(amx, params[2], filepath);
    ReadAmxString(amx, params[3], filename);
    ReadAmxString(amx, params[4], authKey);
    ReadAmxString(amx, params[5], customHeaders);
    return static_cast<cell>(ImplUploadFile(
        url,
        filepath,
        filename,
        authKey,
        customHeaders,
        params[6] != 0,
        static_cast<int>(params[7]),
        params[8] != 0));
}

static cell AMX_NATIVE_CALL n_REST_CreateUploadClient(AMX* amx, cell* params) {
    std::string baseUrl, defaultHeaders;
    ReadAmxString(amx, params[1], baseUrl);
    ReadAmxString(amx, params[2], defaultHeaders);
    return static_cast<cell>(ImplCreateUploadClient(baseUrl, defaultHeaders, params[3] != 0));
}

static cell AMX_NATIVE_CALL n_REST_RemoveUploadClient(AMX* amx, cell* params) {
    (void)amx;
    return ImplRemoveUploadClient(static_cast<int>(params[1])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_SetUploadClientHeader(AMX* amx, cell* params) {
    std::string name, value;
    ReadAmxString(amx, params[2], name);
    ReadAmxString(amx, params[3], value);
    return ImplSetUploadClientHeader(static_cast<int>(params[1]), name, value) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_RemoveUploadClientHeader(AMX* amx, cell* params) {
    std::string name;
    ReadAmxString(amx, params[2], name);
    return ImplRemoveUploadClientHeader(static_cast<int>(params[1]), name) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_UploadFileWithClient(AMX* amx, cell* params) {
    std::string path, filepath, filename, authKey, customHeaders;
    ReadAmxString(amx, params[2], path);
    ReadAmxString(amx, params[3], filepath);
    ReadAmxString(amx, params[4], filename);
    ReadAmxString(amx, params[5], authKey);
    ReadAmxString(amx, params[6], customHeaders);
    return static_cast<cell>(ImplUploadFileWithClient(
        static_cast<int>(params[1]),
        path,
        filepath,
        filename,
        authKey,
        customHeaders,
        params[7] != 0,
        static_cast<int>(params[8])));
}

static cell AMX_NATIVE_CALL n_REST_CancelUpload(AMX* amx, cell* params) {
    (void)amx;
    return ImplCancelUpload(static_cast<int>(params[1])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetUploadStatus(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetUploadStatus(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_GetUploadProgress(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetUploadProgress(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_GetUploadResponse(AMX* amx, cell* params) {
    std::string value;
    bool ok = ImplGetUploadResponse(static_cast<int>(params[1]), value, static_cast<int>(params[3]));
    if (!WriteAmxString(amx, params[2], value, static_cast<int>(params[3]))) return 0;
    return ok ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetUploadErrorCode(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetUploadErrorCode(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_GetUploadErrorType(AMX* amx, cell* params) {
    std::string value;
    bool ok = ImplGetUploadErrorType(static_cast<int>(params[1]), value, static_cast<int>(params[3]));
    if (!WriteAmxString(amx, params[2], value, static_cast<int>(params[3]))) return 0;
    return ok ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetUploadHttpStatus(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetUploadHttpStatus(static_cast<int>(params[1])));
}

// Outgoing requests
static cell AMX_NATIVE_CALL n_REST_RequestsClient(AMX* amx, cell* params) {
    std::string endpoint, defaultHeaders;
    ReadAmxString(amx, params[1], endpoint);
    ReadAmxString(amx, params[2], defaultHeaders);
    return static_cast<cell>(ImplCreateRequestClient(endpoint, defaultHeaders, params[3] != 0));
}

static cell AMX_NATIVE_CALL n_REST_RemoveRequestsClient(AMX* amx, cell* params) {
    (void)amx;
    return ImplRemoveRequestClient(static_cast<int>(params[1])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_SetRequestsClientHeader(AMX* amx, cell* params) {
    std::string name, value;
    ReadAmxString(amx, params[2], name);
    ReadAmxString(amx, params[3], value);
    return ImplSetRequestClientHeader(static_cast<int>(params[1]), name, value) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_RemoveRequestsClientHeader(AMX* amx, cell* params) {
    std::string name;
    ReadAmxString(amx, params[2], name);
    return ImplRemoveRequestClientHeader(static_cast<int>(params[1]), name) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_Request(AMX* amx, cell* params) {
    std::string path, callback, body, headers;
    ReadAmxString(amx, params[2], path);
    ReadAmxString(amx, params[4], callback);
    ReadAmxString(amx, params[5], body);
    ReadAmxString(amx, params[6], headers);
    return static_cast<cell>(ImplRequest(
        static_cast<int>(params[1]),
        path,
        static_cast<int>(params[3]),
        callback,
        body,
        headers));
}

static cell AMX_NATIVE_CALL n_REST_RequestJSON(AMX* amx, cell* params) {
    std::string path, callback, headers;
    ReadAmxString(amx, params[2], path);
    ReadAmxString(amx, params[4], callback);
    ReadAmxString(amx, params[6], headers);
    return static_cast<cell>(ImplRequestJSON(
        static_cast<int>(params[1]),
        path,
        static_cast<int>(params[3]),
        callback,
        static_cast<int>(params[5]),
        headers));
}

static cell AMX_NATIVE_CALL n_REST_CancelRequest(AMX* amx, cell* params) {
    (void)amx;
    return ImplCancelRequest(static_cast<int>(params[1])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetRequestStatus(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetRequestStatus(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_GetRequestHttpStatus(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetRequestHttpStatus(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_GetRequestErrorCode(AMX* amx, cell* params) {
    (void)amx;
    return static_cast<cell>(ImplGetRequestErrorCode(static_cast<int>(params[1])));
}

static cell AMX_NATIVE_CALL n_REST_GetRequestErrorType(AMX* amx, cell* params) {
    std::string value;
    bool ok = ImplGetRequestErrorType(static_cast<int>(params[1]), value, static_cast<int>(params[3]));
    if (!WriteAmxString(amx, params[2], value, static_cast<int>(params[3]))) return 0;
    return ok ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_GetRequestResponse(AMX* amx, cell* params) {
    std::string value;
    bool ok = ImplGetRequestResponse(static_cast<int>(params[1]), value, static_cast<int>(params[3]));
    if (!WriteAmxString(amx, params[2], value, static_cast<int>(params[3]))) return 0;
    return ok ? 1 : 0;
}

// WebSocket
static cell AMX_NATIVE_CALL n_REST_WebSocketClient(AMX* amx, cell* params) {
    std::string address, callback, headers;
    ReadAmxString(amx, params[1], address);
    ReadAmxString(amx, params[2], callback);
    ReadAmxString(amx, params[3], headers);
    return static_cast<cell>(ImplWebSocketClient(address, callback, headers, params[4] != 0));
}

static cell AMX_NATIVE_CALL n_REST_JsonWebSocketClient(AMX* amx, cell* params) {
    std::string address, callback, headers;
    ReadAmxString(amx, params[1], address);
    ReadAmxString(amx, params[2], callback);
    ReadAmxString(amx, params[3], headers);
    return static_cast<cell>(ImplJsonWebSocketClient(address, callback, headers, params[4] != 0));
}

static cell AMX_NATIVE_CALL n_REST_WebSocketSend(AMX* amx, cell* params) {
    std::string data;
    ReadAmxString(amx, params[2], data);
    return ImplWebSocketSend(static_cast<int>(params[1]), data) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_JsonWebSocketSend(AMX* amx, cell* params) {
    (void)amx;
    return ImplJsonWebSocketSend(static_cast<int>(params[1]), static_cast<int>(params[2])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_WebSocketClose(AMX* amx, cell* params) {
    std::string reason;
    ReadAmxString(amx, params[3], reason);
    return ImplWebSocketClose(static_cast<int>(params[1]), static_cast<int>(params[2]), reason) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_RemoveWebSocketClient(AMX* amx, cell* params) {
    (void)amx;
    return ImplRemoveWebSocketClient(static_cast<int>(params[1])) ? 1 : 0;
}

static cell AMX_NATIVE_CALL n_REST_IsWebSocketOpen(AMX* amx, cell* params) {
    (void)amx;
    return ImplIsWebSocketOpen(static_cast<int>(params[1])) ? 1 : 0;
}

// CRC32 utilities
static cell AMX_NATIVE_CALL n_REST_VerifyCRC32(AMX* amx, cell* params) {
    std::string filepath, expectedCrc;
    ReadAmxString(amx, params[1], filepath);
    ReadAmxString(amx, params[2], expectedCrc);
    return static_cast<cell>(ImplVerifyCRC32(filepath, expectedCrc));
}

static cell AMX_NATIVE_CALL n_REST_GetFileCRC32(AMX* amx, cell* params) {
    std::string filepath, crc;
    ReadAmxString(amx, params[1], filepath);
    int ok = ImplGetFileCRC32(filepath, crc);
    if (!WriteAmxString(amx, params[2], crc, static_cast<int>(params[3]))) return 0;
    return static_cast<cell>(ok);
}

static cell AMX_NATIVE_CALL n_REST_CompareFiles(AMX* amx, cell* params) {
    std::string path1, path2;
    ReadAmxString(amx, params[1], path1);
    ReadAmxString(amx, params[2], path2);
    return static_cast<cell>(ImplCompareFiles(path1, path2));
}

// -----------------------------------------------------------------------------
// Natives table
// -----------------------------------------------------------------------------

#define NATIVE(name) { #name, n_##name }

static const AMX_NATIVE_INFO PluginNatives[] = {
    NATIVE(REST_Start),
    NATIVE(REST_StartTLS),
    NATIVE(REST_Stop),
    NATIVE(REST_IsRunning),
    NATIVE(REST_GetPort),
    NATIVE(REST_IsTLSEnabled),

    NATIVE(REST_RegisterRoute),
    NATIVE(REST_AddKey),
    NATIVE(REST_RemoveKey),
    NATIVE(REST_SetConflict),
    NATIVE(REST_SetCorruptAction),
    NATIVE(REST_SetRequireCRC32),
    NATIVE(REST_RemoveRoute),
    NATIVE(REST_AllowList),
    NATIVE(REST_AllowDownload),
    NATIVE(REST_AllowDelete),
    NATIVE(REST_AllowInfo),
    NATIVE(REST_GetFileCount),
    NATIVE(REST_GetFileName),
    NATIVE(REST_DeleteFile),
    NATIVE(REST_GetFileSize),

    NATIVE(REST_Route),
    NATIVE(REST_RemoveAPIRoute),
    NATIVE(REST_SetRouteAuthKey),

    NATIVE(REST_GetRequestIP),
    NATIVE(REST_GetRequestMethod),
    NATIVE(REST_GetRequestPath),
    NATIVE(REST_GetRequestBody),
    NATIVE(REST_GetRequestBodyLength),
    NATIVE(REST_GetParam),
    NATIVE(REST_GetParamInt),
    NATIVE(REST_GetQuery),
    NATIVE(REST_GetQueryInt),
    NATIVE(REST_GetHeader),

    NATIVE(REST_JsonParseNode),
    NATIVE(REST_RequestJsonNode),
    NATIVE(REST_NodeType),
    NATIVE(REST_NodeStringify),
    NATIVE(REST_NodeRelease),
    NATIVE(REST_JsonObject),
    NATIVE(REST_JsonArray),
    NATIVE(REST_NodeString),
    NATIVE(REST_NodeInt),
    NATIVE(REST_NodeFloat),
    NATIVE(REST_NodeBool),
    NATIVE(REST_NodeNull),
    NATIVE(REST_JsonAppend),
    NATIVE(REST_NodeSet),
    NATIVE(REST_NodeSetString),
    NATIVE(REST_NodeSetInt),
    NATIVE(REST_NodeSetFloat),
    NATIVE(REST_NodeSetBool),
    NATIVE(REST_NodeSetNull),
    NATIVE(REST_NodeHas),
    NATIVE(REST_NodeGet),
    NATIVE(REST_NodeGetString),
    NATIVE(REST_NodeGetInt),
    NATIVE(REST_NodeGetFloat),
    NATIVE(REST_NodeGetBool),
    NATIVE(REST_NodeArrayLength),
    NATIVE(REST_NodeArrayGet),
    NATIVE(REST_NodeArrayPush),
    NATIVE(REST_NodeArrayPushString),
    NATIVE(REST_NodeArrayPushInt),
    NATIVE(REST_NodeArrayPushFloat),
    NATIVE(REST_NodeArrayPushBool),
    NATIVE(REST_NodeArrayPushNull),

    NATIVE(REST_Respond),
    NATIVE(REST_RespondJSON),
    NATIVE(REST_RespondNode),
    NATIVE(REST_RespondError),
    NATIVE(REST_SetResponseHeader),

    NATIVE(REST_UploadFile),
    NATIVE(REST_CreateUploadClient),
    NATIVE(REST_RemoveUploadClient),
    NATIVE(REST_SetUploadClientHeader),
    NATIVE(REST_RemoveUploadClientHeader),
    NATIVE(REST_UploadFileWithClient),
    NATIVE(REST_CancelUpload),
    NATIVE(REST_GetUploadStatus),
    NATIVE(REST_GetUploadProgress),
    NATIVE(REST_GetUploadResponse),
    NATIVE(REST_GetUploadErrorCode),
    NATIVE(REST_GetUploadErrorType),
    NATIVE(REST_GetUploadHttpStatus),

    NATIVE(REST_RequestsClient),
    NATIVE(REST_RemoveRequestsClient),
    NATIVE(REST_SetRequestsClientHeader),
    NATIVE(REST_RemoveRequestsClientHeader),
    NATIVE(REST_Request),
    NATIVE(REST_RequestJSON),
    NATIVE(REST_CancelRequest),
    NATIVE(REST_GetRequestStatus),
    NATIVE(REST_GetRequestHttpStatus),
    NATIVE(REST_GetRequestErrorCode),
    NATIVE(REST_GetRequestErrorType),
    NATIVE(REST_GetRequestResponse),

    NATIVE(REST_WebSocketClient),
    NATIVE(REST_JsonWebSocketClient),
    NATIVE(REST_WebSocketSend),
    NATIVE(REST_JsonWebSocketSend),
    NATIVE(REST_WebSocketClose),
    NATIVE(REST_RemoveWebSocketClient),
    NATIVE(REST_IsWebSocketOpen),

    NATIVE(REST_VerifyCRC32),
    NATIVE(REST_GetFileCRC32),
    NATIVE(REST_CompareFiles),

    // Optional compatibility exports
    NATIVE(REST_NodeObject),
    NATIVE(REST_NodeArray),

    { nullptr, nullptr }
};

#undef NATIVE

// -----------------------------------------------------------------------------
// Event draining for SA-MP ProcessTick
// -----------------------------------------------------------------------------

static void DrainAllEvents() {
    auto amxList = GetLoadedAMXSnapshot();
    if (amxList.empty()) return;

    // API route callbacks: callback(requestId)
    auto apiEvents = ImplDrainApiEvents();
    for (const auto& evt : apiEvents) {
        for (AMX* amx : amxList) {
            int idx = -1;
            if (amx_FindPublic(amx, evt.callbackName.c_str(), &idx) == AMX_ERR_NONE) {
                amx_Push(amx, evt.requestId);
                amx_Exec(amx, nullptr, idx);
            }
        }
    }

    // Incoming upload callbacks
    auto incomingUploadEvents = ImplDrainUploadEvents();
    for (const auto& evt : incomingUploadEvents) {
        switch (evt.type) {
            case UploadEvent::Type::Completed:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, "OnIncomingUploadCompleted", &idx) == AMX_ERR_NONE) {
                        std::string crcHex = CRC32::toHex(evt.crc32Checksum);
                        cell strAddr[4];
                        amx_Push(amx, evt.crc32Matched ? 1 : 0);
                        amx_PushString(amx, &strAddr[3], nullptr, crcHex.c_str(), 0, 0);
                        amx_PushString(amx, &strAddr[2], nullptr, evt.filepath.c_str(), 0, 0);
                        amx_PushString(amx, &strAddr[1], nullptr, evt.filename.c_str(), 0, 0);
                        amx_PushString(amx, &strAddr[0], nullptr, evt.endpoint.c_str(), 0, 0);
                        amx_Push(amx, evt.routeId);
                        amx_Push(amx, evt.uploadId);
                        amx_Exec(amx, nullptr, idx);
                        for (int i = 0; i < 4; ++i) amx_Release(amx, strAddr[i]);
                    }
                }
                break;

            case UploadEvent::Type::Failed:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, "OnIncomingUploadFailed", &idx) == AMX_ERR_NONE) {
                        std::string crcHex = CRC32::toHex(evt.crc32Checksum);
                        cell strAddr[2];
                        amx_PushString(amx, &strAddr[1], nullptr, crcHex.c_str(), 0, 0);
                        amx_PushString(amx, &strAddr[0], nullptr, evt.reason.c_str(), 0, 0);
                        amx_Push(amx, evt.uploadId);
                        amx_Exec(amx, nullptr, idx);
                        amx_Release(amx, strAddr[0]);
                        amx_Release(amx, strAddr[1]);
                    }
                }
                break;

            case UploadEvent::Type::Progress:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, "OnIncomingUploadProgress", &idx) == AMX_ERR_NONE) {
                        amx_Push(amx, evt.progressPct);
                        amx_Push(amx, evt.uploadId);
                        amx_Exec(amx, nullptr, idx);
                    }
                }
                break;
        }
    }

    // Outgoing upload callbacks
    auto outgoingUploadEvents = ImplDrainOutgoingUploadEvents();
    for (const auto& evt : outgoingUploadEvents) {
        switch (evt.type) {
            case OutgoingUploadEvent::Type::Started:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, "OnOutgoingUploadStarted", &idx) == AMX_ERR_NONE) {
                        amx_Push(amx, evt.uploadId);
                        amx_Exec(amx, nullptr, idx);
                    }
                }
                break;

            case OutgoingUploadEvent::Type::Progress:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, "OnOutgoingUploadProgress", &idx) == AMX_ERR_NONE) {
                        amx_Push(amx, evt.progressPct);
                        amx_Push(amx, evt.uploadId);
                        amx_Exec(amx, nullptr, idx);
                    }
                }
                break;

            case OutgoingUploadEvent::Type::Completed:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, "OnOutgoingUploadCompleted", &idx) == AMX_ERR_NONE) {
                        std::string crcHex = CRC32::toHex(evt.crc32Checksum);
                        cell strAddr[2];
                        amx_PushString(amx, &strAddr[1], nullptr, crcHex.c_str(), 0, 0);
                        amx_PushString(amx, &strAddr[0], nullptr, evt.responseBody.c_str(), 0, 0);
                        amx_Push(amx, evt.httpStatus);
                        amx_Push(amx, evt.uploadId);
                        amx_Exec(amx, nullptr, idx);
                        amx_Release(amx, strAddr[0]);
                        amx_Release(amx, strAddr[1]);
                    }
                }
                break;

            case OutgoingUploadEvent::Type::Failed:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, "OnOutgoingUploadFailed", &idx) == AMX_ERR_NONE) {
                        cell strAddr[2];
                        amx_Push(amx, evt.httpStatus);
                        amx_PushString(amx, &strAddr[1], nullptr, evt.errorMessage.c_str(), 0, 0);
                        amx_PushString(amx, &strAddr[0], nullptr, evt.errorType.c_str(), 0, 0);
                        amx_Push(amx, evt.errorCode);
                        amx_Push(amx, evt.uploadId);
                        amx_Exec(amx, nullptr, idx);
                        amx_Release(amx, strAddr[0]);
                        amx_Release(amx, strAddr[1]);
                    }
                }
                break;
        }
    }

    // Outgoing request callbacks
    auto requestEvents = ImplDrainRequestEvents();
    for (const auto& evt : requestEvents) {
        switch (evt.type) {
            case OutgoingRequestEvent::Type::CompletedText:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, evt.callbackName.c_str(), &idx) == AMX_ERR_NONE) {
                        cell strAddr;
                        amx_Push(amx, static_cast<cell>(evt.responseBody.size()));
                        amx_PushString(amx, &strAddr, nullptr, evt.responseBody.c_str(), 0, 0);
                        amx_Push(amx, evt.httpStatus);
                        amx_Push(amx, evt.requestId);
                        amx_Exec(amx, nullptr, idx);
                        amx_Release(amx, strAddr);
                    }
                }
                break;

            case OutgoingRequestEvent::Type::CompletedJson:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, evt.callbackName.c_str(), &idx) == AMX_ERR_NONE) {
                        amx_Push(amx, evt.nodeId);
                        amx_Push(amx, evt.httpStatus);
                        amx_Push(amx, evt.requestId);
                        amx_Exec(amx, nullptr, idx);
                    }
                }
                break;

            case OutgoingRequestEvent::Type::Failed:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, "OnRequestFailure", &idx) == AMX_ERR_NONE) {
                        cell strAddr[2];
                        amx_Push(amx, evt.httpStatus);
                        amx_PushString(amx, &strAddr[1], nullptr, evt.errorMessage.c_str(), 0, 0);
                        amx_PushString(amx, &strAddr[0], nullptr, evt.errorType.c_str(), 0, 0);
                        amx_Push(amx, evt.errorCode);
                        amx_Push(amx, evt.requestId);
                        amx_Exec(amx, nullptr, idx);
                        amx_Release(amx, strAddr[0]);
                        amx_Release(amx, strAddr[1]);
                    }
                }
                break;
        }
    }

    // WebSocket callbacks
    auto wsEvents = ImplDrainWebSocketEvents();
    for (const auto& evt : wsEvents) {
        switch (evt.type) {
            case WebSocketEvent::Type::MessageText:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, evt.callbackName.c_str(), &idx) == AMX_ERR_NONE) {
                        cell strAddr;
                        amx_Push(amx, static_cast<cell>(evt.textPayload.size()));
                        amx_PushString(amx, &strAddr, nullptr, evt.textPayload.c_str(), 0, 0);
                        amx_Push(amx, evt.socketId);
                        amx_Exec(amx, nullptr, idx);
                        amx_Release(amx, strAddr);
                    }
                }
                break;

            case WebSocketEvent::Type::MessageJson:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, evt.callbackName.c_str(), &idx) == AMX_ERR_NONE) {
                        amx_Push(amx, evt.jsonNodeId);
                        amx_Push(amx, evt.socketId);
                        amx_Exec(amx, nullptr, idx);
                    }
                }
                break;

            case WebSocketEvent::Type::Disconnected:
                for (AMX* amx : amxList) {
                    int idx = -1;
                    if (amx_FindPublic(amx, "OnWebSocketDisconnect", &idx) == AMX_ERR_NONE) {
                        cell strAddr;
                        amx_Push(amx, evt.errorCode);
                        amx_Push(amx, static_cast<cell>(evt.closeReason.size()));
                        amx_PushString(amx, &strAddr, nullptr, evt.closeReason.c_str(), 0, 0);
                        amx_Push(amx, evt.closeStatus);
                        amx_Push(amx, evt.isJson ? 1 : 0);
                        amx_Push(amx, evt.socketId);
                        amx_Exec(amx, nullptr, idx);
                        amx_Release(amx, strAddr);
                    }
                }
                break;
        }
    }
}

// -----------------------------------------------------------------------------
// SA-MP plugin entry points
// -----------------------------------------------------------------------------

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports() {
    return SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void** ppData) {
    pAMXFunctions = ppData[PLUGIN_DATA_AMX_EXPORTS];
    logprintf = reinterpret_cast<logprintf_t>(ppData[PLUGIN_DATA_LOGPRINTF]);
    ImplSetLogger(&PawnRestLogSink);

    if (!ImplInitialize()) {
        return false;
    }

    g_LastDrainTime = std::chrono::steady_clock::now();

    logprintf("");
    logprintf("  PawnREST v1.3.0 loaded (SA-MP)");
    logprintf("  Author: Fanorisky (https://github.com/Fanorisky/PawnREST)");
    logprintf("");

    return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload() {
    logprintf("");
    logprintf("  PawnREST v1.3.0 unloaded");
    logprintf("");

    ImplShutdown();
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick() {
    auto now = std::chrono::steady_clock::now();
    int intervalMs = 1000 / Config::DRAIN_FPS;
    if (now - g_LastDrainTime >= std::chrono::milliseconds(intervalMs)) {
        DrainAllEvents();
        g_LastDrainTime = now;
    }
}

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX* amx) {
    std::lock_guard<std::mutex> lock(g_AmxListMutex);
    g_AmxList.push_back(amx);
    return amx_Register(amx, PluginNatives, -1);
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX* amx) {
    std::lock_guard<std::mutex> lock(g_AmxListMutex);
    g_AmxList.erase(std::remove(g_AmxList.begin(), g_AmxList.end(), amx), g_AmxList.end());
    return AMX_ERR_NONE;
}
