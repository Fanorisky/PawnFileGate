#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Pawn/Impl/pawn_natives.hpp>
#include <Server/Components/Pawn/Impl/pawn_impl.hpp>
#include <Server/Components/Timers/timers.hpp>

#include <httplib.h>
#include <crc32.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cctype>

#ifndef PAWNREST_HAS_SSL
#define PAWNREST_HAS_SSL 0
#endif

// Platform-specific includes
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <direct.h>
    #include <io.h>
    #include <windows.h>
    #define mkdir(path, mode) _mkdir(path)
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <ctime>
#endif

namespace Config {
    inline constexpr int DRAIN_FPS = 30;
    inline constexpr int MAX_CONCURRENT = 16;
    inline constexpr size_t TEMP_CLEANUP_SEC = 3600;
    inline const char* TEMP_DIR = ".tmp/";
    inline constexpr int UPLOAD_TIMEOUT_SEC = 300;
    inline constexpr size_t UPLOAD_CHUNK_SIZE = 65536; // 64KB
}

class PawnRESTComponent;
static PawnRESTComponent* g_Component = nullptr;
static PawnRESTComponent* GetComponent() { return g_Component; }

cell AMX_NATIVE_CALL REST_JsonObjectVariadic(AMX* amx, const cell* params);
cell AMX_NATIVE_CALL REST_JsonArrayVariadic(AMX* amx, const cell* params);

enum class ConflictMode {
    Rename,
    Overwrite,
    Reject
};

enum class CorruptAction {
    Delete,
    Quarantine,
    Keep
};

enum class UploadStatus {
    Pending = 0,
    Uploading = 1,
    Completed = 2,
    Failed = 3,
    Cancelled = 4
};

enum class UploadMode {
    Multipart = 0,
    Raw = 1
};

enum class RequestStatus {
    Pending = 0,
    Requesting = 1,
    Completed = 2,
    Failed = 3,
    Cancelled = 4
};

struct UploadRoute {
    int         routeId = -1;
    std::string endpoint;
    std::string destinationPath;
    std::vector<std::string> allowedExtensions;
    std::unordered_set<std::string> authorizedKeys;
    size_t      maxSizeBytes = 10 * 1024 * 1024;
    ConflictMode onConflict  = ConflictMode::Rename;
    CorruptAction onCorrupt  = CorruptAction::Delete;
    std::string quarantinePath = "quarantine/";
    bool        requireCrc32 = false;
    
    // REST API permissions for file routes
    bool        allowList = false;
    bool        allowDownload = false;
    bool        allowDelete = false;
    bool        allowInfo = false;
};

struct UploadEvent {
    enum class Type { Completed, Failed, Progress };

    Type        type;
    int         uploadId  = -1;
    int         routeId   = -1;
    std::string endpoint;
    std::string filename;
    std::string filepath;
    std::string reason;
    int         progressPct = 0;

    uint32_t    crc32Checksum = 0;
    bool        crc32Matched = true;
    std::string expectedCrc32;
};

struct OutgoingUpload {
    int         uploadId = -1;
    std::string url;
    std::string filepath;
    std::string filename;
    std::string authKey;
    std::string customHeader;
    bool        calculateCrc32 = true;
    UploadMode  mode = UploadMode::Multipart;
    bool        verifyTls = true;

    UploadStatus status = UploadStatus::Pending;
    int         progressPct = 0;
    uint32_t    crc32Checksum = 0;
    std::string responseBody;
    int         httpStatus = 0;
    int         errorCode = 0;
    std::string errorType;
    std::string errorMessage;
    size_t      bytesUploaded = 0;
    size_t      totalBytes = 0;

    std::shared_ptr<std::atomic<bool>> cancelToken = std::make_shared<std::atomic<bool>>(false);
};

struct UploadClient {
    int         clientId = -1;
    std::string baseUrl;
    std::unordered_map<std::string, std::string> defaultHeaders;
    bool        verifyTls = true;
};

struct OutgoingUploadEvent {
    enum class Type { Started, Progress, Completed, Failed };

    Type        type;
    int         uploadId = -1;
    int         progressPct = 0;
    uint32_t    crc32Checksum = 0;
    int         httpStatus = 0;
    int         errorCode = 0;
    std::string errorType;
    std::string responseBody;
    std::string errorMessage;
};

struct OutgoingRequest {
    int         requestId = -1;
    std::string url;
    int         method = 0;
    std::string callbackName;
    std::string body;
    std::string customHeader;
    bool        expectJson = false;
    bool        verifyTls = true;

    RequestStatus status = RequestStatus::Pending;
    int         httpStatus = 0;
    int         errorCode = 0;
    std::string errorType;
    std::string errorMessage;
    std::string responseBody;

    std::shared_ptr<std::atomic<bool>> cancelToken = std::make_shared<std::atomic<bool>>(false);
};

struct OutgoingRequestEvent {
    enum class Type { CompletedText, CompletedJson, Failed };

    Type        type = Type::Failed;
    int         requestId = -1;
    std::string callbackName;
    int         httpStatus = 0;
    std::string responseBody;
    int         nodeId = -1;
    int         errorCode = 0;
    std::string errorType;
    std::string errorMessage;
};

struct WebSocketConnection {
    int         socketId = -1;
    std::string address;
    std::string callbackName;
    bool        jsonMode = false;
    bool        verifyTls = true;
    std::unordered_map<std::string, std::string> headers;
    std::unique_ptr<httplib::ws::WebSocketClient> client;
    std::shared_ptr<std::atomic<bool>> stopToken = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> disconnectEmitted { false };
    std::thread readThread;
    std::mutex sendMutex;
    int closeStatus = static_cast<int>(httplib::ws::CloseStatus::Normal);
    std::string closeReason;
};

struct WebSocketEvent {
    enum class Type { MessageText, MessageJson, Disconnected };

    Type        type = Type::MessageText;
    int         socketId = -1;
    std::string callbackName;
    bool        isJson = false;
    std::string textPayload;
    int         jsonNodeId = -1;
    int         closeStatus = static_cast<int>(httplib::ws::CloseStatus::Normal);
    std::string closeReason;
    int         errorCode = 0;
};

struct ValidationResult {
    bool        ok = false;
    std::string reason;
};

// ═══════════════════════════════════════════════════════════════════════════
// REST API Infrastructure
// ═══════════════════════════════════════════════════════════════════════════

enum class HttpMethod {
    GET = 0,
    POST = 1,
    PUT = 2,
    PATCH = 3,
    DELETE_ = 4, // DELETE is a macro on some platforms
    HEAD = 5,
    OPTIONS = 6
};

struct APIRoute {
    int                  routeId = -1;
    HttpMethod           method = HttpMethod::GET;
    std::string          endpoint;       // e.g. "/api/players" or "/api/player/{id}"
    std::string          callbackName;   // Pawn callback name
    std::unordered_set<std::string> authKeys;
    std::vector<std::string> paramNames; // extracted from {name} in endpoint
    std::regex           pattern;        // compiled regex for matching
    bool                 requireAuth = false;
};

struct RequestContext {
    int                  requestId = -1;
    HttpMethod           method = HttpMethod::GET;
    std::string          path;
    std::string          body;
    std::string          clientIP;
    std::unordered_map<std::string, std::string> params;   // URL params like {id}
    std::unordered_map<std::string, std::string> queries;  // Query string ?a=1&b=2
    std::unordered_map<std::string, std::string> headers;
    
    // Response building
    std::string          responseJson;
    std::vector<std::string> jsonStack;  // for nested objects/arrays
    bool                 jsonStarted = false;
    std::unordered_map<std::string, std::string> responseHeaders;
    
    // State
    std::atomic<bool>    responded { false };
    httplib::Response*   httpRes = nullptr;
};

struct APIRequestEvent {
    int         requestId = -1;
    int         routeId = -1;
    std::string callbackName;
};
